//! Byte-exactness is the whole point: every input must decompress to itself.

use tncp::{compress, compress_image, decompress, decompress_image};

fn rt(data: &[u8]) {
    let c = compress(data);
    let d = decompress(&c).expect("decompress");
    assert_eq!(d, data, "round-trip mismatch (len {})", data.len());
}

#[test]
fn empty() {
    rt(&[]);
}

#[test]
fn single_bytes() {
    for b in 0u16..256 {
        rt(&[b as u8]);
    }
}

#[test]
fn runs_and_patterns() {
    rt(&[0u8; 1000]);
    rt(&[0xFFu8; 1000]);
    rt(&(0..=255u8).cycle().take(5000).collect::<Vec<_>>());
    rt(b"the quick brown fox jumps over the lazy dog. ".repeat(50).as_slice());
}

#[test]
fn pseudo_random_various_sizes() {
    // Deterministic LCG — no external deps, reproducible.
    let mut state: u64 = 0x1234_5678_9abc_def0;
    let mut rng = || {
        state = state.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        (state >> 33) as u8
    };
    for &len in &[1usize, 2, 7, 15, 16, 17, 255, 256, 257, 1023, 4096, 65537] {
        let data: Vec<u8> = (0..len).map(|_| rng()).collect();
        rt(&data);
    }
}

#[test]
fn image_roundtrip_various() {
    let mut state: u64 = 0xCAFEBABE_12345678;
    let mut rng = || {
        state = state.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        (state >> 33) as u8
    };
    // (w, h, channels)
    for &(w, h, c) in &[(1u32, 1u32, 1u8), (16, 16, 3), (17, 9, 4), (64, 48, 3), (100, 1, 1), (1, 100, 4)] {
        let n = (w * h) as usize * c as usize;
        // smooth gradient + noise (exercises the 2D neighbors)
        let px: Vec<u8> = (0..n)
            .map(|i| {
                let pix = i / c as usize;
                let x = (pix as u32 % w) as i32;
                let y = (pix as u32 / w) as i32;
                ((x * 3 + y * 5) as u8).wrapping_add(rng() & 7)
            })
            .collect();
        let comp = compress_image(&px, w, h, c);
        let back = decompress_image(&comp).expect("decompress_image");
        assert_eq!(back, px, "image round-trip mismatch {}x{}x{}", w, h, c);
    }
}

#[test]
fn image_beats_order1_on_gradient() {
    // A smooth gradient image: 2D model should crush it far better than order-1.
    let (w, h, c) = (128u32, 128u32, 3u8);
    let n = (w * h) as usize * c as usize;
    let px: Vec<u8> = (0..n)
        .map(|i| {
            let pix = i / c as usize;
            let x = pix as u32 % w;
            let y = pix as u32 / w;
            (x + y) as u8
        })
        .collect();
    let img = compress_image(&px, w, h, c);
    let gen = compress(&px);
    assert_eq!(decompress_image(&img).unwrap(), px);
    assert!(img.len() < gen.len(), "2D ({}) should beat order-1 ({})", img.len(), gen.len());
}

#[test]
fn compresses_redundant_data() {
    // Highly redundant input should shrink (sanity that the model + coder work).
    let data = b"AAAAAAAAAA".repeat(2000); // 20 KB of 'A'
    let c = compress(&data);
    assert!(c.len() < data.len() / 4, "expected strong compression, got {} from {}", c.len(), data.len());
    assert_eq!(decompress(&c).unwrap(), data);
}
