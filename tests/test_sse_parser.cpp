#include "sse_parser.h"

#include <cassert>
#include <string>
#include <vector>

static void test_single_event() {
  SseParser p;
  std::vector<SseEvent> out;
  const std::string s =
    "event: agent_event\n"
    "id: 12\n"
    "data: {\"ok\":true}\n"
    "\n";
  p.feed(s.data(), s.size(), &out);
  assert(out.size() == 1);
  assert(out[0].event == "agent_event");
  assert(out[0].id == "12");
  assert(out[0].data == "{\"ok\":true}");
}

static void test_multiline_data() {
  SseParser p;
  std::vector<SseEvent> out;
  const std::string s =
    "data: line1\n"
    "data: line2\n"
    "\n";
  p.feed(s.data(), s.size(), &out);
  assert(out.size() == 1);
  assert(out[0].data == "line1\nline2");
}

static void test_chunked_input_and_comments() {
  SseParser p;
  std::vector<SseEvent> out;
  const std::string a =
    ": ping\n"
    "event: e\n"
    "data: part";
  const std::string b =
    "1\n"
    "data: part2\n"
    "\n";
  p.feed(a.data(), a.size(), &out);
  assert(out.empty());
  p.feed(b.data(), b.size(), &out);
  assert(out.size() == 1);
  assert(out[0].event == "e");
  assert(out[0].data == "part1\npart2");
}

static void test_crlf() {
  SseParser p;
  std::vector<SseEvent> out;
  const std::string s =
    "event: x\r\n"
    "data: y\r\n"
    "\r\n";
  p.feed(s.data(), s.size(), &out);
  assert(out.size() == 1);
  assert(out[0].event == "x");
  assert(out[0].data == "y");
}

int main() {
  test_single_event();
  test_multiline_data();
  test_chunked_input_and_comments();
  test_crlf();
  return 0;
}

