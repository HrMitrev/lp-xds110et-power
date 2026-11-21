# .ycm_extra_conf.py - Dynamic YCM configuration backed by Makefile / compile_commands.json.

import json
import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
COMPILE_DB_PATH = os.path.join(SCRIPT_DIR, "compile_commands.json")
GEN_SCRIPT_PATH = os.path.join(SCRIPT_DIR, "scripts", "gen_compile_commands.py")

HEADER_EXTENSIONS = (".h", ".hpp", ".hh", ".hxx")

DROP_FLAGS_WITHOUT_ARG = {
    "-c",
    "-ffunction-sections",
    "-fdata-sections",
    "-Wno-old-style-declaration",
}

DROP_FLAGS_WITH_ARG = {
    "-o",
    "-MF",
    "-MT",
    "-MQ",
}


def ensure_compile_db():
    """Generate compile_commands.json from Makefile if missing or stale."""
    makefile_path = os.path.join(SCRIPT_DIR, "Makefile")
    needs_gen = not os.path.exists(COMPILE_DB_PATH)
    if not needs_gen and os.path.exists(makefile_path):
        needs_gen = os.path.getmtime(makefile_path) > os.path.getmtime(COMPILE_DB_PATH)
    if needs_gen and os.path.exists(GEN_SCRIPT_PATH):
        subprocess.run(
            [sys.executable, GEN_SCRIPT_PATH, SCRIPT_DIR],
            cwd=SCRIPT_DIR,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )


def load_compile_db():
    ensure_compile_db()
    if not os.path.exists(COMPILE_DB_PATH):
        return []
    try:
        with open(COMPILE_DB_PATH, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return []


def extract_ycm_flags(entry):
    """Convert a compile_commands.json entry into YCM/Clang flags."""
    raw_args = entry.get("arguments", [])
    src_file = os.path.realpath(entry.get("file", ""))
    flags = ["-x", "c"]

    idx = 1  # Skip compiler executable
    while idx < len(raw_args):
        tok = raw_args[idx]
        if tok in DROP_FLAGS_WITH_ARG:
            idx += 2
            continue
        if tok in DROP_FLAGS_WITHOUT_ARG:
            idx += 1
            continue
        # Drop the input source path itself
        candidate = tok if os.path.isabs(tok) else os.path.join(entry.get("directory", SCRIPT_DIR), tok)
        if os.path.realpath(candidate) == src_file:
            idx += 1
            continue
        flags.append(tok)
        idx += 1

    if "--target=arm-none-eabi" not in flags:
        flags.insert(0, "--target=arm-none-eabi")
    return flags


def find_entry_for_file(entries, filename):
    if not entries:
        return None

    target = os.path.realpath(filename) if filename else ""
    by_path = {os.path.realpath(e.get("file", "")): e for e in entries}

    # Exact .c match
    if target in by_path:
        return by_path[target]

    # Header file: try matching <stem>.c in the same directory
    root, ext = os.path.splitext(target)
    if ext.lower() in HEADER_EXTENSIONS:
        sibling_c = root + ".c"
        if sibling_c in by_path:
            return by_path[sibling_c]

    # Fallback: src/main.c (or first entry) carries all project includes & defines
    main_c = os.path.realpath(os.path.join(SCRIPT_DIR, "src", "main.c"))
    return by_path.get(main_c, entries[0])


def Settings(**kwargs):
    language = kwargs.get("language")
    if language is not None and language != "cfamily":
        return {}

    filename = kwargs.get("filename", "")
    entries = load_compile_db()
    entry = find_entry_for_file(entries, filename)
    if not entry:
        return {}

    return {
        "flags": extract_ycm_flags(entry),
        "include_paths_relative_to_dir": entry.get("directory", SCRIPT_DIR),
        "override_filename": filename,
    }
