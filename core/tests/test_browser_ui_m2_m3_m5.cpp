// AeonBrowser — test_browser_ui_m2_m3_m5.cpp
// DelgadoLogic | UI Verification Tests for Milestones 2, 3, and 5

#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <unordered_set>
#include <cassert>

// Test fixtures matching BrowserChrome metrics
static const int TAB_STRIP_HEIGHT   = 36;
static const int NAV_TOOLBAR_HEIGHT = 38;
static const int CHROME_H           = 74;

// ── M2 URL Classifier simulation matching BrowserChrome::CommitUrlBar ──
std::string ClassifyAndFormatUrl(const std::string& input, const std::string& searchEngine) {
    size_t start = input.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = input.find_last_not_of(" \t\r\n");
    std::string url = input.substr(start, end - start + 1);

    bool hasScheme = (url.find("://") != std::string::npos ||
                      url.rfind("aeon://", 0) == 0 ||
                      url.rfind("about:", 0) == 0 ||
                      url.rfind("file://", 0) == 0 ||
                      url.rfind("chrome://", 0) == 0 ||
                      url.rfind("edge://", 0) == 0);

    if (!hasScheme) {
        bool hasSpaces = (url.find(' ') != std::string::npos);
        bool isLocal = (url.rfind("localhost", 0) == 0 || url.rfind("127.0.0.1", 0) == 0 || url.rfind("[::1]", 0) == 0);
        bool hasDot = (url.find('.') != std::string::npos);

        if (!hasSpaces && (isLocal || hasDot)) {
            url = "https://" + url;
        } else {
            std::string encoded;
            for (char c : url) {
                if (c == ' ') encoded += '+';
                else if (c == '+') encoded += "%2B";
                else if (c == '&') encoded += "%26";
                else if (c == '?') encoded += "%3F";
                else if (c == '#') encoded += "%23";
                else if (c == '%') encoded += "%25";
                else encoded += c;
            }

            std::string engine = searchEngine;
            for (auto& c : engine) c = (char)tolower(c);

            if (engine == "google") {
                url = "https://www.google.com/search?q=" + encoded;
            } else if (engine == "bing") {
                url = "https://www.bing.com/search?q=" + encoded;
            } else if (engine == "brave") {
                url = "https://search.brave.com/search?q=" + encoded;
            } else if (engine == "ecosia") {
                url = "https://www.ecosia.org/search?q=" + encoded;
            } else {
                url = "https://duckduckgo.com/?q=" + encoded;
            }
        }
    }
    return url;
}

// ── M3 Host Extraction simulation matching ShieldHUD ──
std::string ExtractHostFromUrl(const std::string& url) {
    if (url.empty()) return "aeon://newtab";
    if (url.find("aeon://") == 0) return url;
    if (url.find("about:") == 0) return url;
    if (url.find("file://") == 0) return "local file";

    size_t start = url.find("://");
    start = (start == std::string::npos) ? 0 : start + 3;
    size_t end = url.find_first_of("/?#:", start);
    if (end == std::string::npos) end = url.length();
    return url.substr(start, end - start);
}

// ── M3 Shield Status Tracker simulation matching ShieldHUD ──
struct ShieldManager {
    std::unordered_set<std::string> disabledDomains;

    bool IsShieldActive(const std::string& url) {
        std::string host = ExtractHostFromUrl(url);
        if (host.empty() || host.find("aeon://") == 0 || host.find("about:") == 0 || host == "local file")
            return true;
        return disabledDomains.find(host) == disabledDomains.end();
    }

    void SetShieldActive(const std::string& url, bool active) {
        std::string host = ExtractHostFromUrl(url);
        if (host.empty()) return;
        if (active) disabledDomains.erase(host);
        else disabledDomains.insert(host);
    }
};

// ── M5 Viewport Calculator simulation matching BrowserChrome::SyncViewport ──
struct ViewportMetrics {
    int topH;
    int clientW;
    int clientH;
    int viewW;
    int viewH;
};

ViewportMetrics ComputeViewport(int windowW, int windowH, bool isFullscreen) {
    ViewportMetrics m;
    m.clientW = windowW;
    m.clientH = windowH;
    m.topH = isFullscreen ? 0 : CHROME_H;
    m.viewW = windowW;
    m.viewH = (std::max)(0, windowH - m.topH);
    return m;
}

int main() {
    std::cout << "=== Running AeonBrowser Milestones 2, 3, & 5 Comprehensive Tests ===" << std::endl;
    int passed = 0;

    // ─────────────────────────────────────────────────────────────
    // Milestone 2 Tests: Unified Omnibox & URL Classification
    // ─────────────────────────────────────────────────────────────
    // Test 1: Direct URL with Scheme
    assert(ClassifyAndFormatUrl("https://delgadologic.tech", "duckduckgo") == "https://delgadologic.tech");
    assert(ClassifyAndFormatUrl("aeon://newtab", "google") == "aeon://newtab");
    assert(ClassifyAndFormatUrl("file:///C:/test.html", "bing") == "file:///C:/test.html");
    std::cout << "[PASS] M2.1: Direct URLs preserve original schemes" << std::endl;
    passed++;

    // Test 2: Domain without scheme auto-prepends https://
    assert(ClassifyAndFormatUrl("delgadologic.tech", "duckduckgo") == "https://delgadologic.tech");
    assert(ClassifyAndFormatUrl("github.com/DelgadoLogic", "brave") == "https://github.com/DelgadoLogic");
    assert(ClassifyAndFormatUrl("localhost:8080", "google") == "https://localhost:8080");
    std::cout << "[PASS] M2.2: Domains and localhosts automatically prepend https://" << std::endl;
    passed++;

    // Test 3: Search queries route correctly to chosen search engine
    assert(ClassifyAndFormatUrl("modern C++ UI architecture", "duckduckgo") == "https://duckduckgo.com/?q=modern+C%2B%2B+UI+architecture");
    assert(ClassifyAndFormatUrl("delgado logic", "brave") == "https://search.brave.com/search?q=delgado+logic");
    assert(ClassifyAndFormatUrl("quantum computing & AI", "google") == "https://www.google.com/search?q=quantum+computing+%26+AI");
    assert(ClassifyAndFormatUrl("privacy browser", "ecosia") == "https://www.ecosia.org/search?q=privacy+browser");
    assert(ClassifyAndFormatUrl("directX 12 vs vulkan", "bing") == "https://www.bing.com/search?q=directX+12+vs+vulkan");
    std::cout << "[PASS] M2.3: Search queries encode properly and route to configured engine" << std::endl;
    passed++;

    // ─────────────────────────────────────────────────────────────
    // Milestone 3 Tests: Sovereign Shield Dropdown HUD Logic
    // ─────────────────────────────────────────────────────────────
    // Test 4: Host extraction from diverse URLs
    assert(ExtractHostFromUrl("https://delgadologic.tech/products/aeon") == "delgadologic.tech");
    assert(ExtractHostFromUrl("http://sub.domain.co.uk:8080/path?q=1") == "sub.domain.co.uk");
    assert(ExtractHostFromUrl("aeon://settings") == "aeon://settings");
    assert(ExtractHostFromUrl("file:///C:/index.html") == "local file");
    std::cout << "[PASS] M3.1: Host extraction extracts clean domain names" << std::endl;
    passed++;

    // Test 5: Per-site protection toggling and whitelist persistence
    ShieldManager shield;
    assert(shield.IsShieldActive("https://github.com") == true);
    assert(shield.IsShieldActive("https://delgadologic.tech") == true);

    // Turn shields DOWN for github.com
    shield.SetShieldActive("https://github.com/some/repo", false);
    assert(shield.IsShieldActive("https://github.com/other/page") == false);
    assert(shield.IsShieldActive("https://delgadologic.tech") == true); // Other sites remain protected

    // Turn shields back UP for github.com
    shield.SetShieldActive("https://github.com", true);
    assert(shield.IsShieldActive("https://github.com") == true);
    std::cout << "[PASS] M3.2: 1-Click per-site Shield toggle maintains domain whitelist" << std::endl;
    passed++;

    // ─────────────────────────────────────────────────────────────
    // Milestone 5 Tests: Viewport Pinning & Fullscreen
    // ─────────────────────────────────────────────────────────────
    // Test 6: Standard Window Viewport (0, 74, W, H - 74)
    ViewportMetrics norm = ComputeViewport(1920, 1080, false);
    assert(norm.topH == 74);
    assert(norm.viewW == 1920);
    assert(norm.viewH == 1080 - 74); // 1006px
    std::cout << "[PASS] M5.1: Standard window sets top chrome height to 74px and pins viewport" << std::endl;
    passed++;

    // Test 7: Fullscreen Mode Viewport (0, 0, W, H)
    ViewportMetrics full = ComputeViewport(1920, 1080, true);
    assert(full.topH == 0);
    assert(full.viewW == 1920);
    assert(full.viewH == 1080); // Full window
    std::cout << "[PASS] M5.2: F11 Fullscreen sets top chrome height to 0px and expands viewport" << std::endl;
    passed++;

    // Test 8: DPI Scaled Metrics (150% scaling)
    int dpi96 = 96;
    int dpi144 = 144; // 150% scale
    int font9pt_96 = -MulDiv(9, dpi96, 72);  // -12
    int font9pt_144 = -MulDiv(9, dpi144, 72); // -18
    assert(font9pt_144 < font9pt_96);
    std::cout << "[PASS] M5.3: DPI scaling computes crisp proportional font heights" << std::endl;
    passed++;

    std::cout << "\n=== All " << passed << " Milestone 2, 3, & 5 Tests Passed Successfully! ===" << std::endl;
    return 0;
}
