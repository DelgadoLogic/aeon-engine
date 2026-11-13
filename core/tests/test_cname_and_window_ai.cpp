// AeonBrowser — test_cname_and_window_ai.cpp
// DelgadoLogic | Milestone 3 Verification Suite: CNAME Uncloaking & window.ai Polyfill
#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <chrono>
#include <future>
#include <fstream>

#include "../network/DnsResolver.h"
#include "../extensions/ExtensionRuntime.h"
#include "../../network/aeon_dns.h"
#include "../../engines/aeon_ai.h"

// ── Test Helpers ─────────────────────────────────────────────────────────────
static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (cond) { \
            std::cout << "  [PASS] " << msg << "\n"; \
            g_testsPassed++; \
        } else { \
            std::cerr << "  [FAIL] " << msg << " (line " << __LINE__ << ")\n"; \
            g_testsFailed++; \
        } \
    } while (0)

// ── 1. CNAME JSON Parsing & Multi-Hop Chain Extraction ───────────────────────
void TestCnameJsonParsing() {
    std::cout << "\n=== Test 1: DoH JSON CNAME Chain Extraction ===\n";

    // Single CNAME hop
    std::string singleHopJson = R"({
        "Status": 0,
        "TC": false,
        "RD": true,
        "RA": true,
        "AD": true,
        "CD": false,
        "Question": [{"name": "track.example.com", "type": 1}],
        "Answer": [
            {"name": "track.example.com", "type": 5, "TTL": 300, "data": "cloaked-tracker.adnetwork.com."},
            {"name": "cloaked-tracker.adnetwork.com", "type": 1, "TTL": 300, "data": "192.0.2.1"}
        ]
    })";

    auto chain1 = DnsResolver::ExtractCnameChainFromJson(singleHopJson, "track.example.com");
    TEST_ASSERT(chain1.size() == 1, "Single hop chain size == 1");
    if (!chain1.empty()) {
        TEST_ASSERT(chain1[0] == "cloaked-tracker.adnetwork.com", "Canonical target extracted correctly");
    }

    // Multi-hop CNAME chain (3 hops)
    std::string multiHopJson = R"({
        "Status": 0,
        "Answer": [
            {"name": "analytics.mysite.com", "type": 5, "TTL": 300, "data": "alias1.net."},
            {"name": "alias1.net", "type": 5, "TTL": 300, "data": "alias2.org."},
            {"name": "alias2.org", "type": 5, "TTL": 300, "data": "tracker.criteo.net."},
            {"name": "tracker.criteo.net", "type": 1, "TTL": 300, "data": "203.0.113.50"}
        ]
    })";

    auto chain2 = DnsResolver::ExtractCnameChainFromJson(multiHopJson, "analytics.mysite.com");
    TEST_ASSERT(chain2.size() == 3, "Multi-hop chain size == 3");
    if (chain2.size() == 3) {
        TEST_ASSERT(chain2[0] == "alias1.net", "Hop 1 == alias1.net");
        TEST_ASSERT(chain2[1] == "alias2.org", "Hop 2 == alias2.org");
        TEST_ASSERT(chain2[2] == "tracker.criteo.net", "Terminal canonical target == tracker.criteo.net");
    }

    // Non-CNAME response (Direct A record)
    std::string directAJson = R"({
        "Status": 0,
        "Answer": [
            {"name": "example.com", "type": 1, "TTL": 300, "data": "93.184.216.34"}
        ]
    })";
    auto chain3 = DnsResolver::ExtractCnameChainFromJson(directAJson, "example.com");
    TEST_ASSERT(chain3.empty(), "Direct A record returns empty CNAME chain");
}

// ── 2. CNAME Loop Detection & Depth Limit ────────────────────────────────────
void TestCnameLoopAndDepth() {
    std::cout << "\n=== Test 2: CNAME Loop Detection & Max Depth Limit ===\n";

    // Circular loop: a.com -> b.com -> a.com
    std::string loopJson = R"({
        "Status": 0,
        "Answer": [
            {"name": "a.com", "type": 5, "TTL": 300, "data": "b.com."},
            {"name": "b.com", "type": 5, "TTL": 300, "data": "a.com."}
        ]
    })";

    auto loopChain = DnsResolver::ExtractCnameChainFromJson(loopJson, "a.com");
    TEST_ASSERT(loopChain.size() == 1, "Loop detected: stopped before repeating cycle");
    if (!loopChain.empty()) {
        TEST_ASSERT(loopChain[0] == "b.com", "First hop extracted before cycle break");
    }

    // Deep chain (>10 hops)
    std::string deepJson = R"({
        "Status": 0,
        "Answer": [
            {"name": "h0.com", "type": 5, "data": "h1.com."},
            {"name": "h1.com", "type": 5, "data": "h2.com."},
            {"name": "h2.com", "type": 5, "data": "h3.com."},
            {"name": "h3.com", "type": 5, "data": "h4.com."},
            {"name": "h4.com", "type": 5, "data": "h5.com."},
            {"name": "h5.com", "type": 5, "data": "h6.com."},
            {"name": "h6.com", "type": 5, "data": "h7.com."},
            {"name": "h7.com", "type": 5, "data": "h8.com."},
            {"name": "h8.com", "type": 5, "data": "h9.com."},
            {"name": "h9.com", "type": 5, "data": "h10.com."},
            {"name": "h10.com", "type": 5, "data": "h11.com."},
            {"name": "h11.com", "type": 5, "data": "h12.com."}
        ]
    })";

    auto deepChain = DnsResolver::ExtractCnameChainFromJson(deepJson, "h0.com");
    TEST_ASSERT(deepChain.size() == 10, "Chain depth clamped to max depth 10 hops");
    if (deepChain.size() == 10) {
        TEST_ASSERT(deepChain.back() == "h10.com", "10th hop reached safely");
    }
}

// ── 3. CNAME Cache ───────────────────────────────────────────────────────────
void TestCnameCache() {
    std::cout << "\n=== Test 3: CNAME Cache Operations ===\n";

    DnsResolver::FlushCnameCache();

    DnsResolver::CnameResult res;
    bool found = DnsResolver::GetCachedCname("test-host.com", res);
    TEST_ASSERT(!found, "Cache is initially empty");

    DnsResolver::CnameResult entry;
    entry.initialHost = "test-host.com";
    entry.canonicalHost = "uncloaked-tracker.com";
    entry.chain = { "alias.net", "uncloaked-tracker.com" };
    entry.uncloaked = true;
    entry.isBlocked = true;
    entry.blockedDomain = "uncloaked-tracker.com";

    DnsResolver::SetCachedCname("test-host.com", entry, 300);

    found = DnsResolver::GetCachedCname("test-host.com", res);
    TEST_ASSERT(found, "Cached entry retrieved successfully");
    TEST_ASSERT(res.canonicalHost == "uncloaked-tracker.com", "Cached canonical host matches");
    TEST_ASSERT(res.chain.size() == 2, "Cached chain length matches");
    TEST_ASSERT(res.isBlocked == true, "Cached isBlocked matches");

    DnsResolver::FlushCnameCache();
    found = DnsResolver::GetCachedCname("test-host.com", res);
    TEST_ASSERT(!found, "FlushCnameCache cleared the cache");
}

// ── 4. Native AdBlock with CNAME Uncloaking ──────────────────────────────────
void TestNativeAdBlockCname() {
    std::cout << "\n=== Test 4: Native AdBlock CNAME Uncloaking Integration ===\n";

    // Create a temporary filter list
    const char* filterFile = "test_easylist.txt";
    std::ofstream ofs(filterFile);
    ofs << "[AutoProxy 0.2.9]\n";
    ofs << "! Title: EasyList Test Subset\n";
    ofs << "||doubleclick.net^\n";
    ofs << "||criteo.net^\n";
    ofs << "||telemetry-tracker.com^\n";
    ofs << "@@||allowed-tracker.com^\n";
    ofs.close();

    NativeAdBlock::LoadFilterList(filterFile);
    DeleteFileA(filterFile);

    std::string blockedDomain;

    // 1. Direct block check
    bool directBlock = NativeAdBlock::ShouldBlockWithCname("http://ad.doubleclick.net/pixel.gif", blockedDomain);
    TEST_ASSERT(directBlock == true, "Direct blocklist match blocked");
    TEST_ASSERT(blockedDomain == "ad.doubleclick.net" || blockedDomain == "doubleclick.net", "Direct blockedDomain extracted");

    // 2. Cloaked CNAME Tracker Check
    // Pre-cache CNAME alias for analytics.firstpartybrand.com -> cloaked.criteo.net
    DnsResolver::CnameResult cname1;
    cname1.initialHost = "analytics.firstpartybrand.com";
    cname1.canonicalHost = "cloaked.criteo.net";
    cname1.chain = { "gateway.brand-cdn.com", "cloaked.criteo.net" };
    cname1.uncloaked = true;
    DnsResolver::SetCachedCname("analytics.firstpartybrand.com", cname1, 300);

    bool cloakedBlock = NativeAdBlock::ShouldBlockWithCname("https://analytics.firstpartybrand.com/event.js", blockedDomain);
    TEST_ASSERT(cloakedBlock == true, "Cloaked CNAME tracker blocked by Aeon Shield!");
    TEST_ASSERT(blockedDomain == "cloaked.criteo.net", "Uncloaked canonical domain identified as blocker");

    // 3. Allowed/Whitelisted override check
    DnsResolver::CnameResult cname2;
    cname2.initialHost = "partner.firstpartybrand.com";
    cname2.canonicalHost = "allowed-tracker.com";
    cname2.chain = { "allowed-tracker.com" };
    cname2.uncloaked = true;
    DnsResolver::SetCachedCname("partner.firstpartybrand.com", cname2, 300);

    bool allowed = NativeAdBlock::ShouldBlockWithCname("https://partner.firstpartybrand.com/sdk.js", blockedDomain);
    TEST_ASSERT(allowed == false, "Whitelisted canonical domain @@||allowed-tracker.com allowed");

    // 4. Non-tracker legitimate CDN
    DnsResolver::CnameResult cname3;
    cname3.initialHost = "cdn.firstpartybrand.com";
    cname3.canonicalHost = "assets.edgekey.net";
    cname3.chain = { "assets.edgekey.net" };
    cname3.uncloaked = true;
    DnsResolver::SetCachedCname("cdn.firstpartybrand.com", cname3, 300);

    bool cdnAllowed = NativeAdBlock::ShouldBlockWithCname("https://cdn.firstpartybrand.com/style.css", blockedDomain);
    TEST_ASSERT(cdnAllowed == false, "Legitimate CDN uncloaked CNAME allowed");
}

// ── 5. AeonDNSRecord Structure Verification ──────────────────────────────────
void TestAeonDnsRecordStruct() {
    std::cout << "\n=== Test 5: AeonDNSRecord CNAME Fields ===\n";

    AeonDNSRecord rec = {};
    strcpy_s(rec.hostname, "analytics.company.com");
    strcpy_s(rec.canonical_name, "tracker.criteo.net");
    strcpy_s(rec.ip, "1.2.3.4");
    rec.ttl_seconds = 300;
    rec.from_cache = true;
    rec.cname_uncloaked = true;
    rec.blocked = true;
    strcpy_s(rec.blocked_reason, "cname_cloaked_tracker");

    TEST_ASSERT(strcmp(rec.canonical_name, "tracker.criteo.net") == 0, "AeonDNSRecord.canonical_name field valid");
    TEST_ASSERT(rec.cname_uncloaked == true, "AeonDNSRecord.cname_uncloaked field valid");
    TEST_ASSERT(strcmp(rec.blocked_reason, "cname_cloaked_tracker") == 0, "AeonDNSRecord.blocked_reason field valid");
}

// ── 6. window.ai Prompt API Live Inference Verification ──────────────────────
void TestWindowAiInference() {
    std::cout << "\n=== Test 6: window.ai Local Neural Engine Inference ===\n";

    ResourceBudget budget = {};
    budget.max_ram_bytes = 512 * 1024 * 1024;
    AeonAIInstance().Initialize(budget);

    // Test prompt completion
    std::promise<std::string> promptPromise;
    auto promptFuture = promptPromise.get_future();

    AeonAIInstance().PromptAsync("Summarize the benefits of sovereign privacy browsers",
        [&promptPromise](const std::string& text, bool is_final) {
            if (is_final) {
                promptPromise.set_value(text);
            }
        });

    if (promptFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
        std::string response = promptFuture.get();
        TEST_ASSERT(!response.empty(), "AeonAI::PromptAsync returned valid inference completion");
        std::cout << "  Prompt output: " << response.substr(0, 80) << "...\n";
    } else {
        TEST_ASSERT(false, "AeonAI::PromptAsync timed out");
    }

    // Test streaming completion
    std::promise<std::vector<std::string>> streamPromise;
    auto streamFuture = streamPromise.get_future();
    auto tokens = std::make_shared<std::vector<std::string>>();

    AeonAIInstance().PromptAsync("Explain CNAME uncloaking privacy protection",
        [tokens, &streamPromise](const std::string& token, bool is_final) {
            tokens->push_back(token);
            if (is_final) {
                streamPromise.set_value(*tokens);
            }
        });

    if (streamFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
        auto receivedTokens = streamFuture.get();
        TEST_ASSERT(!receivedTokens.empty(), "AeonAI::PromptAsync streaming received token chunks");
        std::cout << "  Streaming token count: " << receivedTokens.size() << "\n";
    } else {
        TEST_ASSERT(false, "AeonAI::PromptAsync streaming timed out");
    }
}

// ── Main Entry Point ─────────────────────────────────────────────────────────
int main() {
    std::cout << "=================================================================\n";
    std::cout << "AeonBrowser Milestone 3 Test Suite: CNAME Uncloaking & window.ai\n";
    std::cout << "DelgadoLogic | Network Security & Local AI Verification\n";
    std::cout << "=================================================================\n";

    TestCnameJsonParsing();
    TestCnameLoopAndDepth();
    TestCnameCache();
    TestNativeAdBlockCname();
    TestAeonDnsRecordStruct();
    TestWindowAiInference();

    std::cout << "\n=================================================================\n";
    std::cout << "Test Summary: " << g_testsPassed << " passed, " << g_testsFailed << " failed.\n";
    std::cout << "=================================================================\n";

    return (g_testsFailed == 0) ? 0 : 1;
}
