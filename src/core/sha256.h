#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace lantern {

// FIPS 180-4 SHA-256. Used to fingerprint the user's disc image for manifests.
class Sha256 {
 public:
  Sha256();
  void update(std::span<const uint8_t> data);
  std::array<uint8_t, 32> finish();
  static std::string hex(const std::array<uint8_t, 32>& digest);

 private:
  void block(const uint8_t* p);
  std::array<uint32_t, 8> h_;
  std::array<uint8_t, 64> buf_{};
  size_t buf_len_ = 0;
  uint64_t total_ = 0;
};

}  // namespace lantern
