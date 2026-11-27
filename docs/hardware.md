# Wave 723 hardware notes

The target is the Samsung Wave 723 GT-S7230/E with Broadcom BCM2156. The current
CPU model is Unicorn's ARM1176, an ARM11-family approximation. The exact SoC
revision, CP15 state and reset memory map remain unverified. The probe maps
32 MiB from `0x80000000` to contain the linked AMSS addresses and reports
accesses outside that range. That RAM window is a probe configuration.

## S7230EXXKG1_OXGKG2

The supplied ZIP contains eight flash components. The firmware inventory
recognizes the QMD resource header, CSC signature, FimBIN and PFSBIN headers, and
the FOTA ARM entry. The archive lacks a recognized standalone bootloader.

The first AMSS word is ARM `ldr pc, [pc, #0x24]`. Its little-endian literal at
offset `0x2c` is `0x803001fc`, with nearby exception vectors also targeting
`0x8030xxxx`. Together these support loading the image at `0x80300000` for
the current execution probe. The opaque `apps_compressed.bin` still needs its
loader format identified before it can be mapped as a Bada application image.

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

The [Hardware code search](https://github.com/j92580498-max/Bada-os-emulator-/actions/workflows/research.yml)
workflow repeats the searches on demand. GitHub code search requires an
authenticated token. Set the optional `CODE_SEARCH_TOKEN` repository secret if
the runner token does not have search access.
