//! Python's random.Random (the Mersenne Twister, seeded from an int) as far as sample() goes:
//! verify_zip checks the entries random.Random(1).sample() picks, and the port checks the same ones.

const N: usize = 624;
const M: usize = 397;

pub struct Random {
    mt: [u32; N],
    index: usize,
}

impl Random {
    /// random.Random(seed): init_by_array of the seed's 32-bit words, least significant first.
    pub fn new(seed: u64) -> Random {
        let mut key = vec![seed as u32];
        if seed >> 32 != 0 {
            key.push((seed >> 32) as u32);
        }
        let mut r = Random {
            mt: [0; N],
            index: N,
        };
        r.init_genrand(19650218);
        let (mut i, mut j) = (1usize, 0usize);
        for _ in 0..N.max(key.len()) {
            let prev = r.mt[i - 1] ^ (r.mt[i - 1] >> 30);
            r.mt[i] = (r.mt[i] ^ prev.wrapping_mul(1664525))
                .wrapping_add(key[j])
                .wrapping_add(j as u32);
            i += 1;
            j += 1;
            if i >= N {
                r.mt[0] = r.mt[N - 1];
                i = 1;
            }
            if j >= key.len() {
                j = 0;
            }
        }
        for _ in 0..N - 1 {
            let prev = r.mt[i - 1] ^ (r.mt[i - 1] >> 30);
            r.mt[i] = (r.mt[i] ^ prev.wrapping_mul(1566083941)).wrapping_sub(i as u32);
            i += 1;
            if i >= N {
                r.mt[0] = r.mt[N - 1];
                i = 1;
            }
        }
        r.mt[0] = 0x8000_0000;
        r
    }

    fn init_genrand(&mut self, s: u32) {
        self.mt[0] = s;
        for i in 1..N {
            self.mt[i] = 1812433253u32
                .wrapping_mul(self.mt[i - 1] ^ (self.mt[i - 1] >> 30))
                .wrapping_add(i as u32);
        }
        self.index = N;
    }

    fn genrand_u32(&mut self) -> u32 {
        const MAG: [u32; 2] = [0, 0x9908_b0df];
        if self.index >= N {
            for k in 0..N {
                let y = (self.mt[k] & 0x8000_0000) | (self.mt[(k + 1) % N] & 0x7fff_ffff);
                self.mt[k] = self.mt[(k + M) % N] ^ (y >> 1) ^ MAG[(y & 1) as usize];
            }
            self.index = 0;
        }
        let mut y = self.mt[self.index];
        self.index += 1;
        y ^= y >> 11;
        y ^= (y << 7) & 0x9d2c_5680;
        y ^= (y << 15) & 0xefc6_0000;
        y ^ (y >> 18)
    }

    /// getrandbits(k) for k <= 64: 32-bit words, least significant first, the last one shifted.
    pub fn getrandbits(&mut self, k: u32) -> u64 {
        if k == 0 {
            return 0;
        }
        if k <= 32 {
            return (self.genrand_u32() >> (32 - k)) as u64;
        }
        let lo = self.genrand_u32() as u64;
        let hi = (self.genrand_u32() >> (64 - k)) as u64;
        lo | hi << 32
    }

    /// _randbelow(n): getrandbits(n.bit_length()) until it is below n.
    pub fn randbelow(&mut self, n: u64) -> u64 {
        let k = 64 - n.leading_zeros();
        loop {
            let r = self.getrandbits(k);
            if r < n {
                return r;
            }
        }
    }

    /// sample(range(n), k): the indices sample() picks, in its order.
    pub fn sample(&mut self, n: usize, k: usize) -> Vec<usize> {
        assert!(k <= n, "Sample larger than population or is negative");
        let mut setsize = 21usize;
        if k > 5 {
            setsize += 4usize.pow(((k * 3) as f64).log(4.0).ceil() as u32);
        }
        let mut out = Vec::with_capacity(k);
        if n <= setsize {
            let mut pool: Vec<usize> = (0..n).collect();
            for i in 0..k {
                let j = self.randbelow((n - i) as u64) as usize;
                out.push(pool[j]);
                pool[j] = pool[n - i - 1];
            }
        } else {
            let mut seen = std::collections::HashSet::new();
            for _ in 0..k {
                let mut j = self.randbelow(n as u64) as usize;
                while seen.contains(&j) {
                    j = self.randbelow(n as u64) as usize;
                }
                seen.insert(j);
                out.push(j);
            }
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::Random;

    /// random.Random(1).sample(range(n), min(40, n)), from Python 3.11.
    #[test]
    fn sample_as_python() {
        let cases: &[(usize, &[usize])] = &[
            (3, &[0, 2, 1]),
            (10, &[2, 1, 4, 0, 3, 5, 7, 9, 8, 6]),
            (
                40,
                &[
                    8, 36, 4, 16, 7, 31, 28, 30, 24, 25, 6, 3, 15, 0, 12, 13, 19, 26, 14, 39, 35,
                    18, 33, 10, 22, 27, 21, 38, 20, 34, 29, 17, 9, 5, 32, 37, 1, 11, 2, 23,
                ],
            ),
            (
                41,
                &[
                    8, 36, 4, 16, 7, 31, 28, 30, 24, 13, 3, 15, 0, 26, 12, 35, 19, 34, 22, 14, 40,
                    39, 18, 33, 10, 23, 29, 21, 37, 20, 25, 6, 17, 9, 5, 27, 38, 1, 11, 2,
                ],
            ),
            (
                100,
                &[
                    17, 72, 97, 8, 32, 15, 63, 57, 60, 83, 48, 26, 12, 62, 3, 49, 55, 77, 0, 92,
                    34, 29, 75, 13, 40, 85, 2, 74, 69, 1, 89, 27, 54, 98, 28, 56, 93, 35, 14, 22,
                ],
            ),
            (
                277,
                &[
                    68, 32, 130, 60, 253, 230, 241, 194, 107, 48, 249, 14, 199, 221, 1, 228, 136,
                    117, 52, 162, 15, 11, 6, 166, 138, 2, 240, 225, 97, 175, 55, 108, 185, 7, 135,
                    56, 195, 112, 126, 141,
                ],
            ),
            (
                278,
                &[
                    68, 32, 130, 60, 253, 230, 241, 194, 107, 48, 249, 14, 199, 221, 1, 228, 136,
                    117, 52, 162, 15, 11, 13, 277, 4, 195, 110, 216, 270, 113, 224, 119, 176, 118,
                    112, 235, 148, 213, 51, 95,
                ],
            ),
            (
                5000,
                &[
                    1100, 4662, 516, 2089, 965, 4058, 3682, 3868, 3109, 1719, 768, 3996, 232, 3193,
                    3545, 4976, 17, 3648, 2181, 1874, 4842, 837, 2600, 250, 182, 208, 4435, 75,
                    3122, 1774, 3457, 237, 4322, 1816, 3587, 4061, 4529, 1909, 2831, 1891,
                ],
            ),
        ];
        for (n, want) in cases {
            assert_eq!(Random::new(1).sample(*n, (*n).min(40)), *want, "n={n}");
        }
    }
}
