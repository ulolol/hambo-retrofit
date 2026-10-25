
## Corrections from Ghidra symbol reconciliation (2026-10-17)

- `CocoJNI.smali` declares **55** native methods (not 56): dynsym exports only **52**
  `Java_com_miniclip_nativeJNI_CocoJNI_M*` symbols. Five declared-but-unexported:
  `MdatePickerClosed`, `MfacebookShareCanceled`, `MkeyboardInputClosed`,
  `MnetworkTimeResponce`, `MsimplePingResponce` — these may be `RegisterNatives`
 -registered in `JNI_OnLoad` (must be confirmed by decompiling `JNI_OnLoad`), or are
  MOD-tamper scars (possible, given the source file provenance) where the mods
  neutralized ad/facebook/ping paths and dlsym would fail on first call.
- Two exported-but-not-declared: `McheckpointResponse`, `Mend` — engine-driven
  (C→Java), fine for a rebuild but they belong to the contract list.
- `ReceiptValidator.MvalidateResponse` has **no dynsym entry** → the in-app receipt
  path here is dead or RegisterNatives-only; confirm in JNI_OnLoad decompilation.
- Authoritative pre-rebuild checklist: **55 declared + `JNI_OnLoad`**; decompiling
  `JNI_OnLoad` (done — `jni_onload.json`) shows **no `RegisterNatives` call**: the five
  declared-but-unexported natives (`MdatePickerClosed`, `MfacebookShareCanceled`,
  `MkeyboardInputClosed`, `MnetworkTimeResponce`, `MsimplePingResponce`) are dead in this
  MOD variant, and `McheckpointResponse`/`Mend` are dynamically reachable only from other
  Java classes (verify in smali when building the call map).
- Related hard counts (Ghidra, auto-analysis complete):
  * functions: **12,916** (~1.28 MB of instruction bytes), `rea/port/funcdump.tsv`
  * dynsym: 17,353 defined / 335 imported; 6,890 `__objc*` metadata symbols
  * statically linked: GNUstep-style ObjC runtime + Foundation, GNU STL (`_ZSt*`),
    Box2D 2.x (425 `b2*` functions), FreeType (TrueType interpreter `TT_RunIns`),
    VP8/WebP image decoder (`VP8DecodeMB`), PNG, libc-embedded helpers.
- Engine glue (Java smali, com.miniclip.nativeJNI.*): GLSurfaceView fork, binders, audio
  (cocosound), PNG/text (.pngpl level assets) staged from assets/.
