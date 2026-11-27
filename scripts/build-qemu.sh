#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 3 ]]; then
    echo "Usage: bash scripts/build-qemu.sh QEMU_SOURCE BUILD_DIRECTORY PLATFORM" >&2
    exit 2
fi

project_root=$(cd "$(dirname "$0")/.." && pwd)
source_dir=$(cd "$1" && pwd)
mkdir -p "$2"
build_dir=$(cd "$2" && pwd)
platform=$3
binary=qemu-system-arm

cp "$project_root/qemu/wave723.c" "$project_root/qemu/wave723_lcd.c" \
   "$project_root/qemu/wave723_lcd.h" "$project_root/qemu/wave723_onenand.c" \
   "$project_root/qemu/wave723_onenand.h" "$project_root/qemu/wave723_nand_geometry.h" "$source_dir/hw/arm/"
if ! grep -q 'config WAVE723' "$source_dir/hw/arm/Kconfig"; then
    cat >> "$source_dir/hw/arm/Kconfig" <<'KCONFIG'

config WAVE723
    bool
KCONFIG
fi
sed -i "/^arm_ss.add(when: 'CONFIG_WAVE723', if_true:/d" "$source_dir/hw/arm/meson.build"
echo "arm_ss.add(when: 'CONFIG_WAVE723', if_true: files('wave723.c', 'wave723_lcd.c', 'wave723_onenand.c'))" >> "$source_dir/hw/arm/meson.build"
printf 'CONFIG_WAVE723=y\n' > "$source_dir/configs/devices/arm-softmmu/default.mak"

args=(
    --target-list=arm-softmmu
    --without-default-features
    --disable-werror
    --disable-docs
    --disable-plugins
    --disable-tools
)

case "$platform" in
    linux)
        args+=(--enable-fdt=internal)
        ;;
    windows)
        binary=qemu-system-arm.exe
        ;;
    arm64-v8a|x86_64)
        : "${WAVE723_ANDROID_NDK:?Set the Android NDK directory}"
        toolbin="$WAVE723_ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
        arch=aarch64
        if [[ $platform == x86_64 ]]; then arch=x86_64; fi
        triple="$arch-linux-android"
        deps_dir="$build_dir/deps"
        python3 "$source_dir/contrib/portable/build-deps.py" "$deps_dir" \
            --abi "$platform" --ndk "$WAVE723_ANDROID_NDK"
        export PKG_CONFIG_LIBDIR="$deps_dir/prefix/lib/pkgconfig:$deps_dir/prefix/share/pkgconfig"
        printf '#!/bin/sh\nexec pkg-config --static "$@"\n' > "$build_dir/target-pkg-config"
        chmod +x "$build_dir/target-pkg-config"
        export PKG_CONFIG="$build_dir/target-pkg-config"
        export AR="$toolbin/llvm-ar"
        args+=(
            "--cc=$toolbin/${triple}28-clang"
            "--cxx=$toolbin/${triple}28-clang++"
            "--host-cc=gcc"
            "--cpu=$arch"
            --cross-prefix=
            --disable-containers
            "--extra-ldflags=-pie -Wl,-z,max-page-size=16384"
            --disable-pie
        )
        ;;
    *)
        echo "Unknown QEMU host platform $platform" >&2
        exit 2
        ;;
esac

cd "$build_dir"
"$source_dir/configure" "${args[@]}"
make -j "${WAVE723_BUILD_JOBS:-4}" "$binary"
