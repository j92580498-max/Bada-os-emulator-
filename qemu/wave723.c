/* SPDX-License-Identifier: GPL-2.0-only */
/* Samsung Wave 723 BCM2156 reset probes. */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "target/arm/cpregs.h"
#include "target/arm/cpu.h"

#define RAM_BASE 0x80000000
#define AMSS_BASE 0x80300000
#define APPS_BASE 0x81000000
#define RAM_SIZE (32 * MiB)
#define RESET_WRITE 0x08880048
#define TYPE_WAVE723_MACHINE MACHINE_TYPE_NAME("wave723")

typedef struct Wave723Tcm {
    MemoryRegion memory;
    uint32_t region;
} Wave723Tcm;

OBJECT_DECLARE_SIMPLE_TYPE(Wave723State, WAVE723_MACHINE)

struct Wave723State {
    MachineState parent_obj;
    ARMCPU *cpu;
    MemoryRegion unmapped;
    Wave723Tcm tcm[2];
    QEMUTimer *deadline;
    int64_t limit;
    uint32_t reset_vector;
    bool diagnostic;
    bool skipped_reset_write;
    bool reported;
};

/* The AMSS startup programs 32 KiB DTCM and 64 KiB ITCM. */
static const uint32_t tcm_size[] = { 32 * KiB, 64 * KiB };
static const uint32_t tcm_size_bits[] = { 6 << 2, 7 << 2 };

static uint64_t wave723_tcm_read(CPUARMState *env, const ARMCPRegInfo *reg)
{
    Wave723State *s = WAVE723_MACHINE(qdev_get_machine());

    return s->tcm[reg->opc2].region;
}

static void wave723_tcm_write(CPUARMState *env, const ARMCPRegInfo *reg,
                              uint64_t value)
{
    Wave723State *s = WAVE723_MACHINE(qdev_get_machine());
    unsigned bank = reg->opc2;
    Wave723Tcm *tcm = &s->tcm[bank];
    uint32_t base = (uint32_t)value & ~(tcm_size[bank] - 1);
    bool enabled = value & 1;

    BQL_LOCK_GUARD();
    tcm->region = base | tcm_size_bits[bank] | enabled;
    memory_region_transaction_begin();
    memory_region_set_address(&tcm->memory, base);
    memory_region_set_enabled(&tcm->memory, enabled);
    memory_region_transaction_commit();
}

static const ARMCPRegInfo wave723_tcm_regs[] = {
    { .name = "DTCM Region", .cp = 15, .crn = 9, .crm = 1, .opc2 = 0,
      .state = ARM_CP_STATE_AA32, .access = PL1_RW, .type = ARM_CP_IO | ARM_CP_NO_RAW,
      .readfn = wave723_tcm_read, .writefn = wave723_tcm_write },
    { .name = "ITCM Region", .cp = 15, .crn = 9, .crm = 1, .opc2 = 1,
      .state = ARM_CP_STATE_AA32, .access = PL1_RW, .type = ARM_CP_IO | ARM_CP_NO_RAW,
      .readfn = wave723_tcm_read, .writefn = wave723_tcm_write },
};

static void wave723_finish(Wave723State *s, const char *reason, hwaddr address,
                           unsigned size, const uint64_t *value)
{
    CPUARMState *env = &s->cpu->env;

    if (s->reported) {
        return;
    }
    s->reported = true;
    printf("Stop: %s", reason);
    if (size) {
        printf(" at 0x%08" PRIx64 " (%u bytes)", (uint64_t)address, size);
    }
    if (value) {
        printf("\nWrite value 0x%08" PRIx64, *value);
    }
    printf("\nPC 0x%08" PRIx32 "  CPSR 0x%08" PRIx32 "\n",
           env->regs[15], cpsr_read(env));
    for (int i = 0; i < 16; ++i) {
        printf("r%d=0x%08" PRIx32 "%s", i, env->regs[i],
               i % 4 == 3 ? "\n" : "  ");
    }
    fflush(stdout);
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
}

static uint64_t wave723_read(void *opaque, hwaddr address, unsigned size)
{
    wave723_finish(opaque, "unmapped read", address, size, NULL);
    return 0;
}

static void wave723_write(void *opaque, hwaddr address, uint64_t value,
                          unsigned size)
{
    Wave723State *s = opaque;
    uint32_t reset_write_pc = s->reset_vector == APPS_BASE ?
        APPS_BASE + 0x67c : AMSS_BASE + 0x204;

    if (s->diagnostic && !s->skipped_reset_write && address == RESET_WRITE &&
        size == 4 && value == 0 && s->cpu->env.regs[15] == reset_write_pc) {
        s->skipped_reset_write = true;
        printf("Diagnostic: skipped the initial zero write to 0x08880048\n");
        return;
    }
    wave723_finish(s, "unmapped write", address, size, &value);
}

static const MemoryRegionOps wave723_unmapped_ops = {
    .read = wave723_read,
    .write = wave723_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static void wave723_timeout(void *opaque)
{
    wave723_finish(opaque, "instruction limit", 0, 0, NULL);
}

static void wave723_reset(void *opaque)
{
    Wave723State *s = opaque;
    for (unsigned i = 0; i < ARRAY_SIZE(s->tcm); ++i) {
        s->tcm[i].region = tcm_size_bits[i];
        memory_region_set_enabled(&s->tcm[i].memory, false);
    }
    s->reported = false;
    s->skipped_reset_write = false;
    cpu_set_pc(CPU(s->cpu), s->reset_vector);
    timer_mod(s->deadline, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->limit);
}

static void wave723_init(MachineState *machine)
{
    Object *cpu = object_new(machine->cpu_type);
    MemoryRegion *memory = get_system_memory();
    Wave723State *s = WAVE723_MACHINE(machine);
    const char *limit = machine->kernel_cmdline;
    char *end = NULL;

    s->reset_vector = AMSS_BASE;
    if (limit && g_str_has_prefix(limit, "diagnostic:")) {
        s->diagnostic = true;
        limit += strlen("diagnostic:");
    }
    if (limit && g_str_has_prefix(limit, "apps:")) {
        s->reset_vector = APPS_BASE;
        limit += strlen("apps:");
    } else if (limit && g_str_has_prefix(limit, "amss:")) {
        limit += strlen("amss:");
    }
    s->limit = limit && *limit ? g_ascii_strtoll(limit, &end, 10) : 100000;
    if ((end && *end) || s->limit <= 0 || s->limit > 2000000) {
        error_report("Wave 723 instruction limit must be between 1 and 2000000.");
        exit(1);
    }
    if (!machine->kernel_filename || machine->ram_size != RAM_SIZE) {
        error_report("Wave 723 requires a reset image supplied with -kernel and 32 MiB RAM.");
        exit(1);
    }
    object_property_set_bool(cpu, "has_el3", false, &error_fatal);
    s->cpu = ARM_CPU(cpu);
    define_arm_cp_regs(s->cpu, wave723_tcm_regs);
    qdev_realize(DEVICE(cpu), NULL, &error_fatal);
    memory_region_init_io(&s->unmapped, OBJECT(machine), &wave723_unmapped_ops,
                          s, "wave723-unmapped", UINT64_C(1) << 32);
    memory_region_add_subregion_overlap(memory, 0, &s->unmapped, -1);
    memory_region_add_subregion(memory, RAM_BASE, machine->ram);
    for (unsigned i = 0; i < ARRAY_SIZE(s->tcm); ++i) {
        MemoryRegion *region = &s->tcm[i].memory;
        memory_region_init_ram(region, OBJECT(machine), i ? "wave723.itcm" : "wave723.dtcm",
                               tcm_size[i], &error_fatal);
        memory_region_set_enabled(region, false);
        memory_region_add_subregion_overlap(memory, 0, region, 1 + i);
    }
    load_image_targphys(machine->kernel_filename, s->reset_vector,
                        RAM_SIZE - (s->reset_vector - RAM_BASE), &error_fatal);
    s->deadline = timer_new_ns(QEMU_CLOCK_VIRTUAL, wave723_timeout, s);
    qemu_register_reset(wave723_reset, s);
}

static void wave723_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "Samsung Wave 723 (BCM2156 reset probes)";
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm1176");
    mc->default_ram_size = RAM_SIZE;
    mc->default_ram_id = "wave723.ram";
    mc->max_cpus = 1;
    mc->init = wave723_init;
}

static const TypeInfo wave723_machine_info = {
    .name = TYPE_WAVE723_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(Wave723State),
    .class_init = wave723_class_init,
    .interfaces = arm_machine_interfaces,
};

static void wave723_register_types(void)
{
    type_register_static(&wave723_machine_info);
}

type_init(wave723_register_types)
