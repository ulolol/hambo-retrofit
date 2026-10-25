#!/usr/bin/env python3
"""Rebuilds Hambo 1.1.8 (com.miniclip.hambo, 2012 32-bit armeabi) into a native 64-bit, 120Hz APK
for modern Android (Android 7 .. 16+), including 64-bit-only ARMv9 SoCs (Snapdragon 8 Gen 3 / 8 Elite,
Dimensity 9300/9400, Tensor G3/G4) with 16 KB page-size alignment.

Usage:
    python3 build.py path/to/Hambo_1.1.8.apk                  # builds Hambo-1.1.8-arm64-120hz.apk
    python3 build.py path/to/Hambo_1.1.8.apk --free-shopping  # also enables the free-shopping / all-owned mod
    python3 build.py path/to/Hambo_1.1.8.apk --from-source    # compiles the 64-bit Dynarmic JIT loader from source
"""
import argparse
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
WORK = ROOT / "build"
TOOLS = ROOT / "tools"
OUT = ROOT / "Hambo-1.1.8-arm64-120hz.apk"

CLEAN_SHA256 = "d50cf8666555f96bf0e9a80fa23edf52bda23ce6d120b61ff8f32a899ce1b3ff"
MOD_SHA256 = "b6528b8fc17915c3192f6e6db45bc9344d3fc4022fec62c0fc5f05b2d3c4bde0"

APKTOOL = (
    "https://github.com/iBotPeaches/Apktool/releases/download/v2.12.1/apktool_2.12.1.jar",
    "66cf4524a4a45a7f56567d08b2c9b6ec237bcdd78cee69fd4a59c8a0243aeafa",
)
SIGNER = (
    "https://github.com/patrickfav/uber-apk-signer/releases/download/v1.3.0/uber-apk-signer-1.3.0.jar",
    "e1299fd6fcf4da527dd53735b56127e8ea922a321128123b9c32d619bba1d835",
)

KEYSTORE = ROOT / "retro.keystore"
# Local-only self-signed signing key. `retro.keystore` is git-ignored and never
# published; override the alias/password with environment variables if you want
# your own values (`HAMBO_KS_ALIAS`, `HAMBO_KS_PASS`).
KS_ALIAS = os.environ.get("HAMBO_KS_ALIAS", "retro")
KS_PASS = os.environ.get("HAMBO_KS_PASS", "retro123")


def die(msg: str):
    sys.exit("error: " + msg)


def run(*cmd, **kw):
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def download(url: str, dest: Path, digest: str = None) -> Path:
    if dest.exists() and (digest is None or sha256(dest) == digest):
        return dest
    print(f"Downloading {url} ...")
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".part")
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "hambo-retrofit"})
        with urllib.request.urlopen(req) as r, open(tmp, "wb") as f:
            shutil.copyfileobj(r, f)
    except OSError:
        run("curl", "-fL", "-o", tmp, url)
    if digest and sha256(tmp) != digest:
        tmp.unlink()
        die(f"checksum mismatch for {url}")
    tmp.replace(dest)
    return dest


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def write_text(path: Path, text: str):
    path.write_text(text, encoding="utf-8", newline="\n")


def patch_apk_sources(src: Path, free_shopping: bool):
    # 1. Android 14-16+ installability & native lib extraction
    yml = src / "apktool.yml"
    if yml.exists():
        t = read_text(yml)
        t = re.sub(r"targetSdkVersion:\s*'\d+'|targetSdkVersion:\s*\d+", "targetSdkVersion: '24'", t)
        write_text(yml, t)

    manifest = src / "AndroidManifest.xml"
    mt = read_text(manifest)
    if 'android:extractNativeLibs="true"' not in mt:
        mt = mt.replace("<application ", '<application android:extractNativeLibs="true" ', 1)
    write_text(manifest, mt)

    # 2. Unlock 120Hz in ClearRenderer.smali (remove 30 FPS Thread.sleep limiter)
    cr = src / "smali" / "com" / "miniclip" / "nativeJNI" / "ClearRenderer.smali"
    crt = read_text(cr)
    crt = crt.replace("const-wide/32 v0, 0x1fca055", "const-wide/16 v0, 0x0")
    crt = re.sub(
        r"(:cond_8\n)\s*(sget-wide v14, Lcom/miniclip/nativeJNI/ClearRenderer;->animationInterval:J)",
        r"\1    goto :cond_9\n\n    \2",
        crt,
        count=1,
    )
    write_text(cr, crt)

    # 3. Optional free-shopping / all-IAP-owned mod in InAppActivity.smali
    if free_shopping:
        inapp = src / "smali" / "com" / "miniclip" / "nativeJNI" / "InAppActivity.smali"
        iat = read_text(inapp)
        iat = re.sub(
            r"(\.method public isPurchaseReallyOwned\(Ljava/lang/String;\)I.*?\.prologue\n\s*)const/4 v1, 0x0",
            r"\1const/4 v1, 0x1",
            iat,
            count=1,
            flags=re.S,
        )
        write_text(inapp, iat)


def build_loader_from_source() -> Path:
    ndk = os.environ.get("NDK") or os.environ.get("ANDROID_NDK_HOME")
    if not ndk:
        cands = sorted(glob.glob(str(Path.home() / "Library/Android/sdk/ndk/*")) +
                       glob.glob(str(Path.home() / "Android/Sdk/ndk/*")) +
                       glob.glob(str(Path.home() / "android-ndk-cache/*")))
        if not cands:
            die("Android NDK not found; set NDK=/path/to/ndk or omit --from-source to use prebuilt/arm64-v8a/libhambo.so")
        ndk = cands[-1]
    ndk_path = Path(ndk)
    dyn_dir = ROOT / "port" / "third_party" / "dynarmic"
    if not (dyn_dir / "CMakeLists.txt").exists():
        run("git", "clone", "--depth", "1", "https://github.com/azahar-emu/dynarmic.git", dyn_dir)
        run("git", "-C", dyn_dir, "submodule", "update", "--init", "--depth", "1",
            "externals/fmt", "externals/mcl", "externals/oaknut", "externals/robin-map",
            "externals/xbyak", "externals/zycore", "externals/zydis")
    bdir = ROOT / "port" / "build-arm64-v8a"
    run("cmake", "-S", ROOT / "port", "-B", bdir, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_TOOLCHAIN_FILE={ndk_path / 'build/cmake/android.toolchain.cmake'}",
        "-DANDROID_ABI=arm64-v8a",
        "-DANDROID_PLATFORM=android-24",
        "-DANDROID_STL=c++_static",
        "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH",
        "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH")
    run("ninja", "-C", bdir, "hambo")
    strip = glob.glob(str(ndk_path / "toolchains/llvm/prebuilt/*/bin/llvm-strip*"))[0]
    out = bdir / "libhambo.stripped.so"
    run(strip, "-o", out, bdir / "libhambo.so")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("apk", type=Path, help="path to Hambo 1.1.8 APK (clean or modded)")
    ap.add_argument("--free-shopping", action="store_true", default=True,
                    help="enable free shopping / all items owned mod (default: enabled)")
    ap.add_argument("--no-free-shopping", dest="free_shopping", action="store_false",
                    help="preserve original unmodded purchase checks")
    ap.add_argument("--from-source", action="store_true",
                    help="compile the 64-bit Dynarmic JIT loader from source instead of using prebuilt/")
    args = ap.parse_args()

    if not args.apk.is_file():
        die(f"APK not found: {args.apk}")

    digest = sha256(args.apk)
    if digest == CLEAN_SHA256:
        print("Detected clean Hambo 1.1.8 APK (SHA-256 verified)")
    elif digest == MOD_SHA256:
        print("Detected modded Hambo 1.1.8 APK (SHA-256 verified)")
    else:
        print(f"Warning: unknown APK SHA-256 ({digest}), proceeding anyway...")

    loader = build_loader_from_source() if args.from_source else (ROOT / "prebuilt" / "arm64-v8a" / "libhambo.so")
    if not loader.is_file():
        die(f"Loader not found at {loader}")

    apktool_cmd = shutil.which("apktool")
    signer_jar = download(SIGNER[0], TOOLS / "uber-apk-signer-1.3.0.jar", SIGNER[1])

    shutil.rmtree(WORK, ignore_errors=True)
    src = WORK / "src"
    if apktool_cmd:
        run(apktool_cmd, "d", "-f", "-o", src, args.apk.resolve())
    else:
        apktool_jar = download(APKTOOL[0], TOOLS / "apktool_2.12.1.jar", APKTOOL[1])
        run("java", "-jar", apktool_jar, "d", "-f", "-o", src, args.apk.resolve())

    patch_apk_sources(src, args.free_shopping)

    guest_so = src / "lib" / "armeabi" / "libhambo.so"
    if not guest_so.is_file():
        die("lib/armeabi/libhambo.so not found in input APK")

    arm64_dir = src / "lib" / "arm64-v8a"
    arm64_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(loader, arm64_dir / "libhambo.so")
    shutil.copyfile(guest_so, arm64_dir / "libhambo_guest.so")
    shutil.rmtree(src / "lib" / "armeabi", ignore_errors=True)

    unsigned = WORK / "hambo-arm64-unsigned.apk"
    if apktool_cmd:
        run(apktool_cmd, "b", src, "-o", unsigned)
    else:
        run("java", "-jar", TOOLS / "apktool_2.12.1.jar", "b", src, "-o", unsigned)

    if not KEYSTORE.exists():
        run("keytool", "-genkeypair", "-keystore", KEYSTORE, "-storepass", KS_PASS,
            "-keypass", KS_PASS, "-alias", KS_ALIAS, "-keyalg", "RSA", "-keysize", "2048",
            "-validity", "36500", "-dname", "CN=Hambo Retrofit, O=Retrofit")

    signed_dir = WORK / "signed"
    run("java", "-jar", signer_jar, "--apks", unsigned, "--ks", KEYSTORE,
        "--ksAlias", KS_ALIAS, "--ksPass", KS_PASS, "--ksKeyPass", KS_PASS, "--out", signed_dir)
    shutil.copyfile(next(signed_dir.glob("*-signed.apk")), OUT)
    print(f"\nSuccess! Built 64-bit 120Hz APK -> {OUT}")


if __name__ == "__main__":
    main()
