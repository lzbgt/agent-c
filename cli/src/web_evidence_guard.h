#pragma once

#include <cstddef>
#include <ctime>
#include <string>
#include <vector>

namespace Json { class Value; }

// Host-only provenance check. It checks citation presence, not claim truth or
// publication/event freshness. Source metadata remains untrusted evidence.
class WebEvidenceGuard {
 public:
  enum class Action { Accept, Repair, Reject };
  struct Decision { Action action = Action::Accept; std::string prompt; };

  explicit WebEvidenceGuard(std::time_t now = std::time(nullptr)) : now_(now) {}
  void observe_tool_result(const std::string& executed_tool, const Json::Value& result);
  void observe_assistant_message(const std::string& text);
  Decision check_completion();
  const std::string& failure_message() const { return failure_message_; }

 private:
  struct Source {
    std::string url;
    std::string published_at;
    bool fetched = false;
  };
  void add_source(Source source);
  std::string repair_prompt() const;
  std::vector<Source> sources_;
  std::string answer_;
  bool repaired_ = false;
  std::string failure_message_;
  std::time_t now_;
};
