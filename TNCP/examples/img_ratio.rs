use tncp::{compress, compress_image, decompress_image};
fn main() {
    let a: Vec<String> = std::env::args().collect();
    // args: raw_path width height channels png_size
    let data = std::fs::read(&a[1]).unwrap();
    let w: u32 = a[2].parse().unwrap();
    let h: u32 = a[3].parse().unwrap();
    let c: u8 = a[4].parse().unwrap();
    let pngsz: usize = a.get(5).and_then(|s| s.parse().ok()).unwrap_or(0);
    let t0 = std::time::Instant::now();
    let img = compress_image(&data, w, h, c);
    let enc_ms = t0.elapsed().as_millis();
    let ok = decompress_image(&img).map(|d| d == data).unwrap_or(false);
    let o1 = compress(&data);
    println!("{:<16} raw={:>9}  PNG(deflate)={:>8} ({:4.1}%)  order1={:>9} ({:4.1}%)  2D-CM={:>9} ({:4.1}%)  enc={}ms RT={}",
        std::path::Path::new(&a[1]).file_stem().unwrap().to_string_lossy(),
        data.len(), pngsz, pct(pngsz,data.len()),
        o1.len(), pct(o1.len(),data.len()),
        img.len(), pct(img.len(),data.len()), enc_ms, if ok {"OK"} else {"FAIL"});
}
fn pct(a: usize, b: usize) -> f64 { if b==0 {0.0} else {100.0*a as f64/b as f64} }
