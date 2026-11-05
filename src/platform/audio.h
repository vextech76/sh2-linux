#pragma once

#include <SDL3/SDL_audio.h>

#include <cstdint>
#include <deque>
#include <mutex>

namespace lantern::platform {

// SDL3 playback stream fed with 48 kHz stereo signed-16 PCM. The game thread pushes mixed SPU2
// samples; the SDL callback drains them and pads with silence when the producer falls behind.
class Audio {
 public:
  Audio() = default;
  ~Audio();
  Audio(const Audio&) = delete;
  Audio& operator=(const Audio&) = delete;

  bool open(int sample_rate = 48000, int channels = 2);
  void close();
  void push(const int16_t* interleaved, uint32_t frames);
  bool active() const { return stream_ != nullptr; }

 private:
  static void SDLCALL callback(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount);
  void fill(int16_t* out, int samples);

  SDL_AudioStream* stream_ = nullptr;
  std::mutex mutex_;
  std::deque<int16_t> queue_;
  int channels_ = 2;
  uint32_t sample_rate_ = 48000;
};

}  // namespace lantern::platform
