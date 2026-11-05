#include "platform/audio.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "core/log.h"

namespace lantern::platform {

Audio::~Audio() { close(); }

bool Audio::open(int sample_rate, int channels) {
  if (stream_) return true;
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    log::warn("audio", "SDL audio unavailable: {}", SDL_GetError());
    return false;
  }
  SDL_AudioSpec spec{};
  spec.format = SDL_AUDIO_S16;
  spec.channels = channels;
  spec.freq = sample_rate;
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, callback, this);
  if (!stream_) {
    log::warn("audio", "no playback device: {}", SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return false;
  }
  channels_ = channels;
  sample_rate_ = static_cast<uint32_t>(sample_rate);
  SDL_ResumeAudioStreamDevice(stream_);
  log::info("audio", "playing SPU2 PCM at {} Hz, {} channels", sample_rate, channels);
  return true;
}

void Audio::close() {
  if (!stream_) return;
  SDL_DestroyAudioStream(stream_);
  stream_ = nullptr;
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void Audio::push(const int16_t* interleaved, uint32_t frames) {
  const size_t count = size_t{frames} * static_cast<size_t>(channels_);
  std::lock_guard lock(mutex_);
  const size_t cap = size_t{sample_rate_} * static_cast<size_t>(channels_) * 2;  // 2 seconds
  if (queue_.size() + count > cap) {
    const size_t drop = queue_.size() + count - cap;
    queue_.erase(queue_.begin(), queue_.begin() + static_cast<long>(std::min(drop, queue_.size())));
  }
  queue_.insert(queue_.end(), interleaved, interleaved + count);
}

void Audio::fill(int16_t* out, int samples) {
  std::lock_guard lock(mutex_);
  int n = 0;
  while (n < samples && !queue_.empty()) {
    out[n++] = queue_.front();
    queue_.pop_front();
  }
  while (n < samples) out[n++] = 0;
}

void SDLCALL Audio::callback(void* userdata, SDL_AudioStream* stream, int additional_amount, int) {
  auto* self = static_cast<Audio*>(userdata);
  if (additional_amount <= 0) return;
  const int samples = additional_amount / static_cast<int>(sizeof(int16_t));
  std::vector<int16_t> buffer(static_cast<size_t>(samples));
  self->fill(buffer.data(), samples);
  SDL_PutAudioStreamData(stream, buffer.data(), additional_amount);
}

}  // namespace lantern::platform
