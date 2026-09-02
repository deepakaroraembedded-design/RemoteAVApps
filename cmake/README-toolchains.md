# Cross-compiling the thin client

The stack targets embedded/Linux devices. Example toolchain file for aarch64
(adjust paths to your sysroot):

```cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CROSS_PREFIX aarch64-linux-gnu-)
set(CMAKE_C_COMPILER ${CROSS_PREFIX}gcc)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-aarch64)  # optional, for ctest
```

Configure:

```sh
cmake -S . -B build-arm64 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake \
  -DVMC_ENABLE_TESTS=OFF
cmake --build build-arm64
```

Backend notes:

- `src/platform/linux/time.c` — Linux monotonic clock. For bare-metal, provide
  `vmc_time_now_us()`/`vmc_sleep_ms()` and drop `_DEFAULT_SOURCE`.
- `src/input/evdev_input.c` — Linux evdev. An I2C/SPI touch backend should
  implement `vmc_input_ops` (see `include/vmc/input/input.h`).
- Video decoder + display are interface-only in this iteration; hardware
  backends (HEVC VPU, KMS/DRM) plug in behind `vmc/video/decoder.h` and
  `vmc/video/display.h`.
