#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wave723 {

struct PanelFrame;

// The AMSS vector targets in XXKG1 are linked at this address.
constexpr std::uint32_t amssBase = 0x80300000;
constexpr std::uint32_t appsBase = 0x81000000;
constexpr std::uint32_t probeRamBase = 0x80000000;
constexpr std::uint32_t probeRamSize = 32U * 1024 * 1024;

enum class ResetTarget { Amss, Apps };

class QemuWorkspace {
public:
    explicit QemuWorkspace(const std::filesystem::path& parent);
    ~QemuWorkspace();
    QemuWorkspace(const QemuWorkspace&) = delete;
    QemuWorkspace& operator=(const QemuWorkspace&) = delete;
    std::filesystem::path path;
};

std::string runReset(ResetTarget target, const std::vector<std::uint8_t>& image,
                     std::uint32_t handler, std::uint32_t instructionLimit,
                     const std::filesystem::path& qemu, const QemuWorkspace& workspace, bool diagnostic,
                     PanelFrame* panel = nullptr);

}  // namespace wave723
