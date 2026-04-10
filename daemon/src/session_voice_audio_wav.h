#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace agentd {

bool write_pcm16_wav_file(
  const std::string& path,
  const int16_t* samples,
  size_t sample_count,
  int sample_rate_hz,
  int channels,
  std::string* out_err
);

}  // namespace agentd
