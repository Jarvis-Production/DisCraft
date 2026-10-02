#!/usr/bin/env python3
"""Packs a DisCraft release into dist/:

  DisCraft-<version>.zip          extract into the Dishonored folder (next to Binaries\\): the plugin,
                                  its ini, the ASI loader that loads it, and DisCraft-Minecraft.zip,
                                  the Minecraft it starts
  discraft-fabric-<version>.jar   the Minecraft mod on its own (for your own launcher)

DisCraft-Minecraft.zip holds a portable Prism Launcher with a ready "DisCraft" instance (Minecraft
26.3, Fabric, Fabric API, e4mc, DisCraft). The plugin unpacks it to %LOCALAPPDATA%\\DisCraft and starts
it; Prism asks the player to sign in once, then downloads Minecraft and Java itself.

Every third-party download is pinned to a version and checked against its hash.

    python tools/package.py [--build] [--jar PATH] [--asi PATH]

--build first builds both halves (Gradle for the Fabric mod; CMake for the plugin: MSVC on Windows,
the MinGW-w64 toolchain elsewhere).
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

PRISM_VERSION = "11.1.1"
PINNED = {
    # name: (url, algorithm, hash)
    f"PrismLauncher-Windows-MSVC-Portable-{PRISM_VERSION}.zip": (
        f"https://github.com/PrismLauncher/PrismLauncher/releases/download/{PRISM_VERSION}/PrismLauncher-Windows-MSVC-Portable-{PRISM_VERSION}.zip",
        "sha256", "ab35a770fb06d89d2ccc098079db5db329fb4e68f42b72babd8b095efde3d2d7"),
    f"PrismLauncher-{PRISM_VERSION}-LICENSE.txt": (
        f"https://raw.githubusercontent.com/PrismLauncher/PrismLauncher/{PRISM_VERSION}/LICENSE", None, None),
    "fabric-api-0.161.0+26.3.jar": (
        "https://cdn.modrinth.com/data/P7dR8mSH/versions/bNnaTiuM/fabric-api-0.161.0%2B26.3.jar",
        "sha512", "ed6b2586d6fde11fde8472f5a527c51e99b67026e46f94d4bfd85e7e28ce5ee299173ee16ad576ceb51f39f98d30a811086a6deb1a86a524859cc16e12da109d"),
    "e4mc-fabric-6.2.2-modern.jar": (
        "https://cdn.modrinth.com/data/qANg5Jrr/versions/AouleFRY/e4mc-fabric-6.2.2-modern.jar",
        "sha512", "01ef0a8c5b76e2cb0effd337bad3350d8807d100d0ec661e01b2ffb20af7b652f756c5eaa11bee233c37905bfd7b573f7a85f3d15bfd2833962c76f02cd59a86"),
    # Ultimate ASI Loader (32-bit, MIT): loads DisCraft.asi into Dishonored. Shipped as d3d9.dll.
    "Ultimate-ASI-Loader-v9.7.4.zip": (
        "https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/v9.7.4/Ultimate-ASI-Loader.zip",
        "sha256", "952cebfc30d525afc2bdbaca954329d405ded3aa688a83027354dae14dfd5c5f"),
    "Ultimate-ASI-Loader-v9.7.4-LICENSE.txt": (
        "https://raw.githubusercontent.com/ThirteenAG/Ultimate-ASI-Loader/v9.7.4/license", None, None),
}


def version():
    text = (ROOT / "fabric" / "gradle.properties").read_text(encoding="utf-8")
    return re.search(r"^version=(.+)$", text, re.M).group(1).strip()


def fetch(name, cache):
    url, algorithm, expected = PINNED[name]
    path = cache / name
    if not path.exists():
        cache.mkdir(parents=True, exist_ok=True)
        print(f"downloading {url}")
        with urllib.request.urlopen(url, timeout=120) as response, open(path, "wb") as out:
            shutil.copyfileobj(response, out)
    if algorithm:
        actual = hashlib.new(algorithm, path.read_bytes()).hexdigest()
        if actual != expected:
            path.unlink()
            sys.exit(f"{name} doesn't match its pinned {algorithm} hash ({actual})")
    return path


def zip_entries(path, entries):
    """entries: {name in zip (forward slashes): file on disk}"""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name in sorted(entries):
            z.write(entries[name], name)


def zip_folder(path, folder):
    entries = {}
    for file in sorted(Path(folder).rglob("*")):
        if file.is_file():
            entries[file.relative_to(folder).as_posix()] = file
    zip_entries(path, entries)


def build():
    gradlew = ROOT / "fabric" / ("gradlew.bat" if os.name == "nt" else "gradlew")
    subprocess.run([str(gradlew), "build", "--no-configuration-cache"], cwd=ROOT / "fabric", check=True)
    out = ROOT / "build" / "native"
    if os.name == "nt":
        subprocess.run(["cmake", "-S", "native", "-B", str(out), "-A", "Win32"], cwd=ROOT, check=True)
        subprocess.run(["cmake", "--build", str(out), "--config", "Release"], cwd=ROOT, check=True)
    else:
        subprocess.run(["cmake", "-S", "native", "-B", str(out), "-DCMAKE_BUILD_TYPE=Release",
                        f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'native' / 'cmake' / 'mingw-i686.cmake'}"], cwd=ROOT, check=True)
        subprocess.run(["cmake", "--build", str(out)], cwd=ROOT, check=True)


def find_asi():
    for candidate in (ROOT / "build" / "native" / "Release" / "DisCraft.asi", ROOT / "build" / "native" / "DisCraft.asi"):
        if candidate.exists():
            return candidate
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", action="store_true", help="build both halves first")
    parser.add_argument("--jar", type=Path, help="the Fabric mod jar (default: fabric/build/libs/discraft-<version>.jar)")
    parser.add_argument("--asi", type=Path, help="the plugin (default: build/native/[Release/]DisCraft.asi)")
    args = parser.parse_args()
    ver = version()
    if args.build:
        build()
    jar = args.jar or ROOT / "fabric" / "build" / "libs" / f"discraft-{ver}.jar"
    asi = args.asi or find_asi()
    for f, what in ((jar, "the Fabric mod jar"), (asi, "DisCraft.asi")):
        if not f or not Path(f).exists():
            sys.exit(f"missing {what} ({f}): build first, or pass --build")

    cache = ROOT / ".tools" / "downloads"
    files = {name: fetch(name, cache) for name in PINNED}

    dist = ROOT / "dist"
    if dist.exists():
        shutil.rmtree(dist)
    dist.mkdir()

    # The bundled Minecraft: Prism (portable), the DisCraft instance, its mods, Prism's defaults.
    bundle = dist / "bundle"
    shutil.copytree(ROOT / "tools" / "minecraft-bundle", bundle)
    with zipfile.ZipFile(files[f"PrismLauncher-Windows-MSVC-Portable-{PRISM_VERSION}.zip"]) as z:
        z.extractall(bundle / "Prism")
    shutil.copy(files[f"PrismLauncher-{PRISM_VERSION}-LICENSE.txt"], bundle / "Prism" / "LICENSE-PrismLauncher.txt")
    notice = bundle / "Prism" / "THIRD-PARTY.txt"
    notice.write_text(notice.read_text(encoding="utf-8").replace("{PRISM_VERSION}", PRISM_VERSION), encoding="utf-8")
    mods = bundle / "Prism" / "instances" / "DisCraft" / ".minecraft" / "mods"
    mods.mkdir(parents=True)
    shutil.copy(files["fabric-api-0.161.0+26.3.jar"], mods)
    shutil.copy(files["e4mc-fabric-6.2.2-modern.jar"], mods)
    shutil.copy(jar, mods / f"discraft-{ver}.jar")
    (bundle / "bundle-version.txt").write_text(
        f"DisCraft {ver}, Prism Launcher {PRISM_VERSION}, fabric-api-0.161.0+26.3.jar, e4mc-fabric-6.2.2-modern.jar", encoding="utf-8")
    minecraft_zip = dist / "DisCraft-Minecraft.zip"
    zip_folder(minecraft_zip, bundle)
    shutil.rmtree(bundle)

    loader = dist / "d3d9.dll"
    with zipfile.ZipFile(files["Ultimate-ASI-Loader-v9.7.4.zip"]) as z:
        loader.write_bytes(z.read("dinput8.dll"))

    win32 = "Binaries/Win32"
    zip_entries(dist / f"DisCraft-{ver}.zip", {
        f"{win32}/DisCraft.asi": asi,
        f"{win32}/DisCraft.ini": ROOT / "native" / "DisCraft.ini",
        f"{win32}/d3d9.dll": loader,
        f"{win32}/DisCraft/DisCraft-Minecraft.zip": minecraft_zip,
        f"{win32}/DisCraft/LICENSE.txt": ROOT / "LICENSE",
        f"{win32}/DisCraft/THIRD-PARTY-NOTICES.md": ROOT / "THIRD-PARTY-NOTICES.md",
        f"{win32}/DisCraft/LICENSE-UltimateASILoader.txt": files["Ultimate-ASI-Loader-v9.7.4-LICENSE.txt"],
    })
    shutil.copy(jar, dist / f"discraft-fabric-{ver}.jar")
    minecraft_zip.unlink()
    loader.unlink()
    for f in sorted(dist.iterdir()):
        print(f"{f.name:40} {f.stat().st_size:>12,} bytes")


if __name__ == "__main__":
    main()
