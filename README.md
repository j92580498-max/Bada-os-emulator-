# Wave 723

Experimental Samsung Wave 723 (GT-S7230/E, Broadcom BCM2156) emulation for
Android, Windows, and Linux. The current milestone is firmware inspection and
execution of the AMSS and APPS ARM reset vectors in QEMU. It stops at the first unsupported memory
access, CPU error, or instruction limit and reports the CPU state. Bada does
not boot yet. Display, touchscreen, storage and bootloader emulation are pending.

## Run

Download a package from a successful [Build and test run](https://github.com/j92580498-max/Bada-os-emulator-/actions/workflows/build.yml).

- Android 9 or later on ARM64 and x86-64. Install `app-debug.apk`, open the app,
  and select the Wave 723 firmware ZIP. Tap **Trace reset** to execute AMSS.
- Windows x86-64 and Linux x86-64 or ARM64. Extract the package and launch
  `wave723-ui` (`wave723-ui.exe` on Windows). Open or drop the firmware ZIP into
  the window. The command-line tool provides the same data for scripting.

```sh
./wave723 inspect /path/to/S7230EXXKG1_OXGKG2.zip
./wave723 trace /path/to/S7230EXXKG1_OXGKG2.zip 100000
./wave723 trace-apps /path/to/S7230EXXKG1_OXGKG2.zip 100000
```

Add `--diagnostic` to either trace command to skip the single observed startup
write to `0x08880048` and report the next blocker.

Use your own firmware archive. Firmware is opened read-only. On Android the
selected archive is copied to app-private cache for random access.

## Build

Desktop requires CMake 3.22+, a C/C++17 toolchain and Python 3 for the smoke
tests. CI uses MSVC for the Windows frontend, MinGW for Windows QEMU and GCC
for Linux. The [CI workflow](.github/workflows/build.yml) lists dependencies and
the QEMU revision. Pass `-DWAVE723_DESKTOP=OFF` for a CLI-only build.

```sh
# After checking out the QEMU revision listed in the CI workflow:
bash scripts/build-qemu.sh /path/to/qemu-ios build-qemu linux
mkdir -p build-qemu/runtime
cp build-qemu/qemu-system-arm build-qemu/runtime/
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWAVE723_QEMU_RUNTIME=build-qemu/runtime
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

For Android use JDK 17 with Android SDK 35, NDK 27.2.12479018 and CMake 3.22.1.
Set `WAVE723_ANDROID_NDK` to the NDK path. Build QEMU with
`scripts/build-qemu.sh` for each Android ABI and place the resulting
`qemu-system-arm` at `android/app/src/main/jniLibs/<abi>/libwave723_qemu.so`.

```sh
cd android
./gradlew :app:assembleDebug
```

GitHub Actions builds and tests the Linux packages using synthetic ARM and
Thumb images. Dispatch the workflow with **Include the Windows and Android
packages** enabled to build those platforms. This project is distributed under GPL-2.0 and uses
[QEMU from LightTouch's Android-capable fork](https://github.com/blake-lucas/qemu-ios/commit/1955aea6930119c34b968c0596dc3cd72d67e839),
[miniz](https://github.com/richgel999/miniz/commit/293d4db1b7d0ffee9756d035b9ac6f7431ef8492),
and [SDL](https://github.com/libsdl-org/SDL/commit/badbf8da4ee72b3ef599c721ffc9899e8d7c8d90).

See [hardware notes](docs/hardware.md) for firmware observations and references.
