#!/usr/bin/env python3
"""
Fetch TI SimpleLink MSP432E4 DriverLib and CMSIS header files.

Scrapes the latest package URL from the official TI SIMPLELINK-MSP432E4-SDK
download page and decompresses the required SDK files directly from the
BitRock InstallBuilder (CookFS v2) installer into firmware_src/sdk/.
"""

import argparse
import bz2
import hashlib
import lzma
import re
import shutil
import struct
import sys
import tempfile
import urllib.error
import urllib.request
import zlib
from collections import defaultdict
from pathlib import Path
from typing import BinaryIO, Callable

DOWNLOAD_PAGE_URL = "https://www.ti.com/tool/download/SIMPLELINK-MSP432E4-SDK/"
USER_AGENT = "Mozilla/5.0"
HTTP_TIMEOUT = 60

COOKFS_MAGIC = b"CFS0002"
COOKFS_INDEX_MAGIC = b"CFS2.200"

# Relative paths under the SDK's source/ directory to extract into sdk/
EXTRACT_PREFIXES = (
    "ti/devices/msp432e4/driverlib/",
    "ti/devices/msp432e4/inc/",
    "third_party/CMSIS/Include/",
)

BIGFILE_RE = re.compile(r"^(.*)___bitrockBigFile(\d+)$")
DOWNLOAD_URL_RE = re.compile(
    r'href=["\'](https://dr-download\.ti\.com/[^"\']*simplelink_msp432e4_sdk_[^"\']+\.(?:run|exe))["\']',
    re.IGNORECASE,
)
SHA256_COPY_RE = re.compile(r'value=["\']([0-9a-fA-F]{64})["\']')
VERSION_RE = re.compile(r"/(\d+\.\d+\.\d+\.\d+)/")


def _http_request(url: str, headers: dict[str, str] | None = None) -> urllib.request.Request:
    req_headers = {"User-Agent": USER_AGENT}
    if headers:
        req_headers.update(headers)
    return urllib.request.Request(url, headers=req_headers)


def _parse_version(url: str) -> tuple[tuple[int, ...], str | None]:
    m = VERSION_RE.search(url)
    if not m:
        return (0,), None
    ver_str = m.group(1)
    return tuple(int(part) for part in ver_str.split(".")), ver_str


def discover_latest_package(page_url: str = DOWNLOAD_PAGE_URL) -> tuple[str, str | None, str | None]:
    """Fetch the TI download page and return (download_url, version, sha256)."""
    print(f"Fetching latest SDK metadata from {page_url}...")
    with urllib.request.urlopen(_http_request(page_url), timeout=HTTP_TIMEOUT) as resp:
        html = resp.read().decode("utf-8", errors="replace")

    candidates: list[tuple[tuple[int, ...], int, str, str | None, str | None]] = []
    for match in DOWNLOAD_URL_RE.finditer(html):
        url = match.group(1)
        # Prefer the smaller .run installer over .exe; both embed CookFS v2
        ext_pref = 1 if url.lower().endswith(".run") else 0
        # Look for the SHA-256 checksum element immediately following this link
        snippet = html[match.end() : match.end() + 2048]
        sha_match = SHA256_COPY_RE.search(snippet)
        sha256 = sha_match.group(1).lower() if sha_match else None
        ver_tuple, ver_str = _parse_version(url)
        candidates.append((ver_tuple, ext_pref, url, ver_str, sha256))

    if not candidates:
        raise RuntimeError(
            f"Could not locate any SIMPLELINK-MSP432E4-SDK installer links on {page_url}"
        )

    candidates.sort(key=lambda item: (item[0], item[1]), reverse=True)
    _, _, best_url, version, best_sha256 = candidates[0]
    return best_url, version, best_sha256


def decompress_cookfs_page(buf: bytes) -> bytes:
    """Decompress a single CookFS v2 page (raw, deflate, bzip2, or LZMA-Alone)."""
    if not buf:
        return b""
    cid = buf[0]
    payload = buf[1:]
    if cid == 0:
        return payload
    if cid == 1:
        return zlib.decompress(payload, -15)
    if cid == 2:
        # CookFS prefixes bzip2 pages with a 4-byte uncompressed length
        return bz2.decompress(payload[4:])
    if cid == 255:
        return lzma.decompress(payload, format=lzma.FORMAT_ALONE)
    raise ValueError(f"Unsupported CookFS compression id: {cid}")


def verify_cookfs_page(page_data: bytes, expected_digest: bytes, page_idx: int) -> None:
    """Verify a decompressed CookFS page against its stored CRC32 or MD5 digest."""
    if expected_digest[:8] == b"\x00" * 8:
        actual_crc = struct.pack(
            ">IIII", 0, 0, len(page_data), zlib.crc32(page_data) & 0xFFFFFFFF
        )
        if actual_crc == expected_digest:
            return
    if hashlib.md5(page_data).digest() == expected_digest:
        return
    raise ValueError(f"CookFS page {page_idx} integrity check failed")


def parse_cookfs_fsindex(fsindex: bytes) -> dict[str, list[tuple[int, int, int]]]:
    """Parse a CFS2.200 filesystem index into {path: [(page_idx, offset, size), ...]}."""
    if not fsindex.startswith(COOKFS_INDEX_MAGIC):
        raise ValueError(f"Unexpected CookFS fsindex magic: {fsindex[:8]!r}")

    off = len(COOKFS_INDEX_MAGIC)
    files: dict[str, list[tuple[int, int, int]]] = {}

    def _parse_dir(prefix: str) -> None:
        nonlocal off
        (num_items,) = struct.unpack_from(">I", fsindex, off)
        off += 4
        for _ in range(num_items):
            name_len = fsindex[off]
            off += 1
            name = fsindex[off : off + name_len].decode("utf-8")
            off += name_len + 1  # trailing NUL byte
            _mtime, num_blocks = struct.unpack_from(">Qi", fsindex, off)
            off += 12
            full_path = f"{prefix}/{name}" if prefix else name
            if num_blocks == -1:
                _parse_dir(full_path)
            else:
                bos = struct.unpack_from(f">{num_blocks * 3}I", fsindex, off)
                off += num_blocks * 12
                files[full_path] = [
                    (bos[i], bos[i + 1], bos[i + 2]) for i in range(0, len(bos), 3)
                ]

    _parse_dir("")
    return files


class _RangeSource:
    """Random-access byte source backed by HTTP Range requests or a local file."""

    def __init__(
        self,
        total_size: int,
        reader: Callable[[int, int], bytes],
    ) -> None:
        self.total_size = total_size
        self._reader = reader

    def read(self, offset: int, length: int) -> bytes:
        if offset < 0 or length < 0 or offset + length > self.total_size:
            raise ValueError(
                f"Read out of bounds: offset={offset}, length={length}, size={self.total_size}"
            )
        if length == 0:
            return b""
        data = self._reader(offset, length)
        if len(data) != length:
            raise IOError(f"Short read: expected {length} bytes, got {len(data)}")
        return data


def _open_http_range_source(url: str, expected_sha256: str | None) -> _RangeSource | None:
    """Probe HTTP Range support; returns a _RangeSource or None if unsupported."""
    req = _http_request(url, headers={"Range": "bytes=0-0"})
    try:
        resp = urllib.request.urlopen(req, timeout=HTTP_TIMEOUT)
    except urllib.error.HTTPError as e:
        if e.code in (405, 416, 501):
            return None
        raise
    with resp:
        if resp.status != 206:
            return None
        content_range = resp.headers.get("Content-Range", "")
        m = re.match(r"bytes\s+\d+-\d+/(\d+)", content_range)
        if not m:
            return None
        total_size = int(m.group(1))

        etag = resp.headers.get("ETag", "").strip('"').lower()
        if expected_sha256 and len(etag) == 64 and etag != expected_sha256:
            raise RuntimeError(
                f"ETag/SHA-256 mismatch for {url}: expected {expected_sha256}, got {etag}"
            )

    def _fetch_range(offset: int, length: int) -> bytes:
        end = offset + length - 1
        range_req = _http_request(url, headers={"Range": f"bytes={offset}-{end}"})
        with urllib.request.urlopen(range_req, timeout=HTTP_TIMEOUT) as r:
            if r.status != 206:
                raise IOError(f"Expected HTTP 206 for range request, got {r.status}")
            return r.read()

    return _RangeSource(total_size, _fetch_range)


def _download_full_installer(url: str, expected_sha256: str | None) -> BinaryIO:
    """Fallback full download when HTTP Range requests are unavailable."""
    print(f"Downloading full installer from {url}...")
    tmp = tempfile.TemporaryFile()
    hasher = hashlib.sha256()
    downloaded = 0

    with urllib.request.urlopen(_http_request(url), timeout=HTTP_TIMEOUT) as resp:
        total = int(resp.headers.get("Content-Length", 0))
        while True:
            chunk = resp.read(1024 * 1024)
            if not chunk:
                break
            tmp.write(chunk)
            hasher.update(chunk)
            downloaded += len(chunk)
            if total > 0:
                pct = downloaded * 100.0 / total
                print(
                    f"\r  Downloaded {downloaded / (1024 * 1024):.1f} / "
                    f"{total / (1024 * 1024):.1f} MiB ({pct:.1f}%)",
                    end="",
                    flush=True,
                )
    if downloaded:
        print()

    digest = hasher.hexdigest().lower()
    if expected_sha256 and digest != expected_sha256:
        tmp.close()
        raise RuntimeError(
            f"SHA-256 mismatch for downloaded package: expected {expected_sha256}, got {digest}"
        )

    tmp.seek(0)
    return tmp


def _locate_cookfs_tail(source: _RangeSource) -> tuple[int, int, bytes]:
    """Scan backwards from EOF to locate the CFS0002 archive trailer.

    Returns (cfs_end_offset, buffer_start_offset, buffer_bytes).
    """
    chunk_size = 5 * 1024 * 1024
    max_scan = min(source.total_size, 32 * 1024 * 1024)
    scanned = 0
    buf = b""
    buf_start = source.total_size

    while scanned < max_scan:
        read_len = min(chunk_size, source.total_size - scanned)
        new_start = source.total_size - scanned - read_len
        piece = source.read(new_start, read_len)
        buf = piece + buf
        buf_start = new_start
        scanned += read_len

        pos = buf.rfind(COOKFS_MAGIC)
        if pos != -1:
            cfs_end = buf_start + pos + len(COOKFS_MAGIC)
            return cfs_end, buf_start, buf

    raise RuntimeError("Could not locate CFS0002 CookFS archive trailer in installer")


def extract_sdk_from_cookfs(source: _RangeSource, dest_dir: Path) -> int:
    """Locate, decompress, verify, and extract SDK files from a CookFS v2 installer."""
    cfs_end, buf_start, tail_buf = _locate_cookfs_tail(source)

    def _read_cached(offset: int, length: int) -> bytes:
        if offset >= buf_start and offset + length <= buf_start + len(tail_buf):
            rel = offset - buf_start
            return tail_buf[rel : rel + length]
        return source.read(offset, length)

    trailer = _read_cached(cfs_end - 16, 16)
    if trailer[9:16] != COOKFS_MAGIC:
        raise RuntimeError("Invalid CookFS trailer signature")

    idx_size, num_pages = struct.unpack(">II", trailer[:8])
    meta_size = num_pages * 20 + idx_size
    idx_offset = cfs_end - 16 - meta_size
    if idx_offset < 0:
        raise RuntimeError("Corrupt CookFS index offset")

    meta = _read_cached(idx_offset, meta_size)
    hashes_raw = meta[: num_pages * 16]
    sizes_raw = meta[num_pages * 16 : num_pages * 20]
    idx_raw = meta[num_pages * 20 :]

    page_hashes = [hashes_raw[i * 16 : (i + 1) * 16] for i in range(num_pages)]
    page_sizes = struct.unpack(f">{num_pages}I", sizes_raw)
    start_offset = idx_offset - sum(page_sizes)
    if start_offset < 0:
        raise RuntimeError("Corrupt CookFS page table")

    page_ranges: list[tuple[int, int]] = []
    cur = start_offset
    for size in page_sizes:
        page_ranges.append((cur, size))
        cur += size

    fsindex_bytes = decompress_cookfs_page(idx_raw)
    all_files = parse_cookfs_fsindex(fsindex_bytes)

    # Group BitRock split big-file chunks (___bitrockBigFileN) with their base file
    grouped: dict[str, list[tuple[int, list[tuple[int, int, int]]]]] = defaultdict(list)
    for archive_path, triples in all_files.items():
        m = BIGFILE_RE.match(archive_path)
        if m:
            grouped[m.group(1)].append((int(m.group(2)), triples))
        else:
            grouped[archive_path].append((0, triples))

    # Select only the required SDK files under */source/<prefix>
    selected: dict[str, list[tuple[int, list[tuple[int, int, int]]]]] = {}
    for archive_path, chunks in grouped.items():
        marker = "/source/"
        idx = archive_path.find(marker)
        if idx == -1:
            continue
        rel_path = archive_path[idx + len(marker) :]
        if rel_path.startswith(EXTRACT_PREFIXES):
            selected[rel_path] = sorted(chunks, key=lambda item: item[0])

    if not selected:
        raise RuntimeError("No MSP432E4 SDK files found in installer CookFS archive")

    needed_pages = sorted(
        {
            page_idx
            for chunks in selected.values()
            for _, triples in chunks
            for page_idx, _, _ in triples
        }
    )

    total_compressed = sum(page_sizes[p] for p in needed_pages)
    print(
        f"Fetching and decompressing {len(needed_pages)}/{num_pages} CookFS pages "
        f"({total_compressed / (1024 * 1024):.2f} MiB compressed)..."
    )

    # Coalesce contiguous page indices to minimize HTTP Range round-trips
    runs: list[list[int]] = []
    for p in needed_pages:
        if runs and p == runs[-1][-1] + 1:
            runs[-1].append(p)
        else:
            runs.append([p])

    decompressed_pages: dict[int, bytes] = {}
    for run in runs:
        run_start = page_ranges[run[0]][0]
        run_end = page_ranges[run[-1]][0] + page_ranges[run[-1]][1]
        run_bytes = source.read(run_start, run_end - run_start)
        for p in run:
            p_off, p_size = page_ranges[p]
            rel_off = p_off - run_start
            raw_page = run_bytes[rel_off : rel_off + p_size]
            page_data = decompress_cookfs_page(raw_page)
            verify_cookfs_page(page_data, page_hashes[p], p)
            decompressed_pages[p] = page_data

    dest_resolved = dest_dir.resolve()
    dest_resolved.mkdir(parents=True, exist_ok=True)

    extracted_count = 0
    for rel_path, chunks in sorted(selected.items()):
        target_path = (dest_resolved / rel_path).resolve()
        if dest_resolved not in target_path.parents:
            raise RuntimeError(f"Refusing to write outside destination directory: {rel_path}")
        target_path.parent.mkdir(parents=True, exist_ok=True)
        with target_path.open("wb") as out_f:
            for _, triples in chunks:
                for page_idx, offset, size in triples:
                    out_f.write(decompressed_pages[page_idx][offset : offset + size])
        extracted_count += 1

    return extracted_count


def is_sdk_present(dest_dir: Path) -> bool:
    ti_dir = dest_dir / "ti" / "devices" / "msp432e4"
    return (
        (ti_dir / "driverlib" / "driverlib.h").exists()
        and (ti_dir / "inc" / "msp432e401y.h").exists()
        and (dest_dir / "third_party" / "CMSIS" / "Include" / "core_cm4.h").exists()
    )


def fetch_sdk(dest_dir: Path, force: bool = False) -> None:
    if not force and is_sdk_present(dest_dir):
        print(f"SDK already exists in {dest_dir}. Skipping download.")
        return

    pkg_url, version, expected_sha256 = discover_latest_package()
    ver_label = f" v{version}" if version else ""
    print(f"Found SimpleLink MSP432E4 SDK{ver_label}: {pkg_url}")

    # Extract into a staging directory first so dest_dir is never left half-populated
    dest_dir.parent.mkdir(parents=True, exist_ok=True)
    staging_dir = Path(tempfile.mkdtemp(prefix=".sdk_staging_", dir=dest_dir.parent))
    try:
        source = _open_http_range_source(pkg_url, expected_sha256)
        if source is not None:
            print(
                f"Using HTTP range extraction ({source.total_size / (1024 * 1024):.1f} MiB package)..."
            )
            count = extract_sdk_from_cookfs(source, staging_dir)
        else:
            with _download_full_installer(pkg_url, expected_sha256) as tmp:
                tmp.seek(0, 2)
                total_size = tmp.tell()

                def _read_file(offset: int, length: int) -> bytes:
                    tmp.seek(offset)
                    return tmp.read(length)

                count = extract_sdk_from_cookfs(_RangeSource(total_size, _read_file), staging_dir)

        if not is_sdk_present(staging_dir):
            raise RuntimeError("Extracted SDK is missing expected header files")

        if dest_dir.exists():
            shutil.rmtree(dest_dir)
        staging_dir.rename(dest_dir)
        print(f"SDK{ver_label} successfully extracted ({count} files) to {dest_dir}.")
    except Exception:
        shutil.rmtree(staging_dir, ignore_errors=True)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Fetch and decompress the latest TI SimpleLink MSP432E4 SDK into firmware_src/sdk."
    )
    parser.add_argument(
        "--dest",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "sdk",
        help="Destination directory for extracted SDK files (default: firmware_src/sdk)",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Re-download and overwrite existing SDK files",
    )
    args = parser.parse_args()
    fetch_sdk(args.dest, force=args.force)
    return 0


if __name__ == "__main__":
    sys.exit(main())
