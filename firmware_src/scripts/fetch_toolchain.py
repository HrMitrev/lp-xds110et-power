#!/usr/bin/env python3
"""
fetch_toolchain.py - Download and install standalone GCC v16 for ARM Embedded (Windows).

Fetches the latest GCC 16 arm-none-eabi toolchain from MSYS2 UCRT64 binary repositories,
resolving all dependencies (binutils, newlib, and host runtime DLLs: gmp, mpfr, mpc,
isl, zstd, zlib, libwinpthread) and extracting them into a standalone directory.

No existing MSYS2 or pacman installation is required.
"""

import argparse
import io
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

# Ensure zstandard is available for .tar.zst decompression
try:
    import zstandard
except ImportError:
    print("Installing required 'zstandard' Python package...")
    subprocess.check_call(
        [sys.executable, "-m", "pip", "install", "zstandard"]
    )
    import zstandard

REPO_BASE = "https://repo.msys2.org/mingw"
MIRROR_BASE = "https://mirror.msys2.org/mingw"
ROOT_PACKAGE = "mingw-w64-ucrt-x86_64-arm-none-eabi-gcc"
DEFAULT_ENV = "ucrt64"


def fetch_bytes(url: str, desc: str = "") -> bytes:
    if desc:
        print(f"  Fetching {desc}...")
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return resp.read()
    except Exception as e:
        # Try mirror fallback if primary repo fails
        if REPO_BASE in url:
            fallback_url = url.replace(REPO_BASE, MIRROR_BASE)
            print(f"    Primary failed ({e}), trying mirror: {fallback_url}")
            with urllib.request.urlopen(
                urllib.request.Request(
                    fallback_url, headers={"User-Agent": "Mozilla/5.0"}
                ),
                timeout=30,
            ) as resp:
                return resp.read()
        raise


def parse_db(db_data: bytes) -> dict:
    packages = {}
    with tarfile.open(fileobj=io.BytesIO(db_data), mode="r:*") as tf:
        for m in tf.getmembers():
            if not m.name.endswith("/desc"):
                continue

            desc = tf.extractfile(m).read().decode("utf-8")
            lines = [l.strip() for l in desc.splitlines()]
            pkg = {}
            key = None
            for line in lines:
                if line.startswith("%") and line.endswith("%"):
                    key = line[1:-1]
                    pkg[key] = []
                elif line and key:
                    pkg[key].append(line)

            if "NAME" in pkg and "FILENAME" in pkg:
                name = pkg["NAME"][0]
                packages[name] = {
                    "filename": pkg["FILENAME"][0],
                    "version": pkg.get("VERSION", ["unknown"])[0],
                    "csize": int(pkg.get("CSIZE", [0])[0]),
                    "isize": int(pkg.get("ISIZE", [0])[0]),
                    "depends": pkg.get("DEPENDS", []),
                }
    return packages


def resolve_dependencies(root_pkg: str, packages: dict) -> list[str]:
    to_install = []
    seen = set()
    queue = [root_pkg]

    while queue:
        curr = queue.pop(0)
        pkg_name = (
            curr.split(">=")[0]
            .split("<=")[0]
            .split("=")[0]
            .split("<")[0]
            .split(">")[0]
        )
        if pkg_name in seen:
            continue
        seen.add(pkg_name)

        if pkg_name not in packages:
            # Check if any package provides it
            found = False
            for p, info in packages.items():
                if p == pkg_name:
                    pkg_name = p
                    found = True
                    break
            if not found:
                continue

        to_install.append(pkg_name)
        for dep in packages[pkg_name]["depends"]:
            dep_name = (
                dep.split(">=")[0]
                .split("<=")[0]
                .split("=")[0]
                .split("<")[0]
                .split(">")[0]
            )
            if dep_name not in seen:
                queue.append(dep_name)

    return to_install


def extract_pkg(pkg_bytes: bytes, dest_dir: Path, env: str):
    dctx = zstandard.ZstdDecompressor()
    stream = dctx.stream_reader(io.BytesIO(pkg_bytes))

    prefix = f"{env}/"
    deferred_links = []

    with tarfile.open(fileobj=stream, mode="r|") as tf:
        for member in tf:
            # Ignore pacman metadata files
            if member.name in (
                ".PKGINFO",
                ".BUILDINFO",
                ".MTREE",
                ".INSTALL",
            ) or member.name.startswith("."):
                continue

            rel_name = member.name
            if rel_name.startswith(prefix):
                rel_name = rel_name[len(prefix) :]

            target_path = dest_dir / rel_name
            if member.isdir():
                target_path.mkdir(parents=True, exist_ok=True)
            elif member.isreg():
                target_path.parent.mkdir(parents=True, exist_ok=True)
                with target_path.open("wb") as out_f:
                    f = tf.extractfile(member)
                    if f:
                        shutil.copyfileobj(f, out_f)
            elif member.islnk() or member.issym():
                target_path.parent.mkdir(parents=True, exist_ok=True)
                deferred_links.append((target_path, member.linkname))

    for target_path, linkname in deferred_links:
        if linkname.startswith(prefix):
            src = dest_dir / linkname[len(prefix) :]
        else:
            src = (target_path.parent / linkname).resolve()

        if src.exists():
            try:
                if src.is_file():
                    shutil.copy2(src, target_path)
                elif src.is_dir():
                    shutil.copytree(src, target_path, dirs_exist_ok=True)
            except Exception as e:
                pass


def main():
    parser = argparse.ArgumentParser(
        description="Fetch standalone GCC 16 arm-none-eabi toolchain from MSYS2."
    )
    parser.add_argument(
        "--dest",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "toolchain",
        help="Destination directory for toolchain (default: firmware_src/toolchain)",
    )
    parser.add_argument(
        "--env",
        default=DEFAULT_ENV,
        choices=["ucrt64", "mingw64"],
        help="MSYS2 environment (default: ucrt64)",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Force re-download even if toolchain exists",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Display required packages without downloading",
    )

    args = parser.parse_args()
    dest = args.dest.resolve()
    gcc_exe = dest / "bin" / "arm-none-eabi-gcc.exe"
    ld_exe = dest / "bin" / "arm-none-eabi-ld.exe"
    newlib_header = dest / "arm-none-eabi" / "include" / "stdio.h"

    if (
        gcc_exe.exists()
        and ld_exe.exists()
        and newlib_header.exists()
        and not args.force
        and not args.dry_run
    ):
        print(f"Complete toolchain (GCC, Binutils, Newlib) already installed in: {dest}")
        try:
            ver = subprocess.check_output(
                [str(gcc_exe), "--version"], text=True
            ).splitlines()[0]
            print(f"Found: {ver}")
            return
        except Exception:
            pass

    print(f"Fetching MSYS2 repository database for {args.env}...")
    db_url = f"{REPO_BASE}/{args.env}/{args.env}.db"
    db_data = fetch_bytes(db_url, f"{args.env}.db")

    packages = parse_db(db_data)
    to_install = resolve_dependencies(ROOT_PACKAGE, packages)

    total_csize = sum(packages[p]["csize"] for p in to_install)
    total_isize = sum(packages[p]["isize"] for p in to_install)

    print("\n--- Toolchain Packages to Install ---")
    for p in sorted(to_install):
        info = packages[p]
        print(f"  {p:<45} {info['version']:<16} ({info['csize'] / 1024 / 1024:.2f} MB)")
    print(
        f"\nTotal download: {total_csize / 1024 / 1024:.2f} MB  |  Unpacked size: {total_isize / 1024 / 1024:.2f} MB"
    )

    if args.dry_run:
        return

    dest.mkdir(parents=True, exist_ok=True)

    print(f"\nInstalling packages to: {dest}")
    for i, p in enumerate(to_install, 1):
        filename = packages[p]["filename"]
        pkg_url = f"{REPO_BASE}/{args.env}/{filename}"
        print(f"[{i:02d}/{len(to_install):02d}] Downloading {filename}...")
        pkg_bytes = fetch_bytes(pkg_url)

        print(f"          Extracting {filename}...")
        extract_pkg(pkg_bytes, dest, args.env)

    print("\nVerifying toolchain installation...")
    # Copy host runtime DLLs to internal GCC and binutils directories so they run standalone
    bin_dir = dest / "bin"
    dlls = list(bin_dir.glob("*.dll"))
    internal_dirs = [
        dest / "arm-none-eabi" / "bin",
    ]
    # Locate lib/gcc/arm-none-eabi/<version>
    gcc_lib = dest / "lib" / "gcc" / "arm-none-eabi"
    if gcc_lib.exists():
        for ver_dir in gcc_lib.iterdir():
            if ver_dir.is_dir():
                internal_dirs.append(ver_dir)

    for target_dir in internal_dirs:
        if target_dir.exists():
            for dll in dlls:
                target_dll = target_dir / dll.name
                if not target_dll.exists():
                    shutil.copy2(dll, target_dll)

    # Patch MSYS2 nano.specs files to fix '=include/newlib-nano' missing sysroot bug with -Wmissing-include-dirs
    for specs_file in dest.rglob("nano.specs"):
        try:
            content = specs_file.read_text(encoding="latin1")
            if "-isystem =/include/newlib-nano" in content:
                specs_file.write_text(
                    content.replace("-isystem =/include/newlib-nano", ""),
                    encoding="latin1",
                )
        except Exception:
            pass

    all_ok = True
    if gcc_exe.exists():
        res = subprocess.run(
            [str(gcc_exe), "--version"], capture_output=True, text=True
        )
        print("  [OK] GCC:      ", res.stdout.splitlines()[0])
    else:
        print("  [FAIL] GCC:     arm-none-eabi-gcc.exe not found")
        all_ok = False

    if ld_exe.exists():
        res = subprocess.run(
            [str(ld_exe), "--version"], capture_output=True, text=True
        )
        print("  [OK] Binutils: ", res.stdout.splitlines()[0])
    else:
        print("  [FAIL] Binutils: arm-none-eabi-ld.exe not found")
        all_ok = False

    if newlib_header.exists():
        print(
            f"  [OK] Newlib:    C standard library headers found at {newlib_header.parent}"
        )
    else:
        print(f"  [FAIL] Newlib:   {newlib_header} not found")
        all_ok = False

    if all_ok:
        print(f"\nComplete toolchain ready at: {dest / 'bin'}")
    else:
        print("\nError: Toolchain verification failed.")
        sys.exit(1)


if __name__ == "__main__":
    main()
