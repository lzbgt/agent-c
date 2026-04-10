#pragma once

#include <cstddef>
#include <cstdint>

namespace agentd {

struct Pcm16AudioProcessSummary {
  int64_t sample_count = 0;
  int peak_abs_pcm16 = 0;
  int rms_pcm16 = 0;
};

bool summarize_pcm16_audio(
  const int16_t* samples,
  size_t sample_count,
  Pcm16AudioProcessSummary* out_summary
);

}  // namespace agentd
