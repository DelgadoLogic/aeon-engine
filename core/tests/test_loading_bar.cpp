// AeonBrowser — test_loading_bar.cpp
// DelgadoLogic | Verification Test Suite for Omnibox Loading Bar & Progress State Machine

#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <cassert>
#include <cmath>
#include <algorithm>

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "  [FAIL] " << msg << " (line " << __LINE__ << ")" << std::endl; \
            exit(1); \
        } \
    } while (0)

#define TEST_ASSERT_EQ(a, b, msg) \
    do { \
        if ((a) != (b)) { \
            std::cerr << "  [FAIL] " << msg << " (expected " << (b) << ", got " << (a) << " at line " << __LINE__ << ")" << std::endl; \
            exit(1); \
        } \
    } while (0)

// Helper mirroring ReverseMapUrl in BrowserChrome.cpp
static std::string ReverseMapUrl(const std::string& url) {
    if (url.rfind("file:///", 0) != 0) return url;

    struct { const char* suffix; const char* aeonUrl; } mappings[] = {
        { "newtab/newtab.html",        "aeon://newtab" },
        { "pages/intelligence.html",  "aeon://intelligence" },
        { "pages/settings.html",      "aeon://settings" },
        { "pages/history.html",       "aeon://history" },
        { "pages/bookmarks.html",     "aeon://bookmarks" },
        { "pages/downloads.html",     "aeon://downloads" },
        { "pages/passwords.html",     "aeon://passwords" },
        { "pages/diagnostics.html",   "aeon://diagnostics" },
        { "pages/crash.html",         "aeon://crash" },
        { "pages/drm-info.html",      "aeon://drm-info" },
    };

    for (auto& m : mappings) {
        std::string lower = url;
        for (auto& c : lower) { if (c == '\\') c = '/'; c = (char)tolower(c); }
        size_t qPos = lower.find_first_of("?#");
        std::string pathPart = (qPos != std::string::npos) ? lower.substr(0, qPos) : lower;

        std::string suffLower = m.suffix;
        for (auto& c : suffLower) c = (char)tolower(c);
        std::string decoded = pathPart;
        size_t pos;
        while ((pos = decoded.find("%20")) != std::string::npos)
            decoded.replace(pos, 3, " ");
        if (decoded.size() >= suffLower.size() &&
            decoded.compare(decoded.size() - suffLower.size(),
                           suffLower.size(), suffLower) == 0) {
            return m.aeonUrl;
        }
    }
    return url;
}

// Simulated Tab State matching ChromeTab
struct SimTab {
    unsigned int id;
    std::string url;
    bool loading;
    int progressPercent;
    float animProgress;
};

// Simulation of showLoadingBar condition in PaintNavBar
static bool ShouldShowLoadingBar(const SimTab& t) {
    std::string displayUrl = ReverseMapUrl(t.url);
    bool isNewTab = (t.url == "aeon://newtab" || displayUrl == "aeon://newtab" || t.url == "about:blank" || t.url.empty());
    bool isLoading = t.loading;
    float curAnimPct = t.animProgress;
    return (isLoading && curAnimPct > 0.0f) ||
           (!isLoading && !isNewTab && curAnimPct > 0.0f && curAnimPct <= 100.0f && t.progressPercent >= 100);
}

// Simulation of OnTimer tick logic for ID_LOADING_TIMER
static bool SimulateTimerTick(SimTab& t, bool doCreep) {
    std::string mapped = ReverseMapUrl(t.url);
    bool isNewTab = (t.url == "aeon://newtab" || mapped == "aeon://newtab" || t.url == "about:blank" || t.url.empty());
    if (isNewTab && !t.loading && t.progressPercent == 0) {
        t.animProgress = 0.0f;
        return false;
    }

    bool anyActive = false;
    if (t.loading) {
        anyActive = true;
        if (doCreep && t.progressPercent < 85) {
            t.progressPercent++;
        }
        float target = (float)(std::max)(t.progressPercent, 10);
        if (t.animProgress < target) {
            t.animProgress += (target - t.animProgress) * 0.20f + 0.3f;
            if (t.animProgress > target) t.animProgress = target;
        }
    } else {
        // Not loading (page finished or stopped)
        if (t.progressPercent >= 100 && t.animProgress > 0.0f) {
            if (t.animProgress < 100.0f) {
                float finishStep = (100.0f - t.animProgress) * 0.35f + 3.0f;
                t.animProgress += finishStep;
                if (t.animProgress >= 100.0f) {
                    t.animProgress = 100.0f;
                }
                anyActive = true;
            } else {
                // Displayed 100% on previous frame; now cleanly reset
                t.animProgress = 0.0f;
                t.progressPercent = 0;
            }
        } else {
            t.animProgress = 0.0f;
            t.progressPercent = 0;
        }
    }
    return anyActive;
}

// Simulation of SetTabLoaded
static void SimulateSetTabLoaded(SimTab& t) {
    t.loading = false;
    std::string mapped = ReverseMapUrl(t.url);
    if (mapped == "aeon://newtab" || t.url == "about:blank" || t.url.empty() || t.animProgress <= 0.0f) {
        t.progressPercent = 0;
        t.animProgress = 0.0f;
    } else {
        t.progressPercent = 100;
    }
}

// Simulation of SetTabProgress
static void SimulateSetTabProgress(SimTab& t, int percent) {
    if (percent >= 100) {
        t.loading = false;
        if (t.animProgress <= 0.0f) {
            t.animProgress = 0.0f;
            t.progressPercent = 0;
        } else {
            t.progressPercent = 100;
        }
    } else if (percent > 0) {
        t.loading = true;
        t.progressPercent = percent;
        if (t.animProgress <= 0.0f) {
            t.animProgress = 10.0f;
        }
    } else {
        t.loading = false;
        t.progressPercent = 0;
        t.animProgress = 0.0f;
    }
}

// Simulation of Navigate
static void SimulateNavigate(SimTab& t, const char* url) {
    t.url = url ? url : "";
    std::string mapped = ReverseMapUrl(t.url);
    if (mapped == "aeon://newtab" || t.url == "about:blank" || t.url.empty()) {
        t.loading = false;
        t.progressPercent = 0;
        t.animProgress = 0.0f;
    } else {
        t.loading = true;
        t.progressPercent = 25;
        t.animProgress = 10.0f;
    }
}

// Simulation of Stop
static void SimulateStop(SimTab& t) {
    t.loading = false;
    t.progressPercent = 0;
    t.animProgress = 0.0f;
}

int main() {
    std::cout << "=================================================================" << std::endl;
    std::cout << "=== AeonBrowser Omnibox Loading Bar & Progress State Tests   ===" << std::endl;
    std::cout << "=================================================================" << std::endl;

    // Test 1: Initial State on Startup / New Tab
    {
        std::cout << "\n[Suite 1: Initial Tab Creation & NewTab Invisibility]" << std::endl;
        SimTab t = { 1, "aeon://newtab", false, 0, 0.0f };
        TEST_ASSERT(!t.loading, "Initial tab loading must be false");
        TEST_ASSERT_EQ(t.progressPercent, 0, "Initial progressPercent must be 0");
        TEST_ASSERT(t.animProgress == 0.0f, "Initial animProgress must be 0.0f");
        TEST_ASSERT(!ShouldShowLoadingBar(t), "Loading bar must be invisible on newtab");

        // Resolved file path check
        SimTab tFile = { 2, "file:///C:/AeonBrowser/publish/Pro/newtab/newtab.html", false, 0, 0.0f };
        TEST_ASSERT(!ShouldShowLoadingBar(tFile), "Loading bar must be invisible on mapped newtab.html");

        // Timer tick on newtab must remain inactive
        bool active = SimulateTimerTick(t, true);
        TEST_ASSERT(!active, "Timer tick on newtab must return inactive");
        TEST_ASSERT(t.animProgress == 0.0f, "animProgress must stay 0 on newtab");

        // SetTabLoaded on newtab should be clean
        SimulateSetTabLoaded(t);
        TEST_ASSERT(!t.loading, "SetTabLoaded on newtab leaves loading false");
        TEST_ASSERT(t.animProgress == 0.0f, "animProgress stays 0");
        TEST_ASSERT(!ShouldShowLoadingBar(t), "Loading bar remains invisible");
        std::cout << "  [PASS] Initial newtab state is completely idle and invisible" << std::endl;
    }

    // Test 2: Navigating away from NewTab via Link / shortcut tile
    {
        std::cout << "\n[Suite 2: Navigating away from NewTab (Link / Shortcut Click)]" << std::endl;
        SimTab t = { 1, "aeon://newtab", false, 0, 0.0f };

        // Link click in webview triggers NavigationStarting -> SetTabProgress(20) BEFORE t.url updates!
        SimulateSetTabProgress(t, 20);
        TEST_ASSERT(t.loading, "SetTabProgress must start loading even if current URL was newtab");
        TEST_ASSERT_EQ(t.progressPercent, 20, "progressPercent must be 20");
        TEST_ASSERT(t.animProgress >= 10.0f, "animProgress must start at least at 10.0f");
        TEST_ASSERT(ShouldShowLoadingBar(t), "Loading bar must be visible immediately when link is clicked");

        // Timer tick must continue animating
        bool active = SimulateTimerTick(t, false);
        TEST_ASSERT(active, "Timer must be active while navigating away from newtab");

        // URL updates when document loads
        t.url = "https://delgadologic.tech/";
        TEST_ASSERT(ShouldShowLoadingBar(t), "Loading bar remains visible after URL update");
        std::cout << "  [PASS] Navigating away from newtab starts loading animation immediately" << std::endl;
    }

    // Test 3: Normal Navigation Lifecycle & 100% Sweep Completion
    {
        std::cout << "\n[Suite 3: Normal Navigation Lifecycle & 100% Sweep Completion]" << std::endl;
        SimTab t = { 1, "https://delgadologic.tech/", false, 0, 0.0f };

        SimulateNavigate(t, "https://delgadologic.tech/");
        TEST_ASSERT(t.loading, "Loading must be true when navigating to web URL");
        TEST_ASSERT_EQ(t.progressPercent, 25, "Initial navigation progress percent must be 25");
        TEST_ASSERT(t.animProgress == 10.0f, "Initial animProgress must start at 10.0f");
        TEST_ASSERT(ShouldShowLoadingBar(t), "Loading bar must be visible during navigation");

        // Easing towards 25%
        for (int i = 0; i < 5; ++i) {
            SimulateTimerTick(t, false);
        }
        TEST_ASSERT(t.animProgress > 15.0f && t.animProgress <= 25.0f, "animProgress must ease towards 25%");

        // ContentLoading: 65%
        SimulateSetTabProgress(t, 65);
        for (int i = 0; i < 5; ++i) {
            SimulateTimerTick(t, false);
        }
        TEST_ASSERT(t.animProgress > 35.0f && t.animProgress <= 65.0f, "animProgress must ease towards 65%");

        // Navigation completed: SetTabLoaded
        SimulateSetTabLoaded(t);
        TEST_ASSERT(!t.loading, "loading must be false upon completion");
        TEST_ASSERT_EQ(t.progressPercent, 100, "progressPercent must be 100 for finishing sweep");
        TEST_ASSERT(ShouldShowLoadingBar(t), "Loading bar shows finishing sweep to 100%");

        // Verify that animProgress reaches exactly 100.0f and is visible at 100%
        bool saw100 = false;
        int ticks = 0;
        while (SimulateTimerTick(t, false)) {
            ticks++;
            if (t.animProgress == 100.0f) {
                saw100 = true;
                TEST_ASSERT(ShouldShowLoadingBar(t), "Loading bar MUST be visible at 100% full sweep");
            }
            TEST_ASSERT(ticks < 50, "Finishing sweep must terminate");
        }

        TEST_ASSERT(saw100, "Finishing sweep must visibly reach 100.0f full width before disappearing");
        TEST_ASSERT_EQ(t.progressPercent, 0, "progressPercent must reset to 0 after completion");
        TEST_ASSERT(t.animProgress == 0.0f, "animProgress must reset to 0.0f after completion");
        TEST_ASSERT(!ShouldShowLoadingBar(t), "Loading bar must be completely invisible after finish");
        std::cout << "  [PASS] Navigation cleanly swept to 100% full-width and disappeared" << std::endl;
    }

    // Test 4: Abort / Stop Button Handling
    {
        std::cout << "\n[Suite 4: Abort / Stop Button Handling]" << std::endl;
        SimTab t = { 1, "https://slowsite.example.com", true, 40, 32.5f };
        TEST_ASSERT(ShouldShowLoadingBar(t), "Loading bar visible before stop");

        SimulateStop(t);
        TEST_ASSERT(!t.loading, "loading must be false after stop");
        TEST_ASSERT_EQ(t.progressPercent, 0, "progressPercent must be 0 after stop");
        TEST_ASSERT(t.animProgress == 0.0f, "animProgress must be 0.0f after stop");
        TEST_ASSERT(!ShouldShowLoadingBar(t), "Loading bar must immediately disappear after stop");

        bool active = SimulateTimerTick(t, false);
        TEST_ASSERT(!active, "Timer tick after stop must not be active");
        std::cout << "  [PASS] Stop button immediately clears loading bar without lingering" << std::endl;
    }

    // Test 5: Multi-Tab Stop Concurrency
    {
        std::cout << "\n[Suite 5: Multi-Tab Stop Concurrency]" << std::endl;
        SimTab tab0 = { 1, "https://tab0.example.com", true, 50, 45.0f };
        SimTab tab1 = { 2, "https://tab1.example.com", true, 30, 25.0f };

        // Stop tab1
        SimulateStop(tab1);
        TEST_ASSERT(!tab1.loading, "Tab 1 must be stopped");
        TEST_ASSERT(tab1.animProgress == 0.0f, "Tab 1 animProgress must be 0");

        // Tab 0 must STILL be loading and animating!
        TEST_ASSERT(tab0.loading, "Tab 0 must still be loading");
        TEST_ASSERT(tab0.animProgress > 0.0f, "Tab 0 animProgress must still be active");
        bool active = SimulateTimerTick(tab0, false);
        TEST_ASSERT(active, "Timer must remain active for Tab 0");
        std::cout << "  [PASS] Stopping one tab does not kill timer or freeze other tabs" << std::endl;
    }

    // Test 6: Multi-Tab Tab Switching & Omnibox Isolation
    {
        std::cout << "\n[Suite 6: Multi-Tab Tab Switching & Omnibox Isolation]" << std::endl;
        SimTab tabLoading = { 1, "https://tab0.example.com", true, 50, 45.0f };
        SimTab tabIdle    = { 2, "aeon://newtab", false, 0, 0.0f };

        TEST_ASSERT(ShouldShowLoadingBar(tabLoading), "Active loading tab must show loading bar");
        TEST_ASSERT(!ShouldShowLoadingBar(tabIdle), "Active idle tab must NOT show loading bar");

        // Switch active tab to tabIdle: omnibox is clean
        TEST_ASSERT(!ShouldShowLoadingBar(tabIdle), "Switched omnibox has no ghost loading bar");
        // tabLoading in background continues running timer
        TEST_ASSERT(SimulateTimerTick(tabLoading, false), "Background tab continues ticking");
        std::cout << "  [PASS] Omnibox correctly isolates active tab from background loading state" << std::endl;
    }

    // Test 7: Progress Bar Geometry & Clamping
    {
        std::cout << "\n[Suite 7: Progress Bar Geometry & Clamping]" << std::endl;
        int urlLeft = 102;
        int urlRight = 1000;
        int cardW = urlRight - urlLeft; // 898
        int trackW = cardW - 14;        // 884

        // 0% -> 0 width
        float pct0 = 0.0f;
        int progW0 = (int)((float)trackW * (pct0 / 100.0f));
        TEST_ASSERT_EQ(progW0, 0, "0% progress width must be 0");

        // 50% -> 442 width
        float pct50 = 50.0f;
        int progW50 = (int)((float)trackW * (pct50 / 100.0f));
        TEST_ASSERT_EQ(progW50, 442, "50% progress width must be half of track width");

        // 100% -> trackW width
        float pct100 = 100.0f;
        int progW100 = (int)((float)trackW * (pct100 / 100.0f));
        TEST_ASSERT_EQ(progW100, trackW, "100% progress width must equal track width");

        // Over-clamp test
        float pctOver = 120.0f;
        float clampedOver = (std::min)(100.0f, (std::max)(0.0f, pctOver));
        int progWOver = (int)((float)trackW * (clampedOver / 100.0f));
        TEST_ASSERT_EQ(progWOver, trackW, "Over 100% progress width must clamp to track width");
        std::cout << "  [PASS] Geometry math clamped and calculated correctly" << std::endl;
    }

    std::cout << "\n=================================================================" << std::endl;
    std::cout << "=== ALL 7/7 LOADING BAR VERIFICATION TEST SUITES PASSED!       ===" << std::endl;
    std::cout << "=================================================================" << std::endl;
    return 0;
}
