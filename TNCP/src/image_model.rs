//! 2D image context-mixing predictor (PNG-family specialized).
//!
//! Predicts each pixel byte bit-by-bit from its spatial neighborhood (W/N/NW/NE/WW,
//! same channel) AND a 1D match model (catches repeated byte runs — UI/text — that
//! LZ/deflate win on), mixed by an integer logistic [`Mixer`], then refined by an
//! [`Apm`] (SSE). Fully integer/deterministic → byte-exact decode. The model keeps
//! its own pixel buffer (identical on encode & decode), so all contexts reproduce.

use crate::cm::{make_dt, sm_p, sm_update, Apm, Mixer, Stretch, SM_INIT};

const TBITS: u32 = 21;
const TSIZE: usize = 1 << TBITS;
const NSPATIAL: usize = 9; // spatial + cross-channel context models
const NINPUTS: usize = NSPATIAL + 1; // + match model

const MBITS: u32 = 22; // match hash table
const MSIZE: usize = 1 << MBITS;
const MINMATCH: usize = 4; // bytes of context to key a match
const MAX_MLEN: u32 = 0xFFFF;

pub struct ImageModel {
    width: usize,
    channels: usize,
    buf: Vec<u8>,
    idx: usize,
    c0: u32,
    tables: Vec<Vec<u32>>,    // StateMap cells
    dt: Vec<i32>,             // count→rate table
    stretch: Stretch,
    mixer: Mixer,
    cur: [usize; NSPATIAL],

    // ── match model ──
    mt: Vec<u32>,      // ctx-hash → buf position + 0 means empty (we store pos, 0 valid only if buf has byte 0; use pos as-is with separate "seen")
    match_ptr: usize,  // predicted next-byte position in buf
    match_len: u32,
    pred_byte: u8,
    have_match: bool,
    match_active: bool, // this bit's match prediction is usable (prefix still agrees)
    mcount: Vec<u32>,   // StateMap: [min(len,31)*2 + predbit] → P(bit=1)
    mslot: usize,

    // ── SSE ──
    apm: Apm,           // keyed by c0 (256 ctx)
    apm_in: u16,        // mixer output before SSE (for blending)
}

#[inline]
fn hash(model: u64, key: u64) -> usize {
    let mut x = key.wrapping_mul(0x9E37_79B9_7F4A_7C15) ^ model.wrapping_mul(0xD1B5_4A32_D192_ED03);
    x ^= x >> 29;
    x = x.wrapping_mul(0xBF58_476D_1CE4_E5B9);
    x ^= x >> 32;
    (x as usize) & (TSIZE - 1)
}

impl ImageModel {
    pub fn new(width: usize, channels: usize) -> Self {
        let width = width.max(1);
        let channels = channels.max(1);
        ImageModel {
            width,
            channels,
            buf: Vec::new(),
            idx: 0,
            c0: 1,
            tables: (0..NSPATIAL).map(|_| vec![SM_INIT; TSIZE]).collect(),
            dt: make_dt(),
            stretch: Stretch::new(),
            mixer: Mixer::new(NINPUTS, 4 * 8 * 2),
            cur: [0; NSPATIAL],
            mt: vec![u32::MAX; MSIZE], // MAX = empty sentinel
            match_ptr: 0,
            match_len: 0,
            pred_byte: 0,
            have_match: false,
            match_active: false,
            mcount: vec![SM_INIT; 32 * 2],
            mslot: 0,
            apm: Apm::new(256),
            apm_in: 2048,
        }
    }

    #[inline]
    pub fn predict(&mut self) -> u16 {
        let cw = self.channels;
        let row = self.width * cw;
        let ch = self.idx % cw;
        let pix = self.idx / cw;
        let x = pix % self.width;
        let y = pix / self.width;

        let w  = if x > 0 { self.buf[self.idx - cw] as u64 } else { 0 };
        let n  = if y > 0 { self.buf[self.idx - row] as u64 } else { 0 };
        let nw = if x > 0 && y > 0 { self.buf[self.idx - row - cw] as u64 } else { 0 };
        let ne = if y > 0 && x + 1 < self.width { self.buf[self.idx - row + cw] as u64 } else { 0 };
        let ww = if x > 1 { self.buf[self.idx - 2 * cw] as u64 } else { 0 };
        let nn = if y > 1 { self.buf[self.idx - 2 * row] as u64 } else { 0 };
        let grad = ((w as i64 + n as i64 - nw as i64).clamp(0, 255)) as u64;
        // cross-channel: earlier channels of the CURRENT pixel (color decorrelation,
        // WebP's main edge). ch0 = this pixel's first channel; cp = previous channel.
        let ch0 = if ch > 0 { self.buf[self.idx - ch] as u64 } else { 0 };
        let cp = if ch > 0 { self.buf[self.idx - 1] as u64 } else { 0 };

        let c0 = self.c0 as u64;
        let chb = (ch as u64) << 32;
        let keys = [
            chb | c0,
            chb | c0 | (w << 8),
            chb | c0 | (n << 8),
            chb | c0 | (w << 8) | (n << 16),
            chb | c0 | (grad << 8),
            chb | c0 | (nw << 8) | (ne << 16),
            chb | c0 | (w << 8) | (ww << 16),
            chb | c0 | (cp << 8) | (ch0 << 16), // cross-channel (prev + first channel of pixel)
            chb | c0 | (n << 8) | (nn << 16),   // vertical trend (N, NN)
        ];
        for m in 0..NSPATIAL {
            let slot = hash(m as u64, keys[m]);
            self.cur[m] = slot;
            self.mixer.add(self.stretch.get(sm_p(self.tables[m][slot])));
        }

        // ── match model input ──
        let bitpos = (31 - self.c0.leading_zeros()) as usize; // 0..7
        self.match_active = false;
        let mut mst = 0i32;
        if self.have_match {
            let partial = self.c0 - (1 << bitpos); // bits coded so far in this byte
            let pred_prefix = (self.pred_byte as u32) >> (8 - bitpos);
            if bitpos == 0 || partial == pred_prefix {
                let predbit = ((self.pred_byte as u32) >> (7 - bitpos)) & 1;
                let lenb = self.match_len.min(31) as usize;
                self.mslot = lenb * 2 + predbit as usize;
                self.match_active = true;
                mst = self.stretch.get(sm_p(self.mcount[self.mslot]));
            }
        }
        self.mixer.add(mst);

        self.mixer.set_ctx((ch.min(3) * 8 + bitpos) * 2 + self.match_active as usize);
        let pm = self.mixer.predict();
        self.apm_in = pm;

        // ── SSE refine + blend ──
        let pa = self.apm.refine(pm, (self.c0 as usize) & 0xFF, &self.stretch);
        (((pm as u32) + 3 * (pa as u32)) >> 2).clamp(1, 4094) as u16
    }

    #[inline]
    pub fn update(&mut self, bit: u8) {
        for m in 0..NSPATIAL {
            sm_update(&mut self.tables[m][self.cur[m]], bit, &self.dt, 255);
        }
        if self.match_active {
            sm_update(&mut self.mcount[self.mslot], bit, &self.dt, 255);
        }
        self.mixer.update(bit);
        self.apm.update(bit, 7);

        self.c0 = (self.c0 << 1) | bit as u32;
        if self.c0 >= 256 {
            let byte = (self.c0 & 0xFF) as u8;
            self.buf.push(byte);
            self.idx += 1;
            self.c0 = 1;
            self.update_match(byte);
        }
    }

    /// After a byte completes: extend or break the current match, then (re)acquire
    /// one from the MINMATCH-byte context hash and set the predicted next byte.
    #[inline]
    fn update_match(&mut self, byte: u8) {
        let blen = self.buf.len();
        if self.have_match {
            if byte == self.pred_byte {
                self.match_ptr += 1;
                self.match_len = (self.match_len + 1).min(MAX_MLEN);
            } else {
                self.have_match = false;
                self.match_len = 0;
            }
        }
        if blen >= MINMATCH {
            let mut h = 0u64;
            for j in 0..MINMATCH {
                h = h.wrapping_mul(0x1_0000_0001_B3) ^ self.buf[blen - MINMATCH + j] as u64;
            }
            let hi = (h.wrapping_mul(0x9E37_79B9_7F4A_7C15) >> (64 - MBITS)) as usize;
            let cand = self.mt[hi];
            self.mt[hi] = blen as u32; // position of the next byte to come
            if !self.have_match && cand != u32::MAX && (cand as usize) < blen {
                self.match_ptr = cand as usize;
                self.have_match = true;
                self.match_len = 1;
            }
        }
        self.have_match = self.have_match && self.match_ptr < self.buf.len();
        self.pred_byte = if self.have_match { self.buf[self.match_ptr] } else { 0 };
    }
}
