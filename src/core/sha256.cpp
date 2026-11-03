#include "core/sha256.h"

#include <bit>
#include <cstring>

namespace lantern {
namespace {
constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
}  // namespace

Sha256::Sha256() : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::block(const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = static_cast<uint32_t>(p[i * 4]) << 24 | static_cast<uint32_t>(p[i * 4 + 1]) << 16 |
           static_cast<uint32_t>(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
  for (int i = 16; i < 64; ++i) {
    uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (int i = 0; i < 64; ++i) {
    uint32_t t1 = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
    uint32_t t2 = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h_[0] += a;
  h_[1] += b;
  h_[2] += c;
  h_[3] += d;
  h_[4] += e;
  h_[5] += f;
  h_[6] += g;
  h_[7] += h;
}

void Sha256::update(std::span<const uint8_t> data) {
  total_ += data.size();
  size_t i = 0;
  if (buf_len_) {
    size_t take = std::min(data.size(), 64 - buf_len_);
    std::memcpy(buf_.data() + buf_len_, data.data(), take);
    buf_len_ += take;
    i = take;
    if (buf_len_ < 64) return;
    block(buf_.data());
    buf_len_ = 0;
  }
  for (; i + 64 <= data.size(); i += 64) block(data.data() + i);
  if (i < data.size()) {
    buf_len_ = data.size() - i;
    std::memcpy(buf_.data(), data.data() + i, buf_len_);
  }
}

std::array<uint8_t, 32> Sha256::finish() {
  uint64_t bits = total_ * 8;
  uint8_t pad[72] = {0x80};
  size_t pad_len = (buf_len_ < 56 ? 56 - buf_len_ : 120 - buf_len_);
  for (int i = 0; i < 8; ++i) pad[pad_len + static_cast<size_t>(i)] = static_cast<uint8_t>(bits >> (56 - 8 * i));
  update({pad, pad_len + 8});
  std::array<uint8_t, 32> out;
  for (int i = 0; i < 8; ++i)
    for (int j = 0; j < 4; ++j) out[static_cast<size_t>(i * 4 + j)] = static_cast<uint8_t>(h_[i] >> (24 - 8 * j));
  return out;
}

std::string Sha256::hex(const std::array<uint8_t, 32>& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string s;
  for (uint8_t b : digest) {
    s += kHex[b >> 4];
    s += kHex[b & 15];
  }
  return s;
}

}  // namespace lantern
