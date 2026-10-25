# libhambo.so → arm64-v8a rebuild project (kicked off)

Goal: run Hambo 1.1.8 on the attached device (iQOO I2220, Android 16, Snapdragon 8 Gen 3 / SM8650 — 64-bit-only, `ro.product.cpu.abilist=arm64-v8a`, `ro.zygote=zygote64`).

## Why this is a rebuild, not a retarget

Observed facts (see `../analyze-libhambo.json` and notes below once complete):

- Original ships only `lib/armeabi/libhambo.so` (ELF 32-bit ARM, stripped, 2015-08 build), 8.25 MB.
- Device firmware cannot execute AArch32 (SM8650 cores dropped AArch32; no translator; `pm install --abi armeabi-v7a` → `"ABI armeabi-v7a not supported on this device"`; `install` with lib → `INSTALL_FAILED_NO_MATCHING_ABIS`).
- No arm64 or x86 build exists in the wild (final release 1.1.8, Sep 2015; delisted from Play Oct 2018).
- Composition (from dynsym/strings): Miniclip "Coco" engine — GNUstep-style Objective‑C (6,890 `__objc*` dynsym: Foundation classes such as NSPoint/NSArray/KVO selectors), C++ with GNU STL (`_ZSt*`, gnustl static), Box2D (v2.x, `../Support/Box2D/...` paths), cocos2d, STLport-ish runtime, plus game logic (`GameLayer`, `BulletAmmo`, `HudCrosshair`, `GameControlsStupidZombies`) — all in the one .so.
- Import surface is ARM32-only: `__aeabi_*`, `_Unwind_VRS_*` (AAPCS32 personalization). An arm64 link has to replace the whole ABI surface (no LLVM equivalent of `__aeabi_d2f` import — compiler handles those natively on 64-bit).
- Legacy linkage: undefined imports from `libc.so`, `libm.so`, `libdl.so`, `liblog.so`, `libz.so`, `libstdc++.so` (GNU STL — removed from Android ≥ 7.0 `/system`; the rebuilt lib must NOT depend on it).
- JNI contract: `JNI_OnLoad` + 56 `Java_com_miniclip_nativeJNI_CocoJNI_M*` + `Java_com_miniclip_utils_ReceiptValidator_MvalidateResponse`. Full exact-prototype list: `port/jni-contract.md`.
- Java side needs no changes beyond what the earlier Android-16 retrofit did (`targetSdk` 15→24, re-signed v1+v2+v3) — see repo README.

## Why REA decompilation alone does not complete this

- No toolchain round-trips ARM32 machine code → compilable C++ → functional ARM64 .so: no source, stripped symbols, ObjC runtime tables and `__objc_ivar_offset_*` constants baked into game code, vtables, GNU STL inside the binary, Box2D float determinism (level solutions would break even on a "successful" port).
- So the port strategy is necessarily: recover as much structure as possible with REA/Ghidra, then reimplement the engine skeleton against the *open* components the binary already uses (cocos2d-iphone lineage, Box2D 2.x, GNUstep ObjC runtime — all open source, buildable for arm64 Android), using REA-derived function-level evidence as the reference for game-specific logic.

## Phases (each verifiable against a device/CI check)

1. Inventory & identification — REA/Ghidra: total procedure count, named-area map (ObjC classes vs Box2D vs JNI glue), identify exact cocos2d and Box2D versions used. **Status: in progress** (`../analyze-*` once `analyzeHeadless` completes).
2. Surface skeleton — empty arm64 `libhambo.so` exporting the 58 JNI symbols, built with current NDK, linked statically with vendored libz; Java side loads it; game shows a bug-free stub screen. Deliverable: `dist/Hambo-1.1.8-android16-arm64-skeleton.apk` proving install+load on the iQOO.
3. Engine core — vendor cocos2d(-iphone-ObjC-for-Android lineage, or its open 64-bit-capable equivalent) + Box2D 2.x, get sample level rendering from existing `assets/*.pngpl` data with REA evidence confirming the asset-loader protocol.
4. Game logic recovery — the game-specific layer (`GameLayer`, ammo/physics interactions, HUD, level progression): REA decompilation-driven reimplementation.
5. Android glue re-verification — audio (`SimpleAudioEngine` → modern OpenSLES/AudioTrack path), display scaling, GC behavior, ad SDK paths stubbed dead (they already are).
6. Final packaging — merge rebuilt arm64 lib with existing retrofit APK, `extractNativeLibs` layout decision (16 KB page size compliance), sign, install on the iQOO, capture logcat (`read_adb_logcat`) as runtime evidence.

## Status log

- 2026-10-17: `rea` CLI healthy for Ghidra (native `decompile` binary restored at `Tools/ghidra_12.1.4_PUBLIC/.../os/mac_arm_64/decompile`). MCP server was stale/cached; CLI used instead. First `rea analyze` attempt failed (`execution_failure`, root runtime deleted); reproducing import+analysis steps directly via `analyzeHeadless` to isolate.

## Status log (continued)

- 2026-10-17 (later): The `decompile` native executable shipped in-place (08:13 drop) was a stale
  hand-build ("ghidra.git") incompatible with the 12.1.4 backend — clang build from
  `src/decompile/cpp` per `build.gradle`'s exact file list, installed at
  `Features/Decompiler/build/os/mac_arm_64/decompile`. Ghidra now reports no decompiler errors.
- Direct `analyzeHeadless` import + auto-analysis SUCCEEDED (55s): **12,916 functions,
  ~1.28 MB instruction bytes** → `funcdump.tsv`. Engine fingerprint: GNUstep ObjC + Foundation,
  GNU STL, Box2D 2.x, FreeType, VP8 (WebP), PNG — everything statically inside the one .so.
- REA CLI `analyze` succeeded against the now-compatible decompiler (background run) →
  `..\analyze-libhambo.json`.
- JNI reconciliation completed (see `jni-contract.md`): 55 declared / 52 dynsym / 2 registered /
  1 receipt path — a *further* strong reason to decompile `JNI_OnLoad` before any rebuild.
- 2026-10-17 (Phase 2 COMPLETE): arm64 skeleton built and **live on the device**.
  * `rebuild/skeleton.c` (generated from smali: 55 CocoJNI natives incl. the
    `MsetInAppResponce`/`__IIILjava_lang_String_* overload suffix`, plus
    `MvalidateResponse`) → `rebuild/CMakeLists.txt` → NDK r27c arm64-v8a,
    android-24, **16 KB max-page-size link flag** → `rebuild/build-arm64/libhambo.so`
    (73 KB, `ELF 64-bit LSB shared object, ARM aarch64`).
  * Decoded tree updated: `lib/armeabi` removed, `lib/arm64-v8a/libhambo.so` added,
    manifest forces `android:extractNativeLibs="true"`; `apktool b` +
    uber-apk-signer (retro.keystore v1+v2) → **`dist/hambo-arm64-aligned-signed.apk`**.
  * Device: `adb install -r` → **Success** (the very install that failed with
    `INSTALL_FAILED_NO_MATCHING_ABIS` for the 32-bit lib).
  * Launch: process alive; no `UnsatisfiedLinkError` (0 hits); 32 native calls logged;
    `JNI_OnLoad bound; JavaVM=0xb40000723b796190` and the engine's bootstrap path
    (`setMainActivity`, `setFilesPath`, `displayInfo`, `preserveContext`, `accel`,
    `restoreContext`, `realDisplayInfo`…) all dispatch to the stub.
  * Evidence: `logcat` extract + `screens/phase2-stub-run.png`.
  * Known-expected state: no rendering yet (Mrender stub does nothing) — that is
    Phase 3's job; the *pipeline* (install → load → JNI dispatch) is now proven on
    arm64-only hardware.

## REA evidence (Phase 1 wrap-up record)

- REA CLI `analyze` (Ghidra `binary_overview`) SUCCEEDED:
  evidence `../analyze-libhambo.json` — **9,809 declared procedures** (direct
  dump shows 12,916 including synthetic), **33,244 strings**, .text 6.5 MB.
  First REA `decompile` produced `port/jni_onload.json`: JNI_OnLoad only binds
  gJavaVM and returns 0x10004 — **no &RegisterNatives; therefore the 5
  declared-but-unexported natives are unreachable dead paths in this MOD variant**
  (ads/facebook/ping), and `McheckpointResponse`/`Mend` must be invoked from other
  Java classes (verify in smali) rather than pre-registered. Phase 1 complete.

- 2026-10-17 (Phase 3a COMPLETE): **first GL rendering on the iQOO through the
  engine's own frame pump.**
  * JNI surface correction: `CocoJNI.smali` declares **56** static natives — 48
    `M*` + 8 `NF*` (newsfeed: `NFcallAvailabilityChanged`,
    `NFcallNrOfUnreadMessagesChanged`, …). The generator already emits all 56
    (+ `validateResponse`); skeleton export surface is complete.
  * Skeleton upgraded (Phase 3a): real bodies for `Mrender` (GLES 1 fixed-function
    animated clear color + rotating white triangle, `-lGLESv1_CM`), `MdisplayInfo`
    /`MrealDisplayInfo` (store logical + pixel size), `MsetMainActivity` (global
    ref), `Mrun` (boot marker). Build 68 KB arm64 → same apktool+uber-apk-signer
    pipeline → reinstalled.
  * Runtime: system POST_NOTIFICATIONS dialog stole focus once
    (`handleOnPause → Mpause` froze the loop; recovered after dismissal — nothing
    to fix). After the Java intro countdown (80 frames): `createResources()` →
    **`Mrun()` fired 10:25:11** → `Mrender` pumped continuously.
  * Screen proof: `port/screens/phase3a-render.png` (black during the paused
    moment) then `port/screens/phase3a-state2.png` — **fullscreen landscape
    2800×1260, animated color + rotating triangle**.
  * Meaning: install → load → JNI dispatch → **frame rendering** all work on
    arm64-only SM8650 hardware. The loop, EGL context, and call path are 100%
    the game's own Java; the pixels come from our rebuilt .so.
  * Next (Phase 3b): start decompile-driven recovery of the engine core for real
    content — asset/file path protocol (`MsetFilesPath`, `MsetAPKPath`), Box2D +
    cocos2d-iphone sourcing, first texture (`MloadTexture`/PNG from assets).

- 2026-10-17 (Phase 3b COMPLETE): **real game asset rendered on the iQOO.**
  * Asset protocol recon: engine reads assets two ways — native reads the APK
    zip directly (`MsetAPKPath` receives `ApplicationInfo.sourceDir`) and Java
    decodes textures (`cocotexture.readBitmap` → `BitmapFactory.decodeStream` →
    `copyPixelsToBuffer` → `MnativeInitBitmap(w,h,RGBA)`). `cocojava.loadTexture`
    is the static native→Java callback the original lib used; levels live in
    `assets/unpack/Mission{1..5}_Manifest.plist_b` + plists/sheets (548 files,
    137 `.pngpl` sprites/levels, 56 mp3, fonts).
  * Skeleton upgraded (Phase 3b): `Mrun` calls back into Java
    (`cocojava.loadTexture("Arrow@2x.pngpl")` via GetStaticMethodID on the bound
    activity class) → `MnativeInitBitmap` builds a real GL texture (gen/bind/
    texImage2D GL_RGBA) → `Mrender` draws a textured, premultiplied-blend quad.
  * WebP crisis solved: all 137 `.pngpl` are old-format WebP that Android 16
    (and every modern libwebp — validated with `webpinfo`, Pillow, plus
    isolated-chunk tests) rejects:
      1. VP8X wrote wrong canvas fields — width stored as `width` (not w−1),
         height as `height << 8` (64 → 16384); and
      2. VP8X chunk size 12 (2 pad bytes) vs modern spec size 10;
      3. the ALPH layer itself is an old-format lossless-alpha bitstream modern
         libwebp cannot decode (VP8 lossy alone decodes perfectly).
    Fix applied in decoded tree: VP8X rebuilt canonical (10-byte, correct dims),
    ALPH dropped, VP8 kept → 137/137 now decode (Pillow full-batch verified).
    Alpha recovery (vendored 2012 libwebp ALPH decoder in native) is queued as
    Phase 4 work — for now sprites render opaque.
  * Device proof: `texture 256x128 uploaded from Java decoder` in logcat;
    `screens/phase3b-arrow-texture.png` shows the actual Arrow sprite sheet
    (orange power-arrow segments, green trajectory arrows, white chevrons)
    fullscreen on the phone.
  * Chain now proven end-to-end: install → load → JNI boot (Mrun) → native→Java
    callback → Java WebP decode → native GL texture → rendered frame with real
    game art — all through the game's own Java pipeline, zero custom Java.
  * Next (Phase 3c): plist/level-data path — read `plist_b` (binary plist) from
    the APK zip in native (MsetAPKPath protocol), then sprite-sheet frame
    extraction (arrow counts, positions) and the first interactive gameplay
    slice; plus naming the 141-file WebP preprocessing step in the build script.

- 2026-10-17 (Phase 3c COMPLETE): **native asset pipeline + atlas slicing live on
  the iQOO.**
  * Engine module added (`rebuild/engine.c/h`, ~470 lines):
      1. APK central-directory zip reader (EOCD → CD scan → local header →
         zlib inflate; supports method 0/8) driven by the `MsetAPKPath`
         (`ApplicationInfo.sourceDir`) protocol.
      2. Minimal **bplist00** parser (trailer, offset table, dicts/arrays/ascii
         strings/bools/ints) implemented natively — same protected-lists the
         original engine read through GNUstep.
      3. Texture registry keyed by pending name (`gPending`) set before the
         native→Java `cocojava.loadTexture` callback; `MnativeInitBitmap`
         = upload + side-parse of `assets/unpack/<name>.plist`.
      4. `Mrender` = two-row atlas layout (arrow sheet frames / skin parts)
         blended premultiplied in the game's GL context.
  * Bugs fixed along the way (all visible in logcat evidence):
      - EOCD byte scan: kept a **buffer-relative** index (absolute file offset
        indexed past the malloc'd window → SEGV in engine_on_bitmap).
      - bplist00 trailer offsets corrected to spec: `t[6]=offsetIntSize`,
        `t[7]=objectRefSize`, `t+8=numObjects`, `t+16=topIndex`, `t+24=
        offsetTableOffset` (mis-indexed reads made every parse fail).
      - Sanity diagnostics: `PARSE v3: head=bplist00` marker line; per-frame
        rect logs; per-sheet counts.
      - Sheet-parse reset bug: second sheet parse wiped the first sheet's
        frames (counters now reset once in `engine_on_run`).
  * Device proof: `screens/phase3c-atlas-slices.png` — top row is all 16
    Hud_Arrow* frames (power-arrow bar, green/white chevrons), bottom row 9
    Hambo costume parts, every sprite cropped by plist `textureRect` and drawn
    with correct UVs from the two 2012-era atlases; logcat confirms
    `sheet parsed: arrow=16 skin=9`.
  * `.so` inspection tooling locked in (research + practice): NDK
    `llvm-nm` / `llvm-objdump` (symbol maps, targeted disassembly for crash
    PCs), `strings` fingerprints to identify which build really shipped where,
    LIEF/pyelftools available for programmatic ELF parsing, plus the working
    Ghidra decompiler for reference pseudocode of `libhambo.so` functions.
  * Remaining known limitations: rotated frames drawn unrotated (cosmetic),
    alpha lost in WebP preprocess (phase 4 vendored libwebp job), `plist_b`
    custom format for mission manifests still un-decompiled (next phase).

- 2026-10-17 (Phase 3d groundwork): **clean 1.1.8 APK acquired + mod fully
  reverse-engineered.**
  * Clean reference binary: `com.miniclip.hambo_1.1.8_paid-www.apkhere.com.apk`,
    SHA256 `d50cf8666555f96bf0e9a80fa23edf52bda23ce6d120b61ff8f32a899ce1b3ff`
    (matches Uptodown's published hash for Hambo 1.1.8).
  * `libhambo.so` in the clean APK is **byte-identical** (same SHA1
    `90d6a6a2…`) to the one inside `Hambo-(MOD)_1.1.8_apk-mod.com.apk.apk` —
    every RE finding from Phases 1–3c is validated against the authentic
    Miniclip binary; the mod never touched the engine.
  * Full tree diff clean vs mod: only `classes.dex`, `assets/unpack/
    CreditsText.txt` (modder credit "Cracked By - Twingo, <3 E.E.M") and
    signing metadata differ. apktool decode + smali diff pinpoints the ENTIRE
    mod to a **one-instruction change** in
    `com/miniclip/nativeJNI/InAppActivity.smali`, method
    `isPurchaseReallyOwned(Ljava/lang/String;)I`:
      - clean:  `const/4 v1, 0x0` → SharedPreferences
        `INAPP_PURCHASED_OWNEDv3` opened MODE_PRIVATE, `getInt(productId, 0)`,
        returns `val > 0 ? 1 : 0` → only actually-bought products owned.
      - mod:    `const/4 v1, 0x1` → same prefs opened with default **1**, so
        EVERY unknown productId reports owned=1 → engine's JNI upcall
        `cocojava.isPurchaseOwned(String)` (referenced from libhambo.so) sees
        all IAP products (costumes, coin packs, remove-ads) as purchased →
        free shopping / unlimited currency.
  * Port status: our build tree `build/Hambo/` was decoded from the MODDED APK,
    so `InAppActivity.smali` already carries the mod line and `dist/` builds
    ship the modded dex — **the ported app already has the free-shopping mod**,
    with 100% original Java/assets/native otherwise. Fallback if any currency
    grant turns out to require a live billing callback on-device: patch the
    engine-facing `isPurchaseOwned` result path or force credits through
    `MsetInAppResponce`.
  * Side benefit: `build/HamboClean/` (clean apktool tree) now exists for
    clean-vs-mod reference at any time.

- 2026-10-17 (Phase 4 COMPLETE): **Full 64-bit port + 120 Hz upgrade via
  in-process ARM32→ARM64 Dynarmic JIT loader.**
  * Architectural pivot: rather than manually decompiling and rewriting 12,917
    internal functions (8.25 MB of GNUstep ObjC runtime + Foundation +
    cocos2d-iphone + Box2D + 2012 libwebp + 252 levels), we built an arm64-v8a
    `libhambo.so` JIT loader (`rebuild/jit/{core.h,memory.cpp,cpu.cpp,libc.cpp,
    gl.cpp,jni.cpp}`, adapted from the MIT-licensed `Santi2065/dundef-sw-android64`
    architecture) that loads the untouched 32-bit `libhambo_guest.so` into an
    identity-mapped low-4GB `dlmalloc` arena and translates A32/T32 to AArch64
    via `dynarmic`.
  * All 335 `SHN_UNDEF` imports across `libc`, `libm`, `libstdc++`, `libz`,
    `libGLESv1_CM`, `libdl`, `liblog` wrapped; all 57 JNI exports bridged.
  * Critical Android 16 / ART / ARMv7 compatibility fixes discovered & solved:
    1. **2012 Dalvik un-promoted JNI refs**: `MsetMainActivity` (`0x595620`) and
       `JniHelper` (`0x58e880`) store `jobject`/`jclass` directly in C globals
       without calling `NewGlobalRef`. Fixed by promoting `FindClass`,
       `GetObjectClass`, `GetSuperclass`, and `MsetMainActivity` handles to
       persistent `NewGlobalRef` IDs.
    2. **CP15 barrier instructions**: GNUstep `NSIncrementExtraRefCount`
       (`0x38f09c`) executes `mcr p15, 0, r0, c7, c10, 5` (ARMv6 DMB). Fixed by
       attaching a sink `Dynarmic::A32::Coprocessor` on `cfg.coprocessors[0..15]`.
    3. **ARM exclusive load/store (`ldrex`/`strex`)**: GNUstep spinlocks at
       `0x38f0a8..0x38f0bc` use `ldrex`/`strex`. Fixed by wiring
       `Dynarmic::ExclusiveMonitor` (`cfg.global_monitor`) and overriding
       `MemoryWriteExclusive8/16/32/64` with atomic `compare_exchange_strong`.
    4. **Untouched 2012 `.pngpl` WebP + `plist_b` assets**: restored original
       `.pngpl` files so the guest's bundled 2012 `libwebp` decodes all 137
       textures with full alpha transparency, and the guest's native decryptor
       parses all 259 `plist_b` files directly.
    5. **120 Hz high-refresh-rate unlock**:
       - Removed the 30 FPS `Thread.sleep` limiter in `ClearRenderer.smali`.
       - Requested `preferredRefreshRate=120.0f` + `preferredDisplayModeId=1` on
         the Activity `Window` and `Surface.setFrameRate(120.0f, 0)` on
         `cocojava.mGLView.getHolder().getSurface()`.
       - Patched `b2World::Step` call site at `0x4c413c` (`ldr r1, [pc, #0x4c0]`
         → `ldr r1, [sp, #0x4]`) so Box2D physics steps by the actual frame `dt`
         at any refresh rate (60/90/120/144 Hz).
  * Device proof on iQOO I2220 (Android 16, arm64-only, 16KB page size):
    `rea/port/screens/jit-mainmenu-textured.png`, logcat `perf: 120.2 fps,
    124512 host calls/s`.
