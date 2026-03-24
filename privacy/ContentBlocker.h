// AeonBrowser — ContentBlocker.h
#pragma once
#include <cstddef>

namespace ContentBlocker {

    struct BlockResult {
        bool  should_block;
        bool  is_cosmetic;     // CSS element-hide only (not a full request block)
        char  matched_rule[128];
    };

    // Load an EasyList/AdBlock Plus compatible filter list from disk.
    // Call multiple times to stack lists (EasyList + EasyPrivacy + our list).
    void LoadFilterList(const char* path);

    // Download updated filter lists from our CDN and reload.
    // Thread-safe — can be called from background update thread.
    bool UpdateFilterLists(const char* cdn_base_url);

    // Check if a request URL should be blocked given the page origin.
    // origin = the page making the request (for per-site rules).
    BlockResult Check(const char* request_url, const char* origin);

    // Return CSS rules to inject for element hiding on this page URL.
    // Caller must free() the returned string.
    char* GetCosmeticCSS(const char* page_url);

    // Fingerprint guard: randomize canvas/WebGL/audio fingerprints.
    void InjectFingerprintGuard(void* js_context);

    // DNS-over-HTTPS: resolve hostname via our DoH provider.
    // Returns IP string into out_buf (max 64 chars). Returns true on success.
    bool ResolveDoH(const char* hostname, char* out_buf, size_t buf_len);

    // Global Privacy Control: inject GPC header into all requests.
    // Called by the networking layer before sending any HTTP request.
    void InjectGpcHeader(void* request_context);

    struct Stats {
        unsigned long long requests_blocked;
        unsigned long long elements_hidden;
        unsigned long long ads_blocked;
        unsigned long long trackers_blocked;
        double             bandwidth_saved_mb;
    };
    Stats GetStats();
}
