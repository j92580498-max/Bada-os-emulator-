# Wave 723 hardware notes

The target is the Samsung Wave 723 GT-S7230/E with Broadcom BCM2156. The current
CPU model is QEMU's ARM1176, an ARM11-family approximation. The exact SoC
revision, CP15 state and reset memory map remain unverified. The probe maps
32 MiB from `0x80000000` for AMSS and expands that window to fit the APPS
image when requested. These sizes are probe configurations.

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
APPS programs `0x08700015` and `0xffff0015` into the TCM region registers,
corresponding to 16 KiB DTCM and 16 KiB ITCM at those bases. The APPS probe
uses those sizes.
Booting bada requires further board emulation.

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

The [Hardware code search](https://github.com/j92580498-max/Bada-os-emulator-/actions/workflows/research.yml)
workflow repeats the searches on demand. GitHub code search requires an
authenticated token. Set the optional `CODE_SEARCH_TOKEN` repository secret if
the runner token does not have search access.
