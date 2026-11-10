#include "cli_run_output.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>
#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
namespace {
std::string encode(const Json::Value& value) {
  Json::StreamWriterBuilder writer; writer["indentation"] = "";
  return Json::writeString(writer,value);
}
}
#endif

namespace {
class RunEventWriter final : public CliTranscriptWriter {
public:
  explicit RunEventWriter(CliRunOutput* output) : output_(output) {}
  void on_event(const char* type, const char* data) override { output_->loop_event(type,data); }
private:
  CliRunOutput* output_;
};
}

CliRunOutput::~CliRunOutput() { cleanup(); }
void CliRunOutput::cleanup() {
  if (temporary_file_) { std::fclose(temporary_file_); temporary_file_ = nullptr; }
  if (!temporary_path_.empty()) {
    std::error_code ignored;
    std::filesystem::remove(temporary_path_,ignored);
    temporary_path_.clear();
  }
}

bool CliRunOutput::parse_options(std::vector<std::string>* args, std::string* error) {
  // Discover --json first so later option/setup failures still have a terminal
  // machine-readable result. Local command output keeps its existing format.
  json_ = std::find(args->begin()+1,args->end(),"--json") != args->end();
  bool seen_json = false, seen_path = false;
  for (size_t i = 1; i < args->size();) {
    const auto flag = (*args)[i];
    if (flag == "--json") {
      if (seen_json) { *error = "Duplicate --json flag"; return false; }
      seen_json = true;
      args->erase(args->begin()+i);
    } else if (flag == "--output-last-message" || flag == "-o") {
      if (seen_path || i+1 == args->size() || (*args)[i+1].empty() || (*args)[i+1][0] == '-') {
        *error = "--output-last-message requires one file path"; return false;
      }
      seen_path = true;
      last_message_path_ = (*args)[i+1];
      args->erase(args->begin()+i,args->begin()+i+2);
    } else ++i;
  }
#if !defined(AGENT_HAVE_JSONCPP)
  if (json_) { *error = "--json requires a jsoncpp build"; json_ = false; return false; }
#endif
  if (requested() && (*args)[0] != "exec" && (*args)[0] != "run" && (*args)[0] != "resume") {
    *error = "--json and --output-last-message require exec, run, or resume with a prompt";
    return false;
  }
  return true;
}

bool CliRunOutput::begin(const std::string& command, const std::string& model, const std::string& tools,
                         const std::string& session_id, std::string* error) {
  if (!last_message_path_.empty()) {
    std::error_code ec;
    const auto target = std::filesystem::absolute(last_message_path_,ec);
    if (ec || !target.has_filename() || std::filesystem::is_directory(target,ec)) {
      *error = "Last-message destination must be a file"; return false;
    }
    last_message_path_ = target.string();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 32 && !temporary_file_; ++attempt) {
      const auto candidate = last_message_path_ + ".agent-tmp-" + std::to_string(stamp) + "-" + std::to_string(attempt);
      // C11 exclusive creation avoids truncating another writer or following a
      // pre-existing temporary symlink. Rename stays on the same filesystem.
      temporary_file_ = std::fopen(candidate.c_str(),"wbx");
      if (temporary_file_) temporary_path_ = candidate;
    }
    if (!temporary_file_) { *error = "Cannot create last-message temporary file beside destination"; return false; }
  }
#if defined(AGENT_HAVE_JSONCPP)
  Json::Value data;
  data["command"] = command; data["model"] = model; data["tools"] = tools;
  data["session_id"] = session_id.empty() ? Json::Value() : Json::Value(session_id);
  std::error_code ec;
  const auto cwd = std::filesystem::current_path(ec);
  if (!ec) data["cwd"] = cwd.string();
  emit("run.started",encode(data).c_str());
#else
  (void)command; (void)model; (void)tools; (void)session_id;
#endif
  return true;
}

void CliRunOutput::emit(const char* type, const char* data_json) {
  if (!json_ || finished_) return;
#if defined(AGENT_HAVE_JSONCPP)
  Json::Value data;
  Json::CharReaderBuilder reader; std::string error;
  std::istringstream input(data_json ? data_json : "{}");
  if (!Json::parseFromStream(reader,input,&data,&error)) { data = Json::Value(Json::objectValue); data["unparsed"] = data_json ? data_json : ""; }
  if (!data.isObject()) { Json::Value wrapped; wrapped["value"] = std::move(data); data = std::move(wrapped); }
  Json::Value event;
  event["schema"] = "agentd.cli.event.v1"; event["sequence"] = Json::UInt64(++sequence_);
  event["type"] = type; event["data"] = std::move(data);
  std::cout << encode(event) << '\n' << std::flush;
#else
  (void)type; (void)data_json;
#endif
}

void CliRunOutput::assistant(const std::string& text, bool streamed) {
  assistant_ = text; has_assistant_ = true;
  if (!json_) { std::cout << (streamed ? "" : text) << '\n'; return; }
#if defined(AGENT_HAVE_JSONCPP)
  Json::Value data; data["text"] = text;
  emit("assistant.completed",encode(data).c_str());
#endif
}
void CliRunOutput::delta(const std::string& text) {
  if (!json_) { std::cout << text << std::flush; return; }
#if defined(AGENT_HAVE_JSONCPP)
  Json::Value data; data["delta"] = text;
  emit("assistant.delta",encode(data).c_str());
#endif
}
void CliRunOutput::loop_event(const char* type, const char* data_json) {
  if (!type) return;
  // Raw model request/response blobs can contain the entire prompt and are
  // already available through the opt-in transcript/audit surfaces.
  const std::string name(type);
  if (name == "llm_request" || name == "llm_response") return;
  emit(name == "assistant_delta" ? "assistant.delta" : ("loop."+name).c_str(),data_json);
}
std::unique_ptr<CliTranscriptWriter> CliRunOutput::event_writer() {
  return std::make_unique<RunEventWriter>(this);
}
void CliRunOutput::set_usage(size_t steps, size_t tool_calls) {
  has_usage_ = true; steps_ = steps; tool_calls_ = tool_calls;
}
void CliRunOutput::set_capture(bool truncated, size_t events, size_t bytes) {
  has_capture_ = true; capture_truncated_ = truncated; captured_events_ = events; captured_bytes_ = bytes;
}

int CliRunOutput::finish(int status) {
  if (finished_) return status;
  bool artifact_failed = false;
  if (status == 0 && requested() && !has_assistant_) {
    status = 1; error_ = "Execution ended without an accepted assistant result";
  }
  if (status == 0 && temporary_file_) {
    const auto written = std::fwrite(assistant_.data(),1,assistant_.size(),temporary_file_);
    const auto closed = std::fclose(temporary_file_); temporary_file_ = nullptr;
    std::error_code ec;
    if (written == assistant_.size() && closed == 0) std::filesystem::rename(temporary_path_,last_message_path_,ec);
    if (written != assistant_.size() || closed != 0 || ec) {
      status = 1; artifact_failed = true; error_ = "Cannot publish last-message file; previous destination preserved";
      std::cerr << error_ << '\n';
    } else temporary_path_.clear();
  }
  cleanup();
#if defined(AGENT_HAVE_JSONCPP)
  Json::Value data; data["exit_code"] = status;
  if (status == 0) {
    if (!last_message_path_.empty()) data["last_message_file"] = last_message_path_;
    if (has_usage_) { data["usage"]["steps_executed"] = Json::UInt64(steps_); data["usage"]["tool_calls_executed"] = Json::UInt64(tool_calls_); }
    if (has_capture_) {
      data["capture"]["truncated"] = capture_truncated_;
      data["capture"]["events"] = Json::UInt64(captured_events_);
      data["capture"]["bytes"] = Json::UInt64(captured_bytes_);
    }
  } else {
    data["error"]["code"] = artifact_failed ? "output_write_failed" : status == 2 ? "invalid_arguments" : "execution_failed";
    data["error"]["message"] = error_.empty() ? "CLI execution failed; see stderr diagnostics" : error_;
  }
  emit(status == 0 ? "run.completed" : "run.failed",encode(data).c_str());
#else
  (void)artifact_failed;
#endif
  finished_ = true;
  return status;
}
