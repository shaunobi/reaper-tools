#include "srt/onnx.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

TEST_CASE("smoke model runs on the CPU provider") {
    std::filesystem::path model = std::filesystem::path(SRT_TEST_DATA_DIR) / "smoke.onnx";
    std::string report = srt::runSmokeTest(model, {srt::Provider::Cpu});
    REQUIRE(report.find("CPU") != std::string::npos);
}

TEST_CASE("smoke model runs on the best available provider") {
    std::filesystem::path model = std::filesystem::path(SRT_TEST_DATA_DIR) / "smoke.onnx";
    auto providers = srt::OnnxRuntime::availableProviders();
    REQUIRE(!providers.empty());
    REQUIRE(providers.back() == srt::Provider::Cpu);
    std::string report = srt::runSmokeTest(model, providers);
    REQUIRE(report.find("smoke model OK") != std::string::npos);
}

TEST_CASE("embedded smoke model matches the file on disk") {
    std::string report = srt::runEmbeddedSmokeTest({srt::Provider::Cpu});
    REQUIRE(report.find("smoke model OK") != std::string::npos);
}
