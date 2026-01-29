#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct SseEvent {
  std::string event; // optional
  std::string id;    // optional
  std::string data;  // concatenated "data:" lines (joined with '\n')
};

// Minimal incremental Server-Sent Events parser (text/event-stream).
//
// - Feed arbitrary byte chunks; events are emitted once a blank line is received.
// - Supports "event:", "id:", and multi-line "data:".
// - Ignores comments (":" lines) and unknown fields.
class SseParser {
 public:
  void feed(const char* bytes, size_t len, std::vector<SseEvent>* out_events);
  void reset();

 private:
  std::string buf_;
  std::string cur_event_;
  std::string cur_id_;
  std::string cur_data_;

  void handle_line(const std::string& line, std::vector<SseEvent>* out_events);
  void flush_event(std::vector<SseEvent>* out_events);
};

