# preflate-rs vendoring (for the `-preflate` mode / TCPF output)

`-preflate` links a Rust **static library** built from
[microsoft/preflate-rs](https://github.com/microsoft/preflate-rs) (v0.7.6) plus a
small FFI shim (`src/ffi_oneshot.rs`, kept here). The libs are large build
artifacts and are **gitignored** (`lib/`); rebuild them with:

```bash
make preflate-libs PREFLATE_SRC=/path/to/preflate-rs
```

## What the patch is

1. Copy `src/ffi_oneshot.rs` into the preflate-rs `dll/` crate
   (`dll/src/ffi_oneshot.rs`) and add `pub mod ffi_oneshot;` to `dll/src/lib.rs`.
   It exposes a one-shot C API (the stock streaming API hardcodes
   `max_chain_length=1024`; we want 4096 for best ratio):
   - `pf_container_compress(in, in_len, max_chain, &out, &out_len)`
   - `pf_container_recreate(in, in_len, &out, &out_len)`
   - `pf_free(ptr, len)`
   These wrap `preflate_whole_into_container` / `recreate_whole_from_container`.
2. In `dll/Cargo.toml`: `crate-type = ["staticlib", "cdylib"]`.

## Build

```bash
cd $PREFLATE_SRC
cargo build --release -p preflate_rs_0_7                                  # → target/release/libpreflate_rs_0_7.a
cargo build --release --target x86_64-pc-windows-gnu -p preflate_rs_0_7   # → target/x86_64-pc-windows-gnu/release/...
```
Then copy the two `.a` into `lib/linux/` and `lib/win/` (the `make preflate-libs`
target does this and `strip --strip-debug`s the Linux one).

## Linking notes

The Rust staticlib pulls these OS-provided system libs (no shippable deps —
the final binary stays 100% autonomous):
- Linux:   `-lpthread -ldl -lm`
- Windows: `-lws2_32 -luserenv -lbcrypt -lntdll`

Verified: fully static on both platforms (`ldd` → "not a dynamic executable";
Windows imports only OS DLLs), byte-exact round-trip, deterministic across
platforms (a Linux-made TCPF decodes byte-identical on Windows).
