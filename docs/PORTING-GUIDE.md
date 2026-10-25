# Porting 32-Bit `armeabi` Android Games to 64-Bit Android 14–16+

This document explains the engineering decisions, root-cause diagnoses, and step-by-step methodology used to port **Hambo (2012)** to 64-bit-only Android 16 devices at 120 Hz, and serves as a practical reference for porting other legacy 32-bit Android games.

---

## 1. Choosing the Right Porting Strategy: Clean-Room Rewrite vs. In-Process JIT Loader

When an old Android game ships only `lib/armeabi/libgame.so` (or `lib/armeabi-v7a/libgame.so`), modern ARMv9 phones (Snapdragon 8 Gen 3 / 8 Elite, Tensor G3/G4, Dimensity 9300/9400) lack 32-bit hardware execution at EL0 and Android 14+ refuses to install 32-bit-only APKs.

During this project, we evaluated two approaches:

### Approach A: Decompile-Driven Clean-Room Engine Rebuild (`legacy-cleanroom-poc/`)
- **How it works**: Replace `libhambo.so` with a brand-new `arm64-v8a` C/C++ library that implements the JNI surface and gradually reconstructs the asset parsers and game logic from Ghidra decompilation.
- **When it is useful**:
  - Great for early bring-up (proving manifest retrofits, 16 KB page alignment, JNI signatures, GLES context creation, and basic asset formats like `bplist00`).
  - Viable if the native library is a small utility or thin wrapper (< 100 functions).
- **Why it hits a wall on full games**:
  - `libhambo.so` is **8.25 MB** with **12,917 functions** (statically linking the GNUstep Objective-C runtime, Foundation, cocos2d-iphone, Box2D, 2012 `libwebp`, custom `plist_b` encryption, and 252 levels of game logic). Rewriting 12,917 functions from pseudocode takes months and never achieves 100% physics/gameplay fidelity.

### Approach B: In-Process ARM32 → ARM64 Dynarmic JIT Loader (`port/src/`)
- **How it works**:
  - Ship a 64-bit `lib/arm64-v8a/libhambo.so` loader alongside the untouched 32-bit `lib/arm64-v8a/libhambo_guest.so`.
  - Map the 32-bit ELF into an identity-mapped low-4GB memory arena (`0x10000000..0xFFFF0000`) so every 32-bit guest pointer is directly a valid 64-bit host pointer.
  - JIT-compile the guest's 32-bit ARM/Thumb machine code to native ARM64 using **Dynarmic**.
  - Intercept and wrap **only the external boundary** (JNI entry points + `SHN_UNDEF` imported symbols).
- **Why it wins**:
  - While `libhambo.so` has **12,917 internal functions**, it only has **335 imported symbols** (`libc`, `libm`, `pthread`, `zlib`, `libGLESv1_CM`) and **57 JNI exports**.
  - All internal subsystems—GNUstep Objective-C, cocos2d-iphone, Box2D, custom `plist_b` decryption, and the bundled 2012 `libwebp` alpha decoder—run 100% unmodified at native speed.

---

## 2. Step-by-Step Playbook for Porting Another 32-Bit Android Game

### Step 1: Inspect the Target `.so`'s External Boundary
Run `llvm-readelf` on the original 32-bit `.so`:

```bash
llvm-readelf -d libgame.so | grep NEEDED
llvm-readelf --dyn-syms -W libgame.so | awk '$7 == "UND" && $8 != "" {print $8}' | sort -u
llvm-readelf --dyn-syms -W libgame.so | awk '$7 != "UND" && $8 ~ /^Java_|^JNI_/ {print $8}'
```

1. **Check `DT_NEEDED`**:
   - Typical 2010–2015 games link `libc.so`, `libm.so`, `libstdc++.so`, `libz.so`, `libdl.so`, `liblog.so`, and `libGLESv1_CM.so` or `libGLESv2.so`.
   - `port/src/libc.cpp` and `port/src/cpu.cpp` already cover ~260+ standard `libc`/`libm`/`pthread`/`zlib`/`__aeabi_*` symbols.
   - `port/src/gl.cpp` covers OpenGL ES 1.1 (`libGLESv1_CM.so`); for OpenGL ES 2.0 games, swap in GLES2 wrappers (see `Santi2065/dundef-sw-android64`).
2. **Check Relocation Types & Data Imports**:
   - Standard ARM32 relocations (`R_ARM_RELATIVE=23`, `R_ARM_ABS32=2`, `R_ARM_GLOB_DAT=21`, `R_ARM_JUMP_SLOT=22`) are handled in `port/src/memory.cpp`.
   - Check for `STT_OBJECT` undefined symbols (`__sF`, `__stack_chk_guard`, `_ctype_`, `_tolower_tab_`, `_toupper_tab_`, `__page_size`, `environ`). All of these are registered via `register_data(...)` in `port/src/libc.cpp`.

### Step 2: Check Soft-Float (`__aeabi_*`) Imports vs. In-Binary Definitions
Many 2010–2013 `armeabi` games were compiled for ARMv5TE soft-float without hardware VFP or `sdiv`/`udiv`:
- Some games statically link `libgcc` (`__aeabi_fadd`, `__aeabi_idiv`, etc. have `st_shndx != 0`).
- Other games (like Hambo) import `__aeabi_*` as `SHN_UNDEF` symbols (`st_shndx == 0`) via PLT/GOT.
- **Our loader handles both**:
  - `init_softfp_imports()` (called *before* `load_guest()`) writes short 5-instruction ARM VFP/IDIV stubs into the guest arena and registers their addresses via `register_data()`, so PLT/GOT relocations point directly to VFP/IDIV instructions inside the JIT without even triggering an `svc` exit.
  - `init_softfp(g_lib)` (called *after* `load_guest()`) patches any in-binary `__aeabi_*` routines to branch to the VFP/IDIV stubs.

### Step 3: Handle 32-Bit vs. 64-Bit Struct & ABI Differences
Because guest pointers (`u32`) are identity-mapped in the low 4 GB, functions taking `void*`, `char*`, `int*`, or `float*` need **zero pointer translation**. You only need custom wrappers in `libc.cpp` where 32-bit ARM EABI struct layouts or type widths differ from 64-bit `aarch64`:
- **`struct stat` (`stat`, `lstat`, `fstat`)**: 32-bit ARM EABI `stat64` is 104 bytes (`stat_to_guest`).
- **`struct tm` (`gmtime`, `localtime_r`, `mktime`, `strftime`)**: 32-bit `tm` is 44 bytes (`tm_to_guest` / `tm_from_guest`).
- **`struct timeval` / `struct timespec` (`gettimeofday`, `clock_gettime`, `select`, `utimes`, `pthread_cond_timedwait`)**: Two 32-bit words (`tv_sec`, `tv_usec`/`tv_nsec`).
- **`struct iovec` (`writev`)**: 8 bytes (`{ u32 iov_base, u32 iov_len }`) in guest vs. 16 bytes in host.
- **`z_stream` (`inflateInit_`, `inflateInit2_`, `inflate`, `inflateEnd`, `deflate*`)**: 56 bytes in 32-bit guest vs. 112 bytes in 64-bit host (`z_from_guest` / `z_to_guest` shadow `z_stream` map in `libc.cpp`).
- **`mmap` / `munmap`**: Calling 64-bit host `mmap(NULL, ...)` returns a pointer above 4 GB (`0x7f...`), which faults `mem::g()`. Instead, allocate 4 KB-aligned memory inside the low-4GB guest arena and use `pread(fd, p, len, off)` for file-backed mappings.
- **`setjmp` / `longjmp`**: Save and restore `r4..r11`, `sp` (`r13`), and `lr` (`r14`) directly in the guest `jmp_buf` inside the `svc` handler; when the `longjmp` thunk executes `bx lr`, it returns directly to `setjmp`'s caller in the guest.

### Step 4: Watch Out for 5 Classic Traps on Modern Android / ART

1. **2012 Dalvik Un-Promoted JNI References (`FindClass` & `jobject` globals)**:
   - Old Dalvik allowed C/C++ code to store a `jclass` returned by `env->FindClass(...)` or a `jobject` passed to a JNI method directly into a C global variable without calling `NewGlobalRef`.
   - Modern 64-bit ART aborts immediately (`use of invalid jobject`) as soon as that local reference's frame exits or is used from another thread.
   - **Fix**: In `jni.cpp`, promote `FindClass`, `GetObjectClass`, `GetSuperclass`, and persistent activity/context objects to `NewGlobalRef` handles (`put_global`) and ignore `DeleteLocalRef` on those persistent IDs.
2. **Guest Storing `JNIEnv*` Across Threads**:
   - Old games often save `JNIEnv*` from the main UI thread into a global variable and dereference it later on the `GLThread`.
   - **Fix**: Pass a single constant guest `g_guest_env` table address to the guest, and inside every host `JNIEnv` thunk look up the calling host thread's real `JNIEnv*` via `thread_local JNIEnv* t_env` (`AttachCurrentThread` on demand).
3. **ARMv6/v7 CP15 Coprocessor Instructions (`mcr p15, ...`)**:
   - Older libraries (such as GNUstep Objective-C) use `mcr p15, 0, r0, c7, c10, 5` for a Data Memory Barrier (`DMB`).
   - If `Dynarmic::A32::UserConfig::coprocessors[15]` is `nullptr`, Dynarmic aborts with `ASSERT_FALSE("Should raise coproc exception here")`.
   - **Fix**: Attach a `Coprocessor` implementation to `cfg.coprocessors[0..15]` returning `&g_coproc_sink` for `CompileSendOneWord` / `CompileGetOneWord`.
4. **ARM Exclusive Load/Store (`ldrex` / `strex`)**:
   - Libraries with custom spinlocks use `ldrex` and `strex`.
   - Two things are required in Dynarmic:
     1. Set `cfg.global_monitor = &g_exclusive_monitor;` (otherwise `ldrex` dereferences `nullptr`).
     2. Override `MemoryWriteExclusive8/16/32/64` in `UserCallbacks` using `std::atomic<T>::compare_exchange_strong` (otherwise the default `UserCallbacks::MemoryWriteExclusive*` returns `false` and `strex` loops forever!).
5. **OEM Logcat Filtering (Vivo / iQOO / Xiaomi)**:
   - Some OEM builds suppress `ANDROID_LOG_INFO` / `ANDROID_LOG_DEBUG` from third-party apps in `logcat`. Using `ANDROID_LOG_ERROR` during bring-up ensures loader diagnostics are always visible.

---

## 3. Unlocking 120 Hz on Legacy Games

To bring a 30/60 FPS 2012 game to 120 Hz without breaking gameplay speed:

1. **Remove Java-Side Sleep Throttles**:
   - Check `GLSurfaceView.Renderer.onDrawFrame` (e.g., `ClearRenderer.smali`) for `Thread.sleep(...)` calls that enforce a 30 or 60 FPS cap, and bypass them so rendering is paced by hardware VSYNC.
2. **Request 120 Hz on Both `Window` and `SurfaceView`**:
   - On Android 14–16, `SurfaceFlinger` tracks frame rate votes per layer. Setting `WindowManager.LayoutParams.preferredRefreshRate = 120.0f` and `preferredDisplayModeId` on the `Activity` window is necessary, **and** calling `Surface.setFrameRate(120.0f, 0)` on `GLSurfaceView.getHolder().getSurface()` tells `SurfaceFlinger` not to cap the `SurfaceView` layer at the 60 Hz `Normal` category.
3. **Check Physics Timestep (`b2World::Step`)**:
   - Locate `b2World::Step(float timeStep, int32 velocityIterations, int32 positionIterations)` in the guest binary.
   - In Hambo (`0x4c413c`), `tick:(ccTime)dt` received the real frame `dt` in `r2` (saved at `[sp, #0x4]`), but loaded a hardcoded `1.0f / 60.0f` literal (`0x3c888889` at `0x4c4604`) into `r1` for `b2World::Step`.
   - Patching `0x4c413c` in memory from `0xe59f14c0` (`ldr r1, [pc, #0x4c0]`) to `0xe59d1004` (`ldr r1, [sp, #0x4]`) passes the real elapsed frame `dt` to `b2World::Step`, keeping physics at true 1x real-time speed across 60 Hz, 90 Hz, 120 Hz, and 144 Hz displays.
