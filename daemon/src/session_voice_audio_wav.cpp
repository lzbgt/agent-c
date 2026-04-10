#include "session_voice_audio_wav.h"

#include <fstream>

namespace agentd {
namespace {

void write_le16(std::ostream& out, uint16_t value) {
  const char bytes[2] = {
    static_cast<char>(value & 0xff),
    static_cast<char>((value >> 8) & 0xff),
  };
  out.write(bytes, sizeof(bytes));
}

void write_le32(std::ostream& out, uint32_t value) {
  const char bytes[4] = {
    static_cast<char>(value & 0xff),
    static_cast<char>((value >> 8) & 0xff),
    static_cast<char>((value >> 16) & 0xff),
    static_cast<char>((value >> 24) & 0xff),
  };
  out.write(bytes, sizeof(bytes));
}

}  // namespace

bool write_pcm16_wav_file(
  const std::string& path,
  const int16_t* samples,
  size_t sample_count,
  int sample_rate_hz,
  int channels,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (path.empty()) {
    if (out_err) *out_err = "wav output path missing";
    return false;
  }
  if (sample_rate_hz <= 0) {
    if (out_err) *out_err = "wav sample_rate_hz must be positive";
    return false;
  }
  if (channels <= 0 || channels > 8) {
    if (out_err) *out_err = "wav channels must be between 1 and 8";
    return false;
  }
  if (sample_count > 0 && !samples) {
    if (out_err) *out_err = "wav samples missing";
    return false;
  }
  if (sample_count % static_cast<size_t>(channels) != 0) {
    if (out_err) *out_err = "wav sample_count must be divisible by channels";
    return false;
  }

  const uint32_t data_bytes = static_cast<uint32_t>(sample_count * sizeof(int16_t));
  const uint32_t riff_size = 36u + data_bytes;
  const uint16_t block_align = static_cast<uint16_t>(channels * sizeof(int16_t));
  const uint32_t byte_rate = static_cast<uint32_t>(sample_rate_hz) * block_align;

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    if (out_err) *out_err = "failed to open wav output path";
    return false;
  }

  out.write("RIFF", 4);
  write_le32(out, riff_size);
  out.write("WAVE", 4);
  out.write("fmt ", 4);
  write_le32(out, 16u);
  write_le16(out, 1u);
  write_le16(out, static_cast<uint16_t>(channels));
  write_le32(out, static_cast<uint32_t>(sample_rate_hz));
  write_le32(out, byte_rate);
  write_le16(out, block_align);
  write_le16(out, 16u);
  out.write("data", 4);
  write_le32(out, data_bytes);
  if (data_bytes > 0) {
    out.write(reinterpret_cast<const char*>(samples), static_cast<std::streamsize>(data_bytes));
  }
  if (!out.good()) {
    if (out_err) *out_err = "failed to write wav output";
    return false;
  }
  return true;
}

}  // namespace agentd
