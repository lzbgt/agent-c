#include "tool_loop_compaction.h"

#if !defined(AGENT_HAVE_JSONCPP)
int main() { return 0; }
#else

#include <json/json.h>

#include <iostream>

static int g_failures = 0;

static void assert_true(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    g_failures++;
  }
}

static Json::Value msg(const char* role, const char* content, const char* name = nullptr) {
  Json::Value m(Json::objectValue);
  m["role"] = role;
  m["content"] = content;
  if (name) {
    m["name"] = name;
  }
  return m;
}

static void test_basic_compaction_inserts_summary() {
  Json::Value messages(Json::arrayValue);
  messages.append(msg("system", "s1"));
  messages.append(msg("system", "s2"));
  messages.append(msg("user", "u1"));
  messages.append(msg("assistant", "a1"));
  messages.append(msg("user", "u2"));
  messages.append(msg("assistant", "a2"));

  ToolLoopCompactionOptions opt;
  opt.max_chars = 10; // force compaction
  opt.keep_last_messages = 2;
  opt.insert_summary = true;

  ToolLoopCompactionReport rep;
  const bool changed = tool_loop_compaction_maybe_compact(&messages, opt, &rep);
  assert_true(changed, "expected compaction to change messages");
  assert_true(rep.pinned_system_messages == 2, "expected pinned_system_messages=2");
  assert_true(rep.dropped_messages == 2, "expected dropped_messages=2");
  assert_true(rep.inserted_summary, "expected inserted_summary");
  assert_true(!rep.summary.empty(), "expected non-empty summary");

  assert_true(messages.isArray(), "messages must be array");
  assert_true(messages.size() == 5, "expected 5 messages after compaction (2 pinned + summary + 2 suffix)");
  assert_true(messages[0]["role"].asString() == "system" && messages[0]["content"].asString() == "s1", "pinned[0] mismatch");
  assert_true(messages[1]["role"].asString() == "system" && messages[1]["content"].asString() == "s2", "pinned[1] mismatch");
  assert_true(messages[2]["role"].asString() == "system", "summary role mismatch");
  assert_true(messages[2]["name"].asString() == tool_loop_compaction_summary_name(), "summary name mismatch");
  assert_true(messages[3]["role"].asString() == "user" && messages[3]["content"].asString() == "u2", "suffix user mismatch");
  assert_true(messages[4]["role"].asString() == "assistant" && messages[4]["content"].asString() == "a2", "suffix assistant mismatch");
}

static void test_summary_is_not_pinned() {
  Json::Value messages(Json::arrayValue);
  messages.append(msg("system", "pinned"));
  messages.append(msg("system", "old summary", tool_loop_compaction_summary_name()));
  messages.append(msg("system", "not pinned"));
  messages.append(msg("user", "u"));

  const size_t pinned = tool_loop_compaction_pinned_system_prefix_count(messages);
  assert_true(pinned == 1, "expected pinned_system_prefix_count to stop at summary name");
}

int main() {
  test_basic_compaction_inserts_summary();
  test_summary_is_not_pinned();

  if (g_failures) {
    std::cerr << g_failures << " failures\n";
    return 1;
  }
  return 0;
}

#endif

