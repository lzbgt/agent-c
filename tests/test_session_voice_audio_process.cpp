#include "session_voice_audio_process.h"

#include <cassert>
#include <cstdint>
#include <limits>

namespace {

void test_summarize_pcm16_audio_handles_empty_input() {
  agentd::Pcm16AudioProcessSummary summary;
  assert(agentd::summarize_pcm16_audio(nullptr, 0, &summary));
  assert(summary.sample_count == 0);
  assert(summary.peak_abs_pcm16 == 0);
  assert(summary.rms_pcm16 == 0);
}

void test_summarize_pcm16_audio_computes_peak_and_rms() {
  const int16_t samples[] = {1000, -1000, 1000, -1000};
  agentd::Pcm16AudioProcessSummary summary;
  assert(agentd::summarize_pcm16_audio(samples, 4, &summary));
  assert(summary.sample_count == 4);
  assert(summary.peak_abs_pcm16 == 1000);
  assert(summary.rms_pcm16 == 1000);
}

void test_summarize_pcm16_audio_handles_int16_min_peak() {
  const int16_t samples[] = {std::numeric_limits<int16_t>::min(), 0};
  agentd::Pcm16AudioProcessSummary summary;
  assert(agentd::summarize_pcm16_audio(samples, 2, &summary));
  assert(summary.sample_count == 2);
  assert(summary.peak_abs_pcm16 == 32768);
  assert(summary.rms_pcm16 > 0);
}

}  // namespace

int main() {
  test_summarize_pcm16_audio_handles_empty_input();
  test_summarize_pcm16_audio_computes_peak_and_rms();
  test_summarize_pcm16_audio_handles_int16_min_peak();
  return 0;
}
