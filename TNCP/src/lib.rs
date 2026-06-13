//! TNCP — Tovy Neural Compresor PNG (lossless).
//!
//! Pipeline: a [`Predictor`] estimates P(next bit = 1); the byte-exact range
//! coder turns the bit + probability into a stream. Decode runs the same
//! predictor in lockstep. Swap the baseline predictor for a (deterministic,
//! integer) neural model to improve ratio — see DESIGN.md.

mod cm;
mod image_model;
mod model;
mod rangecoder;

pub use image_model::ImageModel;
pub use model::{Order1, Predictor};
use rangecoder::{Decoder, Encoder};

/// Compress `data` losslessly. The output is self-contained: an 8-byte LE length
/// header followed by the coded bitstream.
pub fn compress(data: &[u8]) -> Vec<u8> {
    let mut out = (data.len() as u64).to_le_bytes().to_vec();
    let mut enc = Encoder::new();
    let mut m = Order1::new();
    for &byte in data {
        let mut k = 8;
        while k > 0 {
            k -= 1;
            let bit = (byte >> k) & 1;
            let p = m.predict();
            enc.encode_bit(p, bit);
            m.update(bit);
        }
    }
    out.extend(enc.finish());
    out
}

/// Decompress a buffer produced by [`compress`]. Returns `None` on a truncated
/// header or an implausible length.
pub fn decompress(comp: &[u8]) -> Option<Vec<u8>> {
    if comp.len() < 8 {
        return None;
    }
    let n = u64::from_le_bytes(comp[0..8].try_into().unwrap()) as usize;
    // Sanity cap vs corrupt input claiming a huge length.
    if n > (1usize << 34) {
        return None;
    }
    let mut dec = Decoder::new(&comp[8..]);
    let mut m = Order1::new();
    let mut out = Vec::with_capacity(n);
    for _ in 0..n {
        let mut byte = 0u8;
        for _ in 0..8 {
            let p = m.predict();
            let bit = dec.decode_bit(p);
            m.update(bit);
            byte = (byte << 1) | bit;
        }
        out.push(byte);
    }
    Some(out)
}

// ─── PNG-family 2D image path (step 2: context-mixing predictor) ──────────────

/// Compress raw pixel bytes with the 2D image model. `channels` = bytes per pixel
/// (8-bit assumed). Output is self-contained: [w u32][h u32][channels u8][len u64]
/// then the coded bitstream.
pub fn compress_image(pixels: &[u8], width: u32, height: u32, channels: u8) -> Vec<u8> {
    let mut out = Vec::with_capacity(pixels.len() / 2 + 17);
    out.extend_from_slice(&width.to_le_bytes());
    out.extend_from_slice(&height.to_le_bytes());
    out.push(channels);
    out.extend_from_slice(&(pixels.len() as u64).to_le_bytes());

    let mut enc = Encoder::new();
    let mut m = ImageModel::new(width as usize, channels.max(1) as usize);
    for &byte in pixels {
        let mut k = 8;
        while k > 0 {
            k -= 1;
            let bit = (byte >> k) & 1;
            let p = m.predict();
            enc.encode_bit(p, bit);
            m.update(bit);
        }
    }
    out.extend(enc.finish());
    out
}

/// Decompress a buffer produced by [`compress_image`].
pub fn decompress_image(comp: &[u8]) -> Option<Vec<u8>> {
    if comp.len() < 17 {
        return None;
    }
    let width = u32::from_le_bytes(comp[0..4].try_into().unwrap());
    let _height = u32::from_le_bytes(comp[4..8].try_into().unwrap());
    let channels = comp[8];
    let n = u64::from_le_bytes(comp[9..17].try_into().unwrap()) as usize;
    if n > (1usize << 34) {
        return None;
    }
    let mut dec = Decoder::new(&comp[17..]);
    let mut m = ImageModel::new(width as usize, channels.max(1) as usize);
    let mut out = Vec::with_capacity(n);
    for _ in 0..n {
        let mut byte = 0u8;
        for _ in 0..8 {
            let p = m.predict();
            let bit = dec.decode_bit(p);
            m.update(bit);
            byte = (byte << 1) | bit;
        }
        out.push(byte);
    }
    Some(out)
}

// ─── C FFI (mirrors the preflate-rs shim, ready to link into packPNG) ─────────

/// Compress `in_len` bytes at `inp`. On success returns 0 and sets `*out` to a
/// heap buffer of `*out_len` bytes (free with [`tncp_free`]).
///
/// # Safety
/// `inp` must point to `in_len` readable bytes; `out`/`out_len` must be valid.
#[no_mangle]
pub unsafe extern "C" fn tncp_compress(
    inp: *const u8,
    in_len: usize,
    out: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    if (inp.is_null() && in_len != 0) || out.is_null() || out_len.is_null() {
        return -1;
    }
    let data = if in_len == 0 { &[][..] } else { std::slice::from_raw_parts(inp, in_len) };
    let v = compress(data);
    write_out(v, out, out_len)
}

/// Decompress a TNCP buffer. On success returns 0 and sets `*out`/`*out_len`.
///
/// # Safety
/// `inp` must point to `in_len` readable bytes; `out`/`out_len` must be valid.
#[no_mangle]
pub unsafe extern "C" fn tncp_decompress(
    inp: *const u8,
    in_len: usize,
    out: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    if inp.is_null() || out.is_null() || out_len.is_null() {
        return -1;
    }
    let data = std::slice::from_raw_parts(inp, in_len);
    match decompress(data) {
        Some(v) => write_out(v, out, out_len),
        None => -2,
    }
}

/// Compress raw pixels with the 2D image model. See [`compress_image`].
///
/// # Safety
/// `inp` must point to `in_len` readable bytes; `out`/`out_len` must be valid.
#[no_mangle]
pub unsafe extern "C" fn tncp_compress_image(
    inp: *const u8,
    in_len: usize,
    width: u32,
    height: u32,
    channels: u8,
    out: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    if (inp.is_null() && in_len != 0) || out.is_null() || out_len.is_null() {
        return -1;
    }
    let data = if in_len == 0 { &[][..] } else { std::slice::from_raw_parts(inp, in_len) };
    write_out(compress_image(data, width, height, channels), out, out_len)
}

/// Decompress a buffer from [`tncp_compress_image`].
///
/// # Safety
/// `inp` must point to `in_len` readable bytes; `out`/`out_len` must be valid.
#[no_mangle]
pub unsafe extern "C" fn tncp_decompress_image(
    inp: *const u8,
    in_len: usize,
    out: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    if inp.is_null() || out.is_null() || out_len.is_null() {
        return -1;
    }
    let data = std::slice::from_raw_parts(inp, in_len);
    match decompress_image(data) {
        Some(v) => write_out(v, out, out_len),
        None => -2,
    }
}

/// Free a buffer returned by [`tncp_compress`] / [`tncp_decompress`].
///
/// # Safety
/// `p`/`len` must come from a prior TNCP call and be used at most once.
#[no_mangle]
pub unsafe extern "C" fn tncp_free(p: *mut u8, len: usize) {
    if !p.is_null() && len != 0 {
        drop(Vec::from_raw_parts(p, len, len));
    }
}

unsafe fn write_out(mut v: Vec<u8>, out: *mut *mut u8, out_len: *mut usize) -> i32 {
    v.shrink_to_fit();
    let len = v.len();
    let ptr = v.as_mut_ptr();
    std::mem::forget(v); // ownership transferred to the caller (free with tncp_free)
    *out = ptr;
    *out_len = len;
    0
}
