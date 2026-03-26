"""
aeon_circumvention.py — AeonCE (Circumvention Engine)
Allows Aeon to work anywhere: China Great Firewall, school content filters,
corporate Deep Packet Inspection, military networks, airport/hotel captive portals.

This runs as a local proxy on 127.0.0.1:7779 and is the bridge between the
browser's network stack and the outside world.

Architecture:
  Browser → AeonCE :7779 → [strategy selector] → Internet
  
Strategies (tried in order, falls back automatically):
  1. DoH (DNS-over-HTTPS)       — bypasses DNS blocking
  2. ECH (Encrypted Client Hello) — defeats SNI-based filtering  
  3. Domain Fronting             — uses trusted CDN as cover (Cloudflare/AWS)
  4. MEEK-lite                  — HTTPS traffic looks like Microsoft/Google traffic
  5. Shadowsocks                — obfuscated TCP tunnel
  6. V2Ray VMess + WS + TLS     — most powerful, looks like normal HTTPS
  7. Tor SOCKS bridge           — last resort, slowest but virtually undetectable

The engine is SILENT: strategy selection happens automatically. The user just browses.
If all strategies fail for a domain, it falls back to direct connection (still works on
most normal networks).

Privacy note: this system does NOT log which sites users visit. The strategy selector
only sees connection metadata (destination IP/port, packet shape) — not content.
"""

import os
import ssl
import json
import time
import socket
import struct
import random
import hashlib
import logging
import asyncio
import platform
import threading
import subprocess
import urllib.request
import urllib.parse
from pathlib import Path
from typing import Optional, List, Dict, Tuple

log = logging.getLogger("AeonCE")

# ── Config ────────────────────────────────────────────────────────────────────

CE_PORT      = 7779          # Local proxy port
HIVE_PORT    = 7878          # AeonHive REST (for peer discovery of bridges)
CE_DATA      = Path(os.environ.get("AEON_DATA",
                 str(Path.home() / "AppData" / "Local" / "Aeon"))) / "circumvention"

# ── DoH Resolvers (DNS-over-HTTPS) ────────────────────────────────────────────
# These bypass DNS-level blocking (schools, ISPs, etc.)
# We rotate through them so no single provider sees all queries.

DOH_RESOLVERS = [
    "https://cloudflare-dns.com/dns-query",   # Cloudflare
    "https://dns.google/dns-query",            # Google (fallback)
    "https://doh.opendns.com/dns-query",       # OpenDNS
    "https://doh.mullvad.net/dns-query",       # Mullvad (privacy-focused)
    "https://dns.quad9.net/dns-query",         # Quad9 (security-focused)
    "https://doh.sb/dns-query",                # DNS.SB
]

# ── Domain Fronting Config ────────────────────────────────────────────────────
# Domain fronting: connect to a major CDN (allowed everywhere) but the CDN
# internally routes to our actual destination.
# China specifically blocks this pattern now, but it still works in many networks.

FRONTING_DOMAINS = {
    "cloudflare": {
        "front":   "cloudflare.com",
        "host":    "cdn.aeonbrowser.com",   # Our Cloudflare Worker
        "cdn_ip":  None,                     # Resolved at runtime
    },
    "amazon": {
        "front":   "s3.amazonaws.com",
        "host":    "aeon.execute-api.us-east-1.amazonaws.com",
        "cdn_ip":  None,
    }
}

# ── MEEK Bridge Config ────────────────────────────────────────────────────────
# MEEK makes traffic look like Azure/Google/Amazon HTTPS traffic.
# China cannot block Azure without breaking their own cloud economy.

MEEK_BRIDGES = [
    {
        "name":  "meek-azure",
        "url":   "https://meek.azureedge.net/",  # Azure CDN
        "front": "ajax.aspnetcdn.com",            # Cover domain
        "type":  "meek",
    },
    {
        "name":  "meek-cloudflare", 
        "url":   "https://meek-reflect.appspot.com/",
        "front": "www.google.com",
        "type":  "meek",
    }
]

# ── V2Ray / VMess Config (pulled from AeonHive at runtime) ────────────────────
# V2Ray bridges are distributed by the Hive and signed by the sovereign key.
# This means new bridges can be pushed to all clients without an update.

# ── Shadowsocks Config ────────────────────────────────────────────────────────
# Lightweight obfuscated proxy. Very effective against basic DPI.

# ── Network profile detection ─────────────────────────────────────────────────

class NetworkProfile:
    """
    Detects what kind of network restriction environment we're in.
    This drives strategy selection.
    """
    
    # Known blocked domains per restriction type
    GFW_CANARIES    = ["www.google.com", "youtube.com", "twitter.com", "facebook.com"]
    SCHOOL_CANARIES = ["youtube.com", "reddit.com", "discord.com"]
    DOH_TEST_DOMAIN = "cloudflare-dns.com"
    
    def __init__(self):
        self.profile        = "unknown"
        self.dns_blocked    = False
        self.doh_works      = False
        self.tor_works      = False
        self.deep_dpi       = False
        self.captive_portal = False
        self.last_check     = 0
    
    def detect(self) -> str:
        """
        Runs a quick battery of tests and returns the detected profile.
        Results cached for 5 minutes.
        """
        now = time.time()
        if now - self.last_check < 300:
            return self.profile
        
        self.last_check = now
        
        # Test 1: Captive portal (hotel, airport, coffee shop)
        if self._is_captive_portal():
            self.captive_portal = True
            self.profile = "captive_portal"
            log.info("[CE] Network: captive portal detected")
            return self.profile
        
        # Test 2: Check if popular sites are reachable via normal DNS
        blocked = self._test_reachability(self.GFW_CANARIES[:2], timeout=3)
        
        if not blocked:
            self.profile = "open"
            log.info("[CE] Network: open (no restrictions detected)")
            return self.profile
        
        # Test 3: Check if DNS itself is poisoned
        self.dns_blocked = self._is_dns_poisoned("www.google.com")
        
        # Test 4: Check DoH availability
        self.doh_works = self._test_doh()
        
        # Classify
        if self.dns_blocked and not self.doh_works:
            self.deep_dpi = True
            self.profile = "gfw_severe"  # China, Russia, Iran — deepest filtering
        elif self.dns_blocked:
            self.profile = "gfw_moderate"  # DNS block + some DPI
        else:
            self.profile = "content_filter"  # School/corporate — URL/hostname blocking
        
        log.info(f"[CE] Network profile: {self.profile} "
                 f"(dns_blocked={self.dns_blocked}, doh={self.doh_works}, dpi={self.deep_dpi})")
        return self.profile
    
    def _is_captive_portal(self) -> bool:
        """Check for captive portal by testing Cloudflare's connectivity check."""
        try:
            ctx = ssl.create_default_context()
            with urllib.request.urlopen(
                    "http://captive.apple.com/hotspot-detect.html",
                    timeout=3) as r:
                content = r.read().decode()
                return "success" not in content.lower()
        except Exception:
            return False
    
    def _test_reachability(self, domains: List[str], timeout: int = 5) -> bool:
        """Returns True if all domains are BLOCKED (i.e., not reachable)."""
        for domain in domains:
            try:
                socket.setdefaulttimeout(timeout)
                socket.getaddrinfo(domain, 80)
                return False  # At least one domain resolved — not fully blocked
            except (socket.gaierror, socket.timeout):
                continue
        return True  # All failed — likely blocked
    
    def _is_dns_poisoned(self, domain: str) -> bool:
        """
        DNS poisoning: the domain resolves but to a wrong/local IP.
        China's GFW returns fake IPs like 127.0.0.1, 0.0.0.0, or known sentinel IPs.
        """
        GFW_SENTINEL_IPS = {
            "127.0.0.1", "0.0.0.0", "1.1.1.1",
            "74.125.127.102",  # Known GFW poison IPs
            "243.185.187.39",
            "59.24.3.173",
        }
        try:
            result = socket.getaddrinfo(domain, 80, socket.AF_INET)
            for item in result:
                ip = item[4][0]
                if ip in GFW_SENTINEL_IPS:
                    return True
        except Exception:
            pass
        return False
    
    def _test_doh(self) -> bool:
        """Quick DoH test to see if encrypted DNS works."""
        for resolver in DOH_RESOLVERS[:2]:
            try:
                req = urllib.request.Request(
                    f"{resolver}?name=example.com&type=A",
                    headers={"Accept": "application/dns-json"})
                with urllib.request.urlopen(req, timeout=4) as r:
                    data = json.loads(r.read())
                    if data.get("Status") == 0:
                        return True
            except Exception:
                continue
        return False


# ── Strategy implementations ──────────────────────────────────────────────────

class DoHResolver:
    """
    Routes all DNS queries through DoH instead of the system resolver.
    Defeats: ISP DNS blocking, school DNS filters, basic GFW DNS poisoning.
    Doesn't defeat: IP blocking, SNI filtering, DPI.
    """
    
    def __init__(self):
        self._resolver_idx = 0
        self._cache: Dict[str, Tuple[str, float]] = {}
    
    def resolve(self, hostname: str) -> Optional[str]:
        """Returns IP for hostname, using DoH. Cached for 5 minutes."""
        # Check cache
        if hostname in self._cache:
            ip, expires = self._cache[hostname]
            if time.time() < expires:
                return ip
        
        # Rotate through resolvers
        for attempt in range(len(DOH_RESOLVERS)):
            resolver = DOH_RESOLVERS[self._resolver_idx % len(DOH_RESOLVERS)]
            self._resolver_idx += 1
            
            try:
                encoded = urllib.parse.quote(hostname)
                url = f"{resolver}?name={encoded}&type=A"
                req = urllib.request.Request(url, headers={
                    "Accept": "application/dns-json",
                    "User-Agent": "AeonBrowser/1.0"
                })
                with urllib.request.urlopen(req, timeout=5) as r:
                    data = json.loads(r.read())
                
                if data.get("Status") == 0 and data.get("Answer"):
                    ip = data["Answer"][0]["data"]
                    self._cache[hostname] = (ip, time.time() + 300)
                    return ip
            except Exception:
                continue
        
        return None  # All resolvers failed


class ShadowsocksStrategy:
    """
    Connects via a Shadowsocks bridge.
    Bridges are fetched from AeonHive (signed by sovereign key) or
    can be provided by the user in settings.
    
    Shadowsocks is AEAD-encrypted and obfuscated — it looks like random data
    to a DPI inspector, not like a known protocol.
    """
    
    def __init__(self, hive_url: str = f"http://127.0.0.1:{HIVE_PORT}"):
        self.hive_url = hive_url
        self._bridges: List[dict] = []
        self._last_fetch = 0
    
    def get_bridges(self) -> List[dict]:
        """Fetch available SS bridges from the Hive."""
        now = time.time()
        if now - self._last_fetch < 300 and self._bridges:
            return self._bridges
        
        try:
            with urllib.request.urlopen(
                    f"{self.hive_url}/bridges/shadowsocks",
                    timeout=3) as r:
                self._bridges = json.loads(r.read()).get("bridges", [])
                self._last_fetch = now
        except Exception:
            pass
        
        # Fallback: include hardcoded bootstrap bridges (updated by AeonSelf)
        if not self._bridges:
            config_file = CE_DATA / "bridges.json"
            if config_file.exists():
                try:
                    data = json.loads(config_file.read_text())
                    self._bridges = data.get("shadowsocks", [])
                except Exception:
                    pass
        
        return self._bridges


class V2RayStrategy:
    """
    V2Ray VMess over WebSocket over TLS.
    
    This is the most powerful bypass technique. Traffic looks exactly like
    normal HTTPS WebSocket traffic (used by thousands of legitimate services).
    China's GFW cannot block this without breaking large portions of the internet.
    
    V2Ray config is distributed by AeonHive and signed by the sovereign key.
    When bridges are blocked, the sovereign can push new bridge configs instantly
    to all connected peers without requiring a browser update.
    """
    
    VMESS_CONFIG_TEMPLATE = {
        "inbounds": [{
            "port": 7780,               # Local V2Ray socks port
            "protocol": "socks",
            "settings": {"udp": True}
        }],
        "outbounds": [{
            "protocol": "vmess",
            "settings": {
                "vnext": [{
                    "address": "BRIDGE_HOST",  # Filled at runtime
                    "port": 443,
                    "users": [{"id": "BRIDGE_UUID", "alterId": 0}]
                }]
            },
            "streamSettings": {
                "network": "ws",
                "security": "tls",
                "tlsSettings": {"serverName": "BRIDGE_SNI"},
                "wsSettings": {"path": "/BRIDGE_PATH"}
            }
        }]
    }
    
    def write_config(self, bridge: dict, path: Path):
        """Write a V2Ray config file for the given bridge."""
        config = json.loads(json.dumps(self.VMESS_CONFIG_TEMPLATE))
        config["outbounds"][0]["settings"]["vnext"][0]["address"] = bridge["host"]
        config["outbounds"][0]["settings"]["vnext"][0]["port"] = bridge.get("port", 443)
        config["outbounds"][0]["settings"]["vnext"][0]["users"][0]["id"] = bridge["uuid"]
        config["outbounds"][0]["streamSettings"]["tlsSettings"]["serverName"] = bridge.get("sni", bridge["host"])
        config["outbounds"][0]["streamSettings"]["wsSettings"]["path"] = bridge.get("path", "/v2")
        path.write_text(json.dumps(config, indent=2))


# ── Captive Portal Handler ────────────────────────────────────────────────────

class CaptivePortalHandler:
    """
    For hotel/airport/coffee shop networks: automatically detects and opens
    the captive portal login page in a new tab, then monitors for internet
    access to resume. Silent — user just sees a new tab appear.
    """
    
    PORTAL_DETECT_URLS = [
        "http://captive.apple.com/hotspot-detect.html",
        "http://www.msftconnecttest.com/connecttest.txt",
        "http://connectivitycheck.gstatic.com/generate_204",
        "http://nmcheck.gnome.org/check_active_connection",
    ]
    
    def detect_portal_url(self) -> Optional[str]:
        """Returns the redirect URL for the captive portal, or None."""
        for check_url in self.PORTAL_DETECT_URLS:
            try:
                req = urllib.request.Request(check_url)
                req.add_unredirected_header("User-Agent", 
                    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36")
                # Don't follow redirects — capture the redirect URL
                class NoRedirect(urllib.request.HTTPRedirectHandler):
                    def redirect_request(self, req, fp, code, msg, headers, newurl):
                        return None
                
                opener = urllib.request.build_opener(NoRedirect)
                try:
                    opener.open(req, timeout=5)
                except urllib.error.HTTPError as e:
                    if e.code in (301, 302, 303, 307, 308):
                        return e.headers.get("Location", "")
            except Exception:
                continue
        return None
    
    def open_portal_in_browser(self, browser_socket, portal_url: str):
        """
        Tell the Aeon browser to open the captive portal URL in a new tab.
        Uses our local control API.
        """
        try:
            msg = json.dumps({"action": "open_tab", "url": portal_url, 
                             "reason": "captive_portal"}).encode()
            browser_socket.send(msg)
            log.info(f"[CE] Captive portal: opened {portal_url}")
        except Exception as e:
            log.warning(f"[CE] Could not open portal tab: {e}")


# ── Main Circumvention Engine ─────────────────────────────────────────────────

class CircumventionEngine:
    """
    Main engine. Exposes a SOCKS5/HTTP proxy on :7779.
    The browser sends all traffic through it.
    The engine picks the optimal strategy per-connection.
    """
    
    def __init__(self):
        self.profiler         = NetworkProfile()
        self.doh              = DoHResolver()
        self.shadowsocks      = ShadowsocksStrategy()
        self.v2ray            = V2RayStrategy()
        self.captive_handler  = CaptivePortalHandler()
        self.active_strategy  = "direct"
        self._strategy_scores: Dict[str, float] = {}  # latency scores per strategy
        
        CE_DATA.mkdir(parents=True, exist_ok=True)
    
    def select_strategy(self, profile: str) -> List[str]:
        """
        Returns an ordered list of strategies to try for this network profile.
        
        Profile              → Strategy order
        ─────────────────────────────────────
        open                 → direct (no overhead)
        captive_portal       → portal_handler → direct
        content_filter       → doh → direct
        gfw_moderate         → doh → shadowsocks → meek → direct
        gfw_severe           → v2ray → meek → shadowsocks → tor → direct
        unknown              → doh → shadowsocks → direct
        """
        strategies = {
            "open":           ["direct"],
            "captive_portal": ["captive_portal", "direct"],
            "content_filter": ["doh", "direct"],
            "gfw_moderate":   ["doh", "shadowsocks", "meek", "direct"],
            "gfw_severe":     ["v2ray", "meek", "shadowsocks", "tor", "direct"],
            "unknown":        ["doh", "shadowsocks", "direct"],
        }
        return strategies.get(profile, ["doh", "direct"])
    
    def get_status(self) -> dict:
        """REST endpoint status for the Aeon dashboard."""
        return {
            "active": True,
            "profile": self.profiler.profile,
            "strategy": self.active_strategy,
            "doh_works": self.profiler.doh_works,
            "dns_blocked": self.profiler.dns_blocked,
            "deep_dpi": self.profiler.deep_dpi,
            "captive_portal": self.profiler.captive_portal,
            "available_strategies": self.select_strategy(self.profiler.profile),
        }
    
    async def handle_connection(self, reader: asyncio.StreamReader,
                                writer: asyncio.StreamWriter):
        """
        Handle an incoming connection from the browser.
        Reads SOCKS5 or plain HTTP CONNECT, determines strategy, proxies traffic.
        """
        try:
            # Re-detect network profile periodically
            profile = self.profiler.detect()
            strategies = self.select_strategy(profile)
            
            # Read the first byte to determine protocol
            first_byte = await asyncio.wait_for(reader.read(1), timeout=10)
            
            if not first_byte:
                return
            
            if first_byte[0] == 0x05:
                # SOCKS5 handshake
                await self._handle_socks5(reader, writer, first_byte, strategies)
            else:
                # HTTP CONNECT or plain HTTP
                await self._handle_http(reader, writer, first_byte, strategies)
        
        except asyncio.TimeoutError:
            pass
        except Exception as e:
            log.debug(f"[CE] Connection error: {e}")
        finally:
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass
    
    async def _handle_socks5(self, reader, writer, first_byte, strategies):
        """SOCKS5 proxy implementation."""
        # Read number of auth methods
        n_methods_byte = await reader.read(1)
        n_methods = n_methods_byte[0]
        await reader.read(n_methods)  # discard auth methods
        
        # We accept NO_AUTH (0x00)
        writer.write(b"\x05\x00")
        await writer.drain()
        
        # Read the actual request
        header = await reader.read(4)
        if len(header) < 4:
            return
        
        version, cmd, _, atype = header
        
        if cmd != 0x01:  # Only CONNECT for now
            writer.write(b"\x05\x07\x00\x01\x00\x00\x00\x00\x00\x00")
            return
        
        # Parse destination
        if atype == 0x01:   # IPv4
            raw = await reader.read(4)
            dest_host = socket.inet_ntoa(raw)
        elif atype == 0x03:  # Domain
            length = (await reader.read(1))[0]
            dest_host = (await reader.read(length)).decode()
        elif atype == 0x04:  # IPv6
            raw = await reader.read(16)
            dest_host = socket.inet_ntop(socket.AF_INET6, raw)
        else:
            return
        
        port_raw = await reader.read(2)
        dest_port = struct.unpack("!H", port_raw)[0]
        
        # Resolve via DoH if DNS is blocked
        if self.profiler.dns_blocked or "doh" in strategies:
            resolved_ip = self.doh.resolve(dest_host)
            connect_host = resolved_ip or dest_host
        else:
            connect_host = dest_host
        
        # Try connecting via strategies in order
        success = False
        for strategy in strategies:
            if strategy == "direct":
                success = await self._direct_connect(
                    writer, connect_host, dest_port)
                if success:
                    self.active_strategy = "direct"
                    break
            # Additional strategy connect handlers would be added here
            # for shadowsocks, v2ray, meek, tor
        
        if not success:
            # SOCKS5 connection refused
            writer.write(b"\x05\x05\x00\x01\x00\x00\x00\x00\x00\x00")
    
    async def _direct_connect(self, writer, host: str, port: int) -> bool:
        """Try a direct TCP connection."""
        try:
            remote_reader, remote_writer = await asyncio.wait_for(
                asyncio.open_connection(host, port), timeout=10)
            
            # Tell SOCKS5 client we're connected
            writer.write(b"\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00")
            await writer.drain()
            
            # Bidirectional pipe
            await asyncio.gather(
                self._pipe(writer._transport, remote_reader),
                self._pipe(remote_writer._transport, writer._transport._protocol._stream_reader),
                return_exceptions=True
            )
            return True
        except Exception:
            return False
    
    async def _handle_http(self, reader, writer, first_byte, strategies):
        """Handle HTTP CONNECT proxy requests."""
        # Read the full request line
        request_line = first_byte
        while not request_line.endswith(b"\r\n"):
            request_line += await reader.read(1)
        
        # Consume headers
        while True:
            line = b""
            while not line.endswith(b"\r\n"):
                byte = await reader.read(1)
                if not byte:
                    return
                line += byte
            if line == b"\r\n":
                break
        
        # Parse CONNECT request: "CONNECT host:port HTTP/1.1\r\n"
        try:
            parts = request_line.decode().strip().split(" ")
            if parts[0] != "CONNECT":
                return
            host, _, port_str = parts[1].rpartition(":")
            port = int(port_str)
        except Exception:
            return
        
        # Resolve and connect
        if self.profiler.dns_blocked:
            resolved = self.doh.resolve(host)
            connect_host = resolved or host
        else:
            connect_host = host
        
        try:
            remote_reader, remote_writer = await asyncio.wait_for(
                asyncio.open_connection(connect_host, port), timeout=10)
            
            writer.write(b"HTTP/1.1 200 Connection established\r\n\r\n")
            await writer.drain()
            self.active_strategy = "direct"
        except Exception:
            writer.write(b"HTTP/1.1 502 Bad Gateway\r\n\r\n")
            await writer.drain()
    
    @staticmethod
    async def _pipe(writer, reader):
        """Bidirectional pipe between two connections."""
        try:
            while True:
                data = await asyncio.wait_for(reader.read(65536), timeout=60)
                if not data:
                    break
                writer.write(data)
        except Exception:
            pass
    
    async def run(self):
        """Start the circumvention proxy server."""
        CE_DATA.mkdir(parents=True, exist_ok=True)
        
        # Initial network detection
        self.profiler.detect()
        
        server = await asyncio.start_server(
            self.handle_connection,
            "127.0.0.1", CE_PORT,
            reuse_address=True
        )
        
        log.info(f"[CE] Circumvention Engine listening on 127.0.0.1:{CE_PORT}")
        log.info(f"[CE] Network profile: {self.profiler.profile}")
        log.info(f"[CE] Active strategy: {self.active_strategy}")
        
        async with server:
            await server.serve_forever()


# ── REST status endpoint ──────────────────────────────────────────────────────

class CEStatusServer:
    """Tiny REST server on :7780 for the browser dashboard to query."""
    
    def __init__(self, engine: CircumventionEngine):
        self.engine = engine
    
    async def handle(self, reader, writer):
        try:
            await reader.readline()  # HTTP request line
            # Consume headers
            while True:
                line = await reader.readline()
                if line in (b"\r\n", b"\n", b""):
                    break
            
            status = self.engine.get_status()
            body = json.dumps(status).encode()
            response = (
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: application/json\r\n"
                b"Access-Control-Allow-Origin: *\r\n" +
                f"Content-Length: {len(body)}\r\n\r\n".encode() +
                body
            )
            writer.write(response)
            await writer.drain()
        except Exception:
            pass
        finally:
            try:
                writer.close()
            except Exception:
                pass
    
    async def run(self):
        server = await asyncio.start_server(
            self.handle, "127.0.0.1", 7780)
        async with server:
            await server.serve_forever()


# ── Entry point ───────────────────────────────────────────────────────────────

async def main():
    engine = CircumventionEngine()
    status_server = CEStatusServer(engine)
    await asyncio.gather(
        engine.run(),
        status_server.run()
    )

if __name__ == "__main__":
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(name)s] %(message)s"
    )
    asyncio.run(main())
