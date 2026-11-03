//! Half size: each output sample the rounded mean of a 2x2 block (an odd last row or column
//! averages with itself), for luma and both chroma planes of an I420 picture.
use crate::Picture;

fn half_plane(src: &[u8], w: usize, h: usize) -> (Vec<u8>, usize, usize) {
    let (ow, oh) = (w.div_ceil(2), h.div_ceil(2));
    let mut out = vec![0u8; ow * oh];
    for oy in 0..oh {
        let r0 = &src[2 * oy * w..2 * oy * w + w];
        let y1 = (2 * oy + 1).min(h - 1);
        let r1 = &src[y1 * w..y1 * w + w];
        let o = &mut out[oy * ow..oy * ow + ow];
        for (ox, d) in o.iter_mut().enumerate() {
            let x0 = 2 * ox;
            let x1 = (x0 + 1).min(w - 1);
            let s = r0[x0] as u32 + r0[x1] as u32 + r1[x0] as u32 + r1[x1] as u32;
            *d = ((s + 2) >> 2) as u8;
        }
    }
    (out, ow, oh)
}

/// The picture at half its width and height (rounded up).
pub fn half(p: &Picture) -> Picture {
    let (y, w, h) = half_plane(&p.y, p.w, p.h);
    let (cw, ch) = (p.w.div_ceil(2), p.h.div_ceil(2));
    let (u, ..) = half_plane(&p.u, cw, ch);
    let (v, ..) = half_plane(&p.v, cw, ch);
    // the output's chroma planes are ceil(w / 2) x ceil(h / 2) of its own size: a 2x2 mean of the
    // input's chroma gives ceil(ceil(W / 2) / 2), which equals it for every W
    debug_assert_eq!(u.len(), w.div_ceil(2) * h.div_ceil(2));
    Picture { w, h, y, u, v }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn means_of_2x2_blocks_odd_edges_included() {
        // 3x3 luma: rows 1 2 3 / 4 5 6 / 7 8 9 -> 2x2: mean(1,2,4,5)=3, mean(3,3,6,6)=4.5->5,
        // mean(7,8,7,8)=7.5->8, mean(9,9,9,9)=9
        let p = Picture { w: 3, h: 3, y: (1..=9).collect(), u: vec![10, 20, 30, 40], v: vec![0, 0, 0, 4] };
        let q = half(&p);
        assert_eq!((q.w, q.h), (2, 2));
        assert_eq!(q.y, vec![3, 5, 8, 9]);
        assert_eq!(q.u, vec![25]);
        assert_eq!(q.v, vec![1]);
    }
}
