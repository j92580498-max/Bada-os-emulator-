#include "machine.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace wave723 {
namespace {

namespace fs = std::filesystem;
constexpr auto processTimeout = std::chrono::seconds(20);

class Workspace {
public:
    explicit Workspace(const fs::path& parent) {
        std::random_device random;
        for (int attempt = 0; attempt < 10; ++attempt) {
            path = parent / ("wave723-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (fs::create_directory(path)) return;
        }
        throw std::runtime_error("Could not create a QEMU workspace.");
    }
    ~Workspace() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    fs::path path;
};

fs::path enginePath(const fs::path& requested) {
    if (!requested.empty()) return requested;
#ifdef _WIN32
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), executable.size());
    if (!length || length >= executable.size()) throw std::runtime_error("Could not locate QEMU.");
    executable.resize(length);
    return fs::path(executable).parent_path() / "qemu-system-arm.exe";
#else
    if (const char* configured = std::getenv("WAVE723_QEMU")) return configured;
    return fs::read_symlink("/proc/self/exe").parent_path() / "qemu-system-arm";
#endif
}

#ifdef _WIN32
std::wstring quote(const std::wstring& argument) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto ch : argument) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        result.append(ch == L'"' ? 2 * slashes + 1 : slashes, L'\\');
        slashes = 0;
        result += ch;
    }
    result.append(2 * slashes, L'\\');
    return result + L'"';
}

bool run(const fs::path& engine, const std::vector<fs::path>& args, const fs::path& output) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE log = CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not open the QEMU log.");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = log;
    startup.hStdError = log;
    PROCESS_INFORMATION process{};
    auto command = quote(engine.native());
    for (const auto& arg : args) command += L" " + quote(arg.native());
    const bool started = CreateProcessW(engine.c_str(), command.data(), nullptr, nullptr,
                                        TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                        &startup, &process);
    CloseHandle(log);
    if (!started) {
        throw std::runtime_error("Could not start the bundled QEMU executable.");
    }
    CloseHandle(process.hThread);
    const auto wait = WaitForSingleObject(process.hProcess, 1000 * processTimeout.count());
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hProcess);
        throw std::runtime_error("QEMU exceeded the 20-second host timeout.");
    }
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return code == 0;
}
#else
bool run(const fs::path& engine, const std::vector<fs::path>& args, const fs::path& output) {
    std::vector<std::string> strings{engine.native()};
    for (const auto& arg : args) strings.push_back(arg.native());
    std::vector<char*> argv;
    for (auto& arg : strings) argv.push_back(arg.data());
    argv.push_back(nullptr);

    const auto child = fork();
    if (child < 0) throw std::runtime_error("Could not start QEMU.");
    if (child == 0) {
        const auto log = open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (log < 0 || dup2(log, STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0) _exit(126);
        if (log != STDOUT_FILENO && log != STDERR_FILENO) close(log);
        execv(engine.c_str(), argv.data());
        _exit(127);
    }

    const auto deadline = std::chrono::steady_clock::now() + processTimeout;
    int status = 0;
    while (true) {
        const auto result = waitpid(child, &status, WNOHANG);
        if (result == child) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (result < 0 && errno != EINTR) throw std::runtime_error("Could not wait for QEMU.");
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            throw std::runtime_error("QEMU exceeded the 20-second host timeout.");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
#endif

}  // namespace

std::string runAmss(const std::vector<std::uint8_t>& image, std::uint32_t handler,
                    std::uint32_t instructionLimit, const fs::path& qemu,
                    const fs::path& temporaryDirectory) {
    const auto engine = fs::absolute(enginePath(qemu));
    if (!fs::is_regular_file(engine)) {
        throw std::runtime_error("The Wave 723 QEMU executable is missing.");
    }
    const Workspace workspace(temporaryDirectory);
    const auto amss = workspace.path / "amss.bin";
    {
        std::ofstream file(amss, std::ios::binary);
        file.write(reinterpret_cast<const char*>(image.data()), image.size());
        file.close();
        if (!file) throw std::runtime_error("Could not stage AMSS for QEMU.");
    }
    const auto log = workspace.path / "qemu.log";
    const bool ok = run(engine, {
        "-M", "wave723", "-m", "32M", "-accel", "tcg,thread=single,one-insn-per-tb=on",
        "-icount", "shift=0,sleep=off", "-nodefaults", "-display", "none",
        "-monitor", "none", "-serial", "none", "-no-reboot",
        "-kernel", amss, "-append", std::to_string(instructionLimit)
    }, log);
    std::ifstream file(log);
    std::string report(8192, '\0');
    file.read(report.data(), report.size());
    report.resize(file.gcount());
    if (!ok || report.find("Stop: ") == std::string::npos) {
        throw std::runtime_error(report.empty() ? "QEMU failed to run the Wave 723 machine."
                                                : report);
    }
    std::ostringstream description;
    description << "AMSS reset trace (QEMU ARM1176)\nVector 0x" << std::hex << amssBase
                << " -> 0x" << handler << std::dec << "\nInstruction limit: "
                << instructionLimit << '\n' << report;
    return description.str();
}

}  // namespace wave723
