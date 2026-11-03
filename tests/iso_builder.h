#pragma once
// Minimal ISO9660 image builder for tests. Produces synthetic images only — no game data.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace lantern::test {

class IsoBuilder {
 public:
  static constexpr uint32_t kSector = 2048;

  void add_file(const std::string& path, const std::string& content) {
    add_file(path, std::vector<uint8_t>(content.begin(), content.end()));
  }
  void add_file(const std::string& path, std::vector<uint8_t> data) {
    auto [dir, name] = split(path);
    ensure_dir(dir);
    dirs_[dir].files.push_back(name);
    files_[path] = std::move(data);
  }
  void add_dir(const std::string& path) { ensure_dir(path); }

  std::vector<uint8_t> build(const std::string& volume_id = "TEST_VOLUME") {
    // 1. sizes
    std::map<std::string, uint32_t> dir_size;
    for (auto& [path, d] : dirs_) dir_size[path] = dir_bytes(path, d);
    // 2. LBAs: 16 PVD, 17 terminator, dirs from 18, then files
    uint32_t lba = 18;
    for (auto& [path, d] : dirs_) {
      d.lba = lba;
      lba += dir_size[path] / kSector;
    }
    std::map<std::string, uint32_t> file_lba;
    for (auto& [path, data] : files_) {
      file_lba[path] = lba;
      lba += std::max<uint32_t>(1, static_cast<uint32_t>((data.size() + kSector - 1) / kSector));
    }
    std::vector<uint8_t> img(size_t{lba} * kSector, 0);

    // 3. directories
    for (auto& [path, d] : dirs_) {
      std::vector<Rec> recs;
      recs.push_back({std::string(1, '\0'), d.lba, dir_size[path], true});
      const auto& parent = path.empty() ? dirs_.at("") : dirs_.at(split(path).first);
      recs.push_back({std::string(1, '\1'), parent.lba, dir_size[path.empty() ? "" : split(path).first], true});
      for (auto& sub : sorted(d.subdirs)) {
        std::string full = join(path, sub);
        recs.push_back({sub, dirs_[full].lba, dir_size[full], true});
      }
      for (auto& f : sorted(d.files)) {
        std::string full = join(path, f);
        recs.push_back({f + ";1", file_lba[full], static_cast<uint32_t>(files_[full].size()), false});
      }
      size_t pos = size_t{d.lba} * kSector, start = pos;
      for (auto& r : recs) {
        size_t len = rec_len(r.name);
        if ((pos - start) % kSector + len > kSector) pos = start + ((pos - start) / kSector + 1) * kSector;
        write_rec(&img[pos], r);
        pos += len;
      }
    }
    // 4. files
    for (auto& [path, data] : files_) std::memcpy(&img[size_t{file_lba[path]} * kSector], data.data(), data.size());

    // 5. volume descriptors
    uint8_t* pvd = &img[16 * kSector];
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    pad_str(pvd + 8, "PLAYSTATION", 32);
    pad_str(pvd + 40, volume_id, 32);
    le32(pvd + 80, lba);
    be32(pvd + 84, lba);
    pvd[128] = kSector & 0xFF;
    pvd[129] = kSector >> 8;
    write_rec(pvd + 156, {std::string(1, '\0'), dirs_[""].lba, dir_size[""], true});
    uint8_t* term = &img[17 * kSector];
    term[0] = 255;
    std::memcpy(term + 1, "CD001", 5);
    term[6] = 1;
    return img;
  }

 private:
  struct Dir {
    std::vector<std::string> subdirs, files;
    uint32_t lba = 0;
  };
  struct Rec {
    std::string name;
    uint32_t lba, size;
    bool dir;
  };

  static std::pair<std::string, std::string> split(const std::string& path) {
    auto slash = path.rfind('/');
    if (slash == std::string::npos) return {"", path};
    return {path.substr(0, slash), path.substr(slash + 1)};
  }
  static std::string join(const std::string& dir, const std::string& name) {
    return dir.empty() ? name : dir + "/" + name;
  }
  static std::vector<std::string> sorted(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    return v;
  }
  void ensure_dir(const std::string& path) {
    if (dirs_.contains(path)) return;
    dirs_[path];
    if (path.empty()) return;
    auto [parent, name] = split(path);
    ensure_dir(parent);
    dirs_[parent].subdirs.push_back(name);
  }
  static size_t rec_len(const std::string& name) { return 33 + name.size() + (name.size() % 2 == 0 ? 1 : 0); }
  uint32_t dir_bytes(const std::string& path, const Dir& d) const {
    std::vector<std::string> names{std::string(1, '\0'), std::string(1, '\1')};
    for (auto& s : d.subdirs) names.push_back(s);
    for (auto& f : d.files) names.push_back(f + ";1");
    (void)path;
    size_t pos = 0;
    for (auto& n : names) {
      size_t len = rec_len(n);
      if (pos % kSector + len > kSector) pos = (pos / kSector + 1) * kSector;
      pos += len;
    }
    return static_cast<uint32_t>((pos + kSector - 1) / kSector * kSector);
  }
  static void le32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
  }
  static void be32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i));
  }
  static void pad_str(uint8_t* p, const std::string& s, size_t n) {
    std::memset(p, ' ', n);
    std::memcpy(p, s.data(), std::min(n, s.size()));
  }
  static void write_rec(uint8_t* p, const Rec& r) {
    p[0] = static_cast<uint8_t>(rec_len(r.name));
    le32(p + 2, r.lba);
    be32(p + 6, r.lba);
    le32(p + 10, r.size);
    be32(p + 14, r.size);
    p[25] = r.dir ? 0x02 : 0x00;
    p[28] = 1;
    p[32] = static_cast<uint8_t>(r.name.size());
    std::memcpy(p + 33, r.name.data(), r.name.size());
  }

  std::map<std::string, Dir> dirs_{{"", Dir{}}};
  std::map<std::string, std::vector<uint8_t>> files_;
};

}  // namespace lantern::test
