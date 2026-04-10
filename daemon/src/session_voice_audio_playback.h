#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace agentd {

class Pcm16AudioPlaybackSink {
public:
  Pcm16AudioPlaybackSink();
  ~Pcm16AudioPlaybackSink();

  Pcm16AudioPlaybackSink(const Pcm16AudioPlaybackSink&) = delete;
  Pcm16AudioPlaybackSink& operator=(const Pcm16AudioPlaybackSink&) = delete;

  bool supported() const;
  bool is_open() const;
  int sample_rate_hz() const;
  int channels() const;
  const std::string& device_name() const;

  bool open(int sample_rate_hz, int channels, std::string* out_err);
  bool write_samples(
    const int16_t* samples,
    size_t sample_count,
    size_t* out_samples_written,
    std::string* out_err
  );
  void close();

private:
  void* stream_ = nullptr;
  bool initialized_ = false;
  int sample_rate_hz_ = 0;
  int channels_ = 0;
  std::string device_name_;
};

}  // namespace agentd
