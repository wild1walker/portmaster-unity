//! Unity versions as UnityPy (and the TPK database) number them: major << 48 | minor << 32 |
//! build << 16 | type << 8 | type number, so that comparing the numbers compares the versions.
use std::fmt;

/// A Unity version ("2017.4.9f1"), as UnityPy's UnityVersion int.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug, Default)]
pub struct UnityVersion(pub u64);

impl UnityVersion {
    pub fn new(major: u64, minor: u64, build: u64, typ: u64, type_number: u64) -> Self {
        UnityVersion((major << 48) | (minor << 32) | (build << 16) | (typ << 8) | type_number)
    }

    /// UnityVersion.from_str: `major.minor.build`, then optionally a type letter (a, b, c, f, p, x;
    /// anything else is 255), its number and a postfix (ignored). None when it does not start
    /// with three numbers.
    pub fn parse(s: &str) -> Option<Self> {
        let b = s.as_bytes();
        let mut p = 0;
        let mut nums = [0u64; 3];
        for (i, n) in nums.iter_mut().enumerate() {
            let start = p;
            while p < b.len() && b[p].is_ascii_digit() {
                p += 1;
            }
            if p == start {
                return None;
            }
            *n = s[start..p].parse().ok()?;
            if i < 2 {
                if p >= b.len() || b[p] != b'.' {
                    return None;
                }
                p += 1;
            }
        }
        if p >= b.len() {
            return Some(Self::new(nums[0], nums[1], nums[2], 3, 0));
        }
        // the type: one character (the regex's lazy `.+?`), then its digits
        let ch = s[p..].chars().next()?;
        p += ch.len_utf8();
        let typ = match ch.to_lowercase().next().unwrap_or(ch) {
            'a' => 0,
            'b' => 1,
            'c' => 2,
            'f' => 3,
            'p' => 4,
            'x' => 5,
            _ => 255,
        };
        let start = p;
        while p < b.len() && b[p].is_ascii_digit() {
            p += 1;
        }
        let num = if p > start {
            s[start..p].parse().unwrap_or(0)
        } else {
            0
        };
        Some(Self::new(nums[0], nums[1], nums[2], typ, num))
    }

    pub fn major(&self) -> u64 {
        (self.0 >> 48) & 0xFFFF
    }
    pub fn minor(&self) -> u64 {
        (self.0 >> 32) & 0xFFFF
    }
    pub fn build(&self) -> u64 {
        (self.0 >> 16) & 0xFFFF
    }

    /// (major, minor, build), as tuple comparisons in the Python use it.
    pub fn triple(&self) -> (u64, u64, u64) {
        (self.major(), self.minor(), self.build())
    }
}

impl fmt::Display for UnityVersion {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let t = ["a", "b", "c", "f", "p", "x"]
            .get(((self.0 >> 8) & 0xFF) as usize)
            .copied()
            .unwrap_or("u");
        write!(
            f,
            "{}.{}.{}{}{}",
            self.major(),
            self.minor(),
            self.build(),
            t,
            self.0 & 0xFF
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse() {
        assert_eq!(
            UnityVersion::parse("2017.4.9f1"),
            Some(UnityVersion::new(2017, 4, 9, 3, 1))
        );
        assert_eq!(
            UnityVersion::parse("5.6.0"),
            Some(UnityVersion::new(5, 6, 0, 3, 0))
        );
        assert_eq!(
            UnityVersion::parse("2020.3.1f1c1"),
            Some(UnityVersion::new(2020, 3, 1, 3, 1))
        );
        assert_eq!(
            UnityVersion::parse("6000.0.10b12"),
            Some(UnityVersion::new(6000, 0, 10, 1, 12))
        );
        assert_eq!(
            UnityVersion::parse("2019.4.0q"),
            Some(UnityVersion::new(2019, 4, 0, 255, 0))
        );
        assert_eq!(UnityVersion::parse("2017.4"), None);
        assert!(UnityVersion::parse("2017.4.9f1") < UnityVersion::parse("2017.4.10f1"));
        assert_eq!(
            UnityVersion::parse("2017.4.9f1").unwrap().to_string(),
            "2017.4.9f1"
        );
    }
}
