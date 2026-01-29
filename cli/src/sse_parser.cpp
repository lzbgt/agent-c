#include "sse_parser.h"

#include <cctype>

static inline void rstrip_cr(std::string* s) {
  if (!s) return;
  if (!s->empty() && s->back() == '\r') s->pop_back();
}

static inline std::string lstrip_one_space(std::string_view s) {
  if (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  return std::string(s);
}

void SseParser::reset() {
  buf_.clear();
  cur_event_.clear();
  cur_id_.clear();
  cur_data_.clear();
}

void SseParser::flush_event(std::vector<SseEvent>* out_events) {
  if (!out_events) return;
  if (cur_event_.empty() && cur_id_.empty() && cur_data_.empty()) {
    return;
  }
  // Strip the trailing newline inserted between data lines, if any.
  if (!cur_data_.empty() && cur_data_.back() == '\n') {
    cur_data_.pop_back();
  }
  SseEvent ev;
  ev.event = cur_event_;
  ev.id = cur_id_;
  ev.data = cur_data_;
  out_events->push_back(std::move(ev));
  cur_event_.clear();
  cur_id_.clear();
  cur_data_.clear();
}

void SseParser::handle_line(const std::string& raw_line, std::vector<SseEvent>* out_events) {
  std::string line = raw_line;
  rstrip_cr(&line);

  if (line.empty()) {
    flush_event(out_events);
    return;
  }
  // Comment line.
  if (!line.empty() && line[0] == ':') {
    return;
  }

  const size_t colon = line.find(':');
  const std::string field = (colon == std::string::npos) ? line : line.substr(0, colon);
  const std::string value = (colon == std::string::npos) ? std::string() : lstrip_one_space(std::string_view(line).substr(colon + 1));

  if (field == "event") {
    cur_event_ = value;
    return;
  }
  if (field == "id") {
    cur_id_ = value;
    return;
  }
  if (field == "data") {
    cur_data_ += value;
    cur_data_.push_back('\n');
    return;
  }
  // Ignore retry/unknown fields.
}

void SseParser::feed(const char* bytes, size_t len, std::vector<SseEvent>* out_events) {
  if (!bytes || len == 0) return;
  buf_.append(bytes, bytes + len);

  // Process complete lines.
  for (;;) {
    const size_t nl = buf_.find('\n');
    if (nl == std::string::npos) break;
    const std::string line = buf_.substr(0, nl);
    buf_.erase(0, nl + 1);
    handle_line(line, out_events);
  }
}

