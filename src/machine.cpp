#include "machine.hpp"
#include "wave723/firmware.hpp"

#include <algorithm>
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
namespace fs = std::filesystem;

QemuWorkspace::QemuWorkspace(const fs::path& parent) {
    std::random_device random;
    for (int attempt = 0; attempt < 10; ++attempt) {
        path = parent / ("wave723-" + std::to_string(random()) + "-" + std::to_string(random()));
        if (!fs::create_directory(path)) continue;
#ifndef _WIN32
        std::error_code error;
        fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace, error);
        if (error) {
            fs::remove(path);
            throw std::runtime_error("Could not protect the QEMU workspace.");
        }
#endif
        return;
    }
    throw std::runtime_error("Could not create a QEMU workspace.");
}

QemuWorkspace::~QemuWorkspace() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
}

namespace {

std::string qemuOptionPath(const fs::path& path) {
    auto name = fs::absolute(path).u8string();
    for (std::size_t i = 0; (i = name.find(',', i)) != name.npos; i += 2) {
        name.insert(i, 1, ',');
    }
    return name;
}

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

bool run(const fs::path& engine, const std::vector<fs::path>& args, const fs::path& output,
         std::chrono::seconds timeout) {
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
    const auto wait = WaitForSingleObject(process.hProcess, static_cast<DWORD>(1000 * timeout.count()));
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hProcess);
        throw std::runtime_error("QEMU exceeded the host timeout.");
    }
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return code == 0;
}
#else
bool run(const fs::path& engine, const std::vector<fs::path>& args, const fs::path& output,
         std::chrono::seconds timeout) {
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

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    while (true) {
        const auto result = waitpid(child, &status, WNOHANG);
        if (result == child) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (result < 0 && errno != EINTR) throw std::runtime_error("Could not wait for QEMU.");
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            throw std::runtime_error("QEMU exceeded the host timeout.");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
#endif

}  // namespace

std::string runReset(ResetTarget target, const std::vector<std::uint8_t>& image,
                     std::uint32_t handler, std::uint32_t instructionLimit,
                     const fs::path& qemu, const QemuWorkspace& workspace,
                     bool diagnostic, PanelFrame* panel) {
    const auto engine = fs::absolute(enginePath(qemu));
    if (!fs::is_regular_file(engine)) {
        throw std::runtime_error("The Wave 723 QEMU executable is missing.");
    }
    const auto raw = workspace.path / "reset.bin";
    {
        std::ofstream file(raw, std::ios::binary);
        file.write(reinterpret_cast<const char*>(image.data()), image.size());
        file.close();
        if (!file) throw std::runtime_error("Could not stage the reset image for QEMU.");
    }
    const bool apps = target == ResetTarget::Apps;
    std::string guestOptions = panel ? "capture:" : "";
    if (diagnostic) guestOptions += "diagnostic:";
    guestOptions += apps ? "apps:" : "amss:";
    guestOptions += std::to_string(instructionLimit);
    constexpr std::size_t mib = 1024 * 1024;
    const auto base = apps ? appsBase : amssBase;
    const auto needed = base - probeRamBase + image.size();
    // The XXKG1 APPS startup memory map ends at 0x8fffffff.
    const auto minimumRam = apps && diagnostic ? 256 * mib : probeRamSize;
    const auto ramMiB = std::max(minimumRam / mib, (needed + mib - 1) / mib);
    const auto ramArg = std::to_string(ramMiB) + "M";
    const auto log = workspace.path / "qemu.log";
    std::vector<fs::path> args{
        "-M", "wave723", "-m", ramArg, "-accel",
        apps && panel ? "tcg,thread=single" : "tcg,thread=single,one-insn-per-tb=on",
        "-icount", "shift=0,sleep=off", "-nodefaults", "-display", "none",
        "-monitor", "none", "-serial", "none", "-no-reboot",
        "-kernel", raw, "-append", guestOptions
    };
    const auto flash = workspace.path / "onenand.img";
    const bool hasFlash = fs::is_regular_file(flash);
    const auto ind = workspace.path / "sysparm_ind.img";
    const auto dep = workspace.path / "sysparm_dep.img";
    const bool hasSysparms = fs::is_regular_file(ind);
    if (hasFlash) {
        args.emplace_back("-drive");
        args.push_back(fs::u8path("if=none,id=wave723-factory,format=raw,file=" + qemuOptionPath(flash)));
        args.insert(args.end(), {"-smp", "2", "-initrd", workspace.path / "amss.bin"});
    }
    if (hasSysparms) {
        args.insert(args.end(), {"-device", fs::u8path("loader,addr=0x80080000,force-raw=on,file=" + qemuOptionPath(ind)),
                                 "-device", fs::u8path("loader,addr=0x800b0000,force-raw=on,file=" + qemuOptionPath(dep))});
    }
    const auto timeout = std::chrono::seconds(20 + instructionLimit / 5000000);
    const bool ok = run(engine, args, log, timeout);
    std::ifstream file(log);
    std::string report(65536, '\0');
    file.read(report.data(), report.size());
    report.resize(file.gcount());
    if (!ok || report.find("Stop: ") == std::string::npos) {
        throw std::runtime_error(report.empty() ? "QEMU failed to run the Wave 723 machine."
                                                : report);
    }
    if (panel) {
        auto capture = raw;
        capture += ".panel.rgb";
        constexpr auto size = PanelFrame::width * PanelFrame::height * 3;
        std::ifstream pixels(capture, std::ios::binary | std::ios::ate);
        if (pixels) {
            if (pixels.tellg() != size) {
                throw std::runtime_error("QEMU produced an incomplete LCD capture.");
            }
            panel->rgb.resize(size);
            pixels.seekg(0);
            if (!pixels.read(reinterpret_cast<char*>(panel->rgb.data()), size)) {
                throw std::runtime_error("Could not read QEMU's LCD capture.");
            }
        }
    }
    std::ostringstream description;
    description << (apps ? "APPS" : "AMSS") << " reset trace (QEMU ARM1176)\nVector 0x"
                << std::hex << base
                << " -> 0x" << handler << std::dec << "\nInstruction limit: "
                << instructionLimit << '\n';
    if (apps) description << "APPS bytes staged: " << image.size() << '\n';
    if (hasFlash) description << "OneNAND: XXKG1 FSR volume and flash components attached\n";
    if (hasSysparms) description << "AMSS system parameters: S7230E formats 26 and 1 loaded\n";
    description << report;
    return description.str();
}

}  // namespace wave723
