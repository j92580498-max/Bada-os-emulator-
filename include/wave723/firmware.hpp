#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wave723 {

constexpr std::uint32_t defaultInstructionLimit = 100000;
constexpr std::uint32_t maxInstructionLimit = 2000000;

struct PanelFrame {
    static constexpr int width = 240;
    static constexpr int height = 400;
    std::vector<std::uint8_t> rgb;
};

std::string inspectFirmware(const std::filesystem::path& path);
PanelFrame renderBootLogo(const std::filesystem::path& path,
                          const std::filesystem::path& qemu = {});
void savePanelPng(const PanelFrame& frame, const std::filesystem::path& path);
std::string traceReset(const std::filesystem::path& path,
                       std::uint32_t instructionLimit = defaultInstructionLimit,
                       const std::filesystem::path& qemu = {},
                       bool diagnostic = false);
std::string traceAppsReset(const std::filesystem::path& path,
                           std::uint32_t instructionLimit = defaultInstructionLimit,
                           const std::filesystem::path& qemu = {},
                           bool diagnostic = false);

}  // namespace wave723
