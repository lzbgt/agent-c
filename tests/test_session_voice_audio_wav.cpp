#include "session_voice_audio_wav.h"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

std::filesystem::path make_temp_wav_path(const char* label) {
  return std::filesystem::temp_directory_path() /
         (std::string(label) + "_" + std::to_string((long long)getpid()) + ".wav");
}

uint32_t read_le32(const std::string& bytes, size_t offset) {
  return static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24);
}

uint16_t read_le16(const std::string& bytes, size_t offset) {
  return static_cast<uint16_t>(static_cast<unsigned char>(bytes[offset])) |
         static_cast<uint16_t>(static_cast<unsigned char>(bytes[offset + 1]) << 8);
}

void test_write_pcm16_wav_file_writes_header_and_data() {
  const std::filesystem::path path = make_temp_wav_path("voice_audio_wav");
  std::error_code ec;
  std::filesystem::remove(path, ec);

  const int16_t samples[] = {1000, -1000, 2000, -2000};
  std::string err;
  assert(agentd::write_pcm16_wav_file(path.string(), samples, 4, 8000, 1, &err));
  assert(err.empty());

  std::ifstream in(path, std::ios::binary);
  assert(in.is_open());
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  assert(bytes.size() == 44 + sizeof(samples));
  assert(bytes.substr(0, 4) == "RIFF");
  assert(bytes.substr(8, 4) == "WAVE");
  assert(bytes.substr(12, 4) == "fmt ");
  assert(bytes.substr(36, 4) == "data");
  assert(read_le32(bytes, 4) == 36u + sizeof(samples));
  assert(read_le16(bytes, 20) == 1u);
  assert(read_le16(bytes, 22) == 1u);
  assert(read_le32(bytes, 24) == 8000u);
  assert(read_le32(bytes, 40) == sizeof(samples));
  assert(bytes[44] == static_cast<char>(0xE8));
  assert(bytes[45] == static_cast<char>(0x03));

  std::filesystem::remove(path, ec);
}

void test_write_pcm16_wav_file_rejects_invalid_channel_shape() {
  const std::filesystem::path path = make_temp_wav_path("voice_audio_wav_invalid");
  std::error_code ec;
  std::filesystem::remove(path, ec);

  const int16_t samples[] = {1, 2, 3};
  std::string err;
  assert(!agentd::write_pcm16_wav_file(path.string(), samples, 3, 48000, 2, &err));
  assert(err == "wav sample_count must be divisible by channels");
  assert(!std::filesystem::exists(path));
}

}  // namespace

int main() {
  test_write_pcm16_wav_file_writes_header_and_data();
  test_write_pcm16_wav_file_rejects_invalid_channel_shape();
  return 0;
}
