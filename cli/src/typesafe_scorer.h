#pragma once
#include "memory_retrieval.h"
#if defined(AGENT_HAVE_JSONCPP)
#include "http_client.h"
#include <type_traits>

namespace host_tools_internal {
constexpr uint64_t kTypeSafeRequestTokenReserve = 65536;
constexpr double kTypeSafeUsdPerMillionInput = 0.042;
struct TypeSafeScorerConfig {
  std::string api_key, model = "jev-1.14.0";
  size_t max_attempts = 75, max_judgments = 1300, attempts_per_call = 2;
  uint64_t input_token_budget = 75 * kTypeSafeRequestTokenReserve;
  std::chrono::milliseconds request_timeout{15000}, total_timeout{600000};
};
using TypeSafeTransport = std::function<std::remove_pointer_t<decltype(&agentd::http_request)>>;
// Serial foreground client. One instance owns the whole evaluation budget, not
// one instance per candidate/query. Unknown usage keeps its conservative reserve.
class TypeSafeScorer {
public:
  explicit TypeSafeScorer(TypeSafeScorerConfig, TypeSafeTransport = agentd::http_request);
  bool discover(Json::Value *models, std::function<bool()> cancelled, std::string *error);
  bool score(const MemoryScoringRequest &, MemoryScoringResult *, std::string *error);
  Json::Value usage() const;

private:
  bool call(const std::string &path, const Json::Value &body, size_t judgments,
            std::function<bool()> cancelled, Json::Value *response, std::string *error);
  TypeSafeScorerConfig config_;
  TypeSafeTransport transport_;
  std::chrono::steady_clock::time_point deadline_;
  size_t attempts_ = 0, judgments_ = 0;
  uint64_t budget_tokens_ = 0, actual_tokens_ = 0, output_tokens_ = 0, unknown_reserves_ = 0;
  Json::Value attempts_log_{Json::arrayValue};
  std::string disabled_;
};
// Reads only the exact TYPESAFE_API_KEY entry as literal dotenv data. No shell,
// interpolation, whole-environment import or value-bearing diagnostic.
bool typesafe_key_from_dotenv(const std::filesystem::path &, std::string *key, std::string *error);
} // namespace host_tools_internal
#endif
