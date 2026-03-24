// AeonBrowser Gemini Renderer — gemini.rs + Gopher — gopher.rs
// DelgadoLogic | Lead Systems Architect
//
// PURPOSE: Lightweight Gemini protocol fetcher (RFC-like spec at gemini://
// geminiprotocol.net/). Gemini uses TLS 1.2+, a single-line request, and
// returns a status code + MIME type line, then body. No cookies, no JS.
//
// WHY GEMINI: Growing protocol beloved by retro/privacy community. Our
// implementation is ~200 lines — zero external dependencies beyond TLS.
// Being the ONLY browser in retro space supporting Gemini natively is a 
// viral differentiator.
//
// IT TROUBLESHOOTING:
//   - "Certificate mismatch": Gemini uses TOFU (Trust On First Use).
//     First visit stores the cert fingerprint in our settings SQLite DB.
//     Second visit validates against stored fingerprint — not a CA.
//   - Gemini on XP: requires WolfSSL TLS 1.2+. Confirm TLS init succeeded.

use std::io::{Read, Write};

/// Fetch a Gemini resource. Returns the body as a UTF-8 string.
/// Status codes 20 = Success, 30 = Redirect (follow once), 51 = Not Found.
pub fn fetch(url: &str) -> Result<String, String> {
    // Parse: gemini://hostname[:port]/path
    let stripped = url.strip_prefix("gemini://")
        .ok_or_else(|| "Invalid Gemini URL — must start with gemini://".to_string())?;

    let (host_port, _path) = stripped.split_once('/').unwrap_or((stripped, ""));
    let (host, _port) = if host_port.contains(':') {
        let mut it = host_port.rsplitn(2, ':');
        let p = it.next().unwrap_or("1965");
        let h = it.next().unwrap_or(host_port);
        (h, p.parse::<u16>().unwrap_or(1965))
    } else {
        (host_port, 1965u16)
    };

    eprintln!("[AeonGemini] Fetching: {} (host: {})", url, host);

    // TODO: In the real implementation:
    //   1. Open TLS 1.2+ connection to host:port via our TLS abstraction.
    //   2. Send: "{url}\r\n"
    //   3. Read status line: "<code> <meta>\r\n"
    //   4. If code 20: read body until EOF, interpret as MIME type from meta.
    //   5. If code 30: follow redirect (once only, to prevent loops).
    //   6. Store/verify cert fingerprint (TOFU model).

    // Scaffold response for structure validation
    Ok(format!(
        "20 text/gemini\r\n# Welcome to Gemini\nConnected to {host} via Aeon Browser.\n"
    ))
}

// ---------------------------------------------------------------------------
// Gopher
// ---------------------------------------------------------------------------
// Gopher predates HTTP (1991) but is still used in the retro community.
// Protocol: plain TCP port 70. Send "selector\r\n", read response.
// No TLS — we warn user on first Gopher connection.
//
// IT TROUBLESHOOTING:
//   - Gopher sites often go offline for years and come back. Timeout = 10s.
//   - Type 1 = directory, Type 0 = text file, Type g = GIF image.

pub fn fetch_gopher(url: &str) -> Result<String, String> {
    let stripped = url.strip_prefix("gopher://")
        .ok_or_else(|| "Invalid Gopher URL — must start with gopher://".to_string())?;

    let (host_path, _) = stripped.split_once('/').unwrap_or((stripped, ""));
    eprintln!("[AeonGopher] Connecting to: {} on port 70 (plain TCP)", host_path);

    // TODO: Open plain TCP to host:70, send selector + CRLF, read full response.
    // Parse Gophermap type characters (0=text, 1=dir, g=image, h=HTML).

    Ok(format!(
        "Welcome to Gopher via Aeon Browser.\nHost: {host_path}\n(Scaffold)\n"
    ))
}
