#include "machine.hpp"

#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace wave723 {
namespace {

void check(uc_err error, const char* operation) {
    if (error != UC_ERR_OK) {
        throw std::runtime_error(std::string(operation) + ": " + uc_strerror(error));
    }
}

struct Trace {
    std::array<std::uint32_t, 12> recent{};
    std::uint64_t count = 0;
    uc_mem_type fault{};
    std::uint64_t address = 0;
    int width = 0;
};

const char* accessName(uc_mem_type type) {
    switch (type) {
    case UC_MEM_READ_UNMAPPED: return "unmapped read";
    case UC_MEM_WRITE_UNMAPPED: return "unmapped write";
    case UC_MEM_FETCH_UNMAPPED: return "unmapped instruction fetch";
    case UC_MEM_READ_PROT: return "protected read";
    case UC_MEM_WRITE_PROT: return "protected write";
    case UC_MEM_FETCH_PROT: return "protected instruction fetch";
    default: return "memory fault";
    }
}

void instruction(uc_engine*, std::uint64_t address, std::uint32_t, void* opaque) {
    auto& trace = *static_cast<Trace*>(opaque);
    trace.recent[trace.count % trace.recent.size()] = static_cast<std::uint32_t>(address);
    ++trace.count;
}

bool badMemory(uc_engine*, uc_mem_type type, std::uint64_t address, int size,
               std::int64_t, void* opaque) {
    auto& trace = *static_cast<Trace*>(opaque);
    trace.fault = type;
    trace.address = address;
    trace.width = size;
    return false;
}

std::uint32_t readRegister(uc_engine* cpu, int reg) {
    std::uint32_t value = 0;
    check(uc_reg_read(cpu, reg, &value), "Read ARM register");
    return value;
}

void hex(std::ostream& out, std::uint64_t value) {
    out << "0x" << std::hex << std::setfill('0') << std::setw(8) << value << std::dec;
}

}  // namespace

std::string runAmss(const std::vector<std::uint8_t>& image, std::uint32_t handler,
                    std::uint32_t instructionLimit) {
    uc_engine* raw = nullptr;
    check(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &raw), "Open ARM engine");
    const std::unique_ptr<uc_engine, decltype(&uc_close)> cpu(raw, uc_close);
    check(uc_ctl_set_cpu_model(raw, UC_CPU_ARM_1176), "Select ARM1176 probe CPU");
    check(uc_mem_map(raw, probeRamBase, probeRamSize, UC_PROT_ALL), "Map probe RAM");
    check(uc_mem_write(raw, amssBase, image.data(), image.size()), "Load AMSS");
    const std::uint32_t cpsr = 0xd3;  // ARM supervisor mode with IRQ and FIQ masked.
    check(uc_reg_write(raw, UC_ARM_REG_CPSR, &cpsr), "Initialize CPSR");

    Trace trace;
    uc_hook codeHook = 0;
    uc_hook faultHook = 0;
    check(uc_hook_add(raw, &codeHook, UC_HOOK_CODE, reinterpret_cast<void*>(instruction),
                      &trace, 1, 0), "Install ARM trace hook");
    check(uc_hook_add(raw, &faultHook, UC_HOOK_MEM_INVALID, reinterpret_cast<void*>(badMemory),
                      &trace, 1, 0), "Install ARM memory fault hook");
    const auto result = uc_emu_start(raw, amssBase, UINT64_MAX, 0, instructionLimit);

    std::ostringstream out;
    out << "AMSS reset trace (ARM1176 probe)\n";
    out << "Vector ";
    hex(out, amssBase);
    out << " -> ";
    hex(out, handler);
    out << "\nInstructions reached: " << trace.count << " / " << instructionLimit << "\nStop: ";
    if (trace.width) {
        out << accessName(trace.fault) << " at ";
        hex(out, trace.address);
        out << " (" << trace.width << " bytes)";
    } else if (result != UC_ERR_OK) {
        out << uc_strerror(result);
    } else {
        out << "instruction limit";
    }
    out << "\nPC ";
    hex(out, readRegister(raw, UC_ARM_REG_PC));
    out << "  CPSR ";
    hex(out, readRegister(raw, UC_ARM_REG_CPSR));
    out << "\n\nRegisters\n";
    for (int reg = 0; reg < 13; ++reg) {
        out << 'r' << reg << '=';
        hex(out, readRegister(raw, UC_ARM_REG_R0 + reg));
        out << (reg % 3 == 2 ? "\n" : "  ");
    }
    out << "sp=";
    hex(out, readRegister(raw, UC_ARM_REG_SP));
    out << "  lr=";
    hex(out, readRegister(raw, UC_ARM_REG_LR));
    out << "\n\nRecent instruction addresses\n";
    const auto recent = std::min<std::uint64_t>(trace.count, trace.recent.size());
    for (std::uint64_t i = trace.count - recent; i < trace.count; ++i) {
        hex(out, trace.recent[i % trace.recent.size()]);
        out << ((i - (trace.count - recent)) % 4 == 3 ? '\n' : ' ');
    }
    out << '\n';
    return out.str();
}

}  // namespace wave723
