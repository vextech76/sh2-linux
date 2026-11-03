#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/sha256.h"
#include "ingest/disc.h"
#include "iso_builder.h"

using namespace lantern;
using lantern::test::IsoBuilder;
namespace fs = std::filesystem;

namespace {
std::string sha(std::string_view s) {
  Sha256 h;
  h.update({reinterpret_cast<const uint8_t*>(s.data()), s.size()});
  return Sha256::hex(h.finish());
}

iso::Image synthetic_disc() {
  IsoBuilder b;
  b.add_file("SYSTEM.CNF", "BOOT2 = cdrom0:\\ABCD_123.45;1\nVER = 1.01\nVMODE = PAL\n");
  b.add_file("ABCD_123.45",
             "\x7f"
             "ELF fake");
  b.add_file("MODULES/A.IRX", "a");
  b.add_file("MODULES/B.irx", "b");
  b.add_file("MODULES/IOPRP254.IMG", "rp");
  b.add_file("DATA/BIG.BIN", std::string(10000, 'x'));
  return *iso::Image::from_memory(b.build("SYNTHDISC"), nullptr);
}
}  // namespace

TEST_CASE("sha256 known-answer vectors") {
  CHECK(sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  Sha256 h;
  std::string a(1000, 'a');
  for (int i = 0; i < 1000; ++i) h.update({reinterpret_cast<const uint8_t*>(a.data()), a.size()});
  CHECK(Sha256::hex(h.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha256 is independent of chunking") {
  std::string data(777, 'q');
  for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>(i * 31);
  Sha256 a, b;
  a.update({reinterpret_cast<const uint8_t*>(data.data()), data.size()});
  for (size_t i = 0; i < data.size(); i += 13)
    b.update({reinterpret_cast<const uint8_t*>(data.data()) + i, std::min<size_t>(13, data.size() - i)});
  CHECK(Sha256::hex(a.finish()) == Sha256::hex(b.finish()));
}

TEST_CASE("SYSTEM.CNF parsing") {
  auto c = ingest::parse_system_cnf("BOOT2=cdrom0:\\SLUS_999.99;1\r\nVER=1.00\r\nVMODE=NTSC\r\n", nullptr);
  REQUIRE(c);
  CHECK(c->boot_file == "SLUS_999.99");
  CHECK(c->version == "1.00");
  CHECK(c->video_mode == "NTSC");
  auto nested = ingest::parse_system_cnf("BOOT2 = cdrom0:\\DIR\\GAME.ELF;1", nullptr);
  REQUIRE(nested);
  CHECK(nested->boot_file == "DIR/GAME.ELF");
  std::string err;
  CHECK_FALSE(ingest::parse_system_cnf("BOOT = cdrom:\\PS1.EXE;1", &err));
  CHECK(err.find("BOOT2") != std::string::npos);
}

TEST_CASE("serial and region derivation") {
  CHECK(ingest::serial_from_boot_file("SLUS_123.45") == "SLUS-12345");
  CHECK(ingest::serial_from_boot_file("dir/sles_543.21") == "SLES-54321");
  CHECK(ingest::serial_from_boot_file("GAME.ELF").empty());
  CHECK(ingest::serial_from_boot_file("SLUS_12A.45").empty());
  CHECK(ingest::region_from_serial("SLUS-12345") == "NTSC-U");
  CHECK(ingest::region_from_serial("SLES-12345") == "PAL");
  CHECK(ingest::region_from_serial("SLPM-12345") == "NTSC-J");
  CHECK(ingest::region_from_serial("ABCD-12345") == "unknown");
}

TEST_CASE("identify a synthetic disc") {
  auto img = synthetic_disc();
  std::string err;
  auto info = ingest::identify(img, &err);
  REQUIRE_MESSAGE(info, err);
  CHECK(info->serial == "ABCD-12345");
  CHECK(info->region == "unknown");
  CHECK(info->volume_id == "SYNTHDISC");
  CHECK(info->cnf.video_mode == "PAL");
  CHECK(info->irx_modules == std::vector<std::string>{"/MODULES/A.IRX", "/MODULES/B.irx"});
}

TEST_CASE("identify fails clearly without SYSTEM.CNF or boot file") {
  IsoBuilder b;
  b.add_file("README.TXT", "hi");
  auto img = *iso::Image::from_memory(b.build(), nullptr);
  std::string err;
  CHECK_FALSE(ingest::identify(img, &err));
  CHECK(err.find("SYSTEM.CNF") != std::string::npos);

  IsoBuilder b2;
  b2.add_file("SYSTEM.CNF", "BOOT2 = cdrom0:\\SLUS_000.00;1\n");
  auto img2 = *iso::Image::from_memory(b2.build(), nullptr);
  CHECK_FALSE(ingest::identify(img2, &err));
  CHECK(err.find("missing") != std::string::npos);
}

TEST_CASE("image hash matches hashing the bytes directly") {
  IsoBuilder b;
  b.add_file("SYSTEM.CNF", "BOOT2 = cdrom0:\\SLUS_000.00;1\n");
  auto bytes = b.build();
  Sha256 direct;
  direct.update(bytes);
  auto img = *iso::Image::from_memory(bytes, nullptr);
  uint64_t last = 0;
  auto h = ingest::sha256_image(img, [&](uint64_t done, uint64_t) { last = done; }, nullptr);
  REQUIRE(h);
  CHECK(*h == Sha256::hex(direct.finish()));
  CHECK(last == bytes.size());
}

TEST_CASE("extract boot set and full set, with manifest") {
  auto img = synthetic_disc();
  auto info = *ingest::identify(img, nullptr);
  auto root = fs::temp_directory_path() / "lantern_test_extract";
  fs::remove_all(root);
  std::string err;
  auto res = ingest::extract(img, info, ingest::ExtractSet::Boot, root / "boot", root / "games", "deadbeef", &err);
  REQUIRE_MESSAGE(res, err);
  CHECK(res->files.size() == 5);  // SYSTEM.CNF, ELF, 2 IRX, IOPRP
  CHECK(fs::exists(root / "boot/ABCD_123.45"));
  CHECK(fs::exists(root / "boot/MODULES/A.IRX"));
  CHECK_FALSE(fs::exists(root / "boot/DATA/BIG.BIN"));
  std::stringstream manifest;
  manifest << std::ifstream(root / "boot/manifest.toml").rdbuf();
  CHECK(manifest.str().find("serial = \"ABCD-12345\"") != std::string::npos);
  CHECK(manifest.str().find("image_sha256 = \"deadbeef\"") != std::string::npos);

  auto all = ingest::extract(img, info, ingest::ExtractSet::All, root / "all", root / "games", "x", &err);
  REQUIRE(all);
  CHECK(all->files.size() == 6);
  CHECK(fs::file_size(root / "all/DATA/BIG.BIN") == 10000);
  fs::remove_all(root);
}

TEST_CASE("extract refuses to write inside the game data directory") {
  auto img = synthetic_disc();
  auto info = *ingest::identify(img, nullptr);
  auto games = fs::temp_directory_path() / "lantern_test_games";
  std::string err;
  CHECK_FALSE(ingest::extract(img, info, ingest::ExtractSet::Boot, games / "out", games, "x", &err));
  CHECK(err.find("refusing") != std::string::npos);
  CHECK_FALSE(fs::exists(games / "out"));
}
