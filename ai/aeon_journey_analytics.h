// =============================================================================
// aeon_journey_analytics.h — AeonJourneyAnalytics: Browsing Journey Engine
// Pillar 5: AI-Powered Features — Journey Graph & Intent Detection
//
// What is a "Journey"?
//   A journey is a sequence of related browsing actions toward a goal.
//   Example: Google "best laptop 2026" → visit review sites → compare
//   prices on Amazon/Flipkart → read warranty info → purchase.
//
// This module builds a DIRECTED GRAPH of page visits, detects journey
// boundaries using temporal + semantic signals, and provides:
//
//   1. Real-time journey tracking with stage detection
//   2. Journey completion estimation
//   3. Cross-session journey resumption (user comes back next day)
//   4. Journey-aware tab grouping (feeds into AeonTabIntelligence)
//   5. Aggregate journey analytics (anonymous, on-device only)
//
// Privacy: ZERO cloud processing. All data stays on-device.
// The only data that may leave (opt-in) is aggregate, anonymous
// journey type distributions via AeonHive HiveInsight topic.
// =============================================================================

#pragma once

#include "aeon_component.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class AeonJourneyAnalyticsImpl;

// ---------------------------------------------------------------------------
// Journey Intent — what the user is trying to accomplish
// ---------------------------------------------------------------------------

enum class AeonJourneyIntent : uint8_t {
    Unknown         = 0,
    Research        = 1,    // Learning about a topic
    Shopping        = 2,    // Comparing and purchasing products
    Navigation      = 3,    // Finding a physical location
    Communication   = 4,    // Email, messaging, social interaction
    Entertainment   = 5,    // Watching, listening, reading for fun
    Productivity    = 6,    // Work-related tasks
    FinancialTx     = 7,    // Banking, payments, money transfers
    GovernmentSvc   = 8,    // Applying for documents, paying taxes
    HealthInfo      = 9,    // Medical research, telemedicine
    Education       = 10,   // Coursework, tutorials, learning
};

// ---------------------------------------------------------------------------
// Journey Stage — where in the journey the user currently is
// ---------------------------------------------------------------------------

enum class AeonJourneyStage : uint8_t {
    Entry           = 0,    // Initial search or direct navigation
    Exploration     = 1,    // Browsing multiple sources
    Comparison      = 2,    // Comparing options side-by-side
    Decision        = 3,    // Narrowing down, reading reviews
    Action          = 4,    // Checkout, form fill, sign up
    Confirmation    = 5,    // Order confirmation, receipt
    PostAction      = 6,    // Tracking, support, reviews
};

// ---------------------------------------------------------------------------
// Journey Node — a single page visit in the journey graph
// ---------------------------------------------------------------------------

struct AeonJourneyNode {
    uint64_t    node_id;
    uint64_t    tab_id;             // Which tab this was in
    char        url[2048];
    char        title[256];
    char        domain[256];
    uint64_t    visit_utc;          // When the page was visited
    uint32_t    dwell_time_secs;    // How long user spent on page
    float       engagement_score;   // 0.0 = bounce, 1.0 = deep read
    AeonJourneyStage stage;         // Detected stage of this node

    // Navigation context
    bool        from_search;        // Arrived via search results
    bool        from_link;          // Arrived via clicked link
    bool        from_direct;        // Direct URL entry or bookmark
    bool        from_back_button;   // User went back and retried
};

// ---------------------------------------------------------------------------
// Journey Edge — a navigation link between two nodes
// ---------------------------------------------------------------------------

struct AeonJourneyEdge {
    uint64_t    from_node_id;
    uint64_t    to_node_id;
    uint64_t    timestamp_utc;

    enum class Type : uint8_t {
        Click       = 0,    // Clicked a link on the page
        Search      = 1,    // New search query
        BackButton  = 2,    // Pressed back
        NewTab      = 3,    // Opened in new tab
        Redirect    = 4,    // Automatic redirect
        Bookmark    = 5,    // From bookmarks
        External    = 6,    // From external app (share intent)
    } type;
};

// ---------------------------------------------------------------------------
// Journey Summary — aggregate stats for a complete journey
// ---------------------------------------------------------------------------

struct AeonJourneySummary {
    uint64_t        journey_id;
    char            title[128];
    AeonJourneyIntent intent;
    AeonJourneyStage final_stage;

    uint64_t        start_utc;
    uint64_t        end_utc;
    uint32_t        total_pages;
    uint32_t        unique_domains;
    uint32_t        total_time_secs;
    uint32_t        search_queries;
    uint32_t        back_presses;       // Backtracking indicator
    uint32_t        tab_count;          // Active tabs in this journey

    // Efficiency metrics
    float           linearity;          // 1.0 = straight path, 0.0 = lots of backtracking
    float           completion;         // Did they achieve the goal?
    float           satisfaction;       // Estimated satisfaction (ML-based)

    // Cross-session info
    uint32_t        session_count;      // How many sessions this journey spanned
    uint32_t        days_elapsed;       // Calendar days from start to end
};

// ---------------------------------------------------------------------------
// Browsing Pattern — recurring behavior model
// ---------------------------------------------------------------------------

struct AeonBrowsingPattern {
    char            name[128];          // "Morning News Routine"
    char            description[256];   // "You read news from 3 sources at 8am"

    uint32_t        occurrence_count;   // How many times this pattern occurred
    float           confidence;

    // Time pattern
    uint32_t        typical_hour;       // Most common start hour (0-23)
    uint32_t        typical_duration_mins;
    uint8_t         day_mask;           // Bitmask: bit 0 = Mon, bit 6 = Sun

    // Site pattern
    struct SiteFreq {
        char        domain[256];
        float       frequency;         // 0.0 - 1.0
    };
    SiteFreq        top_sites[8];
    uint32_t        site_count;
};

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

using AeonJourneyStartCallback = std::function<void(
    uint64_t journey_id, AeonJourneyIntent intent)>;
using AeonJourneyStageCallback = std::function<void(
    uint64_t journey_id, AeonJourneyStage old_stage, AeonJourneyStage new_stage)>;
using AeonJourneyCompleteCallback = std::function<void(
    const AeonJourneySummary& summary)>;
using AeonPatternDetectedCallback = std::function<void(
    const AeonBrowsingPattern& pattern)>;

// ---------------------------------------------------------------------------
// AeonJourneyAnalytics — Journey Detection & Analytics Engine
// ---------------------------------------------------------------------------

class AeonJourneyAnalytics : public AeonComponentBase {
public:
    AeonJourneyAnalytics();
    ~AeonJourneyAnalytics() override;

    AeonJourneyAnalytics(const AeonJourneyAnalytics&) = delete;
    AeonJourneyAnalytics& operator=(const AeonJourneyAnalytics&) = delete;

    // ── IAeonComponent (via AeonComponentBase) ───────────────────────────

    const char* ComponentId()      const override { return "aeon.journey_analytics"; }
    const char* ComponentVersion() const override { return "1.0.0"; }
    const char* UpstreamRef()      const override { return "delgadologic/aeon@pillar5"; }
    bool Initialize(const ResourceBudget& budget) override;
    void Shutdown() override;

    // ── Event Ingestion ──────────────────────────────────────────────────

    // Feed navigation events into the journey engine
    void OnPageVisit(uint64_t tab_id, const char* url, const char* title,
                     bool from_search, bool from_link, bool from_back);
    void OnPageDwell(uint64_t tab_id, uint32_t dwell_secs, float engagement);
    void OnSearchQuery(uint64_t tab_id, const char* query);
    void OnTabSwitch(uint64_t from_tab_id, uint64_t to_tab_id);
    void OnFormSubmit(uint64_t tab_id, const char* url);
    void OnDownload(uint64_t tab_id, const char* url, const char* filename);

    // ── Journey Queries ──────────────────────────────────────────────────

    // Get all currently active journeys
    std::vector<AeonJourneySummary> GetActiveJourneys() const;

    // Get completed journeys (historical)
    std::vector<AeonJourneySummary> GetCompletedJourneys(
        uint32_t max_count = 50) const;

    // Get the journey graph for a specific journey
    void GetJourneyGraph(uint64_t journey_id,
                          std::vector<AeonJourneyNode>& nodes,
                          std::vector<AeonJourneyEdge>& edges) const;

    // Find which journey a tab belongs to (-1 if none)
    int64_t GetJourneyForTab(uint64_t tab_id) const;

    // ── Intent Detection ─────────────────────────────────────────────────

    // Detect intent from a URL + title + context
    AeonJourneyIntent DetectIntent(const char* url, const char* title,
                                    const char* referrer = nullptr) const;

    // ── Pattern Analysis ─────────────────────────────────────────────────

    // Get detected recurring browsing patterns
    std::vector<AeonBrowsingPattern> GetPatterns() const;

    // Get time-of-day browsing distribution (24-element array, normalized)
    void GetHourlyDistribution(float out_hours[24]) const;

    // Get day-of-week browsing distribution (7-element array)
    void GetDailyDistribution(float out_days[7]) const;

    // Get top domains by time spent
    struct DomainStat {
        char    domain[256];
        uint32_t total_time_secs;
        uint32_t visit_count;
        float   avg_dwell_secs;
    };
    std::vector<DomainStat> GetTopDomains(uint32_t max_count = 20) const;

    // ── Cross-Session Resumption ─────────────────────────────────────────

    // Check if there are any incomplete journeys that can be resumed
    std::vector<AeonJourneySummary> GetResumableJourneys() const;

    // Resume a previous journey (user clicked "Continue where you left off")
    void ResumeJourney(uint64_t journey_id, uint64_t new_tab_id);

    // ── Data Management ──────────────────────────────────────────────────

    // Export analytics data (for backup or transfer)
    std::vector<uint8_t> ExportData() const;

    // Import analytics data
    bool ImportData(const uint8_t* data, size_t len);

    // Clear all analytics data
    void ClearAllData();

    // Get storage size used by analytics (bytes)
    uint64_t GetStorageUsed() const;

    // ── Diagnostics ──────────────────────────────────────────────────────

    std::string DiagnosticReport() const;
    bool CanOffloadToHive() const override { return false; }

    // ── Callbacks ────────────────────────────────────────────────────────

    void OnJourneyStarted(AeonJourneyStartCallback cb);
    void OnStageChanged(AeonJourneyStageCallback cb);
    void OnJourneyCompleted(AeonJourneyCompleteCallback cb);
    void OnPatternDetected(AeonPatternDetectedCallback cb);

private:
    AeonJourneyAnalyticsImpl* m_impl = nullptr;
};

// Lifecycle managed by AeonMain.cpp via g_JourneyAI global pointer.

