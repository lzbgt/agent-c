#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

struct CliTranscriptStyle {
  bool use_color = false;
  bool dim_prefix = true;
};

// Event writers consume tool-loop events and emit user-facing transcript output.
// Designed to support multiple sinks (TTY + JSONL file) via composition.
class CliTranscriptWriter {
public:
  virtual ~CliTranscriptWriter() = default;

  virtual void on_event(const char* type, const char* data_json) = 0;
  virtual void flush() {}
};

class CliTranscriptWriterMux final : public CliTranscriptWriter {
public:
  void add(std::unique_ptr<CliTranscriptWriter> w);
  void on_event(const char* type, const char* data_json) override;
  void flush() override;

private:
  std::vector<std::unique_ptr<CliTranscriptWriter>> writers_;
};

// Human-friendly console transcript (IM-style): shows tool use + results, hides noisy JSON-ish fields.
class CliPrettyConsoleWriter final : public CliTranscriptWriter {
public:
  struct Options {
    std::ostream* out = nullptr; // typically stderr
    std::ostream* assistant_delta_out = nullptr; // typically stdout
    bool stream_deltas = false;
    CliTranscriptStyle style;
  };

  explicit CliPrettyConsoleWriter(Options opt);
  void on_event(const char* type, const char* data_json) override;
  void flush() override;

private:
  Options opt_;
  uint64_t tool_index_ = 0;

#if defined(AGENT_HAVE_JSONCPP)
  static bool parse_json_any(const std::string& s, Json::Value* out_v);
  static bool parse_json_object(const std::string& s, Json::Value* out_obj);
  static std::string json_get_string(const Json::Value& obj, const char* key);
  static int64_t json_get_i64(const Json::Value& obj, const char* key, int64_t def = 0);
  static bool json_get_bool(const Json::Value& obj, const char* key, bool def = false);

  void print_dim(const std::string& s);
  void print_error_line(const std::string& s);
  void print_ok_line(const std::string& s);
  void print_header_line(const std::string& s);

  void on_tool_call(const Json::Value& data);
  void on_tool_result(const Json::Value& data);

  static std::string join_argv(const Json::Value& argv);
  static std::string format_tool_invocation(const std::string& tool_name, const std::string& args_json);
  static std::string summarize_failure_reason(const std::string& tool_name, const std::string& tool_out);
  static void print_output_block(std::ostream& os, const CliTranscriptStyle& style, const std::string& out);
#endif
};

// JSONL transcript sink: each event becomes one JSON line, suitable for later replay/debugging.
class CliJsonlFileWriter final : public CliTranscriptWriter {
public:
  explicit CliJsonlFileWriter(const std::string& path);
  ~CliJsonlFileWriter() override;

  void on_event(const char* type, const char* data_json) override;
  void flush() override;

private:
  std::string path_;
  void* fp_ = nullptr; // FILE*, kept as void* to avoid headers in the interface.
};

// Best-effort default: enable ANSI colors only when writing to an interactive TTY.
CliTranscriptStyle cli_default_transcript_style(bool is_tty);

