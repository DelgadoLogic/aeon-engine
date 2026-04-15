// =============================================================================
// aeon_journey_analytics.cpp — AeonJourneyAnalytics Implementation
// Pillar 5: Browsing Journey Detection, Intent Classification & Patterns
//
// Builds a directed graph of page visits per session, uses temporal +
// semantic proximity to detect journey boundaries, classifies user intent,
// and tracks recurring browsing patterns — all fully on-device.
// =============================================================================

#include "aeon_journey_analytics.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <numeric>
#include <unordered_map>
#include <vector>

// =============================================================================
// Intent detection keyword tables
// =============================================================================

struct IntentSignal {
    AeonJourneyIntent intent;
    const char*       keywords[12];
};

static const IntentSignal INTENT_SIGNALS[] = {
    { AeonJourneyIntent::Shopping,
      { "buy", "cart", "checkout", "price", "shop", "order",
        "add to cart", "deal", "discount", "compare", "review", nullptr } },
    { AeonJourneyIntent::Research,
      { "what is", "how to", "guide", "tutorial", "wiki", "explain",
        "definition", "meaning", "overview", "introduction", nullptr } },
    { AeonJourneyIntent::FinancialTx,
      { "bank", "transfer", "payment", "mpesa", "upi", "balance",
        "account", "deposit", "withdraw", "loan", nullptr } },
    { AeonJourneyIntent::GovernmentSvc,
      { "passport", "visa", "license", "registration", "tax",
        "government", "civic", "apply", "renew", nullptr } },
    { AeonJourneyIntent::HealthInfo,
      { "symptom", "doctor", "hospital", "medicine", "treatment",
        "health", "diagnosis", "pharmacy", "clinic", nullptr } },
    { AeonJourneyIntent::Education,
      { "course", "learn", "study", "class", "lecture", "exam",
        "university", "school", "assignment", "homework", nullptr } },
    { AeonJourneyIntent::Entertainment,
      { "watch", "movie", "video", "music", "stream", "play",
        "game", "listen", "episode", "trailer", nullptr } },
    { AeonJourneyIntent::Communication,
      { "inbox", "compose", "reply", "message", "chat", "email",
        "send", "call", "contact", nullptr } },
    { AeonJourneyIntent::Navigation,
      { "directions", "map", "route", "navigate", "location",
        "near me", "distance", "drive to", nullptr } },
    { AeonJourneyIntent::Productivity,
      { "spreadsheet", "document", "presentation", "project",
        "task", "board", "workspace", "edit", nullptr } },
};
static constexpr size_t INTENT_SIGNAL_COUNT =
    sizeof(INTENT_SIGNALS) / sizeof(INTENT_SIGNALS[0]);


// =============================================================================
// Journey stage detection heuristics
// =============================================================================

static AeonJourneyStage DetectStageFromNode(
    const AeonJourneyNode& node,
    uint32_t nodes_in_journey,
    bool has_form_submit,
    bool has_download)
{
    // Confirmation indicators
    if (has_form_submit && nodes_in_journey > 3) {
        std::string url_lower(node.url);
        for (auto& c : url_lower) c = (char)tolower((unsigned char)c);

        if (url_lower.find("confirm") != std::string::npos ||
            url_lower.find("receipt") != std::string::npos ||
            url_lower.find("success") != std::string::npos ||
            url_lower.find("thank") != std::string::npos) {
            return AeonJourneyStage::Confirmation;
        }
        return AeonJourneyStage::Action;
    }

    // Download = action
    if (has_download) return AeonJourneyStage::Action;

    // Entry = first node or from search
    if (nodes_in_journey <= 1 || node.from_search)
        return AeonJourneyStage::Entry;

    // Back button = comparison/decision
    if (node.from_back_button)
        return AeonJourneyStage::Comparison;

    // If many pages visited, likely in decision stage
    if (nodes_in_journey > 6)
        return AeonJourneyStage::Decision;

    // Mid-journey = exploration
    return AeonJourneyStage::Exploration;
}


// =============================================================================
// AeonJourneyAnalyticsImpl (PIMPL)
// =============================================================================

class AeonJourneyAnalyticsImpl {
public:
    mutable std::mutex                mtx;
    bool                              initialized = false;

    // ── Active journeys ──────────────────────────────────────────────────

    struct JourneyData {
        AeonJourneySummary              summary;
        std::vector<AeonJourneyNode>    nodes;
        std::vector<AeonJourneyEdge>    edges;
        std::unordered_map<uint64_t, uint64_t> tab_to_latest_node;
        bool                            has_form_submit = false;
        bool                            has_download    = false;
        uint64_t                        next_node_id    = 1;
    };

    std::unordered_map<uint64_t, JourneyData>   active_journeys;
    std::vector<AeonJourneySummary>              completed_journeys;
    uint64_t                                     next_journey_id = 1;

    // ── Tab → Journey mapping ────────────────────────────────────────────

    std::unordered_map<uint64_t, uint64_t>      tab_journey_map;

    // ── Pattern detection ────────────────────────────────────────────────

    struct VisitRecord {
        char        domain[256];
        uint32_t    hour;
        uint32_t    day_of_week;
        uint64_t    timestamp;
        uint32_t    dwell_secs;
    };
    std::vector<VisitRecord>                    visit_history;
    static constexpr size_t MAX_HISTORY = 10000;

    std::vector<AeonBrowsingPattern>            detected_patterns;

    // ── Domain statistics ────────────────────────────────────────────────

    struct DomainAccum {
        uint32_t    total_time_secs = 0;
        uint32_t    visit_count     = 0;
    };
    std::unordered_map<std::string, DomainAccum> domain_stats;

    // Hourly + daily distributions
    uint32_t    hourly_visits[24] = {0};
    uint32_t    daily_visits[7]   = {0};

    // ── Callbacks ────────────────────────────────────────────────────────

    AeonJourneyStartCallback        on_journey_start;
    AeonJourneyStageCallback        on_stage_changed;
    AeonJourneyCompleteCallback     on_journey_complete;
    AeonPatternDetectedCallback     on_pattern_detected;

    // ── Helper: domain extraction ────────────────────────────────────────

    std::string ExtractDomain(const char* url) {
        std::string u(url);
        size_t proto = u.find("://");
        if (proto == std::string::npos) return u;
        size_t start = proto + 3;
        size_t end = u.find('/', start);
        if (end == std::string::npos) end = u.size();
        return u.substr(start, end - start);
    }

    // ── Helper: semantic similarity ──────────────────────────────────────
    // Simple domain + keyword overlap check. In production, use embeddings.

    float SemanticSimilarity(const AeonJourneyNode& a, const char* url,
                              const char* title) {
        std::string d1 = ExtractDomain(a.url);
        std::string d2 = ExtractDomain(url);

        // Same domain = high similarity
        if (d1 == d2) return 0.9f;

        // Check title word overlap
        std::string t1(a.title), t2(title ? title : "");
        for (auto& c : t1) c = (char)tolower((unsigned char)c);
        for (auto& c : t2) c = (char)tolower((unsigned char)c);

        // Simple word overlap
        int overlap = 0, total = 0;
        size_t pos = 0;
        while (pos < t1.size()) {
            size_t space = t1.find(' ', pos);
            if (space == std::string::npos) space = t1.size();
            std::string word = t1.substr(pos, space - pos);
            if (word.size() > 3) {
                total++;
                if (t2.find(word) != std::string::npos) overlap++;
            }
            pos = space + 1;
        }

        if (total == 0) return 0.1f;
        return 0.3f + 0.6f * ((float)overlap / total);
    }

    // ── Helper: find or create journey for a tab ─────────────────────────

    uint64_t FindOrCreateJourney(uint64_t tab_id, const char* url,
                                  const char* title, bool from_search,
                                  bool& out_new_journey,
                                  AeonJourneyIntent& out_intent) {
        out_new_journey = false;

        // Check if tab already belongs to a journey
        auto it = tab_journey_map.find(tab_id);
        if (it != tab_journey_map.end()) {
            auto jit = active_journeys.find(it->second);
            if (jit != active_journeys.end()) {
                return it->second;
            }
        }

        // Check if this new tab is semantically related to existing journey
        for (auto& [jid, jdata] : active_journeys) {
            if (jdata.nodes.empty()) continue;
            float sim = SemanticSimilarity(jdata.nodes.back(), url, title);
            if (sim > 0.6f) {
                tab_journey_map[tab_id] = jid;
                return jid;
            }
        }

        // Create new journey
        uint64_t jid = next_journey_id++;

        JourneyData jdata;
        memset(&jdata.summary, 0, sizeof(jdata.summary));
        jdata.summary.journey_id = jid;
        jdata.summary.start_utc  = (uint64_t)time(nullptr);
        jdata.summary.session_count = 1;

        // Detect intent
        AeonJourneyIntent intent = DetectIntentFromText(url, title);
        jdata.summary.intent = intent;

        // Auto-generate journey name
        if (title && strlen(title) > 0) {
            snprintf(jdata.summary.title, sizeof(jdata.summary.title),
                     "Journey: %s", title);
        } else {
            std::string domain = ExtractDomain(url);
            snprintf(jdata.summary.title, sizeof(jdata.summary.title),
                     "Journey: %s", domain.c_str());
        }

        active_journeys[jid] = std::move(jdata);
        tab_journey_map[tab_id] = jid;

        // Signal to caller that a new journey was created — callback
        // must be fired OUTSIDE the lock to prevent re-entrancy.
        out_new_journey = true;
        out_intent = intent;

        return jid;
    }

    // ── Helper: intent detection ─────────────────────────────────────────

    AeonJourneyIntent DetectIntentFromText(const char* url, const char* title) {
        std::string combined;
        if (url) combined += url;
        combined += " ";
        if (title) combined += title;
        for (auto& c : combined) c = (char)tolower((unsigned char)c);

        int best_score = 0;
        AeonJourneyIntent best_intent = AeonJourneyIntent::Unknown;

        for (size_t i = 0; i < INTENT_SIGNAL_COUNT; ++i) {
            int score = 0;
            for (int k = 0; INTENT_SIGNALS[i].keywords[k]; ++k) {
                if (combined.find(INTENT_SIGNALS[i].keywords[k]) != std::string::npos) {
                    score++;
                }
            }
            if (score > best_score) {
                best_score = score;
                best_intent = INTENT_SIGNALS[i].intent;
            }
        }

        return best_intent;
    }

    // ── Helper: journey completion heuristic ─────────────────────────────

    void UpdateJourneyCompletion(JourneyData& jdata) {
        auto& s = jdata.summary;

        // Base completion on stage progression
        AeonJourneyStage max_stage = AeonJourneyStage::Entry;
        for (const auto& node : jdata.nodes) {
            if (node.stage > max_stage) max_stage = node.stage;
        }

        s.final_stage = max_stage;
        s.completion = (float)(static_cast<uint8_t>(max_stage) + 1) / 7.0f;

        // Linearity = 1.0 - (back_presses / total_pages)
        if (s.total_pages > 0) {
            s.linearity = 1.0f - ((float)s.back_presses /
                                  (float)s.total_pages);
            if (s.linearity < 0) s.linearity = 0;
        }

        // Satisfaction estimate (simple heuristic)
        s.satisfaction = s.completion * 0.5f + s.linearity * 0.3f +
                         (s.total_time_secs > 60 ? 0.2f : 0.0f);
    }

    // ── Helper: check if journey should be completed ─────────────────────

    bool ShouldCompleteJourney(const JourneyData& jdata) {
        uint64_t now = (uint64_t)time(nullptr);
        uint64_t idle = now - jdata.summary.end_utc;

        // Complete if idle for 30+ minutes and has reached action/confirmation
        if (idle > 1800 &&
            jdata.summary.final_stage >= AeonJourneyStage::Action) {
            return true;
        }

        // Complete if idle for 2+ hours regardless of stage
        if (idle > 7200) {
            return true;
        }

        return false;
    }
};


// =============================================================================
// AeonJourneyAnalytics Implementation
// =============================================================================

AeonJourneyAnalytics::AeonJourneyAnalytics()
    : m_impl(new AeonJourneyAnalyticsImpl()) {}

AeonJourneyAnalytics::~AeonJourneyAnalytics() {
    if (m_impl) {
        Shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

bool AeonJourneyAnalytics::Initialize(const ResourceBudget& budget) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    if (m_impl->initialized) return true;

    // Store the resource budget from the browser
    SetResourceBudget(budget);

    memset(m_impl->hourly_visits, 0, sizeof(m_impl->hourly_visits));
    memset(m_impl->daily_visits, 0, sizeof(m_impl->daily_visits));

    m_impl->initialized = true;
    m_healthy = true;
    return true;
}

void AeonJourneyAnalytics::Shutdown() {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    if (!m_impl->initialized) return;

    m_impl->active_journeys.clear();
    m_impl->completed_journeys.clear();
    m_impl->tab_journey_map.clear();
    m_impl->visit_history.clear();
    m_impl->domain_stats.clear();
    m_impl->detected_patterns.clear();

    m_impl->initialized = false;
}


// ── Event Ingestion ──────────────────────────────────────────────────────────

void AeonJourneyAnalytics::OnPageVisit(uint64_t tab_id, const char* url,
                                        const char* title, bool from_search,
                                        bool from_link, bool from_back) {
    // Deferred callback data — collected under lock, fired after release.
    // This prevents re-entrancy deadlocks if a callback calls back into
    // this engine while the mutex is held.
    bool                    fire_journey_start = false;
    uint64_t                deferred_jid       = 0;
    AeonJourneyIntent       deferred_intent    = AeonJourneyIntent::Unknown;
    bool                    fire_stage_change  = false;
    AeonJourneyStage        deferred_old_stage = AeonJourneyStage::Entry;
    AeonJourneyStage        deferred_new_stage = AeonJourneyStage::Entry;
    std::vector<AeonJourneySummary> deferred_completions;

    {
        std::lock_guard<std::mutex> lock(m_impl->mtx);

        uint64_t now = (uint64_t)time(nullptr);

        // Find or create journey (callback deferred via out params)
        bool new_journey = false;
        AeonJourneyIntent new_intent = AeonJourneyIntent::Unknown;
        uint64_t jid = m_impl->FindOrCreateJourney(
            tab_id, url, title, from_search, new_journey, new_intent);

        if (new_journey && m_impl->on_journey_start) {
            fire_journey_start = true;
            deferred_jid = jid;
            deferred_intent = new_intent;
        }

        auto jit = m_impl->active_journeys.find(jid);
        if (jit != m_impl->active_journeys.end()) {
            auto& jdata = jit->second;

            // Create node
            AeonJourneyNode node;
            memset(&node, 0, sizeof(node));
            node.node_id = jdata.next_node_id++;
            node.tab_id  = tab_id;
            strncpy(node.url, url, sizeof(node.url) - 1);
            if (title) strncpy(node.title, title, sizeof(node.title) - 1);

            std::string domain = m_impl->ExtractDomain(url);
            strncpy(node.domain, domain.c_str(), sizeof(node.domain) - 1);

            node.visit_utc    = now;
            node.from_search  = from_search;
            node.from_link    = from_link;
            node.from_back_button = from_back;
            node.from_direct  = !from_search && !from_link && !from_back;

            // Detect stage
            node.stage = DetectStageFromNode(node, (uint32_t)jdata.nodes.size(),
                                             jdata.has_form_submit, jdata.has_download);

            // Check for stage change
            AeonJourneyStage old_stage = jdata.summary.final_stage;

            // Create edge from previous node in same tab
            auto tab_node_it = jdata.tab_to_latest_node.find(tab_id);
            if (tab_node_it != jdata.tab_to_latest_node.end()) {
                AeonJourneyEdge edge;
                edge.from_node_id = tab_node_it->second;
                edge.to_node_id   = node.node_id;
                edge.timestamp_utc = now;

                if (from_search)    edge.type = AeonJourneyEdge::Type::Search;
                else if (from_back) edge.type = AeonJourneyEdge::Type::BackButton;
                else if (from_link) edge.type = AeonJourneyEdge::Type::Click;
                else                edge.type = AeonJourneyEdge::Type::Click;

                jdata.edges.push_back(edge);
            }

            // Update tab->node mapping
            jdata.tab_to_latest_node[tab_id] = node.node_id;

            // Add node
            jdata.nodes.push_back(node);

            // Update summary
            jdata.summary.total_pages++;
            jdata.summary.end_utc = now;
            if (from_search) jdata.summary.search_queries++;
            if (from_back)   jdata.summary.back_presses++;

            // Count unique domains
            std::map<std::string, bool> domains;
            for (const auto& n : jdata.nodes) domains[n.domain] = true;
            jdata.summary.unique_domains = (uint32_t)domains.size();

            // Time
            jdata.summary.total_time_secs = (uint32_t)(now - jdata.summary.start_utc);
            jdata.summary.days_elapsed =
                (uint32_t)((now - jdata.summary.start_utc) / 86400) + 1;

            // Update completion
            m_impl->UpdateJourneyCompletion(jdata);

            // Capture stage change for deferred callback
            if (node.stage != old_stage && m_impl->on_stage_changed) {
                fire_stage_change = true;
                deferred_jid = jid;
                deferred_old_stage = old_stage;
                deferred_new_stage = node.stage;
            }

            // Record in visit history
            if (m_impl->visit_history.size() < AeonJourneyAnalyticsImpl::MAX_HISTORY) {
                AeonJourneyAnalyticsImpl::VisitRecord rec;
                strncpy(rec.domain, domain.c_str(), sizeof(rec.domain) - 1);
                rec.domain[sizeof(rec.domain) - 1] = '\0';
                time_t now_t = (time_t)now;
                struct tm* lt = localtime(&now_t);
                rec.hour = lt ? lt->tm_hour : 0;
                rec.day_of_week = lt ? lt->tm_wday : 0;
                rec.timestamp = now;
                rec.dwell_secs = 0;
                m_impl->visit_history.push_back(rec);

                // Update distributions
                m_impl->hourly_visits[rec.hour]++;
                m_impl->daily_visits[rec.day_of_week]++;
            }

            // Update domain stats
            m_impl->domain_stats[domain].visit_count++;
        }

        // Check completed journeys — capture completions for deferred callbacks
        // Use cached jid instead of re-calling FindOrCreateJourney
        for (auto it = m_impl->active_journeys.begin();
             it != m_impl->active_journeys.end(); ) {
            if (it->first != deferred_jid && m_impl->ShouldCompleteJourney(it->second)) {
                m_impl->completed_journeys.push_back(it->second.summary);
                if (m_impl->on_journey_complete) {
                    deferred_completions.push_back(it->second.summary);
                }
                // Clean up tab mappings
                uint64_t completed_jid = it->first;
                for (auto tm = m_impl->tab_journey_map.begin();
                     tm != m_impl->tab_journey_map.end(); ) {
                    if (tm->second == completed_jid)
                        tm = m_impl->tab_journey_map.erase(tm);
                    else
                        ++tm;
                }
                it = m_impl->active_journeys.erase(it);
            } else {
                ++it;
            }
        }
    } // ── mutex released ──

    // Fire all deferred callbacks outside the lock
    if (fire_journey_start && m_impl->on_journey_start) {
        m_impl->on_journey_start(deferred_jid, deferred_intent);
    }
    if (fire_stage_change && m_impl->on_stage_changed) {
        m_impl->on_stage_changed(deferred_jid, deferred_old_stage, deferred_new_stage);
    }
    for (const auto& summary : deferred_completions) {
        m_impl->on_journey_complete(summary);
    }
}

void AeonJourneyAnalytics::OnPageDwell(uint64_t tab_id, uint32_t dwell_secs,
                                        float engagement) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    auto tm = m_impl->tab_journey_map.find(tab_id);
    if (tm == m_impl->tab_journey_map.end()) return;

    auto jit = m_impl->active_journeys.find(tm->second);
    if (jit == m_impl->active_journeys.end()) return;

    auto& jdata = jit->second;
    auto node_it = jdata.tab_to_latest_node.find(tab_id);
    if (node_it == jdata.tab_to_latest_node.end()) return;

    // Update node
    for (auto& node : jdata.nodes) {
        if (node.node_id == node_it->second) {
            node.dwell_time_secs = dwell_secs;
            node.engagement_score = engagement;
            break;
        }
    }

    // Update domain stats
    if (!jdata.nodes.empty()) {
        std::string domain = jdata.nodes.back().domain;
        m_impl->domain_stats[domain].total_time_secs += dwell_secs;
    }
}

void AeonJourneyAnalytics::OnSearchQuery(uint64_t tab_id, const char* query) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    auto tm = m_impl->tab_journey_map.find(tab_id);
    if (tm == m_impl->tab_journey_map.end()) return;

    auto jit = m_impl->active_journeys.find(tm->second);
    if (jit == m_impl->active_journeys.end()) return;

    jit->second.summary.search_queries++;

    // Re-detect intent based on search query
    AeonJourneyIntent new_intent = m_impl->DetectIntentFromText(query, query);
    if (new_intent != AeonJourneyIntent::Unknown) {
        jit->second.summary.intent = new_intent;
    }
}

void AeonJourneyAnalytics::OnTabSwitch(uint64_t from_tab_id, uint64_t to_tab_id) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    // Check if to_tab belongs to same journey
    auto from_it = m_impl->tab_journey_map.find(from_tab_id);
    auto to_it   = m_impl->tab_journey_map.find(to_tab_id);

    if (from_it != m_impl->tab_journey_map.end() &&
        to_it != m_impl->tab_journey_map.end() &&
        from_it->second == to_it->second) {
        // Same journey — update tab count
        auto jit = m_impl->active_journeys.find(from_it->second);
        if (jit != m_impl->active_journeys.end()) {
            jit->second.summary.tab_count =
                (uint32_t)jit->second.tab_to_latest_node.size();
        }
    }
}

void AeonJourneyAnalytics::OnFormSubmit(uint64_t tab_id, const char* url) {
    // Deferred callback state
    bool     fire_stage_cb   = false;
    uint64_t deferred_jid    = 0;
    AeonJourneyStage deferred_old = AeonJourneyStage::Entry;
    AeonJourneyStage deferred_new = AeonJourneyStage::Entry;

    {
        std::lock_guard<std::mutex> lock(m_impl->mtx);

        auto tm = m_impl->tab_journey_map.find(tab_id);
        if (tm == m_impl->tab_journey_map.end()) return;

        auto jit = m_impl->active_journeys.find(tm->second);
        if (jit == m_impl->active_journeys.end()) return;

        jit->second.has_form_submit = true;

        // Likely action stage
        if (!jit->second.nodes.empty()) {
            auto& last = jit->second.nodes.back();
            AeonJourneyStage old = last.stage;
            last.stage = AeonJourneyStage::Action;

            if (old != last.stage && m_impl->on_stage_changed) {
                fire_stage_cb = true;
                deferred_jid  = tm->second;
                deferred_old  = old;
                deferred_new  = last.stage;
            }
        }

        m_impl->UpdateJourneyCompletion(jit->second);
    } // ── mutex released ──

    // Fire deferred callback outside the lock
    if (fire_stage_cb) {
        m_impl->on_stage_changed(deferred_jid, deferred_old, deferred_new);
    }
}

void AeonJourneyAnalytics::OnDownload(uint64_t tab_id, const char* url,
                                       const char* filename) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    auto tm = m_impl->tab_journey_map.find(tab_id);
    if (tm == m_impl->tab_journey_map.end()) return;

    auto jit = m_impl->active_journeys.find(tm->second);
    if (jit == m_impl->active_journeys.end()) return;

    jit->second.has_download = true;
    m_impl->UpdateJourneyCompletion(jit->second);
}


// ── Journey Queries ──────────────────────────────────────────────────────────

std::vector<AeonJourneySummary> AeonJourneyAnalytics::GetActiveJourneys() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    std::vector<AeonJourneySummary> result;
    for (const auto& [id, jdata] : m_impl->active_journeys) {
        result.push_back(jdata.summary);
    }
    return result;
}

std::vector<AeonJourneySummary> AeonJourneyAnalytics::GetCompletedJourneys(
    uint32_t max_count) const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    auto& completed = m_impl->completed_journeys;
    if (completed.size() <= max_count) return completed;

    return std::vector<AeonJourneySummary>(
        completed.end() - max_count, completed.end());
}

void AeonJourneyAnalytics::GetJourneyGraph(
    uint64_t journey_id,
    std::vector<AeonJourneyNode>& nodes,
    std::vector<AeonJourneyEdge>& edges) const
{
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    auto jit = m_impl->active_journeys.find(journey_id);
    if (jit == m_impl->active_journeys.end()) {
        nodes.clear();
        edges.clear();
        return;
    }

    nodes = jit->second.nodes;
    edges = jit->second.edges;
}

int64_t AeonJourneyAnalytics::GetJourneyForTab(uint64_t tab_id) const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    auto it = m_impl->tab_journey_map.find(tab_id);
    if (it != m_impl->tab_journey_map.end()) return (int64_t)it->second;
    return -1;
}


// ── Intent Detection (public API) ───────────────────────────────────────────

AeonJourneyIntent AeonJourneyAnalytics::DetectIntent(
    const char* url, const char* title, const char* referrer) const
{
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    // Combine all available text
    std::string combined;
    if (url) combined += url;
    combined += " ";
    if (title) combined += title;
    if (referrer) { combined += " "; combined += referrer; }

    return m_impl->DetectIntentFromText(combined.c_str(), combined.c_str());
}


// ── Pattern Analysis ─────────────────────────────────────────────────────────

std::vector<AeonBrowsingPattern> AeonJourneyAnalytics::GetPatterns() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    return m_impl->detected_patterns;
}

void AeonJourneyAnalytics::GetHourlyDistribution(float out_hours[24]) const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    uint32_t total = 0;
    for (int i = 0; i < 24; ++i) total += m_impl->hourly_visits[i];

    if (total == 0) {
        memset(out_hours, 0, 24 * sizeof(float));
        return;
    }

    for (int i = 0; i < 24; ++i) {
        out_hours[i] = (float)m_impl->hourly_visits[i] / (float)total;
    }
}

void AeonJourneyAnalytics::GetDailyDistribution(float out_days[7]) const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    uint32_t total = 0;
    for (int i = 0; i < 7; ++i) total += m_impl->daily_visits[i];

    if (total == 0) {
        memset(out_days, 0, 7 * sizeof(float));
        return;
    }

    for (int i = 0; i < 7; ++i) {
        out_days[i] = (float)m_impl->daily_visits[i] / (float)total;
    }
}

std::vector<AeonJourneyAnalytics::DomainStat>
AeonJourneyAnalytics::GetTopDomains(uint32_t max_count) const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    std::vector<DomainStat> result;
    for (const auto& [domain, accum] : m_impl->domain_stats) {
        DomainStat ds;
        memset(&ds, 0, sizeof(ds));
        strncpy(ds.domain, domain.c_str(), sizeof(ds.domain) - 1);
        ds.total_time_secs = accum.total_time_secs;
        ds.visit_count = accum.visit_count;
        ds.avg_dwell_secs = accum.visit_count > 0 ?
            (float)accum.total_time_secs / accum.visit_count : 0;
        result.push_back(ds);
    }

    // Sort by total time spent (descending)
    std::sort(result.begin(), result.end(),
        [](const DomainStat& a, const DomainStat& b) {
            return a.total_time_secs > b.total_time_secs;
        });

    if (result.size() > max_count) result.resize(max_count);
    return result;
}


// ── Cross-Session Resumption ────────────────────────────────────────────────

std::vector<AeonJourneySummary> AeonJourneyAnalytics::GetResumableJourneys() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    uint64_t now = (uint64_t)time(nullptr);
    std::vector<AeonJourneySummary> resumable;

    // Active journeys that haven't been touched in 30+ mins but < 7 days
    for (const auto& [id, jdata] : m_impl->active_journeys) {
        uint64_t idle = now - jdata.summary.end_utc;
        if (idle > 1800 && idle < 604800 &&
            jdata.summary.completion < 0.8f) {
            resumable.push_back(jdata.summary);
        }
    }

    // Recently completed journeys that might need follow-up
    for (const auto& s : m_impl->completed_journeys) {
        uint64_t age = now - s.end_utc;
        if (age < 86400 && s.completion < 0.6f) {
            resumable.push_back(s);
        }
    }

    return resumable;
}

void AeonJourneyAnalytics::ResumeJourney(uint64_t journey_id,
                                           uint64_t new_tab_id) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    auto jit = m_impl->active_journeys.find(journey_id);
    if (jit != m_impl->active_journeys.end()) {
        m_impl->tab_journey_map[new_tab_id] = journey_id;
        jit->second.summary.session_count++;
        jit->second.summary.end_utc = (uint64_t)time(nullptr);
    }
}


// ── Data Management ──────────────────────────────────────────────────────────

std::vector<uint8_t> AeonJourneyAnalytics::ExportData() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    // Binary wire format (all little-endian, native struct layout):
    //   [4B magic "AJEX"] [4B version=1]
    //   [4B completed_count] [completed_journeys...]
    //   [4B visit_count]     [visit_records...]
    //   [4B domain_count]    [domain_stats entries...]
    //   [96B hourly_visits[24] + daily_visits[7] padding]

    std::vector<uint8_t> buf;
    buf.reserve(64 * 1024); // Pre-alloc 64KB

    auto push = [&](const void* p, size_t n) {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
        buf.insert(buf.end(), b, b + n);
    };

    // Header
    const char magic[4] = {'A','J','E','X'};
    push(magic, 4);
    uint32_t version = 1;
    push(&version, 4);

    // Completed journeys
    uint32_t ccount = (uint32_t)m_impl->completed_journeys.size();
    push(&ccount, 4);
    for (const auto& s : m_impl->completed_journeys) {
        push(&s, sizeof(AeonJourneySummary));
    }

    // Visit history
    uint32_t vcount = (uint32_t)m_impl->visit_history.size();
    push(&vcount, 4);
    for (const auto& v : m_impl->visit_history) {
        push(&v, sizeof(AeonJourneyAnalyticsImpl::VisitRecord));
    }

    // Domain stats
    uint32_t dcount = (uint32_t)m_impl->domain_stats.size();
    push(&dcount, 4);
    for (const auto& [domain, accum] : m_impl->domain_stats) {
        // Write domain string (256 bytes, zero-padded)
        char dbuf[256] = {0};
        strncpy(dbuf, domain.c_str(), 255);
        push(dbuf, 256);
        push(&accum.total_time_secs, 4);
        push(&accum.visit_count, 4);
    }

    // Distributions
    push(m_impl->hourly_visits, sizeof(m_impl->hourly_visits));
    push(m_impl->daily_visits, sizeof(m_impl->daily_visits));

    return buf;
}

bool AeonJourneyAnalytics::ImportData(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    if (!data || len < 12) return false;

    size_t offset = 0;
    auto read = [&](void* dst, size_t n) -> bool {
        if (offset + n > len) return false;
        memcpy(dst, data + offset, n);
        offset += n;
        return true;
    };

    // Verify magic
    char magic[4];
    if (!read(magic, 4)) return false;
    if (memcmp(magic, "AJEX", 4) != 0) return false;

    uint32_t version;
    if (!read(&version, 4)) return false;
    if (version != 1) return false;

    // Completed journeys
    uint32_t ccount;
    if (!read(&ccount, 4)) return false;
    if (ccount > 100000) return false; // Sanity check
    for (uint32_t i = 0; i < ccount; ++i) {
        AeonJourneySummary s;
        if (!read(&s, sizeof(s))) return false;
        m_impl->completed_journeys.push_back(s);
    }

    // Visit history
    uint32_t vcount;
    if (!read(&vcount, 4)) return false;
    if (vcount > AeonJourneyAnalyticsImpl::MAX_HISTORY) vcount = AeonJourneyAnalyticsImpl::MAX_HISTORY;
    for (uint32_t i = 0; i < vcount; ++i) {
        AeonJourneyAnalyticsImpl::VisitRecord v;
        if (!read(&v, sizeof(v))) return false;
        m_impl->visit_history.push_back(v);
    }

    // Domain stats
    uint32_t dcount;
    if (!read(&dcount, 4)) return false;
    if (dcount > 50000) return false;
    for (uint32_t i = 0; i < dcount; ++i) {
        char dbuf[256];
        if (!read(dbuf, 256)) return false;
        uint32_t total_time, visit_count;
        if (!read(&total_time, 4)) return false;
        if (!read(&visit_count, 4)) return false;

        auto& accum = m_impl->domain_stats[std::string(dbuf)];
        accum.total_time_secs += total_time;
        accum.visit_count += visit_count;
    }

    // Distributions
    if (!read(m_impl->hourly_visits, sizeof(m_impl->hourly_visits))) return false;
    if (!read(m_impl->daily_visits, sizeof(m_impl->daily_visits))) return false;

    return true;
}

void AeonJourneyAnalytics::ClearAllData() {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->active_journeys.clear();
    m_impl->completed_journeys.clear();
    m_impl->tab_journey_map.clear();
    m_impl->visit_history.clear();
    m_impl->domain_stats.clear();
    m_impl->detected_patterns.clear();
    memset(m_impl->hourly_visits, 0, sizeof(m_impl->hourly_visits));
    memset(m_impl->daily_visits, 0, sizeof(m_impl->daily_visits));
}

uint64_t AeonJourneyAnalytics::GetStorageUsed() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    uint64_t total = 0;

    // Active journeys
    for (const auto& [id, jdata] : m_impl->active_journeys) {
        total += sizeof(AeonJourneySummary);
        total += jdata.nodes.size() * sizeof(AeonJourneyNode);
        total += jdata.edges.size() * sizeof(AeonJourneyEdge);
    }

    // Completed journeys
    total += m_impl->completed_journeys.size() * sizeof(AeonJourneySummary);

    // Visit history
    total += m_impl->visit_history.size() *
             sizeof(AeonJourneyAnalyticsImpl::VisitRecord);

    return total;
}


// ── Diagnostics ──────────────────────────────────────────────────────────────

std::string AeonJourneyAnalytics::DiagnosticReport() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    std::string report;
    report.reserve(2048);

    report += "=== AeonJourneyAnalytics Diagnostic Report ===\n\n";

    report += "Active Journeys: " +
              std::to_string(m_impl->active_journeys.size()) + "\n";
    report += "Completed Journeys: " +
              std::to_string(m_impl->completed_journeys.size()) + "\n";
    report += "Tab Mappings: " +
              std::to_string(m_impl->tab_journey_map.size()) + "\n";
    report += "Visit History: " +
              std::to_string(m_impl->visit_history.size()) + " records\n";
    report += "Domain Stats: " +
              std::to_string(m_impl->domain_stats.size()) + " domains\n";
    // Inline storage calculation to avoid deadlock — GetStorageUsed() also locks mtx
    uint64_t storage = 0;
    for (const auto& [id, jdata] : m_impl->active_journeys) {
        storage += sizeof(AeonJourneySummary);
        storage += jdata.nodes.size() * sizeof(AeonJourneyNode);
        storage += jdata.edges.size() * sizeof(AeonJourneyEdge);
    }
    storage += m_impl->completed_journeys.size() * sizeof(AeonJourneySummary);
    storage += m_impl->visit_history.size() *
               sizeof(AeonJourneyAnalyticsImpl::VisitRecord);
    report += "Storage Used: " +
              std::to_string(storage / 1024) + " KB\n\n";

    // Active journey details
    for (const auto& [id, jdata] : m_impl->active_journeys) {
        const char* intent_names[] = {
            "Unknown", "Research", "Shopping", "Navigation",
            "Communication", "Entertainment", "Productivity",
            "Financial", "Government", "Health", "Education"
        };
        report += "Journey #" + std::to_string(id) + ": " +
                  jdata.summary.title + "\n";
        uint8_t idx = static_cast<uint8_t>(jdata.summary.intent);
        if (idx <= 10) {
            report += "  Intent: ";
            report += intent_names[idx];
            report += "\n";
        }
        report += "  Pages: " +
                  std::to_string(jdata.summary.total_pages) + "\n";
        report += "  Completion: " +
                  std::to_string((int)(jdata.summary.completion * 100)) + "%\n";
        report += "  Linearity: " +
                  std::to_string((int)(jdata.summary.linearity * 100)) + "%\n\n";
    }

    // Hourly distribution
    report += "Hourly Distribution:\n";
    uint32_t max_h = *std::max_element(m_impl->hourly_visits,
                                        m_impl->hourly_visits + 24);
    for (int h = 0; h < 24; ++h) {
        char line[64];
        snprintf(line, sizeof(line), "  %02d:00  ", h);
        report += line;

        int bar_len = max_h > 0 ?
            (int)(20.0f * m_impl->hourly_visits[h] / max_h) : 0;
        for (int b = 0; b < bar_len; ++b) report += "#";
        report += " " + std::to_string(m_impl->hourly_visits[h]) + "\n";
    }

    report += "\n=== End Report ===\n";
    return report;
}


// ── Callbacks ────────────────────────────────────────────────────────────────

void AeonJourneyAnalytics::OnJourneyStarted(AeonJourneyStartCallback cb) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->on_journey_start = std::move(cb);
}

void AeonJourneyAnalytics::OnStageChanged(AeonJourneyStageCallback cb) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->on_stage_changed = std::move(cb);
}

void AeonJourneyAnalytics::OnJourneyCompleted(AeonJourneyCompleteCallback cb) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->on_journey_complete = std::move(cb);
}

void AeonJourneyAnalytics::OnPatternDetected(AeonPatternDetectedCallback cb) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->on_pattern_detected = std::move(cb);
}


// Global singleton removed — AeonMain.cpp manages the lifecycle
// via heap allocation with g_JourneyAI global pointer.
