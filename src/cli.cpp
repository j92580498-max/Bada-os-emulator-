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

static int run(int argc, const Arg::value_type* const* argv) {
    using namespace wave723;
    if (argc == 2 && Arg(argv[1]) == NATIVE("--help")) {
        std::cout << "Usage: wave723 inspect FIRMWARE.zip\n"
                     "       wave723 trace FIRMWARE.zip [INSTRUCTION_LIMIT]\n"
                     "       wave723 trace-apps FIRMWARE.zip [INSTRUCTION_LIMIT]\n";
        return 0;
    }
    try {
        if (argc == 3 && Arg(argv[1]) == NATIVE("inspect")) {
            std::cout << inspectFirmware(std::filesystem::path(argv[2]));
            return 0;
        }
        const Arg action = argc > 1 ? Arg(argv[1]) : Arg{};
        if ((argc == 3 || argc == 4) &&
            (action == NATIVE("trace") || action == NATIVE("trace-apps"))) {
            auto limit = defaultInstructionLimit;
            if (argc == 4) {
                const auto text = std::filesystem::path(argv[3]).u8string();
                const auto end = text.data() + text.size();
                const auto parsed = std::from_chars(text.data(), end, limit);
                if (parsed.ec != std::errc{} || parsed.ptr != end) {
                    throw std::runtime_error("Invalid instruction limit.");
                }
            }
            const auto path = std::filesystem::path(argv[2]);
            std::cout << (action == NATIVE("trace-apps") ? traceAppsReset(path, limit)
                                                        : traceReset(path, limit));
            return 0;
        }
        std::cerr << "Usage: wave723 inspect|trace|trace-apps FIRMWARE.zip [INSTRUCTION_LIMIT]\n";
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
