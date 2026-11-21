#!/usr/bin/env python3
"""Run `cargo build --release` through a temporary proxy (Linux/macOS/Windows).

Usage: python build_with_proxy.py [extra cargo build args...]
Override the proxy with the BUILD_PROXY environment variable.
"""

import os
import subprocess
import sys
from pathlib import Path

PROXY = os.environ.get("BUILD_PROXY", "http://192.168.111.1:8088")
MANIFEST = Path(__file__).resolve().parent / "Cargo.toml"


def main() -> int:
    env = os.environ.copy()
    for name in ("HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy"):
        env[name] = PROXY
    cmd = [
        "cargo",
        "build",
        "--manifest-path",
        str(MANIFEST),
        "--release",
        *sys.argv[1:],
    ]
    print(f"Using proxy {PROXY}\n$ {' '.join(cmd)}", flush=True)
    try:
        return subprocess.call(cmd, env=env)
    except FileNotFoundError:
        print("cargo not found in PATH", file=sys.stderr)
        return 127


if __name__ == "__main__":
    sys.exit(main())
