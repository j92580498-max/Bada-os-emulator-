#include "wave723/firmware.hpp"

#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
using Arg = std::wstring_view;
#define NATIVE(s) L##s
#else
using Arg = std::string_view;
#define NATIVE(s) s
#endif

static std::uint32_t parseLimit(const Arg::value_type* value) {
    const auto text = std::filesystem::path(value).u8string();
    const auto end = text.data() + text.size();
    std::uint32_t limit = 0;
    const auto parsed = std::from_chars(text.data(), end, limit);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        throw std::runtime_error("Invalid instruction limit.");
    }
    return limit;
}

static int run(int argc, const Arg::value_type* const* argv) {
    using namespace wave723;
    if (argc == 2 && Arg(argv[1]) == NATIVE("--help")) {
        std::cout << "Usage: wave723 inspect FIRMWARE.zip\n"
                     "       wave723 boot FIRMWARE.zip OUTPUT.png [INSTRUCTION_LIMIT]\n"
                     "       wave723 boot-logo FIRMWARE.zip OUTPUT.png\n"
                     "       wave723 trace FIRMWARE.zip [INSTRUCTION_LIMIT] [--diagnostic]\n"
                     "       wave723 trace-apps FIRMWARE.zip [INSTRUCTION_LIMIT] [--diagnostic]\n";
        return 0;
    }
    try {
        if (argc == 3 && Arg(argv[1]) == NATIVE("inspect")) {
            std::cout << inspectFirmware(std::filesystem::path(argv[2]));
            return 0;
        }
        const Arg action = argc > 1 ? Arg(argv[1]) : Arg{};
        if ((argc == 4 || argc == 5) && action == NATIVE("boot")) {
            const auto limit = argc == 5 ? parseLimit(argv[4]) : bootInstructionLimit;
            const auto result = bootApps(std::filesystem::path(argv[2]), limit);
            std::cout << result.trace;
            if (result.panel.rgb.empty()) {
                std::cerr << "Stock APPS did not produce a visible LCD frame.\n";
                return 1;
            }
            savePanelPng(result.panel, std::filesystem::path(argv[3]));
            std::cout << "Saved the latest stock APPS LCD frame (240x400).\n";
            return 0;
        }
        if (argc == 4 && action == NATIVE("boot-logo")) {
            const auto frame = renderBootLogo(std::filesystem::path(argv[2]));
            savePanelPng(frame, std::filesystem::path(argv[3]));
            std::cout << "Saved QEMU LCD capture (240x400).\n";
            return 0;
        }
        const bool diagnostic = argc >= 4 && Arg(argv[argc - 1]) == NATIVE("--diagnostic");
        const int arguments = argc - diagnostic;
        if ((arguments == 3 || arguments == 4) &&
            (action == NATIVE("trace") || action == NATIVE("trace-apps"))) {
            auto limit = defaultInstructionLimit;
            if (arguments == 4) {
                limit = parseLimit(argv[3]);
            }
            const auto path = std::filesystem::path(argv[2]);
            std::cout << (action == NATIVE("trace-apps") ? traceAppsReset(path, limit, {}, diagnostic)
                                                        : traceReset(path, limit, {}, diagnostic));
            return 0;
        }
        std::cerr << "Run wave723 --help for usage.\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "wave723: " << error.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) { return run(argc, argv); }
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
