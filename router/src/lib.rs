// AeonBrowser Protocol Router — lib.rs
// DelgadoLogic | Senior Security Engineer
//
// PURPOSE: C-compatible FFI entry points so the C++ browser core can call
// our Rust router without unsafe raw pointer leaks.
//
// DESIGN NOTE: We expose a flat C API (no name mangling, no classes) so that:
//   1. The 32-bit C++ browser core on XP can link without name-mangling issues.
//   2. Eventually the 16-bit Open Watcom retro tier can call via a thin stub.
//   3. Any future language binding (Python telemetry scripts, C# PulseBridge)
//      can use the same surface without a Rust dependency.
//
// SAFETY: All raw pointer parameters are validated before use. We never
// store raw pointers across call boundaries — caller owns all memory.

#![allow(non_snake_case)]
#![allow(clippy::missing_safety_doc)]

mod router;
mod downloader;
mod tor;
mod gemini;
mod gopher;

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_uint};

// ---------------------------------------------------------------------------
// Router FFI
// ---------------------------------------------------------------------------

/// Initialise the protocol router. Call once at browser startup.
/// Returns 1 on success, 0 on failure.
#[no_mangle]
pub extern "C" fn aeon_router_init() -> c_int {
    match router::RouterEngine::global_init() {
        Ok(_)  => 1,
        Err(e) => {
            eprintln!("[AeonRouter] init failed: {e}");
            0
        }
    }
}

/// Dispatch a URL to the correct handler. Returns an opaque request ID,
/// or 0 if the URL is unparseable.
///
/// # IT TROUBLESHOOTING
/// - Returns 0 for unknown schemes: add handler in router.rs dispatch table.
/// - Tor requests hang: check aeon_tor_start() was called first.
/// - Gemini cert error: server cert mismatch — TOFU (trust-on-first-use)
///   requires the cert to be stored after first visit.
#[no_mangle]
pub unsafe extern "C" fn aeon_router_dispatch(
    url: *const c_char,
    request_id_out: *mut c_uint,
) -> c_int {
    if url.is_null() || request_id_out.is_null() {
        return 0;
    }
    let url_str = CStr::from_ptr(url).to_string_lossy();
    match router::dispatch(&url_str) {
        Ok(id) => {
            *request_id_out = id;
            1
        }
        Err(e) => {
            eprintln!("[AeonRouter] dispatch error for '{}': {e}", url_str);
            0
        }
    }
}

/// Tear down the router. Call at browser shutdown.
#[no_mangle]
pub extern "C" fn aeon_router_shutdown() {
    router::RouterEngine::global_shutdown();
}

// ---------------------------------------------------------------------------
// Download Manager FFI
// ---------------------------------------------------------------------------

/// Enqueue a download. Supports http(s), ftp(s), magnet, .torrent paths.
/// Returns a download task ID, or 0 on error.
///
/// # IT TROUBLESHOOTING
/// - Magnet fails: DHT bootstrap nodes may be blocked by firewall.
///   Check that UDP is not filtered on port 6881-6889.
/// - FTP fails: passive mode required behind NAT. PASV is default.
/// - Seeding: DISABLED. seed_ratio=0.0, seed_time=0 enforced in code.
#[no_mangle]
pub unsafe extern "C" fn aeon_download_enqueue(
    url:      *const c_char,
    dest_dir: *const c_char,
) -> c_uint {
    if url.is_null() || dest_dir.is_null() {
        return 0;
    }
    let url_s  = CStr::from_ptr(url).to_string_lossy();
    let dest_s = CStr::from_ptr(dest_dir).to_string_lossy();
    downloader::enqueue(&url_s, &dest_s).unwrap_or(0)
}

/// Cancel a running download by task ID.
#[no_mangle]
pub extern "C" fn aeon_download_cancel(task_id: c_uint) {
    downloader::cancel(task_id);
}

/// Get download progress (0–100). Returns 255 on error/unknown ID.
#[no_mangle]
pub extern "C" fn aeon_download_progress(task_id: c_uint) -> u8 {
    downloader::progress(task_id).unwrap_or(255)
}

// ---------------------------------------------------------------------------
// Tor FFI
// ---------------------------------------------------------------------------

/// Start the Tor SOCKS5 proxy via Arti. Binds on 127.0.0.1:9150.
/// Takes 15-60 seconds to bootstrap.
#[no_mangle]
pub extern "C" fn aeon_tor_start() -> c_int {
    match tor::start_tor_socks5("127.0.0.1:9150") {
        Ok(_)  => 1,
        Err(e) => { eprintln!("[AeonTor] {e}"); 0 }
    }
}

/// Stop the Tor proxy.
#[no_mangle]
pub extern "C" fn aeon_tor_stop() {
    tor::stop_tor();
}

// ---------------------------------------------------------------------------
// Gemini FFI
// ---------------------------------------------------------------------------

/// Fetch a Gemini URL. Response body is written to `buf` (max `buf_len` bytes).
/// Returns actual bytes written, or 0 on error.
#[no_mangle]
pub unsafe extern "C" fn aeon_gemini_fetch(
    url:     *const c_char,
    buf:     *mut c_char,
    buf_len: usize,
) -> usize {
    if url.is_null() || buf.is_null() || buf_len == 0 {
        return 0;
    }
    let url_s = CStr::from_ptr(url).to_string_lossy();
    match gemini::fetch(&url_s) {
        Ok(body) => {
            let n = body.len().min(buf_len - 1);
            std::ptr::copy_nonoverlapping(body.as_ptr(), buf as *mut u8, n);
            *buf.add(n) = 0; // null terminate
            n
        }
        Err(e) => {
            eprintln!("[AeonGemini] {e}");
            0
        }
    }
}
