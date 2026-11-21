# Project Instructions

- **Formatting:** Always format modified files after completing code changes:
  - Run `cargo fmt --manifest-path xds110-et/Cargo.toml` only when modifying files in `xds110-et/`.
  - Run `make -C firmware_src clang-format` only when modifying files in `firmware_src/`.
  - Run `uvx --from ruff@latest ruff format` when modifying `.py` files.
- **Reference Code:** Do not analyze the implementation in `py_ref/`.
