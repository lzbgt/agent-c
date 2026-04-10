#include "session_voice_audio_playback.h"

#include <mutex>
#include <string>

#include "string_util.h"

#if defined(AGENTD_HAVE_PORTAUDIO)
#include <portaudio.h>
#endif

namespace agentd {
namespace {

#if defined(AGENTD_HAVE_PORTAUDIO)

std::mutex g_portaudio_mu;
int g_portaudio_ref_count = 0;

std::string portaudio_error_string(PaError err, const std::string& fallback) {
  if (err == paNoError) return "";
  const char* text = Pa_GetErrorText(err);
  if (text && text[0]) return trim_copy(text);
  return fallback;
}

bool retain_portaudio(std::string* out_err) {
  if (out_err) out_err->clear();
  std::lock_guard<std::mutex> lk(g_portaudio_mu);
  if (g_portaudio_ref_count > 0) {
    g_portaudio_ref_count += 1;
    return true;
  }
  const PaError err = Pa_Initialize();
  if (err != paNoError) {
    if (out_err) {
      *out_err = portaudio_error_string(err, "failed to initialize portaudio");
    }
    return false;
  }
  g_portaudio_ref_count = 1;
  return true;
}

void release_portaudio() {
  std::lock_guard<std::mutex> lk(g_portaudio_mu);
  if (g_portaudio_ref_count <= 0) return;
  g_portaudio_ref_count -= 1;
  if (g_portaudio_ref_count == 0) {
    (void)Pa_Terminate();
  }
}

#endif

}  // namespace

Pcm16AudioPlaybackSink::Pcm16AudioPlaybackSink() = default;

Pcm16AudioPlaybackSink::~Pcm16AudioPlaybackSink() {
  close();
}

bool Pcm16AudioPlaybackSink::supported() const {
#if defined(AGENTD_HAVE_PORTAUDIO)
  return true;
#else
  return false;
#endif
}

bool Pcm16AudioPlaybackSink::is_open() const {
  return stream_ != nullptr;
}

int Pcm16AudioPlaybackSink::sample_rate_hz() const {
  return sample_rate_hz_;
}

int Pcm16AudioPlaybackSink::channels() const {
  return channels_;
}

const std::string& Pcm16AudioPlaybackSink::device_name() const {
  return device_name_;
}

bool Pcm16AudioPlaybackSink::open(
  int sample_rate_hz,
  int channels,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (sample_rate_hz <= 0) {
    if (out_err) *out_err = "audio playback sample rate must be positive";
    return false;
  }
  if (channels <= 0 || channels > 8) {
    if (out_err) *out_err = "audio playback channel count must be 1..8";
    return false;
  }

#if !defined(AGENTD_HAVE_PORTAUDIO)
  if (out_err) *out_err = "portaudio playback support not built";
  return false;
#else
  if (stream_ && sample_rate_hz_ == sample_rate_hz && channels_ == channels) {
    return true;
  }
  close();

  std::string init_err;
  if (!retain_portaudio(&init_err)) {
    if (out_err) *out_err = init_err;
    return false;
  }
  initialized_ = true;

  const PaDeviceIndex device_index = Pa_GetDefaultOutputDevice();
  if (device_index == paNoDevice) {
    close();
    if (out_err) *out_err = "default portaudio output device unavailable";
    return false;
  }

  const PaDeviceInfo* device_info = Pa_GetDeviceInfo(device_index);
  if (!device_info) {
    close();
    if (out_err) *out_err = "failed to query default portaudio output device";
    return false;
  }

  PaStreamParameters output;
  output.device = device_index;
  output.channelCount = channels;
  output.sampleFormat = paInt16;
  output.suggestedLatency =
    device_info->defaultLowOutputLatency > 0.0
      ? device_info->defaultLowOutputLatency
      : device_info->defaultHighOutputLatency;
  output.hostApiSpecificStreamInfo = nullptr;

  PaStream* stream = nullptr;
  PaError err = Pa_OpenStream(
    &stream,
    nullptr,
    &output,
    static_cast<double>(sample_rate_hz),
    paFramesPerBufferUnspecified,
    paClipOff,
    nullptr,
    nullptr);
  if (err != paNoError || !stream) {
    close();
    if (out_err) {
      *out_err = portaudio_error_string(err, "failed to open portaudio output stream");
    }
    return false;
  }

  err = Pa_StartStream(stream);
  if (err != paNoError) {
    Pa_CloseStream(stream);
    close();
    if (out_err) {
      *out_err = portaudio_error_string(err, "failed to start portaudio output stream");
    }
    return false;
  }

  stream_ = stream;
  sample_rate_hz_ = sample_rate_hz;
  channels_ = channels;
  device_name_ = device_info->name ? trim_copy(device_info->name) : std::string();
  return true;
#endif
}

bool Pcm16AudioPlaybackSink::write_samples(
  const int16_t* samples,
  size_t sample_count,
  size_t* out_samples_written,
  std::string* out_err
) {
  if (out_samples_written) *out_samples_written = 0;
  if (out_err) out_err->clear();
  if (sample_count == 0) return true;
  if (!samples) {
    if (out_err) *out_err = "audio playback samples missing";
    return false;
  }
  if (!stream_) {
    if (out_err) *out_err = "audio playback stream not open";
    return false;
  }
  if (channels_ <= 0 || sample_count % static_cast<size_t>(channels_) != 0) {
    if (out_err) *out_err = "audio playback sample count must align with channel count";
    return false;
  }

#if !defined(AGENTD_HAVE_PORTAUDIO)
  if (out_err) *out_err = "portaudio playback support not built";
  return false;
#else
  const unsigned long frame_count =
    static_cast<unsigned long>(sample_count / static_cast<size_t>(channels_));
  const PaError err = Pa_WriteStream(
    static_cast<PaStream*>(stream_),
    samples,
    frame_count);
  if (err != paNoError) {
    if (out_err) {
      *out_err = portaudio_error_string(err, "failed to write portaudio output stream");
    }
    return false;
  }
  if (out_samples_written) *out_samples_written = sample_count;
  return true;
#endif
}

void Pcm16AudioPlaybackSink::close() {
#if defined(AGENTD_HAVE_PORTAUDIO)
  if (stream_) {
    PaStream* stream = static_cast<PaStream*>(stream_);
    (void)Pa_StopStream(stream);
    (void)Pa_CloseStream(stream);
    stream_ = nullptr;
  }
  if (initialized_) {
    release_portaudio();
    initialized_ = false;
  }
#else
  stream_ = nullptr;
  initialized_ = false;
#endif
  sample_rate_hz_ = 0;
  channels_ = 0;
  device_name_.clear();
}

}  // namespace agentd
