#include "session_voice_audio_playback.h"

#include <cassert>
#include <cstdint>
#include <string>

using agentd::Pcm16AudioPlaybackSink;

static void test_playback_sink_rejects_invalid_format() {
  Pcm16AudioPlaybackSink sink;
  std::string err;
  assert(!sink.open(0, 2, &err));
  assert(err.find("sample rate") != std::string::npos);
  assert(!sink.open(48000, 0, &err));
  assert(err.find("channel") != std::string::npos);
}

static void test_playback_sink_write_requires_open_stream() {
  Pcm16AudioPlaybackSink sink;
  std::string err;
  const int16_t samples[4] = {0, 1, -1, 2};
  size_t written = 123;
  assert(!sink.write_samples(samples, 4, &written, &err));
  assert(written == 0);
  assert(err.find("not open") != std::string::npos);
}

static void test_playback_sink_close_is_idempotent() {
  Pcm16AudioPlaybackSink sink;
  sink.close();
  sink.close();
  assert(!sink.is_open());
}

int main() {
  test_playback_sink_rejects_invalid_format();
  test_playback_sink_write_requires_open_stream();
  test_playback_sink_close_is_idempotent();
  return 0;
}
