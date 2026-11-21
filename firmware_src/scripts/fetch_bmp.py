#!/usr/bin/env python3
"""
Fetch Black Magic Debug (BMP) firmware sources from the `pr/cc23xx` branch.

Clones (or updates) https://codeberg.org/hrm/blackmagic.git tracking
the `pr/cc23xx` branch into firmware_src/third_party/blackmagic/.
"""

import argparse
import os
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

BMP_REPO_URL = "https://codeberg.org/hrm/blackmagic.git"
BMP_DEFAULT_BRANCH = "pr/cc23xx"


def is_bmp_present(dest_dir: Path) -> bool:
    """Return True if expected Black Magic Debug core sources exist in dest_dir."""
    return (
        (dest_dir / "src" / "gdb_main.c").is_file()
        and (dest_dir / "src" / "target" / "cortexm.c").is_file()
        and (dest_dir / "src" / "include" / "general.h").is_file()
    )


def _clear_readonly_and_retry(func, path, _exc) -> None:
    """rmtree error handler: clear the read-only flag (e.g. git pack files on Windows) and retry."""
    os.chmod(path, stat.S_IWRITE)
    func(path)


def _rmtree(path: Path, ignore_errors: bool = False) -> None:
    """Remove a directory tree, including read-only files."""
    try:
        if sys.version_info >= (3, 12):
            shutil.rmtree(path, onexc=_clear_readonly_and_retry)
        else:
            shutil.rmtree(path, onerror=_clear_readonly_and_retry)
    except OSError:
        if not ignore_errors:
            raise


def _run_git(args: list[str], cwd: Path | None = None) -> str:
    cmd = ["git", *args]
    res = subprocess.run(cmd, cwd=cwd, check=True, text=True, capture_output=True)
    return res.stdout.strip()


def fetch_bmp(
    dest_dir: Path,
    repo_url: str = BMP_REPO_URL,
    branch: str = BMP_DEFAULT_BRANCH,
    ref: str | None = None,
    depth: int = 1,
    update: bool = False,
    force: bool = False,
    recurse_submodules: bool = False,
) -> None:
    if not force and is_bmp_present(dest_dir):
        if not update:
            rev = ""
            if (dest_dir / ".git").exists():
                try:
                    rev = (
                        f" ({_run_git(['rev-parse', '--short', 'HEAD'], cwd=dest_dir)})"
                    )
                except subprocess.CalledProcessError:
                    pass
            print(
                f"Black Magic Debug sources already exist in {dest_dir}{rev}. Skipping download."
            )
            return

        if not (dest_dir / ".git").exists():
            raise RuntimeError(
                f"{dest_dir} exists but is not a git checkout; re-run with --force to replace it."
            )

        print(f"Updating Black Magic Debug ({branch}) in {dest_dir}...")
        fetch_args = ["fetch", "origin", ref or branch]
        if depth > 0:
            fetch_args.insert(1, f"--depth={depth}")
        _run_git(fetch_args, cwd=dest_dir)
        _run_git(
            ["checkout", "FETCH_HEAD" if ref else f"origin/{branch}"], cwd=dest_dir
        )
        if recurse_submodules:
            _run_git(["submodule", "update", "--init", "--recursive"], cwd=dest_dir)
        rev = _run_git(["rev-parse", "--short", "HEAD"], cwd=dest_dir)
        print(f"Black Magic Debug updated to {rev} in {dest_dir}.")
        return

    dest_dir.parent.mkdir(parents=True, exist_ok=True)
    staging_parent = Path(tempfile.mkdtemp(prefix=".bmp_staging_", dir=dest_dir.parent))
    staging_dir = staging_parent / "blackmagic"
    try:
        print(f"Cloning Black Magic Debug ({repo_url}, branch '{branch}')...")
        clone_args = [
            "clone",
            "--branch",
            branch,
            "--single-branch",
        ]
        if depth > 0 and not ref:
            clone_args.extend(["--depth", str(depth)])
        if recurse_submodules:
            clone_args.append("--recurse-submodules")
        clone_args.extend([repo_url, str(staging_dir)])

        subprocess.run(["git", *clone_args], check=True)

        if ref:
            print(f"Checking out requested ref '{ref}'...")
            _run_git(["checkout", ref], cwd=staging_dir)

        if not is_bmp_present(staging_dir):
            raise RuntimeError(
                f"Cloned repository from {repo_url} (branch '{branch}') is missing expected BMP source files"
            )

        rev = _run_git(["rev-parse", "--short", "HEAD"], cwd=staging_dir)
        if dest_dir.exists():
            _rmtree(dest_dir)
        staging_dir.rename(dest_dir)
        print(
            f"Black Magic Debug ({branch} @ {rev}) successfully fetched to {dest_dir}."
        )
    finally:
        _rmtree(staging_parent, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Fetch Black Magic Debug (pr/cc23xx branch) sources into firmware_src/third_party/blackmagic."
    )
    parser.add_argument(
        "--dest",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "third_party" / "blackmagic",
        help="Destination directory (default: firmware_src/third_party/blackmagic)",
    )
    parser.add_argument(
        "--repo",
        default=BMP_REPO_URL,
        help=f"Git repository URL (default: {BMP_REPO_URL})",
    )
    parser.add_argument(
        "--branch",
        default=BMP_DEFAULT_BRANCH,
        help=f"Git branch to fetch (default: {BMP_DEFAULT_BRANCH})",
    )
    parser.add_argument(
        "--ref",
        default=None,
        help="Optional commit SHA or tag to check out after cloning/fetching",
    )
    parser.add_argument(
        "--depth",
        type=int,
        default=1,
        help="Shallow clone depth (0 for full history; default: 1)",
    )
    parser.add_argument(
        "--update",
        action="store_true",
        help="Fetch and update an existing checkout in-place",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Delete and re-clone existing BMP sources",
    )
    parser.add_argument(
        "--recurse-submodules",
        action="store_true",
        help="Also initialize upstream BMP git submodules (not required for LP-XDS110ET)",
    )
    args = parser.parse_args()
    fetch_bmp(
        dest_dir=args.dest,
        repo_url=args.repo,
        branch=args.branch,
        ref=args.ref,
        depth=args.depth,
        update=args.update,
        force=args.force,
        recurse_submodules=args.recurse_submodules,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
