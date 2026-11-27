//! Small pieces the summaries share: a counter that keeps Python's Counter order, percentages, %g.
use std::collections::HashMap;
use std::hash::Hash;

/// Counts in first-seen order; most_common sorts by count, ties in that order (collections.Counter).
#[derive(Clone, Debug)]
pub struct Counter<K: Eq + Hash + Clone> {
    keys: Vec<K>,
    n: HashMap<K, i64>,
}

impl<K: Eq + Hash + Clone> Default for Counter<K> {
    fn default() -> Self {
        Counter { keys: Vec::new(), n: HashMap::new() }
    }
}

impl<K: Eq + Hash + Clone> Counter<K> {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn add(&mut self, k: K, v: i64) {
        match self.n.get_mut(&k) {
            Some(x) => *x += v,
            None => {
                self.keys.push(k.clone());
                self.n.insert(k, v);
            }
        }
    }
    pub fn get(&self, k: &K) -> i64 {
        self.n.get(k).copied().unwrap_or(0)
    }
    pub fn is_empty(&self) -> bool {
        self.keys.is_empty()
    }
    pub fn len(&self) -> usize {
        self.keys.len()
    }
    /// (key, count) in first-seen order
    pub fn items(&self) -> Vec<(K, i64)> {
        self.keys.iter().map(|k| (k.clone(), self.n[k])).collect()
    }
    pub fn keys(&self) -> &[K] {
        &self.keys
    }
    pub fn values_max(&self) -> i64 {
        self.n.values().copied().max().unwrap_or(0)
    }
    pub fn total(&self) -> i64 {
        self.n.values().sum()
    }
    pub fn most_common(&self, n: Option<usize>) -> Vec<(K, i64)> {
        let mut v = self.items();
        v.sort_by(|a, b| b.1.cmp(&a.1));
        if let Some(n) = n {
            v.truncate(n);
        }
        v
    }
}

/// f'{100.0 * n / d:5.1f}%', or '  -  ' when d is 0
pub fn pct(n: f64, d: f64) -> String {
    if d != 0.0 {
        format!("{:5.1}%", 100.0 * n / d)
    } else {
        "  -  ".to_string()
    }
}

/// Python's format(x, 'g'): 6 significant digits, no trailing zeros, exponent past 1e-4 / 1e6.
pub fn fmt_g(x: f64) -> String {
    if x == 0.0 {
        return if x.is_sign_negative() { "-0".into() } else { "0".into() };
    }
    if !x.is_finite() {
        return if x.is_nan() { "nan".into() } else if x > 0.0 { "inf".into() } else { "-inf".into() };
    }
    let e = format!("{:.5e}", x);
    let (m, exp) = e.split_once('e').unwrap();
    let exp: i32 = exp.parse().unwrap();
    if !(-4..6).contains(&exp) {
        let m = m.trim_end_matches('0').trim_end_matches('.');
        return format!("{m}e{}{:02}", if exp < 0 { '-' } else { '+' }, exp.abs());
    }
    let s = format!("{:.*}", (5 - exp).max(0) as usize, x);
    if s.contains('.') {
        s.trim_end_matches('0').trim_end_matches('.').to_string()
    } else {
        s
    }
}

/// bisect.bisect_right
pub fn bisect_right<T: PartialOrd>(v: &[T], x: &T) -> usize {
    v.partition_point(|a| a <= x)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn g() {
        for (x, s) in [(50.0, "50"), (16.7, "16.7"), (0.5, "0.5"), (1234567.0, "1.23457e+06"), (0.0001, "0.0001"), (100000.0, "100000")] {
            assert_eq!(fmt_g(x), s);
        }
    }

    #[test]
    fn counter_order() {
        let mut c = Counter::new();
        for k in ["b", "a", "c", "a", "b"] {
            c.add(k, 1);
        }
        assert_eq!(c.most_common(None), [("b", 2), ("a", 2), ("c", 1)]);
    }
}
