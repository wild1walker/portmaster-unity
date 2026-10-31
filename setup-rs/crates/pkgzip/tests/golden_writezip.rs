//! The writezip part of tests/golden/pkgzip.sh: write_zip (then verify_zip) of the entries the spec
//! at $PMU_ZIP_SPEC lists, into $PMU_ZIP_OUT, to compare with the Python's archive byte for byte.
//! One entry a line: `<name>\tfile:<path>` or `<name>\thex:<bytes in hex>`.
use pkgzip::zipwrite::{verify_zip, write_zip, Src};

#[test]
#[ignore]
fn write_zip_from_spec() {
    let (Ok(spec), Ok(out)) = (std::env::var("PMU_ZIP_SPEC"), std::env::var("PMU_ZIP_OUT")) else {
        panic!("set PMU_ZIP_SPEC and PMU_ZIP_OUT");
    };
    let text = std::fs::read_to_string(spec).unwrap();
    let entries: Vec<(String, Src)> = text
        .split('\n')
        .filter(|l| !l.is_empty())
        .map(|l| {
            let (name, src) = l.split_once('\t').unwrap();
            let src = match src.split_once(':').unwrap() {
                ("hex", h) => Src::Bytes(
                    (0..h.len())
                        .step_by(2)
                        .map(|i| u8::from_str_radix(&h[i..i + 2], 16).unwrap())
                        .collect(),
                ),
                (_, p) => Src::Path(p.to_string()),
            };
            (name.to_string(), src)
        })
        .collect();
    write_zip(&out, &entries, &mut |m| println!("{m}"), 4).unwrap();
    verify_zip(&out, &entries, &mut |m| println!("{m}"), 40).unwrap();
}
