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

cp "$project_root/qemu/wave723.c" "$source_dir/hw/arm/wave723.c"
if ! grep -q 'config WAVE723' "$source_dir/hw/arm/Kconfig"; then
    cat >> "$source_dir/hw/arm/Kconfig" <<'KCONFIG'

config WAVE723
    bool
KCONFIG
    echo "arm_ss.add(when: 'CONFIG_WAVE723', if_true: files('wave723.c'))" >> "$source_dir/hw/arm/meson.build"
fi
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
    linux|windows)
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
        args+=(
            "--cc=$toolbin/${triple}28-clang"
            "--cxx=$toolbin/${triple}28-clang++"
            "--ar=$toolbin/llvm-ar"
            "--host-cc=gcc"
            "--cpu=$arch"
            --cross-prefix=
            --disable-containers
            "--extra-ldflags=-Wl,-z,max-page-size=16384"
            --static
        )
        ;;
    *)
        echo "Unknown QEMU host platform $platform" >&2
        exit 2
        ;;
esac

cd "$build_dir"
"$source_dir/configure" "${args[@]}"
make -j "${WAVE723_BUILD_JOBS:-4}" qemu-system-arm
