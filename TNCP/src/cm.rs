//! Integer context-mixing primitives (PAQ/lpaq-style), fully deterministic.
//!
//! `squash` maps the logistic "stretch" domain to a 12-bit probability; `stretch`
//! is its inverse. The `Mixer` combines several stretched predictions with
//! context-selected integer weights and learns online. All integer → identical on
//! every platform, which is what lossless decode requires.

const SQ_T: [i32; 33] = [
    1, 2, 3, 6, 10, 16, 27, 45, 73, 120, 194, 310, 488, 747, 1101, 1546, 2047, 2549,
    2994, 3348, 3607, 3785, 3901, 3975, 4022, 4050, 4068, 4079, 4085, 4089, 4092, 4093, 4094,
];

/// stretch-domain `d` (-2047..=2047) → probability 0..4095.
#[inline]
pub fn squash(d: i32) -> i32 {
    if d >= 2047 { return 4095; }
    if d <= -2047 { return 0; }
    let w = d & 127;
    let i = ((d >> 7) + 16) as usize;
    (SQ_T[i] * (128 - w) + SQ_T[i + 1] * w + 64) >> 7
}

/// Precomputed inverse of `squash`: probability 0..4095 → stretch -2047..=2047.
pub struct Stretch {
    t: Vec<i16>,
}

impl Stretch {
    pub fn new() -> Self {
        let mut t = vec![0i16; 4096];
        let mut pi = 0usize;
        for d in -2047..=2047 {
            let p = squash(d) as usize;
            for x in pi..=p {
                t[x] = d as i16;
            }
            pi = p + 1;
        }
        for x in pi..4096 {
            t[x] = 2047;
        }
        Stretch { t }
    }
    #[inline]
    pub fn get(&self, p: u16) -> i32 {
        self.t[(p as usize).min(4095)] as i32
    }
}

/// Logistic mixer with context-selected weight sets.
pub struct Mixer {
    n: usize,           // inputs per context
    w: Vec<i32>,        // n * nctx weights (16.16 fixed point)
    tx: Vec<i32>,       // current stretched inputs
    ctx: usize,         // selected weight-set this step
    pr: i32,            // last prediction (for update)
}

impl Mixer {
    pub fn new(n: usize, nctx: usize) -> Self {
        Mixer { n, w: vec![0; n * nctx], tx: Vec::with_capacity(n), ctx: 0, pr: 2048 }
    }
    #[inline]
    pub fn add(&mut self, stretched: i32) {
        self.tx.push(stretched);
    }
    #[inline]
    pub fn set_ctx(&mut self, c: usize) {
        self.ctx = c;
    }
    /// Mix the added inputs → 12-bit probability.
    #[inline]
    pub fn predict(&mut self) -> u16 {
        let base = self.ctx * self.n;
        let mut dot: i64 = 0;
        for i in 0..self.tx.len() {
            dot += self.tx[i] as i64 * self.w[base + i] as i64;
        }
        self.pr = squash((dot >> 16) as i32);
        self.pr as u16
    }
    /// Learn from the actual bit and reset inputs for the next step.
    #[inline]
    pub fn update(&mut self, bit: u8) {
        let err = (((bit as i32) << 12) - self.pr) * 7;
        let base = self.ctx * self.n;
        for i in 0..self.tx.len() {
            let nw = self.w[base + i] + ((self.tx[i] * err) >> 10);
            self.w[base + i] = nw.clamp(-(1 << 22), 1 << 22);
        }
        self.tx.clear();
    }
}

/// Adaptive Probability Map (SSE): refines a probability through a context-indexed,
/// piecewise-linear map over the stretched domain (lpaq-style, 33 knots/context).
pub struct Apm {
    t: Vec<u16>, // nctx * 33, 16-bit probabilities
    idx: usize,  // knot chosen last `refine` (for `update`)
}

impl Apm {
    pub fn new(nctx: usize) -> Self {
        let mut t = vec![0u16; nctx * 33];
        for c in 0..nctx {
            for j in 0..33 {
                t[c * 33 + j] = (squash((j as i32 - 16) * 128) * 16) as u16;
            }
        }
        Apm { t, idx: 0 }
    }
    /// Refine `pr` (12-bit) under `ctx` → refined 12-bit probability.
    #[inline]
    pub fn refine(&mut self, pr: u16, ctx: usize, st: &Stretch) -> u16 {
        let s = (st.get(pr) + 2048).clamp(0, 4094) as u32; // 0..4094
        let w = s & 127;
        let base = ctx * 33 + (s >> 7) as usize;
        self.idx = base + if w >= 64 { 1 } else { 0 };
        (((self.t[base] as u32 * (128 - w) + self.t[base + 1] as u32 * w) >> 7) >> 4) as u16
    }
    #[inline]
    pub fn update(&mut self, bit: u8, rate: u32) {
        let g: i32 = if bit != 0 { 65535 } else { 0 };
        let v = self.t[self.idx] as i32;
        self.t[self.idx] = (v + ((g - v) >> rate)) as u16;
    }
}

/// Two-layer logistic mixing network (the "neural net" of PAQ8/cmix), trained
/// online by backprop. A hidden layer of context-selected logistic units adds
/// nonlinearity/capacity over the single-layer [`Mixer`]; the output layer
/// combines them. Fully integer/deterministic. Symmetry between hidden units is
/// broken by giving each a DIFFERENT selecting context.
///
/// NOTE: online (no offline pretraining) this did NOT beat the well-tuned 1-layer
/// [`Mixer`] on the representative screenshot case (helped flat images, hurt
/// text/UI, ~2× encode). Kept for a future revisit with offline-pretrained
/// hidden weights or better LR scheduling. Not currently wired in.
#[allow(dead_code)]
pub struct Mixer2 {
    n: usize,           // inputs
    h: usize,           // hidden units
    w1: Vec<Vec<i32>>,  // per hidden unit: nctx_u * n weights (16.16)
    w2: Vec<i32>,       // out_nctx * h
    tx: Vec<i32>,       // current inputs (stretched)
    hs: Vec<i32>,       // hidden stretched outputs
    c1: Vec<usize>,     // chosen ctx per hidden unit (this step)
    c2: usize,          // output ctx
    pr: i32,
}

#[allow(dead_code)]
impl Mixer2 {
    pub fn new(n: usize, hidden_nctx: &[usize], out_nctx: usize) -> Self {
        let h = hidden_nctx.len();
        let w1 = hidden_nctx.iter().map(|&nc| vec![0i32; nc * n]).collect();
        Mixer2 {
            n,
            h,
            w1,
            w2: vec![65536 / h as i32; out_nctx * h], // output ≈ avg of hidden initially
            tx: Vec::with_capacity(n),
            hs: vec![0; h],
            c1: vec![0; h],
            c2: 0,
            pr: 2048,
        }
    }
    #[inline]
    pub fn add(&mut self, stretched: i32) {
        self.tx.push(stretched);
    }
    /// `hctx` = selecting context per hidden unit; `octx` = output-layer context.
    #[inline]
    pub fn predict(&mut self, hctx: &[usize], octx: usize, st: &Stretch) -> u16 {
        self.c2 = octx;
        for u in 0..self.h {
            self.c1[u] = hctx[u];
            let base = hctx[u] * self.n;
            let mut dot: i64 = 0;
            for i in 0..self.tx.len() {
                dot += self.tx[i] as i64 * self.w1[u][base + i] as i64;
            }
            let hp = squash((dot >> 16) as i32) as u16;
            self.hs[u] = st.get(hp);
        }
        let base2 = octx * self.h;
        let mut dot: i64 = 0;
        for u in 0..self.h {
            dot += self.hs[u] as i64 * self.w2[base2 + u] as i64;
        }
        self.pr = squash((dot >> 16) as i32);
        self.pr as u16
    }
    #[inline]
    pub fn update(&mut self, bit: u8) {
        let e2 = ((bit as i32) << 12) - self.pr; // output error
        let base2 = self.c2 * self.h;
        for u in 0..self.h {
            let w2u = self.w2[base2 + u];
            self.w2[base2 + u] = (w2u + ((self.hs[u] * e2) >> 14)).clamp(-(1 << 22), 1 << 22);
            let eh = (e2 * w2u) >> 16; // backprop to hidden unit u
            let base1 = self.c1[u] * self.n;
            for i in 0..self.tx.len() {
                let nw = self.w1[u][base1 + i] + ((self.tx[i] * eh) >> 12);
                self.w1[u][base1 + i] = nw.clamp(-(1 << 22), 1 << 22);
            }
        }
        self.tx.clear();
    }
}

// ── StateMap: count-adaptive probability cell (lpaq-style) ────────────────────
// Each cell is a u32 = (probability : 22 bits) << 10 | (count : 10 bits). The
// adaptation step shrinks as the count grows: fast learning on fresh contexts,
// stable estimates on mature ones — a bigger win than a fixed rate.

/// Initial cell: probability 0.5, count 0.
pub const SM_INIT: u32 = 1 << 31;

/// Per-count adaptation-rate table; rate ∝ 1/count.
pub fn make_dt() -> Vec<i32> {
    (0..1024).map(|i| 16384 / (i + i + 3)).collect()
}

/// 12-bit probability from a StateMap cell.
#[inline]
pub fn sm_p(cell: u32) -> u16 {
    (cell >> 20) as u16
}

/// Update a StateMap cell toward `bit` (count capped at `limit`).
#[inline]
pub fn sm_update(cell: &mut u32, bit: u8, dt: &[i32], limit: u32) {
    let n = *cell & 1023;
    let p = (*cell >> 10) as i64; // 22-bit probability
    if n < limit {
        *cell += 1;
    }
    let target = (bit as i64) << 22;
    // delta affects only the probability field (low 10 count bits masked off).
    let delta = (((target - p) >> 3) * dt[n as usize] as i64) & !1023i64;
    *cell = (*cell as i64).wrapping_add(delta) as u32;
}
