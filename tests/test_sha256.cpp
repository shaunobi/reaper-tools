#include "srt/sha256.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

TEST_CASE("SHA-256 matches known vectors") {
    REQUIRE(srt::sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(srt::sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string longMsg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    REQUIRE(srt::sha256Hex(longMsg.data(), longMsg.size()) ==
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    std::string million(1000000, 'a');
    REQUIRE(srt::sha256Hex(million.data(), million.size()) ==
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha256File hashes a file in chunks") {
    auto p = std::filesystem::temp_directory_path() / "srt_sha_test.bin";
    {
        std::ofstream f(p, std::ios::binary);
        std::string data(3 * 1024 * 1024 + 7, 'x');
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    std::string data(3 * 1024 * 1024 + 7, 'x');
    REQUIRE(srt::sha256File(p) == srt::sha256Hex(data.data(), data.size()));
    std::filesystem::remove(p);
}
