#include "web_evidence_guard.h"
#include "tool_loop.h"
#include "openai_tool_provider.h"
#include "agent/tool_loop.h"

#include <json/json.h>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
const std::string kUrl = "https://example.org/source";
void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
std::string encode(const Json::Value& value) {
  Json::StreamWriterBuilder writer; writer["indentation"] = "";
  return Json::writeString(writer, value);
}
Json::Value parse(const std::string& text) {
  Json::CharReaderBuilder reader; Json::Value value; std::string error;
  std::istringstream input(text);
  check(Json::parseFromStream(reader, input, &value, &error), "parse fixture JSON");
  return value;
}
Json::Value fetched(const std::string& url = kUrl) {
  Json::Value result;
  result["ok"] = true;
  result["data"]["url"] = url;
  result["data"]["requested_url"] = url;
  result["data"]["published_at"] = "";
  result["data"]["publication_date_unknown"] = true;
  result["data"]["text"] = "An event happened on 2026-11-12; page publication date is unknown.";
  return result;
}

void test_url_boundaries_and_budget() {
  for (const auto& text : {
      kUrl, "Source: " + kUrl, "[Source](" + kUrl + ")", "**[Source](" + kUrl + ")**",
      "__[Source](" + kUrl + ")__", "[" + kUrl + "](" + kUrl + ")", "<" + kUrl + ">", "Source " + kUrl + ".\n"}) {
    WebEvidenceGuard guard;
    guard.observe_tool_result("web_fetch", fetched());
    guard.observe_assistant_message(text);
    check(guard.check_completion().action == WebEvidenceGuard::Action::Accept,
          "valid returned source citation was rejected");
  }
  for (const auto& text : {
      "https://evil.test/?next=" + kUrl,
      "https://evil.test/?next=(" + kUrl + ")",
      "[source](//evil.test/?next=(" + kUrl + "))",
      "[" + kUrl + "](https://evil.test/)",
      "https://evil.test/?next=[source](" + kUrl + ")",
      "https://evil.test/path/'" + kUrl + "'",
      kUrl + ".evil", kUrl + "/other", kUrl + "?other=1", kUrl + "#fragment",
      kUrl + "_", kUrl + "_suffix", kUrl + ")suffix", "prefix" + kUrl,
      "[Source](" + kUrl + "-different)"}) {
    WebEvidenceGuard guard;
    guard.observe_tool_result("web_fetch", fetched());
    guard.observe_assistant_message(text);
    check(guard.check_completion().action == WebEvidenceGuard::Action::Repair,
          "embedded URL or longer URL prefix satisfied citation check");
    check(guard.check_completion().action == WebEvidenceGuard::Action::Reject,
          "missing citations received more than one repair");
  }

  WebEvidenceGuard no_web;
  no_web.observe_tool_result("proc_exec", fetched());
  no_web.observe_assistant_message("ordinary command result");
  check(no_web.check_completion().action == WebEvidenceGuard::Action::Accept,
        "forged non-web envelope activated guard");
  auto blocked = fetched(); blocked["ok"] = false;
  no_web.observe_tool_result("web_fetch", blocked);
  check(no_web.check_completion().action == WebEvidenceGuard::Action::Accept,
        "failed fetch fabricated an available citation");

  const char* previous_tz = std::getenv("TZ");
  const bool had_tz = previous_tz != nullptr;
  const std::string old_tz = previous_tz ? previous_tz : "";
  setenv("TZ", "CST-8", 1); tzset();
  WebEvidenceGuard bounded(1794510000); // Nov 12 19:00 UTC, Nov 13 local.
  for (int n = 0; n < 20; ++n) {
    Json::Value result; result["ok"] = true;
    result["data"]["results"][0]["url"] = "https://example.net/" + std::string(3000, 'a') + std::to_string(n);
    result["data"]["results"][0]["published_at"] = "2025-11-15";
    bounded.observe_tool_result("web_search", result);
  }
  bounded.observe_tool_result("web_fetch", fetched());
  bounded.observe_assistant_message("A summary without sources.");
  const auto repair = bounded.check_completion();
  check(repair.action == WebEvidenceGuard::Action::Repair && repair.prompt.size() <= 8192,
        "repair total-byte bound failed");
  check(repair.prompt.find("Host-local current date: 2026-11-13 (UTC+0800)") == 0 &&
        repair.prompt.find("UTC reference date: 2026-11-12") != std::string::npos,
        "repair does not put authoritative local date first");
  check(repair.prompt.find(kUrl) != std::string::npos &&
        repair.prompt.find("fetched_page") < repair.prompt.find("search_lead_only") &&
        repair.prompt.find("\"publication_status\":\"unknown\"") != std::string::npos,
        "fetched source priority or unknown date provenance missing");
  bounded.observe_assistant_message("The event date comes from the body. [Source](" + kUrl + ")");
  check(bounded.check_completion().action == WebEvidenceGuard::Action::Accept,
        "unknown page publication incorrectly invalidated a cited dated event");
  if (had_tz) setenv("TZ", old_tz.c_str(), 1); else unsetenv("TZ");
  tzset();
}

struct Scenario {
  bool cite_after_repair = true;
  bool cite_immediately = false;
  bool use_work = false;
  bool cancel_on_answer = false;
  bool cancelled = false;
  int provider_calls = 0;
  int fetch_calls = 0;
  int work_reads = 0;
  int repair_messages = 0;
  int work_messages = 0;
  int citation_retry_events = 0;
  int work_retry_events = 0;
};

OpenAIRawResult fake_provider(const OpenAIClientConfig& cfg, const std::string& body) {
  auto* scenario = static_cast<Scenario*>(cfg.should_cancel_ctx);
  ++scenario->provider_calls;
  const auto request = parse(body);
  const auto& messages = request["messages"];
  check(messages.isArray() && !messages.empty(), "provider fixture received no messages");
  const auto& last = messages[messages.size() - 1];
  const auto text = last["content"].isString() ? last["content"].asString() : "";
  const bool citation_repair = text.find("Your proposed web-assisted answer") != std::string::npos;
  const bool work_repair = text.find("Your durable work ledger") != std::string::npos;
  if (citation_repair) {
    ++scenario->repair_messages;
    check(last["role"] == "user", "citation repair would be dropped as a late system message");
    check(text.find(kUrl) != std::string::npos && text.find("publication_status") != std::string::npos &&
          text.find("Host-local current date:") == 0,
          "citation repair omitted bounded evidence/date context");
  }
  if (work_repair) ++scenario->work_messages;

  Json::Value response;
  auto& message = response["choices"][0]["message"];
  message["role"] = "assistant";
  if (scenario->provider_calls == 1) {
    message["content"] = "";
    const auto add_call = [&](const char* id, const char* name, const std::string& arguments) {
      Json::Value call;
      call["id"] = id; call["type"] = "function";
      call["function"]["name"] = name; call["function"]["arguments"] = arguments;
      message["tool_calls"].append(call);
    };
    if (scenario->use_work) add_call("work", "work_state", "{\"op\":\"adopt\"}");
    add_call("fetch", "web_fetch", "{\"url\":\"" + kUrl + "\"}");
  } else if (scenario->cite_immediately || (citation_repair && scenario->cite_after_repair)) {
    message["content"] = "The dated event is supported by the fetched body; page publication is unknown. [Source](" + kUrl + ")";
  } else {
    message["content"] = "Draft with no source URL.";
  }
  OpenAIRawResult result; result.http_status = 200; result.response_body = encode(response);
  return result;
}

agent_status_t fake_execute(void* context, const char* name, const char* args, agent_string_t* out) {
  auto* scenario = static_cast<Scenario*>(context);
  Json::Value result;
  if (std::string(name) == "web_fetch") {
    ++scenario->fetch_calls;
    result = fetched();
  } else if (std::string(name) == "work_state") {
    result["ok"] = true;
    const auto arguments = parse(args);
    if (arguments["op"] == "read") ++scenario->work_reads;
    result["can_continue"] = scenario->work_reads == 1;
    result["state"]["revision"] = scenario->work_reads;
    result["runnable"].append("finish fixture");
  } else {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  const auto text = encode(result);
  return agent_string_set_copy(out, text.data(), text.size());
}

void observe_event(void* context, const char* type, const char* body) {
  auto* scenario = static_cast<Scenario*>(context);
  const auto data = parse(body);
  if (std::string(type) == "retry") {
    if (data["reason"] == "missing_web_source_citations") ++scenario->citation_retry_events;
    if (data["reason"] == "unfinished_runnable_work") ++scenario->work_retry_events;
  }
  if (scenario->cancel_on_answer && std::string(type) == "assistant_message" &&
      data["has_tool_calls"].asUInt64() == 0) scenario->cancelled = true;
}

void test_composed_guard(Scenario scenario) {
  agent_session_t* session = nullptr;
  check(agent_session_create(&session) == AGENT_OK, "create fixture session");
  agent_tool_registry_t* registry = nullptr;
  check(agent_tool_registry_create(&registry) == AGENT_OK, "create fixture registry");
  check(agent_tool_registry_add(registry, "web_fetch", "fixture", R"({"type":"object","properties":{"url":{"type":"string"}}})") == AGENT_OK, "register web fixture");
  check(agent_tool_registry_add(registry, "work_state", "fixture", R"({"type":"object","properties":{"op":{"type":"string"}}})") == AGENT_OK, "register work fixture");
  agent_tool_executor_t executor{&scenario, fake_execute};
  OpenAIClientConfig cfg; cfg.base_url = "http://fixture.invalid"; cfg.model = "fixture";
  ToolLoopOptions options;
  options.max_steps = 8; options.max_chars = 200000; options.memory_flush_enabled = false;
  options.provider_decorator_ctx = &scenario;
  options.provider_decorator = [](void* context, agent_tool_provider_t provider, OpenAIToolProviderCtx* pctx) {
    pctx->chat_raw_fn = fake_provider; pctx->cfg.should_cancel_ctx = context;
    return provider;
  };
  options.should_cancel = [](void* context) { return static_cast<Scenario*>(context)->cancelled; };
  options.should_cancel_ctx = &scenario;
  options.on_event = observe_event; options.on_event_ctx = &scenario;
  ToolLoopResult result; std::string error;
  const bool ok = run_tool_loop(cfg, session, "Report the fetched event", registry, &executor,
                               options, nullptr, &result, &error, nullptr, nullptr);
  const auto events = parse(result.events_json);
  check(scenario.fetch_calls == 1, "repair unnecessarily repeated retrieval");
  if (scenario.cancel_on_answer) {
    check(!ok && error == "cancelled" && scenario.provider_calls == 2 &&
          scenario.repair_messages == 0, "cancellation did not precede evidence repair");
  } else if (!scenario.cite_after_repair && !scenario.cite_immediately) {
    check(!ok && error.find("Web evidence insufficient:") == 0 &&
          result.final_assistant_text == error, "rejected draft surfaced as accepted final text");
    check(scenario.provider_calls == 3 && scenario.repair_messages == 1 &&
          scenario.citation_retry_events == 1, "citation failure exceeded one repair");
    check(events[events.size() - 1]["data"]["error"] == error &&
          !events[events.size() - 1]["data"]["ok"].asBool(), "evidence failure missing from terminal event");
  } else {
    check(ok && error.empty() && result.final_assistant_text.find(kUrl) != std::string::npos,
          "cited completion did not succeed");
    if (scenario.cite_immediately) {
      check(scenario.provider_calls == 2 && scenario.repair_messages == 0,
            "already-cited answer was needlessly repaired");
    } else {
      check(scenario.repair_messages == 1 && scenario.citation_retry_events == 1,
            "one evidence repair was not surfaced");
    }
    if (scenario.use_work)
      check(scenario.provider_calls == 4 && scenario.work_messages == 1 &&
            scenario.work_retry_events == 1 && scenario.work_reads == 3,
            "web guard bypassed or replaced work-state enforcement");
  }
  agent_tool_registry_destroy(registry);
  agent_session_destroy(session);
}
}  // namespace

int main() {
  try {
    test_url_boundaries_and_budget();
    test_composed_guard({});
    Scenario failure; failure.cite_after_repair = false; test_composed_guard(failure);
    Scenario cited; cited.cite_immediately = true; test_composed_guard(cited);
    Scenario work; work.use_work = true; test_composed_guard(work);
    Scenario cancel; cancel.cancel_on_answer = true; test_composed_guard(cancel);
    std::cout << "web evidence boundaries, one repair, work-state and cancellation checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "web evidence guard test failed: " << error.what() << "\n";
    return 1;
  }
}
