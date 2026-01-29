#include "toolset_basic.h"

#include <cstring>
#include <cmath>
#include <cctype>
#include <optional>
#include <sstream>
#include <string>

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

namespace {

struct Parser {
  const std::string& s;
  size_t i = 0;

  void skip_ws() {
    while (i < s.size() && std::isspace((unsigned char)s[i])) {
      i++;
    }
  }

  bool eat(char c) {
    skip_ws();
    if (i < s.size() && s[i] == c) {
      i++;
      return true;
    }
    return false;
  }

  std::optional<double> parse_number() {
    skip_ws();
    size_t start = i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
      i++;
    }
    bool any = false;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) {
      any = true;
      i++;
    }
    if (i < s.size() && s[i] == '.') {
      i++;
      while (i < s.size() && std::isdigit((unsigned char)s[i])) {
        any = true;
        i++;
      }
    }
    if (!any) {
      i = start;
      return std::nullopt;
    }
    try {
      return std::stod(s.substr(start, i - start));
    } catch (...) {
      i = start;
      return std::nullopt;
    }
  }

  std::optional<double> parse_factor() {
    skip_ws();
    if (eat('(')) {
      auto v = parse_expr();
      if (!v.has_value() || !eat(')')) {
        return std::nullopt;
      }
      return v;
    }
    return parse_number();
  }

  std::optional<double> parse_term() {
    auto v = parse_factor();
    if (!v.has_value()) {
      return std::nullopt;
    }
    while (true) {
      if (eat('*')) {
        auto rhs = parse_factor();
        if (!rhs.has_value()) {
          return std::nullopt;
        }
        *v *= *rhs;
      } else if (eat('/')) {
        auto rhs = parse_factor();
        if (!rhs.has_value() || *rhs == 0.0) {
          return std::nullopt;
        }
        *v /= *rhs;
      } else {
        break;
      }
    }
    return v;
  }

  std::optional<double> parse_expr() {
    auto v = parse_term();
    if (!v.has_value()) {
      return std::nullopt;
    }
    while (true) {
      if (eat('+')) {
        auto rhs = parse_term();
        if (!rhs.has_value()) {
          return std::nullopt;
        }
        *v += *rhs;
      } else if (eat('-')) {
        auto rhs = parse_term();
        if (!rhs.has_value()) {
          return std::nullopt;
        }
        *v -= *rhs;
      } else {
        break;
      }
    }
    return v;
  }
};

static std::optional<double> eval_expression(const std::string& expr) {
  Parser p{expr};
  auto v = p.parse_expr();
  p.skip_ws();
  if (!v.has_value() || p.i != p.s.size()) {
    return std::nullopt;
  }
  return v;
}

static agent_status_t tool_exec_basic(void* /*ctx*/, const char* tool_name, const char* arguments_json, agent_string_t* out_result) {
  if (!tool_name || !arguments_json || !out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  if (std::string(tool_name) != "calculator") {
    const char* s = "{\"ok\":false,\"error\":\"unknown tool\"}";
    return agent_string_set_copy(out_result, s, strlen(s));
  }

#if !defined(AGENT_HAVE_JSONCPP)
  {
    const char* s = "{\"ok\":false,\"error\":\"calculator requires jsoncpp\"}";
    return agent_string_set_copy(out_result, s, strlen(s));
  }
#else
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(arguments_json);
  Json::Value args;
  if (!Json::parseFromStream(rb, iss, &args, &errs)) {
    const char* s = "{\"ok\":false,\"error\":\"invalid tool arguments JSON\"}";
    return agent_string_set_copy(out_result, s, strlen(s));
  }
  if (!args.isObject() || !args["expression"].isString()) {
    const char* s = "{\"ok\":false,\"error\":\"calculator expects {expression: string}\"}";
    return agent_string_set_copy(out_result, s, strlen(s));
  }

  const std::string expr = args["expression"].asString();
  auto v = eval_expression(expr);
  if (!v.has_value()) {
    const char* s = "{\"ok\":false,\"error\":\"calculator failed\"}";
    return agent_string_set_copy(out_result, s, strlen(s));
  }

  double d = *v;
  std::string rendered;
  if (std::abs(d - std::llround(d)) < 1e-9) {
    rendered = std::to_string((long long)std::llround(d));
  } else {
    std::ostringstream oss;
    oss.precision(15);
    oss << d;
    rendered = oss.str();
  }
  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["value"] = rendered;
  o["expression"] = expr;
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  const std::string json = Json::writeString(wb, o);
  return agent_string_set_copy(out_result, json.c_str(), json.size());
#endif
}

} // namespace

agent_status_t toolset_basic_create(agent_tool_registry_t** out_registry, agent_tool_executor_t* out_executor) {
  if (!out_registry || !out_executor) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  *out_registry = nullptr;
  out_executor->ctx = nullptr;
  out_executor->execute = nullptr;

  agent_tool_registry_t* r = nullptr;
  agent_status_t st = agent_tool_registry_create(&r);
  if (st != AGENT_OK) {
    return st;
  }

  const char* params =
    "{"
    "\"type\":\"object\","
    "\"properties\":{\"expression\":{\"type\":\"string\",\"description\":\"Arithmetic expression, e.g. (1+2)*3\"}},"
    "\"required\":[\"expression\"]"
    "}";

  st = agent_tool_registry_add(
    r,
    "calculator",
    "Evaluate a basic arithmetic expression with + - * / and parentheses. Returns JSON: {ok, value, expression}.",
    params
  );
  if (st != AGENT_OK) {
    agent_tool_registry_destroy(r);
    return st;
  }

  out_executor->ctx = nullptr;
  out_executor->execute = tool_exec_basic;
  *out_registry = r;
  return AGENT_OK;
}
