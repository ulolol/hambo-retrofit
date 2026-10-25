# Hambo (2012) — 64-Bit & 120Hz Android Retrofit

Runs the original 2012 Android release of **Hambo** (`com.miniclip.hambo` v1.1.8, 32-bit `armeabi`) natively on modern **64-bit-only Android 14–16+ devices** (Snapdragon 8 Gen 3 / 8 Elite, Dimensity 9300/9400, Google Tensor G3/G4) at **120 Hz** via an in-process **ARM32 → ARM64 Dynarmic JIT loader**.

> **Note:** This repository contains **no copyrighted game assets or original APK binaries**. Bring your own copy of the Hambo 1.1.8 APK (`com.miniclip.hambo`); `build.py` patches it locally and produces a signed 64-bit, 120Hz APK ready to install.

<p align="center">
  <img src="docs/screenshots/jit-mainmenu-textured.png" width="88%" alt="Hambo 1.1.8 running natively at 120Hz on an arm64-only Android 16 device">
  <br>
  <em>Hambo 1.1.8 running at 120.2 FPS on an arm64-only iQOO I2220 (Android 16, Snapdragon 8 Gen 3, Adreno 750).</em>
</p>

---

## Why Hambo Stopped Working on Modern Android — and How This Fixes It

| Problem on Modern Android (Android 14–16+) | How `hambo-retrofit` Fixes It |
|---|---|
| **`INSTALL_FAILED_NO_MATCHING_ABIS`**: `libhambo.so` is 32-bit `armeabi` only; modern ARMv9 CPUs have no 32-bit execution state | A 64-bit `arm64-v8a/libhambo.so` loader maps the untouched 32-bit `libhambo_guest.so` into a low-4GB arena and JIT-compiles ARM/Thumb to ARM64 via [dynarmic](https://github.com/azahar-emu/dynarmic) |
| **`targetSdkVersion 9` blocked by Android 14+**: Package installer rejects legacy target SDKs | Retrofits `targetSdkVersion` to `24` and sets `android:extractNativeLibs="true"` |
| **16 KB page-size enforcement (Android 15/16)**: Old ELF segments are 4 KB aligned | 64-bit host loader is linked with `-Wl,-z,max-page-size=16384`; guest ELF is mapped in user space with `__page_size = 4096` |
| **2012 Dalvik JNI bugs crash 64-bit ART**: `MsetMainActivity` and `JniHelper` cache local `jobject`/`jclass` pointers in C globals without `NewGlobalRef` | Bidirectional 32↔64 JNI bridge automatically promotes cached `FindClass`/`GetObjectClass`/`MsetMainActivity` references to persistent `GlobalRef` handles |
| **GNUstep Objective-C CP15 barriers & spinlocks**: `NSIncrementExtraRefCount` executes `mcr p15, 0, r0, c7, c10, 5` and `ldrex`/`strex` | Hooks CP15 coprocessor operations and wires `Dynarmic::ExclusiveMonitor` + atomic CAS `MemoryWriteExclusive*` |
| **30 FPS frame limiter in `ClearRenderer`**: `ClearRenderer.onDrawFrame` sleeps `(animationInterval - dt) * 2`, capping the game at ~30.4 FPS | Removes the Java sleep cap, requests `120.0f` Hz on both the `Window` and `SurfaceView`, and patches `b2World::Step` (`0x4c413c`) to step Box2D physics by the real frame `dt` |
| **Defunct 2012 Google Play Billing v2**: In-app store cannot connect to dead billing service | Optional `--free-shopping` patch (enabled by default) makes `InAppActivity.isPurchaseReallyOwned` report all items/missions/costumes as owned |

---

## Quick Start

### Requirements
- **Python 3.8+**
- **Java JDK 17+** (`java` and `keytool` on `PATH`)
- Your own copy of **Hambo 1.1.8 APK**:
  - Clean 1.1.8 SHA-256: `d50cf8666555f96bf0e9a80fa23edf52bda23ce6d120b61ff8f32a899ce1b3ff`
  - Or Modded 1.1.8 SHA-256: `b6528b8fc17915c3192f6e6db45bc9344d3fc4022fec62c0fc5f05b2d3c4bde0`

### Build the 64-Bit 120Hz APK

```bash
python3 build.py path/to/Hambo_1.1.8.apk
```

This uses the prebuilt 64-bit loader in `prebuilt/arm64-v8a/libhambo.so` and outputs **`Hambo-1.1.8-arm64-120hz.apk`**.

On first run a local self-signed keystore (`retro.keystore`) is generated to sign the output. It is **git-ignored and never published**; override the defaults with `HAMBO_KS_ALIAS` / `HAMBO_KS_PASS`.

#### Options
- `--no-free-shopping`: Keep original unmodded purchase checks (by default `--free-shopping` is applied).
- `--from-source`: Compile the 64-bit `libhambo.so` JIT loader from `port/` using CMake, Ninja, Boost headers, and the Android NDK.

### Install via ADB

```bash
adb install -r Hambo-1.1.8-arm64-120hz.apk
```

---

## Architecture Overview

```
+-----------------------------------------------------------------------+
| Android 16 (64-bit ART process: com.miniclip.hambo)                   |
|                                                                       |
|  Java Layer (HamboActivity, cocojava, ClearRenderer @ 120Hz)          |
|       |                                              ^                |
|       | 57 CocoJNI native calls                      | JNI upcalls    |
|       v                                              |                |
|  +-----------------------------------------------------------------+  |
|  | lib/arm64-v8a/libhambo.so (64-bit ARM32->ARM64 JIT Loader)      |  |
|  |  - Bidirectional 32<->64 JNI Bridge (jni.cpp)                   |  |
|  |  - Dynarmic A32/T32 -> AArch64 JIT + ExclusiveMonitor (cpu.cpp) |  |
|  |  - 335 High-Level Emulated Imports: libc, libm, pthread, zlib,  |  |
|  |    OpenGL ES 1.1, mmap, setjmp/longjmp, VFP stubs (libc/gl.cpp) |  |
|  +-----------------------------------------------------------------+  |
|       |                                                               |
|       v  Identity-mapped low-4GB dlmalloc arena (0x76300000..)        |
|  +-----------------------------------------------------------------+  |
|  | lib/arm64-v8a/libhambo_guest.so (Original 2012 32-bit armeabi)  |  |
|  |  - 12,917 functions: GNUstep ObjC, Foundation, cocos2d-iphone,  |  |
|  |    Box2D physics, 2012 libwebp (.pngpl), plist_b decryptor      |  |
|  +-----------------------------------------------------------------+  |
+-----------------------------------------------------------------------+
```

1. **Identity-Mapped Low-4GB Arena (`port/src/memory.cpp`)**: Reserves a 1.25 GB gap below `4 GB` in `/proc/self/maps` managed by `dlmalloc`. Every 32-bit guest pointer zero-extends directly to a valid 64-bit host pointer, allowing zero-copy passthrough for `memcpy`, `fread`, `glTexImage2D`, `glVertexPointer`, `glDrawElements`, etc.
2. **ELF32 Loader (`port/src/memory.cpp`)**: Maps `libhambo_guest.so`, applies relocations (`R_ARM_RELATIVE`, `R_ARM_ABS32`, `R_ARM_GLOB_DAT`, `R_ARM_JUMP_SLOT`), and wires all 335 imported symbols to `svc #id; bx lr` thunks (or direct in-arena ARM VFP/IDIV instruction stubs for `__aeabi_*` math helpers).
3. **Dynarmic JIT + ExclusiveMonitor (`port/src/cpu.cpp`)**: Executes ARM and Thumb code at native speed, handles CP15 barrier instructions, and implements atomic `ldrex`/`strex` via `Dynarmic::ExclusiveMonitor` and `std::atomic::compare_exchange_strong`.
4. **120 Hz + Dynamic Box2D Timestep (`port/src/jni.cpp`)**: Requests 120 Hz on both the Activity `Window` and the `SurfaceView` `Surface`, and patches `0x4c413c` in `libhambo_guest.so` from a hardcoded `1/60` s literal load (`ldr r1, [pc, #0x4c0]`) to load the actual frame delta time `dt` (`ldr r1, [sp, #0x4]`).

---

## Repository Layout

| Path | Description |
|---|---|
| `build.py` | Standalone APK patcher and builder script |
| `port/src/` | 64-bit ARM32→ARM64 JIT loader source (`core.h`, `memory.cpp`, `cpu.cpp`, `libc.cpp`, `gl.cpp`, `jni.cpp`) |
| `port/CMakeLists.txt` | CMake build configuration for NDK `arm64-v8a` (16 KB page-size aligned) |
| `prebuilt/arm64-v8a/libhambo.so` | Prebuilt stripped 64-bit JIT loader (2.7 MB) |
| `legacy-cleanroom-poc/` | Phases 2–3c clean-room C engine PoC (APK zip reader, `bplist00` parser, GLES 1.1 sprite sheet renderer) |
| `docs/PORTING-GUIDE.md` | Complete guide to porting other 32-bit `armeabi` games to 64-bit Android using this architecture |
| `docs/JNI-CONTRACT.md` | Full JNI contract (57 exports) and import surface documentation |
| `docs/PORTING-PLAN.md` | Engineering log across all project phases |
| `docs/screenshots/` | Device verification screenshots across Phases 2, 3a, 3b, 3c, and 4 (120Hz JIT) |

---

## Credits & License

- Code in this repository is licensed under the **MIT License**.
- Loader architecture adapted from [Santi2065/dundef-sw-android64](https://github.com/Santi2065/dundef-sw-android64) (MIT).
- ARM32→ARM64 dynamic recompilation powered by [dynarmic](https://github.com/azahar-emu/dynarmic) (0BSD).
- Low-4GB arena allocator powered by Doug Lea's `dlmalloc` (MIT-0).
