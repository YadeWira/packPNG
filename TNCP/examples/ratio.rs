use tncp::{compress, decompress};
fn main() {
    for path in std::env::args().skip(1) {
        let data = std::fs::read(&path).unwrap();
        let c = compress(&data);
        let ok = decompress(&c).unwrap() == data;
        println!("{:<40} {:>9} -> {:>9}  ({:5.1}%)  RT={}",
            path, data.len(), c.len(),
            100.0 * c.len() as f64 / data.len().max(1) as f64,
            if ok {"OK"} else {"FAIL"});
    }
}
