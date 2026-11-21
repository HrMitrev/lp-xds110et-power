#!/usr/bin/env python3
"""
gen_compile_commands.py - Generate compile_commands.json from firmware_src/Makefile.

Runs a dry-run of `make -n -B V=1` to capture exact compiler invocations,
discovers the active arm-none-eabi-gcc system include paths, and writes a
Clang/clangd/YCM-compatible compile_commands.json in firmware_src/.
"""

import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

GCC_ONLY_FLAGS = {
    "-ffunction-sections",
    "-fdata-sections",
    "-Wno-old-style-declaration",
}


def query_gcc_system_includes(cc: str, mcu_flags: list[str]) -> list[str]:
    """Query the cross-compiler for its built-in <...> include search paths."""
    cmd = [cc, *mcu_flags, "-E", "-Wp,-v", "-x", "c", os.devnull, "-o", os.devnull]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    includes: list[str] = []
    in_search = False
    for line in proc.stderr.splitlines():
        if "#include <...> search starts here:" in line:
            in_search = True
            continue
        if in_search:
            if line.startswith("End of search list."):
                break
            path = os.path.normpath(line.strip())
            if os.path.isdir(path) and path not in includes:
                includes.append(path)
    return includes


def build_clang_arguments(tokens: list[str], sys_includes: list[str]) -> list[str]:
    """Convert an arm-none-eabi-gcc compile command into Clang/clangd arguments."""
    args = [
        tokens[0],
        "--target=arm-none-eabi",
        "-D__GNUC__",
    ]
    skip_next = False
    for tok in tokens[1:]:
        if skip_next:
            args.append(tok)
            skip_next = False
            continue
        if tok in GCC_ONLY_FLAGS:
            continue
        if tok in ("-I", "-isystem", "-D", "-o", "-c"):
            args.append(tok)
            if tok != "-c":
                skip_next = True
            continue
        args.append(tok)

    # Inject GCC toolchain system headers via -isystem before the source file
    src_idx = len(args) - 1
    sys_flags: list[str] = []
    for inc in sys_includes:
        sys_flags.extend(["-isystem", inc])
    return args[:src_idx] + sys_flags + args[src_idx:]


def generate(fw_dir: Path) -> list[dict]:
    """Dry-run make to collect all compilation commands and return JSON entries."""
    make_cmd = ["make", "-n", "-B", "V=1", "all"]
    proc = subprocess.run(make_cmd, cwd=fw_dir, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        raise SystemExit(proc.returncode)

    entries: list[dict] = []
    sys_includes_cache: dict[str, list[str]] = {}

    for raw_line in proc.stdout.splitlines():
        line = raw_line.strip()
        if " -c " not in line or "gcc" not in line:
            continue
        tokens = shlex.split(line)
        if not tokens or "gcc" not in Path(tokens[0]).name:
            continue

        # Locate source file after '-c'
        try:
            c_idx = tokens.index("-c")
            src_rel = tokens[c_idx + 1]
        except (ValueError, IndexError):
            continue

        cc = tokens[0]
        if cc not in sys_includes_cache:
            mcu_flags = [
                t for t in tokens[1:]
                if re.match(r"^-(mcpu|mthumb|mfpu|mfloat-abi)", t)
            ]
            sys_includes_cache[cc] = query_gcc_system_includes(cc, mcu_flags)

        clang_args = build_clang_arguments(tokens, sys_includes_cache[cc])
        entries.append({
            "directory": str(fw_dir),
            "file": str((fw_dir / src_rel).resolve()),
            "arguments": clang_args,
        })

    out_path = fw_dir / "compile_commands.json"
    out_path.write_text(json.dumps(entries, indent=2) + "\n")
    return entries


def main() -> None:
    fw_dir = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent
    entries = generate(fw_dir)
    print(f"Generated {fw_dir / 'compile_commands.json'} ({len(entries)} entries)")


if __name__ == "__main__":
    main()
