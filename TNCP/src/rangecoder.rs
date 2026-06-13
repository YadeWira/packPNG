//! Byte-exact carryless binary range coder (lpaq-style).
//!
//! This is the deterministic, integer-only core. It is independent of the
//! predictor: it codes one bit at a time given a 12-bit probability `p` that the
//! bit is 1. Encode and decode are exact mirrors, so a stream coded with a given
//! sequence of probabilities decodes back to the identical bits — provided the
//! decoder feeds the coder the *same* probabilities (the predictor's job).

const PROB_BITS: u32 = 12; // probabilities are 12-bit: 1..=4095

/// Clamp a probability into the open range the coder requires (never 0 or 4096).
#[inline]
pub fn clamp_p(p: u16) -> u32 {
    (p as u32).clamp(1, (1 << PROB_BITS) - 1)
}

pub struct Encoder {
    x1: u32,
    x2: u32,
    out: Vec<u8>,
}

impl Encoder {
    pub fn new() -> Self {
        Encoder { x1: 0, x2: 0xFFFF_FFFF, out: Vec::new() }
    }

    /// Encode one bit. `p` = P(bit == 1), 12-bit.
    #[inline]
    pub fn encode_bit(&mut self, p: u16, bit: u8) {
        let p = clamp_p(p);
        // xmid is strictly within [x1, x2) because p < 2^12.
        let xmid = self.x1 + ((self.x2 - self.x1) >> PROB_BITS) * p;
        if bit != 0 {
            self.x2 = xmid;
        } else {
            self.x1 = xmid + 1;
        }
        // Renormalize: emit bytes the two bounds now agree on.
        while (self.x1 ^ self.x2) & 0xFF00_0000 == 0 {
            self.out.push((self.x2 >> 24) as u8);
            self.x1 <<= 8;
            self.x2 = (self.x2 << 8) | 0xFF;
        }
    }

    /// Flush the coder and return the compressed bytes (4-byte tail disambiguates
    /// the final interval; the decoder reads 4 priming bytes to match).
    pub fn finish(mut self) -> Vec<u8> {
        for _ in 0..4 {
            self.out.push((self.x1 >> 24) as u8);
            self.x1 <<= 8;
        }
        self.out
    }
}

pub struct Decoder<'a> {
    x1: u32,
    x2: u32,
    x: u32,
    buf: &'a [u8],
    pos: usize,
}

impl<'a> Decoder<'a> {
    pub fn new(buf: &'a [u8]) -> Self {
        let mut d = Decoder { x1: 0, x2: 0xFFFF_FFFF, x: 0, buf, pos: 0 };
        for _ in 0..4 {
            d.x = (d.x << 8) | d.next_byte() as u32;
        }
        d
    }

    /// Reads past end of buffer return 0 (the implicit zero tail), matching the
    /// encoder's flush convention so short streams still round-trip.
    #[inline]
    fn next_byte(&mut self) -> u8 {
        let b = if self.pos < self.buf.len() { self.buf[self.pos] } else { 0 };
        self.pos += 1;
        b
    }

    /// Decode one bit given the same `p` the encoder used at this position.
    #[inline]
    pub fn decode_bit(&mut self, p: u16) -> u8 {
        let p = clamp_p(p);
        let xmid = self.x1 + ((self.x2 - self.x1) >> PROB_BITS) * p;
        let bit = if self.x <= xmid {
            self.x2 = xmid;
            1
        } else {
            self.x1 = xmid + 1;
            0
        };
        while (self.x1 ^ self.x2) & 0xFF00_0000 == 0 {
            self.x1 <<= 8;
            self.x2 = (self.x2 << 8) | 0xFF;
            self.x = (self.x << 8) | self.next_byte() as u32;
        }
        bit
    }
}
