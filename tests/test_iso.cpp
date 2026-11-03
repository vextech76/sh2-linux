#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>

#include "iso/iso9660.h"
#include "iso_builder.h"

using namespace lantern;
using lantern::test::IsoBuilder;

namespace {
std::vector<uint8_t> sample_image() {
  IsoBuilder b;
  b.add_file("SYSTEM.CNF", "BOOT2 = cdrom0:\\TEST_000.01;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n");
  b.add_file("TEST_000.01", std::string(5000, 'E'));
  b.add_file("MODULES/PADMAN.IRX", "irx-data");
  b.add_dir("EMPTY");
  return b.build("SYNTH");
}
std::string as_string(const std::vector<uint8_t>& v) { return std::string(v.begin(), v.end()); }
}  // namespace

TEST_CASE("parses volume descriptor and root") {
  std::string err;
  auto img = iso::Image::from_memory(sample_image(), &err);
  REQUIRE_MESSAGE(img, err);
  CHECK(img->volume().volume_id == "SYNTH");
  CHECK(img->volume().system_id == "PLAYSTATION");
  CHECK(img->root().is_dir);
  auto root = img->list(img->root());
  CHECK(root.size() == 4);  // EMPTY, MODULES, SYSTEM.CNF, TEST_000.01
}

TEST_CASE("find is case-insensitive and accepts separators and version suffix") {
  auto img = iso::Image::from_memory(sample_image(), nullptr);
  REQUIRE(img);
  CHECK(img->find("SYSTEM.CNF"));
  CHECK(img->find("/system.cnf;1"));
  CHECK(img->find("\\MODULES\\padman.irx"));
  CHECK(img->find("modules")->is_dir);
  CHECK_FALSE(img->find("MODULES/NOPE.IRX"));
  CHECK_FALSE(img->find("SYSTEM.CNF/child"));
}

TEST_CASE("reads file contents and bounds-checks") {
  auto img = iso::Image::from_memory(sample_image(), nullptr);
  REQUIRE(img);
  auto elf = img->find("TEST_000.01");
  REQUIRE(elf);
  CHECK(elf->size == 5000);
  auto data = img->read_all(*elf, 1 << 20);
  REQUIRE(data);
  CHECK(as_string(*data) == std::string(5000, 'E'));
  CHECK_FALSE(img->read_all(*elf, 100));  // over the caller's limit
  std::vector<uint8_t> buf(10);
  CHECK(img->read(*elf, 4990, buf));
  CHECK_FALSE(img->read(*elf, 4995, buf));  // past end of file
  CHECK_FALSE(img->read_raw(img->image_size() - 5, buf));
}

TEST_CASE("directories spanning several sectors") {
  IsoBuilder b;
  for (int i = 0; i < 150; ++i) b.add_file(std::format("BIG/FILE{:04}.BIN", i), std::to_string(i));
  auto img = iso::Image::from_memory(b.build(), nullptr);
  REQUIRE(img);
  auto big = img->find("BIG");
  REQUIRE(big);
  CHECK(big->size > iso::kSectorSize);
  CHECK(img->list(*big).size() == 150);
  auto last = img->find("BIG/FILE0149.BIN");
  REQUIRE(last);
  CHECK(as_string(*img->read_all(*last, 100)) == "149");
}

TEST_CASE("walk visits every entry with full paths") {
  auto img = iso::Image::from_memory(sample_image(), nullptr);
  REQUIRE(img);
  std::vector<std::string> paths;
  img->walk([&](const std::string& p, const iso::Entry&) {
    paths.push_back(p);
    return true;
  });
  std::sort(paths.begin(), paths.end());
  CHECK(paths == std::vector<std::string>{"/EMPTY", "/MODULES", "/MODULES/PADMAN.IRX", "/SYSTEM.CNF", "/TEST_000.01"});
}

TEST_CASE("rejects non-ISO data") {
  std::string err;
  CHECK_FALSE(iso::Image::from_memory(std::vector<uint8_t>(64 * 2048, 0), &err));
  CHECK(err.find("primary volume descriptor") != std::string::npos);
  CHECK_FALSE(iso::Image::from_memory({}, &err));
}

TEST_CASE("directory record parser bounds") {
  std::vector<uint8_t> rec(40, 0);
  rec[0] = 40;
  rec[32] = 10;  // name runs past record length (33 + 10 > 40)
  CHECK_FALSE(iso::parse_directory_record(rec));
  rec[32] = 5;
  std::memcpy(&rec[33], "A.B;1", 5);
  auto e = iso::parse_directory_record(rec);
  REQUIRE(e);
  CHECK(e->name == "A.B");
  rec[0] = 20;  // shorter than minimum
  CHECK_FALSE(iso::parse_directory_record(rec));
}

TEST_CASE("opens from a file read-only") {
  auto path = std::filesystem::temp_directory_path() / "lantern_test_image.iso";
  {
    auto bytes = sample_image();
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  std::filesystem::permissions(path, std::filesystem::perms::owner_read);  // reader must cope with r/o
  std::string err;
  auto img = iso::Image::open(path, &err);
  REQUIRE_MESSAGE(img, err);
  CHECK(img->find("MODULES/PADMAN.IRX"));
  auto moved = std::move(*img);
  CHECK(moved.find("SYSTEM.CNF"));
  std::filesystem::permissions(path, std::filesystem::perms::owner_all);
  std::filesystem::remove(path);
  CHECK_FALSE(iso::Image::open("/nonexistent/disc.iso", &err));
}
