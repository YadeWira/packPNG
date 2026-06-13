//! Predictor: the swap point where the neural model will replace the baseline.
//!
//! A `Predictor` must be **deterministic** given its update history (see
//! DESIGN.md): encode and decode run it in lockstep, so any divergence breaks
//! byte-exactness. The baseline below is an integer order-1 bitwise context
//! model — a real, working lossless predictor that establishes the harness.

pub trait Predictor {
    /// 12-bit probability that the next bit is 1.
    fn predict(&self) -> u16;
    /// Feed back the actual bit and advance state. Must be called once per
    /// `predict()`, with the bit that was coded.
    fn update(&mut self, bit: u8);
}

/// Order-1 bitwise context model.
///
/// Context = (previous whole byte `c1`) × (partial current byte `c0`, a 1-prefixed
/// bit accumulator in 1..=255). Each context holds an adaptively-updated 12-bit
/// probability. Pure integer math → deterministic across platforms.
pub struct Order1 {
    t: Vec<u16>, // 256 (c1) * 256 (c0) probabilities
    c1: u32,     // previous byte (0..255)
    c0: u32,     // partial byte, starts at 1, shifts in bits, finalizes at >=256
}

const RATE: u32 = 5; // adaptation rate (higher = slower/steadier)

impl Order1 {
    pub fn new() -> Self {
        Order1 { t: vec![2048; 256 * 256], c1: 0, c0: 1 }
    }

    #[inline]
    fn ctx(&self) -> usize {
        ((self.c1 as usize) << 8) | (self.c0 as usize & 0xFF)
    }
}

impl Predictor for Order1 {
    #[inline]
    fn predict(&self) -> u16 {
        self.t[self.ctx()]
    }

    #[inline]
    fn update(&mut self, bit: u8) {
        let i = self.ctx();
        let p = self.t[i] as u32;
        // Move the probability toward the observed bit.
        self.t[i] = if bit != 0 {
            (p + ((4096 - p) >> RATE)) as u16
        } else {
            (p - (p >> RATE)) as u16
        };
        // Advance the bit accumulator; finalize the byte when 8 bits are in.
        self.c0 = (self.c0 << 1) | bit as u32;
        if self.c0 >= 256 {
            self.c1 = self.c0 & 0xFF;
            self.c0 = 1;
        }
    }
}
