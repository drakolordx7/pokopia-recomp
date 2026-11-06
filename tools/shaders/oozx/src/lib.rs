//! C ABI over oozextract (open Oodle decompressor) for ctypes: oozx_decompress(src, src_len, dst, dst_len) -> bytes
//! written, or -1. dst_len must be the exact uncompressed size. Build: cargo build --release (target/release/oozx.dll);
//! tools/shaders/scripts/extract_generic.py loads it from --oodle, env NVN2_OODLE_DLL or next to itself.
//! Undecodable input (the extractor probes candidate blobs) can panic inside oozextract: the panic is caught and
//! reported as -1, and the panic hook is silenced so nothing is printed.
use oozextract::Extractor;
use std::sync::Once;

static QUIET: Once = Once::new();

#[no_mangle]
pub unsafe extern "C" fn oozx_decompress(src: *const u8, src_len: usize, dst: *mut u8, dst_len: usize) -> i64 {
    QUIET.call_once(|| std::panic::set_hook(Box::new(|_| {})));
    let input = std::slice::from_raw_parts(src, src_len);
    let output = std::slice::from_raw_parts_mut(dst, dst_len);
    let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| Extractor::new().read_from_slice(input, output)));
    match r {
        Ok(Ok(n)) => n as i64,
        _ => -1,
    }
}
