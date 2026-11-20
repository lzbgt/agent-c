#include "typesafe_scorer.h"
#include <atomic>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
using namespace host_tools_internal;
void check(bool ok, const std::string &why) {
  if (!ok) {
    std::cerr << why << '\n';
    std::exit(1);
  }
}
std::string compact(const Json::Value &v) {
  Json::StreamWriterBuilder w;
  w["indentation"] = "";
  return Json::writeString(w, v);
}
Json::Value parsed(const std::string &s) {
  Json::Value v;
  Json::CharReaderBuilder r;
  std::string e;
  std::istringstream in(s);
  check(Json::parseFromStream(r, in, &v, &e), "fixture JSON");
  return v;
}
int main(int argc, char **argv) {
  TypeSafeScorerConfig config;
  config.api_key = "synthetic-only-key";
  MemoryScoringRequest q;
  q.question = "Where is the supplier?";
  q.candidates = {{"evidence:" + std::string(64, 'a'), "Supplier is JuniperWorks."},
                  {"evidence:" + std::string(64, 'b'), "JuniperWorks is based in Riverton."}};
  Json::Value response;
  response["model"] = config.model;
  response["usage"]["input_tokens"] = 120;
  response["usage"]["output_tokens"] = 12;
  for (const auto &p : q.candidates) {
    response["answers"][p.id]["type"] = "noul";
    response["answers"][p.id]["noul"] = 0.8;
  }
  int calls = 0, mode = 0;
  TypeSafeTransport fake = [&](const auto &url, const auto &method, const auto &headers,
                               const auto &body, auto timeout, auto cap, const auto &proxy,
                               auto pin, auto cancelled, void *context, bool env_proxy) {
    ++calls;
    check(url == "https://api.typesafe.ai/v1/systemone" && method == "POST" &&
              headers.at("Authorization") == "Bearer synthetic-only-key",
          "incorrect official scorer request");
    check(body.find(config.api_key) == std::string::npos && timeout > 0 && timeout <= 15000 &&
              cap == 65536 && proxy.empty() && !pin && !env_proxy && cancelled &&
              !cancelled(context),
          "credential, proxy, timeout or cancellation boundary");
    auto payload = parsed(body);
    check(payload["questions"].size() == 2 && payload["state"]["original_question"] == q.question,
          "incorrect typed scorer input");
    agentd::HttpClientResult h;
    h.ok = true;
    h.http_status = 200;
    h.response_body = compact(response);
    if (mode == 1) {
      h.ok = false;
      h.timed_out = true;
      h.error = config.api_key;
      h.response_body = config.api_key;
    }
    if (mode == 2) {
      h.http_status = 499;
      h.response_body = config.api_key;
    }
    if (mode == 3)
      h.http_status = 429;
    if (mode == 4)
      h.http_status = 401;
    if (mode == 5)
      h.response_body = "{\"model\":1,\"model\":2}";
    return h;
  };
  std::string error;
  MemoryScoringResult result;
  TypeSafeScorer valid(config, fake);
  check(valid.score(q, &result, &error) && result.scores.size() == 2 &&
            result.model == config.model &&
            valid.usage()["actual_input_tokens"].asUInt64() == 120 &&
            valid.usage()["unknown_token_reserves"].asUInt64() == 0,
        "valid provider result/usage failed");
  for (mode = 1; mode <= 5; ++mode) {
    calls = 0;
    TypeSafeScorer client(config, fake);
    check(!client.score(q, &result, &error) && calls == (mode == 3 ? 2 : 1),
          "unbounded/incorrect retry policy");
    check(error.find(config.api_key) == std::string::npos &&
              compact(result.usage).find(config.api_key) == std::string::npos,
          "provider diagnostic exposed credential");
    check(client.usage()["accounted_input_tokens"].asUInt64() == uint64_t(calls) * 65536,
          "unknown input usage reservation lost");
    if (mode == 4)
      check(!client.score(q, &result, &error) && calls == 1, "authentication failure was retried");
  }
  mode = 0;
  const auto original = response;
  for (int failure = 0; failure < 8; ++failure) {
    response = original;
    auto &answer = response["answers"][q.candidates[0].id];
    if (failure == 0)
      answer["noul"] = 1.1;
    if (failure == 1)
      answer["noul"] = -0.1;
    if (failure == 2)
      answer["noul"] = "0.8";
    if (failure == 3)
      answer["noul"] = true;
    if (failure == 4)
      answer["type"] = "score";
    if (failure == 5)
      response["answers"].removeMember(q.candidates[1].id);
    if (failure == 6)
      response["model"] = "jev-1.13.0";
    if (failure == 7)
      response["usage"]["input_tokens"] = 65537;
    TypeSafeScorer client(config, fake);
    check(!client.score(q, &result, &error), "invalid typed scorer result accepted");
  }
  response = original;
  auto capped = config;
  capped.max_attempts = 1;
  TypeSafeScorer attempt_cap(capped, fake);
  calls = 0;
  check(attempt_cap.score(q, &result, &error) && !attempt_cap.score(q, &result, &error) &&
            calls == 1,
        "attempt cap exceeded");
  capped = config;
  capped.max_judgments = 1;
  calls = 0;
  TypeSafeScorer judgment_cap(capped, fake);
  check(!judgment_cap.score(q, &result, &error) && calls == 0, "judgment cap admitted HTTP");
  capped = config;
  capped.input_token_budget = 65535;
  TypeSafeScorer token_cap(capped, fake);
  check(!token_cap.score(q, &result, &error) && calls == 0, "token cap admitted HTTP");
  q.cancelled = [] { return true; };
  TypeSafeScorer cancelled(config, fake);
  check(!cancelled.score(q, &result, &error) && calls == 0, "pre-cancelled scorer sent HTTP");
  q.cancelled = {};
  const auto temp = std::filesystem::temp_directory_path() /
                    ("dotenv-scorer-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::string key;
  auto dotenv = [&](const std::string &s) {
    std::ofstream(temp) << s;
    return typesafe_key_from_dotenv(temp, &key, &error);
  };
  check(dotenv("UNRELATED=$(do_not_execute)\nexport\tTYPESAFE_API_KEY='synthetic-only-key' # "
               "comment\n") &&
            key == config.api_key,
        "needed dotenv literal entry not parsed");
  check(!dotenv("TYPESAFE_API_KEY=one\nTYPESAFE_API_KEY=two\n") && key.empty(),
        "duplicate key not refused");
  check(!dotenv("TYPESAFE_API_KEY=\"$(do_not_execute)\"\n"), "dotenv interpolation accepted");
  check(!dotenv("OTHER=unrelated\n"), "missing credential accepted");
  std::filesystem::remove(temp);
  {
    int discoveries = 0;
    TypeSafeTransport discovery = [&](const auto &url, const auto &method, const auto &,
                                      const auto &body, auto, auto, const auto &, auto, auto,
                                      void *, bool env) {
      ++discoveries;
      check(url == "https://api.typesafe.ai/v1/models" && method == "GET" && body.empty() && !env,
            "incorrect account discovery boundary");
      agentd::HttpClientResult h;
      h.ok = true;
      h.http_status = 200;
      h.response_body = "{\"models\":[{\"name\":\"jev-latest\",\"release_date\":\"2026-10-14\","
                        "\"description\":\"not copied\"}]}";
      return h;
    };
    TypeSafeScorer client(config, discovery);
    Json::Value models;
    check(client.discover(&models, {}, &error) && discoveries == 1 && models.size() == 1 &&
              !models[0].isMember("description") &&
              client.usage()["accounted_input_tokens"].asUInt64() == 0,
          "account discovery consumed scoring tokens or copied unrelated provider data");
  }
  // The Python fixture supplies a loopback server; only this test transport rewrites the fixed
  // official URL.
  if (argc == 2) {
    const std::string base = argv[1];
    std::string path = "/ok";
    TypeSafeTransport loopback = [&](const auto &, const auto &method, const auto &headers,
                                     const auto &body, auto timeout, auto cap, const auto &proxy,
                                     auto pin, auto cancel, void *context, bool env) {
      return agentd::http_request(base + path, method, headers, body, timeout, cap, proxy, pin,
                                  cancel, context, env);
    };
    TypeSafeScorer actual(config, loopback);
    check(actual.score(q, &result, &error), "loopback typed HTTP failed");
    path = "/oversize";
    TypeSafeScorer large(config, loopback);
    check(!large.score(q, &result, &error) && large.usage()["http_attempts"].asUInt64() == 1,
          "response byte cap or no-replay boundary failed");
    path = "/slow";
    capped = config;
    capped.request_timeout = std::chrono::milliseconds(50);
    TypeSafeScorer timeout(capped, loopback);
    const auto start = std::chrono::steady_clock::now();
    check(!timeout.score(q, &result, &error) &&
              std::chrono::steady_clock::now() - start < std::chrono::seconds(2),
          "real HTTP timeout was not bounded");
    std::atomic<bool> stop{false};
    q.cancelled = [&] { return stop.load(); };
    capped = config;
    capped.request_timeout = std::chrono::milliseconds(2000);
    TypeSafeScorer stopped(capped, loopback);
    std::thread signal([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      stop = true;
    });
    const bool success = stopped.score(q, &result, &error);
    signal.join();
    check(!success && stopped.usage()["http_attempts"].asUInt64() == 1,
          "real HTTP cancellation was replayed");
  }
  std::cout << "TypeSafe scorer: typed results, budgets, secrets, retry, HTTP byte/timeout/cancel "
               "boundaries passed\n";
}
