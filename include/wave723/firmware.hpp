#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wave723 {

constexpr std::uint32_t defaultInstructionLimit = 100000;
constexpr std::uint32_t maxInstructionLimit = 20000000;
constexpr std::uint32_t bootInstructionLimit = 8000000;

struct PanelFrame {
    static constexpr int width = 240;
    static constexpr int height = 400;
    std::vector<std::uint8_t> rgb;
};

struct BootResult {
    std::string trace;
    PanelFrame panel;
};

std::string inspectFirmware(const std::filesystem::path& path);
BootResult bootApps(const std::filesystem::path& path,
                    std::uint32_t instructionLimit = bootInstructionLimit,
                    const std::filesystem::path& qemu = {});
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
