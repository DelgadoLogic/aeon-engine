// AeonBrowser — router/src/lib.rs
// DelgadoLogic | Rust Network Team
//
// C-ABI protocol router — compiled to aeon_router.dll.
// Dispatches aeon:// special protocols + tor:// + gemini:// + ipfs://.
//
// Called from C++ via:
//   AeonRouter_Navigate(url, callback)
//   AeonRouter_GetStatus()
//   AeonRoute result = AeonRouter_Dispatch(url)
//
// All functions are #[no_mangle] extern "C".
// Async operations are driven by a Tokio runtime spawned at DLL init time.
// Blocking C callbacks are called from a dedicated dispatcher thread.

#![allow(non_snake_case, dead_code)]

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::sync::{Arc, OnceLock};
use tokio::runtime::Runtime;

// ─── Tokio singleton ─────────────────────────────────────────────────────────
static RUNTIME: OnceLock<Arc<Runtime>> = OnceLock::new();

fn rt() -> Arc<Runtime> {
    RUNTIME.get_or_init(|| {
        Arc::new(
            tokio::runtime::Builder::new_multi_thread()
                .worker_threads(2)
                .thread_name("aeon-router")
                .enable_all()
                .build()
                .expect("tokio runtime"),
        )
    })
    .clone()
}

// ─── C-ABI types ─────────────────────────────────────────────────────────────
#[repr(C)]
pub enum Protocol {
    Http      = 0,
    Https     = 1,
    Tor       = 2,   // proxied over tor://
    Gemini    = 3,   // gemini:// RFC 2015 subset
    Gopher    = 4,   // gopher+
    Ipfs      = 5,   // ipfs://CID
    Aeon      = 6,   // aeon://newtab, aeon://settings, etc.
    Magnet    = 7,   // magnet: — forwarded to DownloadManager
    Unknown   = 255,
}

#[repr(C)]
pub enum RouteStatus {
    Ok           = 0,
    TorNotReady  = 1,
    DnsBlocked   = 2,
    Timeout      = 3,
    Error        = 255,
}

#[repr(C)]
pub struct AeonRoute {
    pub protocol:       Protocol,
    pub status:         RouteStatus,
    /// The actual URL/resource to load (may differ from input — e.g. IPFS gateway rewrite)
    pub resolved_url:   *mut c_char,  // caller must free with AeonRouter_Free
    /// Optional proxy URL (socks5://127.0.0.1:9050 for Tor, etc.)
    pub proxy_url:      *mut c_char,
    pub use_ech:        u8,   // 1 = request ECH from engine
    pub tls_1_3_only:   u8,
}

// Callback from C++ when navigation result arrives
pub type NavigateCallback = extern "C" fn(
    url: *const c_char,
    status: RouteStatus,
    proxy: *const c_char,
);

// ─── DLL init / deinit ───────────────────────────────────────────────────────
/// Called once by TierDispatcher when loading the DLL.
#[no_mangle]
pub extern "C" fn AeonRouter_Init() -> i32 {
    let _ = rt(); // Spin up Tokio
    // tracing_subscriber::fmt::init();
    eprintln!("[aeon_router] Initialized. Tokio runtime ready.");
    0
}

#[no_mangle]
pub extern "C" fn AeonRouter_Shutdown() {
    eprintln!("[aeon_router] Shutdown.");
    // Runtime is dropped here via OnceLock when the DLL unloads
}

// ─── Protocol detection ───────────────────────────────────────────────────────
fn detect_protocol(url: &str) -> Protocol {
    if url.starts_with("aeon://")          { Protocol::Aeon   }
    else if url.starts_with("tor://")      { Protocol::Tor    }
    else if url.starts_with("gemini://")   { Protocol::Gemini }
    else if url.starts_with("gopher://")   { Protocol::Gopher }
    else if url.starts_with("ipfs://")     { Protocol::Ipfs   }
    else if url.starts_with("magnet:")     { Protocol::Magnet }
    else if url.starts_with("https://")    { Protocol::Https  }
    else if url.starts_with("http://")     { Protocol::Http   }
    else                                   { Protocol::Unknown }
}

// ─── IPFS gateway rewrite ────────────────────────────────────────────────────
/// Tries local IPFS daemon first, falls back to public gateway.
fn rewrite_ipfs(url: &str) -> String {
    // ipfs://Qm... → http://localhost:8080/ipfs/Qm...
    let cid = url.trim_start_matches("ipfs://");
    // TODO: probe localhost:8080 — if reachable, use local gateway
    format!("https://gateway.ipfs.io/ipfs/{}", cid)
}

// ─── Gemini fetch ─────────────────────────────────────────────────────────────
/// Minimal Gemini/TLS1.3 fetch. Returns body as raw bytes.
/// Gemini response line: "<STATUS> <META>\r\n<BODY>"
async fn fetch_gemini(url: &str) -> Result<Vec<u8>, String> {
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    use tokio::net::TcpStream;

    let rest = url.trim_start_matches("gemini://");
    let (host_port, _path) = rest.split_once('/').unwrap_or((rest, ""));
    let (host, port) = if host_port.contains(':') {
        let mut p = host_port.splitn(2, ':');
        let h = p.next().unwrap_or("localhost");
        let pt = p.next().and_then(|s| s.parse::<u16>().ok()).unwrap_or(1965);
        (h.to_owned(), pt)
    } else {
        (host_port.to_owned(), 1965u16)
    };

    let addr = format!("{}:{}", host, port);
    let stream = TcpStream::connect(&addr).await
        .map_err(|e| format!("connect: {}", e))?;

    // Wrap in rustls TLS 1.3
    // Simplified: use a plain TCP connection for the stub.
    // Production: integrate rustls ClientConfig with webpki-roots.
    let mut stream = stream; // TODO: replace with TlsStream
    let req = format!("{}\r\n", url);
    stream.write_all(req.as_bytes()).await.map_err(|e| e.to_string())?;

    let mut buf = Vec::new();
    stream.read_to_end(&mut buf).await.map_err(|e| e.to_string())?;
    Ok(buf)
}

// ─── Gopher fetch ─────────────────────────────────────────────────────────────
async fn fetch_gopher(url: &str) -> Result<Vec<u8>, String> {
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    use tokio::net::TcpStream;

    let rest = url.trim_start_matches("gopher://");
    let (host_port, path) = rest.split_once('/').unwrap_or((rest, ""));
    let (host, port) = if host_port.contains(':') {
        let mut p = host_port.splitn(2, ':');
        (p.next().unwrap_or("localhost").to_owned(),
         p.next().and_then(|s| s.parse::<u16>().ok()).unwrap_or(70))
    } else {
        (host_port.to_owned(), 70u16)
    };

    let mut stream = TcpStream::connect(format!("{}:{}", host, port)).await
        .map_err(|e| e.to_string())?;
    let req = format!("/{}\r\n", path);
    stream.write_all(req.as_bytes()).await.map_err(|e| e.to_string())?;

    let mut buf = Vec::new();
    stream.read_to_end(&mut buf).await.map_err(|e| e.to_string())?;
    Ok(buf)
}

// ─── Main dispatch ────────────────────────────────────────────────────────────
/// Synchronous dispatch — C++ calls this and gets back a route struct.
/// The caller must free route.resolved_url and route.proxy_url with AeonRouter_Free.
#[no_mangle]
pub extern "C" fn AeonRouter_Dispatch(url_cstr: *const c_char) -> AeonRoute {
    let url = unsafe {
        if url_cstr.is_null() { return err_route(); }
        match CStr::from_ptr(url_cstr).to_str() {
            Ok(s) => s.to_owned(),
            Err(_) => return err_route(),
        }
    };

    let proto = detect_protocol(&url);

    match proto {
        Protocol::Aeon => {
            // Internal pages: pass back as-is, no proxy
            AeonRoute {
                protocol:     Protocol::Aeon,
                status:       RouteStatus::Ok,
                resolved_url: cstring_ptr(&url),
                proxy_url:    null_cstr(),
                use_ech:      0,
                tls_1_3_only: 0,
            }
        }

        Protocol::Magnet => {
            // Forward to C++ DownloadManager
            AeonRoute {
                protocol:     Protocol::Magnet,
                status:       RouteStatus::Ok,
                resolved_url: cstring_ptr(&url),
                proxy_url:    null_cstr(),
                use_ech:      0,
                tls_1_3_only: 0,
            }
        }

        Protocol::Ipfs => {
            let gateway = rewrite_ipfs(&url);
            AeonRoute {
                protocol:     Protocol::Ipfs,
                status:       RouteStatus::Ok,
                resolved_url: cstring_ptr(&gateway),
                proxy_url:    null_cstr(),
                use_ech:      0,
                tls_1_3_only: 0,
            }
        }

        Protocol::Tor => {
            // Route via SOCKS5 Tor proxy (127.0.0.1:9050)
            // In production: spin up embedded Arti and use its SOCKS port
            let http_url = url.replacen("tor://", "https://", 1);
            AeonRoute {
                protocol:     Protocol::Tor,
                status:       RouteStatus::Ok,
                resolved_url: cstring_ptr(&http_url),
                proxy_url:    cstring_ptr("socks5://127.0.0.1:9050"),
                use_ech:      1,
                tls_1_3_only: 1,
            }
        }

        Protocol::Gemini | Protocol::Gopher => {
            // Block until fetch completes (Tokio will handle async internally)
            let result = rt().block_on(async {
                if matches!(proto, Protocol::Gemini) {
                    fetch_gemini(&url).await
                } else {
                    fetch_gopher(&url).await
                }
            });
            match result {
                Ok(_body) => {
                    // TODO: body → data: URI for rendering by engine
                    // For now: return the original URL, engine stub handles it
                    AeonRoute {
                        protocol:     proto,
                        status:       RouteStatus::Ok,
                        resolved_url: cstring_ptr(&url),
                        proxy_url:    null_cstr(),
                        use_ech:      0,
                        tls_1_3_only: 0,
                    }
                }
                Err(e) => {
                    eprintln!("[aeon_router] {} error: {}", url, e);
                    AeonRoute {
                        protocol:     proto,
                        status:       RouteStatus::Error,
                        resolved_url: cstring_ptr(&url),
                        proxy_url:    null_cstr(),
                        use_ech:      0,
                        tls_1_3_only: 0,
                    }
                }
            }
        }

        Protocol::Https => {
            // Normal HTTPS: request ECH and TLS 1.3 minimum
            AeonRoute {
                protocol:     Protocol::Https,
                status:       RouteStatus::Ok,
                resolved_url: cstring_ptr(&url),
                proxy_url:    null_cstr(),
                use_ech:      1,
                tls_1_3_only: 0, // Allow 1.2 for compatibility
            }
        }

        _ => {
            AeonRoute {
                protocol:     Protocol::Http,
                status:       RouteStatus::Ok,
                resolved_url: cstring_ptr(&url),
                proxy_url:    null_cstr(),
                use_ech:      0,
                tls_1_3_only: 0,
            }
        }
    }
}

/// Free a string allocated by AeonRouter_Dispatch.
#[no_mangle]
pub extern "C" fn AeonRouter_Free(ptr: *mut c_char) {
    if ptr.is_null() { return; }
    unsafe { drop(CString::from_raw(ptr)); }
}

/// Async navigate with callback — fire-and-forget.
#[no_mangle]
pub extern "C" fn AeonRouter_Navigate(
    url_cstr: *const c_char,
    callback: NavigateCallback,
) {
    let url = unsafe {
        if url_cstr.is_null() { return; }
        match CStr::from_ptr(url_cstr).to_str() {
            Ok(s) => s.to_owned(),
            Err(_) => return,
        }
    };

    rt().spawn(async move {
        let route = AeonRouter_Dispatch(
            CString::new(url.as_str()).unwrap().as_ptr()
        );
        callback(route.resolved_url, RouteStatus::Ok, route.proxy_url);
        AeonRouter_Free(route.resolved_url);
        AeonRouter_Free(route.proxy_url);
    });
}

// ─── Internal helpers ─────────────────────────────────────────────────────────
fn cstring_ptr(s: &str) -> *mut c_char {
    match CString::new(s) {
        Ok(cs) => cs.into_raw(),
        Err(_) => null_cstr(),
    }
}

fn null_cstr() -> *mut c_char {
    CString::new("").unwrap().into_raw()
}

fn err_route() -> AeonRoute {
    AeonRoute {
        protocol:     Protocol::Unknown,
        status:       RouteStatus::Error,
        resolved_url: null_cstr(),
        proxy_url:    null_cstr(),
        use_ech:      0,
        tls_1_3_only: 0,
    }
}
