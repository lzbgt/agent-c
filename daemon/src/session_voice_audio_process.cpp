#include "session_voice_audio_process.h"

#include <cmath>
#include <limits>

namespace agentd {

bool summarize_pcm16_audio(
  const int16_t* samples,
  size_t sample_count,
  Pcm16AudioProcessSummary* out_summary
) {
  if (!out_summary) return false;
  *out_summary = Pcm16AudioProcessSummary{};
  if (sample_count == 0) return true;
  if (!samples) return false;

  int peak_abs_pcm16 = 0;
  long double sum_squares = 0.0;
  for (size_t i = 0; i < sample_count; ++i) {
    const int32_t sample = static_cast<int32_t>(samples[i]);
    const int abs_sample =
      sample == static_cast<int32_t>(std::numeric_limits<int16_t>::min())
        ? 32768
        : std::abs(sample);
    if (abs_sample > peak_abs_pcm16) peak_abs_pcm16 = abs_sample;
    sum_squares += static_cast<long double>(sample) * static_cast<long double>(sample);
  }

  out_summary->sample_count = static_cast<int64_t>(sample_count);
  out_summary->peak_abs_pcm16 = peak_abs_pcm16;
  out_summary->rms_pcm16 = static_cast<int>(std::llround(
    std::sqrt(sum_squares / static_cast<long double>(sample_count))));
  return true;
}

}  // namespace agentd
