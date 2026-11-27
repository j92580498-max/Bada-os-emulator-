#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wave723 {

// The AMSS vector targets in XXKG1 are linked at this address.
constexpr std::uint32_t amssBase = 0x80300000;
constexpr std::uint32_t probeRamBase = 0x80000000;
constexpr std::uint32_t probeRamSize = 32U * 1024 * 1024;

std::string runAmss(const std::vector<std::uint8_t>& image,
                    std::uint32_t handler, std::uint32_t instructionLimit,
                    const std::filesystem::path& qemu,
                    const std::filesystem::path& temporaryDirectory);

}  // namespace wave723
