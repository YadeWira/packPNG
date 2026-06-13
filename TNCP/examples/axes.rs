use tncp::{compress_image, decompress_image};
use std::time::Instant;
fn best<F: FnMut()>(mut f: F, n: u32) -> u128 {
    let mut b = u128::MAX;
    for _ in 0..n { let t = Instant::now(); f(); let e = t.elapsed().as_millis(); if e < b { b = e; } }
    b
}
fn main() {
    let a: Vec<String> = std::env::args().collect();
    let data = std::fs::read(&a[1]).unwrap();
    let w: u32 = a[2].parse().unwrap();
    let h: u32 = a[3].parse().unwrap();
    let c: u8 = a[4].parse().unwrap();
    let comp = compress_image(&data, w, h, c);
    let enc = best(|| { let _ = compress_image(&data, w, h, c); }, 2);
    let dec = best(|| { let _ = decompress_image(&comp); }, 2);
    let ok = decompress_image(&comp).map(|d| d == data).unwrap_or(false);
    println!("{:<16} raw={:>9}  TNCP={:>8} ({:5.2}%)  enc={:>5}ms  dec={:>5}ms  RT={}",
        std::path::Path::new(&a[1]).file_stem().unwrap().to_string_lossy(),
        data.len(), comp.len(), 100.0*comp.len() as f64/data.len() as f64, enc, dec,
        if ok {"OK"} else {"FAIL"});
}
