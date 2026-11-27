# Wave 723 hardware notes

The target is the Samsung Wave 723 GT-S7230/E with Broadcom BCM2156. The current
stock boot uses QEMU's ARM1176 for APPS and ARM926 for AMSS. The exact SoC
revision, CP15 state and reset memory map remain unverified. The probe maps
32 MiB from `0x80000000` for AMSS. Diagnostic APPS runs map RAM through
`0x8fffffff`, the upper bound used by the XXKG1 startup routine at
`0x81263b9e`. This covers the boot-mode data read at `0x8ff00008`.

Photos of both sides of the supplied board show a `GTS7 230E` marking and
a sticker ending in `EXXKG1`. The main package label is covered by the
sticker and the adjacent Samsung package's full part number is unclear at
the available resolution.

## S7230EXXKG1_OXGKG2

The supplied ZIP contains eight flash components. The firmware inventory
recognizes the QMD resource header, CSC signature, FimBIN and PFSBIN headers, and
the FOTA ARM entry. The archive lacks a recognized standalone bootloader.

The first AMSS word is ARM `ldr pc, [pc, #0x24]`. Its little-endian literal at
offset `0x2c` is `0x803001fc`, with nearby exception vectors also targeting
`0x8030xxxx`. Together these support loading the image at `0x80300000` for
the current execution probe. The first write outside RAM is a 32-bit zero
at `0x08880048`. The related BCM21553 database identifies offset `0x48` in
the system configuration block as RAMCTRL, with standby, test mode and
redundancy-reset fields. The probe supports the zero configuration observed
on both XXKG1 paths and reports other settings for investigation.

AMSS then writes `0x08700019` to CP15
`c9, c1, 0` and `0x0000001d` to `c9, c1, 1`, then copies its exception
vectors to address zero. The probe implements these as 32 KiB of DTCM
and 64 KiB of ITCM controlled by their region registers. QEMU models
these as RAM windows for the separate addresses used during startup.
AMSS initializes its stack pointer to `0x0870dda8` and pushes registers at
`0x0870dda0`. Those addresses fall outside the programmed 32 KiB DTCM window.
Diagnostic mode maps 32 KiB of provisional stack RAM at `0x08708000` so the
trace can continue. Its physical backing on the BCM2156 remains unverified.

The last 1,024 bytes of `apps_compressed.bin` contain a Samsung flash footer
with SEED markers and the version `S723E+XX+KG1`. `trace-apps` verifies the
ZIP member, decrypts its payload and maps it at
`0x81000000`. Its reset literal at offset `0x470` targets `0x81000674`.
The decrypted XXKG1 payload has QMD `0x0503` tables at offsets `0x40000`
and `0x80000`. Their section descriptors at header offset `0x80` define
where to stage the resident code and data. The 94 MiB logical image
includes resident code at `0x81000000` through `0x81dfffff`, including the
raw reset block, and 4 MiB of initialized data at `0x83400000`. The two
paged sections use virtual addresses `0x7c000000` and `0xd6300000`.
The reset loader defers those sections pending flash and paging support.
APPS programs `0x08700015` and `0xffff0015` into the TCM region registers,
corresponding to 16 KiB DTCM and 16 KiB ITCM at those bases. The APPS probe
uses those sizes. At `0x81351dc8`, stock APPS initializes `0x3d10` bytes at
`0xffff0400`, through address `0xffff410f`. The firmware MMU descriptors
map a 32 KiB window at `0xffff0000`. At `0x81000c20`, APPS disables ITCM
briefly to store the AMSS entry address at `0xffff0020`. The model maps
SRAM across the window, with ITCM taking priority when enabled.

At `0x81360362`, APPS reads the GPT0 down-counter at `0x0883010c`.
The model starts it at `0xffffffff` and advances it at 32,768 Hz in virtual
time. Stock APPS inverts the count and divides by 32 for its millisecond
timebase. The six GPT channels have reload, down-counter, control and
write-one-to-clear terminal-count registers. Their IRQ outputs connect to
sources 11 and 68 of the interrupt controller.
[BCM21553 GPT reference](https://github.com/assemblycore/qemu-bcm21553/blob/6cd03d2c2a3c4cfc3b36c0cef46a7dc2821e50d0/hw/arm/bcm21553.c)

The handoff routine also sets bit 21 in `SYSCFG_IOCR3` at `0x0888001c`.
The register retains pin configuration writes and preserves its reserved
bits. Stock APPS then writes `6` to the interrupt controller's
`ARM9_SLEEP` register at `0x08810038`. This releases AMSS at the entry point
written by APPS. Each core has private TCM and sees the same external RAM.
[Broadcom SYSCFG register database](https://github.com/MoltenMotherBoard/platform_kernel_samsung_cori/blob/733fd4478845a1125715f5a05eaf8847d49bafd0/arch/arm/plat-bcmap/include/plat/rdb/brcm_rdb_syscfg.h#L189)

In the stock dual-core boot, APPS configures the RNG, DES and crypto AHB
clock gates, initializes both BSC masters and programs GPIO directions,
pull resistors, output levels and interrupt masks. The BSCs at
`0x088a0020` and `0x088b0020` use QEMU I²C buses and IRQs 7 and 13.
Unpopulated slave addresses report NOACK.
[Broadcom board model](https://github.com/assemblycore/qemu-bcm21553/blob/6cd03d2c2a3c4cfc3b36c0cef46a7dc2821e50d0/hw/arm/bcm21553.c)
[Broadcom GPIO driver](https://github.com/Maroc-OS/Merruk-Technology/blob/fe3048483538f9e1d08a8ee1dd6a8dbbbe82a7b6/common/arch/arm/plat-bcmap/gpio.c)

Eight APPS scheduler paths conditionally mirror the current task pointer
to `0x04ffffe8`. At the first write the MMU is active, TTBR0 is
`0x80270000`, and the section descriptor at `0x8027013c` identity-maps
the port. The model implements a four-byte write destination. The board
device receiving the write has yet to be identified.
[Stock scheduler trace](https://github.com/j92580498-max/Bada-os-emulator-/actions/runs/46029574465)

## OneNAND

Stock APPS reads the 16-bit manufacturer ID at `0x0041e000` and expects
`0xec` for Samsung. This locates the OneNAND window at `0x00400000`.
The model uses the related Totoro model's 512 MiB geometry with 4 KiB
pages, 128 spare bytes per page and 64 pages per block. It implements main
and spare reads, bitwise programming, erase and block protection.
[Linux OneNAND registers](https://github.com/torvalds/linux/blob/9437ee8b6cf607e883b36e44bd755968daaebd48/include/linux/mtd/onenand_regs.h)
[Totoro OneNAND model](https://github.com/assemblycore/qemu-bcm21553/blob/6cd03d2c2a3c4cfc3b36c0cef46a7dc2821e50d0/hw/arm/totoro-onenand-probe.h)

To attach a page-aligned dump of the main NAND area, pass
`-drive if=none,id=wave723-flash,format=raw,readonly=on,file=flash.bin`
when running QEMU directly. The normal XXKG1 boot constructs a private NAND
image with the locked and unlocked FSR pool control blocks and eight partition
records. Volume open returns `0x10000000` and the partition checks finish at
`0x8125395c`. Each boot starts from the supplied update. APPS, AMSS, both
resource images, FOTA and the nested QMD `CSC.rcs` go at their Samsung flash
offsets. In the supplied CSC, the nested descriptor and footer both place
0x680000 payload bytes at `0x07d00000`. Filesystem records in CSC.sfs, ShpApp
and PFS still require installation.
[Samsung FSR return codes](https://github.com/CyanogenMod/samsung-kernel-galaxys/blob/478280015136456e6410f3f6b60260e4b104272f/drivers/tfsr/Inc/FSR_BML.h#L263)

## Modem handoff

The XEKF1 service pair supplies the missing SysParm blocks for XXKG1. The
loader validates the model, extent, version and flash address, then loads
their payloads at `0x80080000` and `0x800b0000`. On a hosted stock run AMSS
published `0x5059504d` and 42 entries at `0x800b4000`. The application
processor subsequently reached its timer initialization at `0x8135365e`.
[Hosted stock trace](https://github.com/j92580498-max/Bada-os-emulator-/actions/runs/46024693589)

In a separate peripheral survey, the firmware recorded a modem assertion in
`src/sharedmem.c` at line 1191 when the DSP command ring filled. Its producer
uses 128 eight-byte slots. The interrupt controller has shared pending sources
and separate APPS and modem masks. A bus error on the modem delivers an ARM926
external abort while APPS continues executing.
[Broadcom command queue driver](https://github.com/Maroc-OS/Merruk-Technology/blob/fe3048483538f9e1d08a8ee1dd6a8dbbbe82a7b6/modules/drivers/sound/brcm/alsa/sharedmem.c)
[Hosted assertion trace](https://github.com/j92580498-max/Bada-os-emulator-/actions/runs/46024248277)

## First LCD frame

The XXKG1 RC2 resource has a table at file offset `0x40000`. Its first
bitmap begins at `0x41000` and contains 192,000 bytes of little-endian RGB565
pixels in a 240×400 frame. The display bring-up program in the ARM guest reads
that bitmap and sends DCS sleep-out, pixel format, memory write and display-on
commands through QEMU. The host displays or saves QEMU's resulting frame.

The LCD model uses the BCM21553 reference PIO register layout. The command,
data and status registers are at offsets `0x00`, `0x04` and `0x1c` from
`0x08030000`. The model implements RGB565 panel memory, addressing windows and
display enable. The QEMU PL080 is mapped at `0x08020000`. The stock LCD
transfer sequence still needs verification.
[Broadcom LCD register definitions](https://github.com/MoltenMotherBoard/platform_kernel_samsung_cori/blob/733fd4478845a1125715f5a05eaf8847d49bafd0/drivers/video/broadcom/dss/bcm215xx/reg_lcdc.h)

Stock APPS contains the same command, data and status addresses. The
`boot` command requests an LCD capture directly from stock APPS execution.

## Source research

- LightTouch provided the reference for a firmware-driven workflow with host
  applications and CI packages.
  [LightTouch](https://github.com/blake-lucas/LightTouch/tree/main)
- A `gh search code BCM2156` result documents BCM2156 and ARM11 in the Wave 578
  and other Samsung handsets.
  [postmarketOS wiki archive](https://github.com/Taaloy/postmarketos-wiki-doc/blob/32e49aed189c699e02679bdb43f921b2c93fbfbe/postmarketos-wiki/html/en/Samsung_Handset_Platform_and_Bada.html)
- A `gh search code BCM21553` result contains QEMU models of a related SoC's
  interrupt controller, timers, UARTs and storage. Their addresses and behavior
  need corroboration against BCM2156 before use.
  [qemu-bcm21553](https://github.com/assemblycore/qemu-bcm21553/blob/6cd03d2c2a3c4cfc3b36c0cef46a7dc2821e50d0/hw/arm/bcm21553.c)
- A related Samsung Linux kernel exposes BCM215xx machine definitions.
  [Samsung BCM21553 kernel](https://github.com/androidarmv6/android_kernel_samsung_bcm21553-common/tree/b35d0c2458d89dc2f177969a7fcb16a03cde6eee/arch/arm/mach-bcm215xx)
- A `gh search code` search for the system configuration register database
  found the RAMCTRL register and its fields on the related BCM21553.
  [Broadcom register database](https://github.com/MoltenMotherBoard/platform_kernel_samsung_cori/blob/733fd4478845a1125715f5a05eaf8847d49bafd0/arch/arm/plat-bcmap/include/plat/rdb/brcm_rdb_syscfg.h#L407)
- The ARM Linux TCM support identifies the CP15 register encodings and size fields.
  [ARM TCM setup](https://github.com/torvalds/linux/blob/master/arch/arm/kernel/tcm.c)
- The TriX bada plugin describes the Samsung footer and format.
  [TriX format definitions](https://github.com/g3gg0/TriX/blob/f35f8bfa90641e17ab4b33ba644482d303faa5cd/plugins/TriX_fmtBADA/fmtBADA.h)
- A `gh search code qmd_get_header` result supplied the QMD table layout,
  control stream and code dictionary.
  [libSFormat QMD reference](https://github.com/Crawlerop/libSFormat/blob/16b0b885689fa6af4d27036beab149c2e0c13659/libSFormat/qmd.h)

The [Hardware code search](https://github.com/j92580498-max/Bada-os-emulator-/actions/workflows/research.yml)
workflow repeats the searches on demand. GitHub code search requires an
authenticated token. Set the optional `CODE_SEARCH_TOKEN` repository secret if
the runner token does not have search access.
