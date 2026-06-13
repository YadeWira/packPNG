//! One-shot C FFI for whole-buffer preflate container compress/recreate.
//! Simpler than the streaming `unmanaged_api` (no output queue) and exposes
//! `max_chain_length`. Intended for static linking into packPNG (C++).

use preflate_container::{
    preflate_whole_into_container_lvl, recreate_whole_from_container, PreflateContainerConfig,
};
use preflate_rs::{
    preflate_whole_deflate_stream, recreate_whole_deflate_stream, PreflateConfig,
};
use std::io::Cursor;

/// Hand a Vec<u8> to C: returns pointer + length, ownership transferred.
/// The caller must release it with `pf_free(ptr, len)`.
fn vec_to_c(v: Vec<u8>, out: *mut *mut u8, out_len: *mut usize) {
    let mut boxed = v.into_boxed_slice();
    let ptr = boxed.as_mut_ptr();
    let len = boxed.len();
    std::mem::forget(boxed);
    unsafe {
        *out = ptr;
        *out_len = len;
    }
}

/// Compress a buffer (that contains deflate/zlib streams) into a preflate
/// container. Returns 0 on success. *out is malloc-style owned by the caller.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pf_container_compress(
    input: *const u8,
    input_len: usize,
    max_chain: u32,
    zstd_level: i32,
    out: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    if input.is_null() || out.is_null() || out_len.is_null() {
        return -1;
    }
    let data = unsafe { std::slice::from_raw_parts(input, input_len) };
    let cfg = PreflateContainerConfig {
        max_chain_length: max_chain,
        validate_compression: false,
        ..PreflateContainerConfig::default()
    };
    let lvl = if zstd_level == 0 { 9 } else { zstd_level };
    let mut output = Vec::new();
    match preflate_whole_into_container_lvl(&cfg, lvl, &mut Cursor::new(data), &mut output) {
        Ok(()) => {
            vec_to_c(output, out, out_len);
            0
        }
        Err(_) => -2,
    }
}

/// Recreate the original bytes from a preflate container. Returns 0 on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pf_container_recreate(
    input: *const u8,
    input_len: usize,
    out: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    if input.is_null() || out.is_null() || out_len.is_null() {
        return -1;
    }
    let data = unsafe { std::slice::from_raw_parts(input, input_len) };
    let mut output = Vec::new();
    match recreate_whole_from_container(&mut Cursor::new(data), &mut output) {
        Ok(()) => {
            vec_to_c(output, out, out_len);
            0
        }
        Err(_) => -2,
    }
}

/// Low-level: split a RAW deflate stream into (plain_text, corrections) so the
/// caller can compress the plain text with its own backend (e.g. kanzi). Both
/// output buffers are owned by the caller (free each with pf_free). Returns 0 ok.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pf_deflate_split(
    input: *const u8,
    input_len: usize,
    max_chain: u32,
    plain_out: *mut *mut u8,
    plain_len: *mut usize,
    corr_out: *mut *mut u8,
    corr_len: *mut usize,
) -> i32 {
    if input.is_null() || plain_out.is_null() || corr_out.is_null() {
        return -1;
    }
    let data = unsafe { std::slice::from_raw_parts(input, input_len) };
    let cfg = PreflateConfig {
        max_chain_length: max_chain,
        verify_compression: false,
        ..PreflateConfig::default()
    };
    match preflate_whole_deflate_stream(data, &cfg) {
        Ok((res, plain)) => {
            vec_to_c(plain.text().to_vec(), plain_out, plain_len);
            vec_to_c(res.corrections, corr_out, corr_len);
            0
        }
        Err(_) => -2,
    }
}

/// Low-level inverse: recreate the byte-exact raw deflate stream from
/// (plain_text, corrections). *out owned by caller (pf_free). Returns 0 ok.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pf_deflate_recreate(
    plain: *const u8,
    plain_len: usize,
    corr: *const u8,
    corr_len: usize,
    out: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    if plain.is_null() || corr.is_null() || out.is_null() {
        return -1;
    }
    let plain_s = unsafe { std::slice::from_raw_parts(plain, plain_len) };
    let corr_s = unsafe { std::slice::from_raw_parts(corr, corr_len) };
    match recreate_whole_deflate_stream(plain_s, corr_s) {
        Ok(deflate) => {
            vec_to_c(deflate, out, out_len);
            0
        }
        Err(_) => -2,
    }
}

/// Free a buffer returned by pf_container_compress / pf_container_recreate /
/// pf_deflate_split / pf_deflate_recreate.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pf_free(ptr: *mut u8, len: usize) {
    if !ptr.is_null() && len > 0 {
        unsafe {
            drop(Vec::from_raw_parts(ptr, len, len));
        }
    }
}
