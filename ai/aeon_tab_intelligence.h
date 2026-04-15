// =============================================================================
// aeon_tab_intelligence.h — AeonTabIntelligence: AI-Powered Tab Organization
// Pillar 5: AI-Powered Features — Tab Groups & Journey Analytics
//
// Problem Statement:
//   Users in emerging markets run dozens of tabs on low-RAM devices.
//   Chrome's tab groups require manual effort. Samsung's is better but
//   still user-initiated. We provide FULLY AUTOMATIC tab organization
//   that learns browsing patterns and clusters tabs by:
//
//   1. Topic Clustering — NLP-based topic extraction from page titles/URLs
//   2. Journey Detection — Graph-based session grouping (search → shop → pay)
//   3. Memory Pressure — Auto-hibernation of cold tabs on RAM-scarce devices
//   4. Smart Reopen — ML-based prediction: "You usually open Gmail at 9am"
//   5. Tab Decay — Tabs untouched for N days get archived, not destroyed
//
// All ML runs ON-DEVICE using quantized TFLite models (<5MB total).
// No browsing data leaves the device. Ever.
//
// AeonHive Integration:
//   - SyncTabs topic for cross-device tab state (encrypted)
//   - HiveInsight topic for anonymous, aggregated pattern data
// =============================================================================

#pragma once

#include "aeon_component.h"
#include "../hive/aeon_hive.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Forward declarations
class AeonTabIntelligenceImpl;

// ---------------------------------------------------------------------------
// Tab State & Metadata
// ---------------------------------------------------------------------------

enum class AeonTabState : uint8_t {
    Active          = 0,    // Currently visible or recently used
    Background      = 1,    // Open but not visible
    Hibernated      = 2,    // DOM discarded, thumbnail preserved
    Archived        = 3,    // Saved to disk, removed from memory
    Predicted       = 4,    // Ghost tab — predicted but not yet opened
};

struct AeonTabInfo {
    uint64_t        tab_id;             // Unique tab identifier
    char            title[256];         // Page title
    char            url[2048];          // Current URL
    char            domain[256];        // Extracted domain
    AeonTabState    state;              // Current state

    uint64_t        created_utc;        // When tab was opened
    uint64_t        last_active_utc;    // Last time user interacted
    uint64_t        last_focus_utc;     // Last time tab was focused

    float           relevance_score;    // 0.0 = cold, 1.0 = hot
    uint32_t        focus_count;        // Number of times focused
    uint32_t        total_focus_secs;   // Total seconds of active focus
    uint64_t        memory_bytes;       // Current RAM usage (0 if hibernated)

    // Topic Classification
    char            primary_topic[64];  // Auto-detected topic (e.g. "Shopping")
    float           topic_confidence;   // 0.0 - 1.0

    // Group Assignment
    int32_t         group_id;           // -1 = ungrouped
    char            group_name[64];     // Auto-generated group name
    uint32_t        group_color;        // ARGB color for visual grouping
};

// ---------------------------------------------------------------------------
// Tab Group — auto-generated cluster
// ---------------------------------------------------------------------------

struct AeonTabGroup {
    int32_t         group_id;
    char            name[64];           // E.g. "Shopping Trip", "Work Email"
    uint32_t        color;              // ARGB
    uint32_t        tab_count;
    uint64_t        total_memory;       // Combined RAM usage
    float           avg_relevance;      // Average relevance score
    char            primary_topic[64];

    // Journey info
    bool            is_journey;         // True = detected browsing journey
    char            journey_stage[32];  // "search", "compare", "purchase"
    float           journey_progress;   // 0.0 - 1.0
};

// ---------------------------------------------------------------------------
// Journey — a detected multi-tab browsing session
// ---------------------------------------------------------------------------

struct AeonJourney {
    uint64_t        journey_id;
    char            name[128];          // Auto-generated: "Laptop Shopping"
    char            intent[64];         // Detected intent: "purchase", "research"
    uint64_t        started_utc;
    uint64_t        last_activity_utc;
    uint32_t        tab_count;          // Active tabs in this journey
    uint32_t        total_pages_visited;
    float           completion;         // 0.0 - 1.0 (ML-estimated)

    // Journey graph
    uint64_t        entry_tab_id;       // First tab (usually search)
    uint64_t        current_tab_id;     // Most recently active tab

    // Journey stages
    struct Stage {
        char        name[32];           // "search", "compare", "decide", "act"
        uint32_t    page_count;
        uint64_t    time_spent_secs;
    };
    Stage           stages[8];
    uint32_t        stage_count;
};

// ---------------------------------------------------------------------------
// Tab Prediction — "You usually open X at Y time"
// ---------------------------------------------------------------------------

struct AeonTabPrediction {
    char            url[2048];
    char            title[256];
    float           confidence;         // 0.0 - 1.0
    char            reason[128];        // "Opened daily at 9am"
    uint32_t        historical_opens;   // How many times opened
    uint32_t        avg_hour;           // Most common hour (0-23)
    uint32_t        avg_day_of_week;    // Most common day (0=Mon, 6=Sun)
};

// ---------------------------------------------------------------------------
// Memory Pressure Levels
// ---------------------------------------------------------------------------

enum class AeonMemoryPressure : uint8_t {
    Normal      = 0,    // >512MB free — no action needed
    Moderate    = 1,    // 256-512MB free — hibernate cold tabs
    Critical    = 2,    // 128-256MB free — aggressive hibernation
    Emergency   = 3,    // <128MB free — archive everything except active tab
};

// ---------------------------------------------------------------------------
// Tab Intelligence Configuration
// ---------------------------------------------------------------------------

struct AeonTabIntelConfig {
    bool            auto_group_enabled;     // Auto-create tab groups
    bool            auto_hibernate_enabled; // Hibernate cold tabs
    bool            journey_detection;      // Detect browsing journeys
    bool            smart_reopen;           // Predict and suggest tabs
    bool            tab_decay;              // Archive old tabs

    uint32_t        hibernate_after_mins;   // Minutes of inactivity before hibernate (default: 30)
    uint32_t        archive_after_days;     // Days before archiving (default: 7)
    uint32_t        max_active_tabs;        // Max tabs before forced hibernation (default: 25)

    float           min_topic_confidence;   // Min confidence for topic assignment (default: 0.6)
    float           min_journey_confidence; // Min confidence for journey detection (default: 0.7)

    // Memory thresholds (MB)
    uint32_t        moderate_threshold_mb;  // Default: 512
    uint32_t        critical_threshold_mb;  // Default: 256
    uint32_t        emergency_threshold_mb; // Default: 128
};

// ---------------------------------------------------------------------------
// Topic Model — on-device NLP
// ---------------------------------------------------------------------------

struct AeonTopicModel {
    const char*     model_path;         // Path to TFLite model
    uint32_t        model_size_bytes;   // Model file size
    uint32_t        vocab_size;         // Token vocabulary size
    uint32_t        embedding_dim;      // Output embedding dimension
    uint32_t        num_topics;         // Number of predefined topics
    float           inference_time_ms;  // Average inference time
};

// Predefined Topics
static const char* AEON_TAB_TOPICS[] = {
    "News & Current Events",
    "Social Media",
    "Shopping & E-Commerce",
    "Email & Communication",
    "Video & Entertainment",
    "Search & Reference",
    "Work & Productivity",
    "Finance & Banking",
    "Gaming",
    "Education & Learning",
    "Health & Medical",
    "Government & Services",
    "Travel & Transport",
    "Food & Dining",
    "Technology",
    "Sports",
    "Other"
};
static constexpr uint32_t AEON_TOPIC_COUNT =
    sizeof(AEON_TAB_TOPICS) / sizeof(AEON_TAB_TOPICS[0]);

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

using AeonTabGroupCallback = std::function<void(const AeonTabGroup& group)>;
using AeonJourneyCallback  = std::function<void(const AeonJourney& journey)>;
using AeonPredictionCallback = std::function<void(
    const std::vector<AeonTabPrediction>& predictions)>;
using AeonMemoryPressureCallback = std::function<void(
    AeonMemoryPressure level, uint64_t free_mb)>;

// ---------------------------------------------------------------------------
// AeonTabIntelligence — AI Tab Organization Engine
// ---------------------------------------------------------------------------

class AeonTabIntelligence : public AeonComponentBase {
public:
    AeonTabIntelligence();
    ~AeonTabIntelligence() override;

    AeonTabIntelligence(const AeonTabIntelligence&) = delete;
    AeonTabIntelligence& operator=(const AeonTabIntelligence&) = delete;

    // ── IAeonComponent (via AeonComponentBase) ───────────────────────────

    const char* ComponentId()      const override { return "aeon.tab_intelligence"; }
    const char* ComponentVersion() const override { return "1.0.0"; }
    const char* UpstreamRef()      const override { return "delgadologic/aeon@pillar5"; }
    bool Initialize(const ResourceBudget& budget) override;
    void Shutdown() override;

    // ── Configuration ─────────────────────────────────────────────────────

    void SetConfig(const AeonTabIntelConfig& cfg);
    AeonTabIntelConfig GetConfig() const;

    // ── Tab Lifecycle ─────────────────────────────────────────────────────

    // Notify the engine of tab events
    void OnTabCreated(const AeonTabInfo& tab);
    void OnTabUpdated(const AeonTabInfo& tab);  // URL/title changed
    void OnTabFocused(uint64_t tab_id);
    void OnTabClosed(uint64_t tab_id);
    void OnTabNavigated(uint64_t tab_id, const char* new_url, const char* new_title);

    // ── Tab Queries ───────────────────────────────────────────────────────

    std::vector<AeonTabInfo> GetAllTabs() const;
    AeonTabInfo GetTab(uint64_t tab_id) const;
    std::vector<AeonTabInfo> GetTabsByState(AeonTabState state) const;
    std::vector<AeonTabInfo> GetTabsByTopic(const char* topic) const;
    std::vector<AeonTabInfo> GetTabsByGroup(int32_t group_id) const;

    // ── Topic Classification ──────────────────────────────────────────────

    // Classify a URL/title into a topic (runs on-device TFLite)
    const char* ClassifyTopic(const char* url, const char* title,
                               float* out_confidence = nullptr) const;

    // Reclassify all tabs (useful after model update)
    void ReclassifyAllTabs();

    // ── Auto-Grouping ────────────────────────────────────────────────────

    // Force a grouping pass (normally runs automatically)
    void RunGroupingPass();

    // Get all current groups
    std::vector<AeonTabGroup> GetGroups() const;

    // Move a tab to a specific group (user override)
    void MoveTabToGroup(uint64_t tab_id, int32_t group_id);

    // Rename a group (user override)
    void RenameGroup(int32_t group_id, const char* name);

    // Dissolve a group (ungroup all tabs in it)
    void DissolveGroup(int32_t group_id);

    // ── Journey Detection ────────────────────────────────────────────────

    std::vector<AeonJourney> GetActiveJourneys() const;
    AeonJourney GetJourney(uint64_t journey_id) const;
    std::vector<AeonJourney> GetCompletedJourneys(uint32_t max_count = 20) const;

    // ── Memory Management ────────────────────────────────────────────────

    // Get current memory pressure level
    AeonMemoryPressure GetMemoryPressure() const;

    // Manually hibernate a specific tab
    void HibernateTab(uint64_t tab_id);

    // Hibernate all tabs except the currently focused one
    void HibernateAllExcept(uint64_t active_tab_id);

    // Restore a hibernated/archived tab
    void RestoreTab(uint64_t tab_id);

    // Get estimated memory savings from hibernation
    uint64_t GetHibernationSavings() const;

    // ── Smart Reopen / Predictions ───────────────────────────────────────

    std::vector<AeonTabPrediction> GetPredictions(uint32_t max_count = 5) const;

    // Log that a prediction was accepted (improves model)
    void AcceptPrediction(const char* url);

    // Log that a prediction was dismissed (improves model)
    void DismissPrediction(const char* url);

    // ── Tab Decay & Archive ──────────────────────────────────────────────

    // Get tabs that are candidates for archival
    std::vector<AeonTabInfo> GetDecayCandidates() const;

    // Archive a tab (save to disk, remove from memory)
    void ArchiveTab(uint64_t tab_id);

    // Get all archived tabs
    std::vector<AeonTabInfo> GetArchivedTabs() const;

    // Restore an archived tab
    void UnarchiveTab(uint64_t tab_id);

    // ── Cross-Device Sync (via AeonHive) ─────────────────────────────────

    // Sync open tabs to other devices (encrypted via SyncTabs topic)
    void SyncTabsToMesh();

    // Get tabs from other devices
    std::vector<AeonTabInfo> GetRemoteTabs() const;

    // ── Analytics & Insights ─────────────────────────────────────────────

    struct BrowsingInsight {
        char        title[128];         // "You spend 3.2h/day on Social Media"
        char        category[32];       // "time_usage", "habit", "suggestion"
        char        suggestion[256];    // "Consider setting a 1h daily limit"
        float       severity;           // 0.0 = info, 1.0 = critical
    };

    std::vector<BrowsingInsight> GetInsights() const;

    // ── Diagnostics ──────────────────────────────────────────────────────

    std::string DiagnosticReport() const;
    bool CanOffloadToHive() const override { return false; }

    // ── Callbacks ────────────────────────────────────────────────────────

    void OnGroupCreated(AeonTabGroupCallback cb);
    void OnJourneyDetected(AeonJourneyCallback cb);
    void OnPredictionsReady(AeonPredictionCallback cb);
    void OnMemoryPressureChanged(AeonMemoryPressureCallback cb);

private:
    AeonTabIntelligenceImpl* m_impl = nullptr;
};

// Lifecycle managed by AeonMain.cpp via g_TabIntel global pointer.

