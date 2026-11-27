#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace wave723 {

constexpr std::uint32_t defaultInstructionLimit = 100000;
constexpr std::uint32_t maxInstructionLimit = 2000000;

std::string inspectFirmware(const std::filesystem::path& path);
std::string traceReset(const std::filesystem::path& path,
                       std::uint32_t instructionLimit = defaultInstructionLimit,
                       const std::filesystem::path& qemu = {},
                       bool diagnostic = false);
std::string traceAppsReset(const std::filesystem::path& path,
                           std::uint32_t instructionLimit = defaultInstructionLimit,
                           const std::filesystem::path& qemu = {},
                           bool diagnostic = false);

}  // namespace wave723
