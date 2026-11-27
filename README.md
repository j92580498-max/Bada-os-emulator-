# Wave 723

Experimental Samsung Wave 723 (GT-S7230/E, Broadcom BCM2156) emulation for
Android, Windows, and Linux. The current milestone is firmware inspection and
execution of the AMSS ARM reset vector. It stops at the first unsupported memory
access, CPU error, or instruction limit and reports the CPU state. Bada does
not boot yet. Display, touchscreen, storage and bootloader emulation are pending.

## Run

Download a package from a successful [Build and test run](https://github.com/j92580498-max/Bada-os-emulator-/actions/workflows/build.yml).

- Android 8.0 or later on ARM64 and x86-64. Install `app-debug.apk`, open the app,
  and select the Wave 723 firmware ZIP. Tap **Trace reset** to execute AMSS.
- Windows x86-64 and Linux x86-64 or ARM64. Extract the package and launch
  `wave723-ui` (`wave723-ui.exe` on Windows). Open or drop the firmware ZIP into
  the window. The command-line tool provides the same data for scripting.

```sh
./wave723 inspect /path/to/S7230EXXKG1_OXGKG2.zip
./wave723 trace /path/to/S7230EXXKG1_OXGKG2.zip 100000
```

Use your own firmware archive. Firmware is opened read-only. On Android the
selected archive is copied to app-private cache for random access.

## Build

Desktop requires CMake 3.22+, a C/C++17 toolchain and Python 3 for the smoke
tests. CI uses MSVC on Windows and GCC on Linux. Linux SDL dependencies are
listed in the build workflow. Pass `-DWAVE723_DESKTOP=OFF` for a CLI-only build.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

For Android use JDK 17 with Android SDK 35, NDK 27.2.12479018 and CMake 3.22.1.

```sh
cd android
./gradlew :app:assembleDebug
```

GitHub Actions builds and tests the packages using original, synthetic ARM and
Thumb test images. Build inputs for Unicorn, miniz and SDL are pinned in
`CMakeLists.txt`. This project is distributed under GPL-2.0 and uses
[Unicorn](https://github.com/unicorn-engine/unicorn/commit/8028ec436f2d9376525352dd38ed9ed6b9f6be10),
[miniz](https://github.com/richgel999/miniz/commit/293d4db1b7d0ffee9756d035b9ac6f7431ef8492),
and [SDL](https://github.com/libsdl-org/SDL/commit/badbf8da4ee72b3ef599c721ffc9899e8d7c8d90).

See [hardware notes](docs/hardware.md) for firmware observations and references.
