#pragma once
#include "cli_transcript_writer.h"
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

// One non-interactive invocation owns its output lifecycle. Loop events are
// progress; only finish() can publish successful execution/artifact completion.
class CliRunOutput {
public:
  CliRunOutput() = default;
  CliRunOutput(const CliRunOutput&) = delete;
  CliRunOutput& operator=(const CliRunOutput&) = delete;
  ~CliRunOutput();
  bool parse_options(std::vector<std::string>* args, std::string* error);
  bool requested() const { return json_ || !last_message_path_.empty(); }
  bool json() const { return json_; }
  bool begin(const std::string& command, const std::string& model, const std::string& tools,
             const std::string& session_id, std::string* error);
  void assistant(const std::string& text, bool streamed = false);
  void delta(const std::string& text);
  void loop_event(const char* type, const char* data_json);
  std::unique_ptr<CliTranscriptWriter> event_writer();
  void set_error(const std::string& message) { error_ = message; }
  void set_usage(size_t steps, size_t tool_calls);
  void set_capture(bool truncated, size_t events, size_t bytes);
  int finish(int status);

private:
  bool json_ = false;
  bool has_assistant_ = false;
  bool finished_ = false;
  uint64_t sequence_ = 0;
  std::string last_message_path_, temporary_path_, assistant_, error_;
  FILE* temporary_file_ = nullptr;
  bool has_usage_ = false;
  size_t steps_ = 0, tool_calls_ = 0;
  bool has_capture_ = false, capture_truncated_ = false;
  size_t captured_events_ = 0, captured_bytes_ = 0;
  void emit(const char* type, const char* data_json);
  void cleanup();
};
