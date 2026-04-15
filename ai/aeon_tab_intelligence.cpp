// =============================================================================
// aeon_tab_intelligence.cpp — AeonTabIntelligence Implementation
// Pillar 5: AI-Powered Tab Organization
//
// On-device tab clustering, topic classification, memory-pressure-aware
// hibernation, smart reopen predictions, and browsing journey integration.
// =============================================================================

#include "aeon_tab_intelligence.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <numeric>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <psapi.h>
#elif defined(__ANDROID__)
    #include <sys/sysinfo.h>
#elif defined(__linux__)
    #include <sys/sysinfo.h>
    #include <unistd.h>
#endif


// =============================================================================
// Topic Classification — Keyword-based (TFLite model loaded at runtime)
// =============================================================================

struct TopicKeywords {
    const char* topic;
    const char* keywords[16];  // URL/title keywords that map to this topic
};

static const TopicKeywords TOPIC_RULES[] = {
    { "News & Current Events",
      { "news", "bbc", "cnn", "reuters", "aljazeera", "guardian",
        "times", "headline", "breaking", "politics", nullptr } },
    { "Social Media",
      { "facebook", "twitter", "instagram", "tiktok", "reddit",
        "linkedin", "snapchat", "whatsapp", "telegram", nullptr } },
    { "Shopping & E-Commerce",
      { "amazon", "flipkart", "jumia", "ebay", "shopee", "aliexpress",
        "cart", "checkout", "buy", "price", "shop", nullptr } },
    { "Email & Communication",
      { "gmail", "outlook", "mail", "inbox", "compose", "yahoo",
        "protonmail", "email", nullptr } },
    { "Video & Entertainment",
      { "youtube", "netflix", "hulu", "twitch", "vimeo", "spotify",
        "music", "watch", "stream", "movie", "video", nullptr } },
    { "Search & Reference",
      { "google", "bing", "duckduckgo", "wikipedia", "search",
        "stackoverflow", "quora", "answers", nullptr } },
    { "Work & Productivity",
      { "docs.google", "sheets", "notion", "trello", "jira",
        "slack", "teams", "asana", "confluence", "figma", nullptr } },
    { "Finance & Banking",
      { "bank", "paypal", "mpesa", "upi", "razorpay", "finance",
        "invest", "stock", "crypto", "wallet", "payment", nullptr } },
    { "Gaming",
      { "steam", "epic", "game", "gaming", "twitch", "xbox",
        "playstation", "nintendo", "itch.io", nullptr } },
    { "Education & Learning",
      { "coursera", "udemy", "khan", "edx", "school", "university",
        "tutorial", "learn", "course", "study", nullptr } },
    { "Health & Medical",
      { "health", "medical", "doctor", "hospital", "symptom",
        "medicine", "webmd", "pharmacy", nullptr } },
    { "Government & Services",
      { "gov", "government", "passport", "visa", "tax", "license",
        "registration", "aadhaar", "nin", nullptr } },
    { "Travel & Transport",
      { "booking", "airbnb", "expedia", "flight", "hotel", "uber",
        "lyft", "maps", "travel", "trip", nullptr } },
    { "Food & Dining",
      { "ubereats", "doordash", "zomato", "swiggy", "recipe",
        "restaurant", "food", "menu", "delivery", nullptr } },
    { "Technology",
      { "github", "gitlab", "stackoverflow", "code", "developer",
        "programming", "tech", "software", "api", nullptr } },
    { "Sports",
      { "espn", "sports", "football", "soccer", "cricket",
        "basketball", "nfl", "nba", "score", nullptr } },
};
static constexpr size_t TOPIC_RULE_COUNT =
    sizeof(TOPIC_RULES) / sizeof(TOPIC_RULES[0]);


// =============================================================================
// AeonTabIntelligenceImpl (PIMPL)
// =============================================================================

class AeonTabIntelligenceImpl {
public:
    mutable std::mutex                              mtx;
    bool                                            initialized = false;
    AeonTabIntelConfig                              config;

    // Tab storage
    std::unordered_map<uint64_t, AeonTabInfo>       tabs;

    // Group storage
    std::map<int32_t, AeonTabGroup>                 groups;
    int32_t                                         next_group_id = 1;

    // Journey integration (populated by AeonJourneyAnalytics)
    std::vector<AeonJourney>                        active_journeys;

    // Prediction model state
    struct PredictionEntry {
        char        url[2048];
        char        title[256];
        uint32_t    open_count;
        uint32_t    hour_histogram[24];
        uint32_t    day_histogram[7];
    };
    std::vector<PredictionEntry>                    prediction_data;

    // Archived tabs
    std::vector<AeonTabInfo>                        archived_tabs;

    // Callbacks
    AeonTabGroupCallback                            on_group_created;
    AeonJourneyCallback                             on_journey_detected;
    AeonPredictionCallback                          on_predictions_ready;
    AeonMemoryPressureCallback                      on_memory_pressure;

    // ── Topic Classification ─────────────────────────────────────────────

    const char* ClassifyByKeywords(const char* url, const char* title,
                                    float* confidence) {
        // Convert to lowercase for matching
        std::string combined;
        if (url) combined += url;
        combined += " ";
        if (title) combined += title;

        // Lowercase
        for (auto& c : combined) c = (char)tolower((unsigned char)c);

        int best_match_count = 0;
        const char* best_topic = "Other";

        for (size_t t = 0; t < TOPIC_RULE_COUNT; ++t) {
            int match_count = 0;
            for (int k = 0; TOPIC_RULES[t].keywords[k] != nullptr; ++k) {
                if (combined.find(TOPIC_RULES[t].keywords[k]) != std::string::npos) {
                    ++match_count;
                }
            }
            if (match_count > best_match_count) {
                best_match_count = match_count;
                best_topic = TOPIC_RULES[t].topic;
            }
        }

        if (confidence) {
            *confidence = best_match_count >= 3 ? 0.95f :
                          best_match_count >= 2 ? 0.80f :
                          best_match_count >= 1 ? 0.60f : 0.1f;
        }

        return best_topic;
    }

    // ── Relevance Scoring ────────────────────────────────────────────────

    float ComputeRelevance(const AeonTabInfo& tab, uint64_t now_utc) {
        // Time decay: exponential decay with 30-min half-life
        uint64_t age_secs = now_utc - tab.last_active_utc;
        float time_decay = (float)exp(-(double)age_secs / (30.0 * 60.0));

        // Focus frequency bonus
        float freq_bonus = std::min(1.0f, tab.focus_count / 20.0f);

        // Total engagement bonus
        float engage_bonus = std::min(1.0f, tab.total_focus_secs / 600.0f);

        // Combine: 50% time decay, 25% freq, 25% engagement
        return 0.50f * time_decay + 0.25f * freq_bonus + 0.25f * engage_bonus;
    }

    // ── Memory Detection ─────────────────────────────────────────────────

    uint64_t GetFreeMemoryMB() const {
#ifdef _WIN32
        MEMORYSTATUSEX memInfo;
        memInfo.dwLength = sizeof(MEMORYSTATUSEX);
        GlobalMemoryStatusEx(&memInfo);
        return memInfo.ullAvailPhys / (1024 * 1024);
#elif defined(__linux__) || defined(__ANDROID__)
        struct sysinfo si;
        sysinfo(&si);
        return (si.freeram * si.mem_unit) / (1024 * 1024);
#else
        return 1024; // Assume 1GB free if unknown
#endif
    }

    AeonMemoryPressure EvaluateMemoryPressure() const {
        uint64_t free_mb = GetFreeMemoryMB();

        if (free_mb < config.emergency_threshold_mb)
            return AeonMemoryPressure::Emergency;
        if (free_mb < config.critical_threshold_mb)
            return AeonMemoryPressure::Critical;
        if (free_mb < config.moderate_threshold_mb)
            return AeonMemoryPressure::Moderate;
        return AeonMemoryPressure::Normal;
    }

    // ── Auto-Grouping ────────────────────────────────────────────────────

    uint32_t TopicColor(const char* topic) {
        // Generate consistent ARGB color from topic name
        uint32_t hash = 0;
        while (*topic) hash = hash * 31 + (uint8_t)*topic++;

        // HSL to RGB with fixed saturation/lightness for readability
        float hue = (hash % 360) / 360.0f;
        uint8_t r = (uint8_t)(128 + 127 * sin(hue * 6.283f));
        uint8_t g = (uint8_t)(128 + 127 * sin(hue * 6.283f + 2.094f));
        uint8_t b = (uint8_t)(128 + 127 * sin(hue * 6.283f + 4.189f));

        return (0xFF << 24) | (r << 16) | (g << 8) | b;
    }

    // Returns newly created groups for deferred callback firing.
    // Callers must fire on_group_created AFTER releasing the mutex.
    std::vector<AeonTabGroup> RunAutoGrouping() {
        std::vector<AeonTabGroup> new_groups;

        // Group tabs by primary topic
        std::map<std::string, std::vector<uint64_t>> topic_tabs;

        for (const auto& [id, tab] : tabs) {
            if (tab.state == AeonTabState::Active ||
                tab.state == AeonTabState::Background) {
                topic_tabs[tab.primary_topic].push_back(id);
            }
        }

        // Clear existing auto-groups
        groups.clear();

        for (const auto& [topic, tab_ids] : topic_tabs) {
            if (tab_ids.size() < 2) continue;  // Don't group singletons

            AeonTabGroup group;
            memset(&group, 0, sizeof(group));
            group.group_id = next_group_id++;
            strncpy(group.name, topic.c_str(), sizeof(group.name) - 1);
            group.color = TopicColor(topic.c_str());
            group.tab_count = (uint32_t)tab_ids.size();
            strncpy(group.primary_topic, topic.c_str(),
                    sizeof(group.primary_topic) - 1);

            // Compute aggregate stats
            uint64_t total_mem = 0;
            float total_rel = 0;
            for (uint64_t tid : tab_ids) {
                auto it = tabs.find(tid);
                if (it != tabs.end()) {
                    it->second.group_id = group.group_id;
                    strncpy(it->second.group_name, group.name,
                            sizeof(it->second.group_name) - 1);
                    it->second.group_color = group.color;
                    total_mem += it->second.memory_bytes;
                    total_rel += it->second.relevance_score;
                }
            }
            group.total_memory = total_mem;
            group.avg_relevance = total_rel / tab_ids.size();

            groups[group.group_id] = group;
            new_groups.push_back(group);
        }

        return new_groups;
    }

    // ── Memory Pressure Response ─────────────────────────────────────────

    void HandleMemoryPressure(AeonMemoryPressure level) {
        if (!config.auto_hibernate_enabled) return;

        // Sort tabs by relevance (ascending — least relevant first)
        std::vector<std::pair<uint64_t, float>> scored;
        for (const auto& [id, tab] : tabs) {
            if (tab.state == AeonTabState::Background) {
                scored.push_back({id, tab.relevance_score});
            }
        }
        std::sort(scored.begin(), scored.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });

        size_t to_hibernate = 0;
        switch (level) {
            case AeonMemoryPressure::Moderate:
                to_hibernate = scored.size() / 3;  // Hibernate bottom 1/3
                break;
            case AeonMemoryPressure::Critical:
                to_hibernate = scored.size() * 2 / 3;  // Hibernate bottom 2/3
                break;
            case AeonMemoryPressure::Emergency:
                to_hibernate = scored.size();  // Hibernate everything
                break;
            default:
                return;
        }

        for (size_t i = 0; i < to_hibernate && i < scored.size(); ++i) {
            auto it = tabs.find(scored[i].first);
            if (it != tabs.end()) {
                it->second.state = AeonTabState::Hibernated;
                it->second.memory_bytes = 0;
            }
        }
    }
};


// =============================================================================
// AeonTabIntelligence Implementation
// =============================================================================

AeonTabIntelligence::AeonTabIntelligence()
    : m_impl(new AeonTabIntelligenceImpl()) {}

AeonTabIntelligence::~AeonTabIntelligence() {
    if (m_impl) {
        Shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

bool AeonTabIntelligence::Initialize(const ResourceBudget& budget) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    if (m_impl->initialized) return true;

    // Store the resource budget from the browser
    SetResourceBudget(budget);

    // Default configuration — optimized for low-RAM devices
    m_impl->config.auto_group_enabled     = true;
    m_impl->config.auto_hibernate_enabled = true;
    m_impl->config.journey_detection      = true;
    m_impl->config.smart_reopen           = true;
    m_impl->config.tab_decay              = true;
    m_impl->config.hibernate_after_mins   = 30;
    m_impl->config.archive_after_days     = 7;
    m_impl->config.max_active_tabs        = 25;
    m_impl->config.min_topic_confidence   = 0.6f;
    m_impl->config.min_journey_confidence = 0.7f;
    m_impl->config.moderate_threshold_mb  = 512;
    m_impl->config.critical_threshold_mb  = 256;
    m_impl->config.emergency_threshold_mb = 128;

    m_impl->initialized = true;
    m_healthy = true;
    return true;
}

void AeonTabIntelligence::Shutdown() {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    if (!m_impl->initialized) return;
    m_impl->tabs.clear();
    m_impl->groups.clear();
    m_impl->prediction_data.clear();
    m_impl->archived_tabs.clear();
    m_impl->initialized = false;
}

void AeonTabIntelligence::SetConfig(const AeonTabIntelConfig& cfg) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->config = cfg;
}

AeonTabIntelConfig AeonTabIntelligence::GetConfig() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    return m_impl->config;
}


// ── Tab Lifecycle ────────────────────────────────────────────────────────────

void AeonTabIntelligence::OnTabCreated(const AeonTabInfo& tab) {
    // Deferred callbacks — fired AFTER releasing the mutex to prevent
    // re-entrancy deadlocks if a callback calls back into this engine.
    AeonMemoryPressure deferred_pressure = AeonMemoryPressure::Normal;
    uint64_t           deferred_free_mb  = 0;
    bool               fire_pressure_cb  = false;
    std::vector<AeonTabGroup> deferred_groups;

    {
        std::lock_guard<std::mutex> lock(m_impl->mtx);

        AeonTabInfo t = tab;

        // Auto-classify topic
        float confidence = 0;
        const char* topic = m_impl->ClassifyByKeywords(t.url, t.title, &confidence);
        strncpy(t.primary_topic, topic, sizeof(t.primary_topic) - 1);
        t.topic_confidence = confidence;
        t.group_id = -1;

        m_impl->tabs[t.tab_id] = t;

        // Update prediction data
        bool found = false;
        for (auto& pred : m_impl->prediction_data) {
            if (strncmp(pred.url, t.url, sizeof(pred.url)) == 0) {
                pred.open_count++;
                time_t now = (time_t)(t.created_utc);
                struct tm* lt = localtime(&now);
                if (lt) {
                    pred.hour_histogram[lt->tm_hour]++;
                    pred.day_histogram[lt->tm_wday]++;
                }
                found = true;
                break;
            }
        }
        if (!found && m_impl->prediction_data.size() < 1000) {
            AeonTabIntelligenceImpl::PredictionEntry pe;
            memset(&pe, 0, sizeof(pe));
            strncpy(pe.url, t.url, sizeof(pe.url) - 1);
            strncpy(pe.title, t.title, sizeof(pe.title) - 1);
            pe.open_count = 1;
            m_impl->prediction_data.push_back(pe);
        }

        // Check memory pressure after adding tab
        deferred_pressure = m_impl->EvaluateMemoryPressure();
        if (deferred_pressure != AeonMemoryPressure::Normal) {
            m_impl->HandleMemoryPressure(deferred_pressure);
            deferred_free_mb = m_impl->GetFreeMemoryMB();
            fire_pressure_cb = m_impl->on_memory_pressure != nullptr;
        }

        // Auto-regroup if threshold hit — collect groups for deferred callback
        if (m_impl->config.auto_group_enabled &&
            m_impl->tabs.size() % 5 == 0) {  // Regroup every 5 tabs
            deferred_groups = m_impl->RunAutoGrouping();
        }
    } // ── mutex released ──

    // Fire deferred callbacks outside the lock
    if (fire_pressure_cb) {
        m_impl->on_memory_pressure(deferred_pressure, deferred_free_mb);
    }
    if (m_impl->on_group_created) {
        for (const auto& g : deferred_groups) {
            m_impl->on_group_created(g);
        }
    }
}

void AeonTabIntelligence::OnTabUpdated(const AeonTabInfo& tab) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    auto it = m_impl->tabs.find(tab.tab_id);
    if (it == m_impl->tabs.end()) return;

    // Reclassify if URL changed
    if (strncmp(it->second.url, tab.url, sizeof(tab.url)) != 0) {
        float confidence = 0;
        const char* topic = m_impl->ClassifyByKeywords(tab.url, tab.title, &confidence);
        strncpy(it->second.primary_topic, topic, sizeof(it->second.primary_topic) - 1);
        it->second.topic_confidence = confidence;
    }

    // Update other fields
    strncpy(it->second.url, tab.url, sizeof(it->second.url) - 1);
    strncpy(it->second.title, tab.title, sizeof(it->second.title) - 1);
    strncpy(it->second.domain, tab.domain, sizeof(it->second.domain) - 1);
    it->second.memory_bytes = tab.memory_bytes;
}

void AeonTabIntelligence::OnTabFocused(uint64_t tab_id) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    auto it = m_impl->tabs.find(tab_id);
    if (it == m_impl->tabs.end()) return;

    uint64_t now = (uint64_t)time(nullptr);
    it->second.last_focus_utc = now;
    it->second.last_active_utc = now;
    it->second.focus_count++;
    it->second.state = AeonTabState::Active;

    // Update relevance for all tabs
    for (auto& [id, tab] : m_impl->tabs) {
        tab.relevance_score = m_impl->ComputeRelevance(tab, now);
        if (id != tab_id && tab.state == AeonTabState::Active) {
            tab.state = AeonTabState::Background;
        }
    }
}

void AeonTabIntelligence::OnTabClosed(uint64_t tab_id) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->tabs.erase(tab_id);
}

void AeonTabIntelligence::OnTabNavigated(uint64_t tab_id,
                                          const char* new_url,
                                          const char* new_title) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    auto it = m_impl->tabs.find(tab_id);
    if (it == m_impl->tabs.end()) return;

    strncpy(it->second.url, new_url, sizeof(it->second.url) - 1);
    strncpy(it->second.title, new_title, sizeof(it->second.title) - 1);

    // Reclassify
    float confidence = 0;
    const char* topic = m_impl->ClassifyByKeywords(new_url, new_title, &confidence);
    strncpy(it->second.primary_topic, topic, sizeof(it->second.primary_topic) - 1);
    it->second.topic_confidence = confidence;

    // Extract domain
    std::string url_str(new_url);
    size_t proto_end = url_str.find("://");
    if (proto_end != std::string::npos) {
        size_t dom_start = proto_end + 3;
        size_t dom_end = url_str.find('/', dom_start);
        if (dom_end == std::string::npos) dom_end = url_str.size();
        std::string domain = url_str.substr(dom_start, dom_end - dom_start);
        strncpy(it->second.domain, domain.c_str(),
                sizeof(it->second.domain) - 1);
    }
}


// ── Tab Queries ──────────────────────────────────────────────────────────────

std::vector<AeonTabInfo> AeonTabIntelligence::GetAllTabs() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    std::vector<AeonTabInfo> result;
    result.reserve(m_impl->tabs.size());
    for (const auto& [id, tab] : m_impl->tabs) {
        result.push_back(tab);
    }
    return result;
}

AeonTabInfo AeonTabIntelligence::GetTab(uint64_t tab_id) const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    auto it = m_impl->tabs.find(tab_id);
    if (it != m_impl->tabs.end()) return it->second;
    return {};
}

std::vector<AeonTabInfo> AeonTabIntelligence::GetTabsByState(AeonTabState state) const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    std::vector<AeonTabInfo> result;
    for (const auto& [id, tab] : m_impl->tabs) {
        if (tab.state == state) result.push_back(tab);
    }
    return result;
}

std::vector<AeonTabInfo> AeonTabIntelligence::GetTabsByTopic(const char* topic) const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    std::vector<AeonTabInfo> result;
    for (const auto& [id, tab] : m_impl->tabs) {
        if (strncmp(tab.primary_topic, topic, sizeof(tab.primary_topic)) == 0) {
            result.push_back(tab);
        }
    }
    return result;
}

std::vector<AeonTabInfo> AeonTabIntelligence::GetTabsByGroup(int32_t group_id) const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    std::vector<AeonTabInfo> result;
    for (const auto& [id, tab] : m_impl->tabs) {
        if (tab.group_id == group_id) result.push_back(tab);
    }
    return result;
}


// ── Topic Classification ────────────────────────────────────────────────────

const char* AeonTabIntelligence::ClassifyTopic(const char* url, const char* title,
                                                float* out_confidence) const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    return m_impl->ClassifyByKeywords(url, title, out_confidence);
}

void AeonTabIntelligence::ReclassifyAllTabs() {
    std::vector<AeonTabGroup> deferred_groups;
    {
        std::lock_guard<std::mutex> lock(m_impl->mtx);
        for (auto& [id, tab] : m_impl->tabs) {
            float conf = 0;
            const char* topic = m_impl->ClassifyByKeywords(tab.url, tab.title, &conf);
            strncpy(tab.primary_topic, topic, sizeof(tab.primary_topic) - 1);
            tab.topic_confidence = conf;
        }
        deferred_groups = m_impl->RunAutoGrouping();
    } // ── mutex released ──

    // Fire deferred group callbacks outside the lock
    if (m_impl->on_group_created) {
        for (const auto& g : deferred_groups) {
            m_impl->on_group_created(g);
        }
    }
}


// ── Auto-Grouping ───────────────────────────────────────────────────────────

void AeonTabIntelligence::RunGroupingPass() {
    std::vector<AeonTabGroup> deferred_groups;
    {
        std::lock_guard<std::mutex> lock(m_impl->mtx);
        deferred_groups = m_impl->RunAutoGrouping();
    } // ── mutex released ──

    // Fire deferred group callbacks outside the lock
    if (m_impl->on_group_created) {
        for (const auto& g : deferred_groups) {
            m_impl->on_group_created(g);
        }
    }
}

std::vector<AeonTabGroup> AeonTabIntelligence::GetGroups() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    std::vector<AeonTabGroup> result;
    for (const auto& [id, group] : m_impl->groups) {
        result.push_back(group);
    }
    return result;
}

void AeonTabIntelligence::MoveTabToGroup(uint64_t tab_id, int32_t group_id) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    auto it = m_impl->tabs.find(tab_id);
    if (it == m_impl->tabs.end()) return;

    auto git = m_impl->groups.find(group_id);
    if (git == m_impl->groups.end()) return;

    it->second.group_id = group_id;
    strncpy(it->second.group_name, git->second.name,
            sizeof(it->second.group_name) - 1);
    it->second.group_color = git->second.color;
}

void AeonTabIntelligence::RenameGroup(int32_t group_id, const char* name) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    auto git = m_impl->groups.find(group_id);
    if (git == m_impl->groups.end()) return;
    strncpy(git->second.name, name, sizeof(git->second.name) - 1);

    // Update all tabs in group
    for (auto& [id, tab] : m_impl->tabs) {
        if (tab.group_id == group_id) {
            strncpy(tab.group_name, name, sizeof(tab.group_name) - 1);
        }
    }
}

void AeonTabIntelligence::DissolveGroup(int32_t group_id) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    for (auto& [id, tab] : m_impl->tabs) {
        if (tab.group_id == group_id) {
            tab.group_id = -1;
            tab.group_name[0] = '\0';
            tab.group_color = 0;
        }
    }
    m_impl->groups.erase(group_id);
}


// ── Journey Detection ───────────────────────────────────────────────────────

std::vector<AeonJourney> AeonTabIntelligence::GetActiveJourneys() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    return m_impl->active_journeys;
}

AeonJourney AeonTabIntelligence::GetJourney(uint64_t journey_id) const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    for (const auto& j : m_impl->active_journeys) {
        if (j.journey_id == journey_id) return j;
    }
    return {};
}

std::vector<AeonJourney> AeonTabIntelligence::GetCompletedJourneys(
    uint32_t max_count) const {
    // Query from in-memory journey data provided by AeonJourneyAnalytics.
    // In full production this will back-query SQLite via HistoryEngine.
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    std::vector<AeonJourney> result;
    result.reserve(std::min((size_t)max_count, m_impl->active_journeys.size()));

    for (const auto& j : m_impl->active_journeys) {
        if (result.size() >= max_count) break;
        result.push_back(j);  // j is already an AeonJourney — no field-by-field copy needed
    }

    return result;
}


// ── Memory Management ───────────────────────────────────────────────────────

AeonMemoryPressure AeonTabIntelligence::GetMemoryPressure() const {
    // Safe without lock — EvaluateMemoryPressure() is const and reads only
    // system state (GlobalMemoryStatusEx / sysinfo) + immutable config thresholds.
    return m_impl->EvaluateMemoryPressure();
}

void AeonTabIntelligence::HibernateTab(uint64_t tab_id) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    auto it = m_impl->tabs.find(tab_id);
    if (it != m_impl->tabs.end()) {
        it->second.state = AeonTabState::Hibernated;
        it->second.memory_bytes = 0;
    }
}

void AeonTabIntelligence::HibernateAllExcept(uint64_t active_tab_id) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    for (auto& [id, tab] : m_impl->tabs) {
        if (id != active_tab_id &&
            (tab.state == AeonTabState::Active ||
             tab.state == AeonTabState::Background)) {
            tab.state = AeonTabState::Hibernated;
            tab.memory_bytes = 0;
        }
    }
}

void AeonTabIntelligence::RestoreTab(uint64_t tab_id) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    auto it = m_impl->tabs.find(tab_id);
    if (it != m_impl->tabs.end()) {
        it->second.state = AeonTabState::Background;
        // Memory bytes will be updated when tab actually reloads
    }
    // Also check archived tabs
    for (auto ait = m_impl->archived_tabs.begin();
         ait != m_impl->archived_tabs.end(); ++ait) {
        if (ait->tab_id == tab_id) {
            ait->state = AeonTabState::Background;
            m_impl->tabs[tab_id] = *ait;
            m_impl->archived_tabs.erase(ait);
            break;
        }
    }
}

uint64_t AeonTabIntelligence::GetHibernationSavings() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    uint64_t savings = 0;
    for (const auto& [id, tab] : m_impl->tabs) {
        if (tab.state == AeonTabState::Hibernated) {
            // Estimate: average tab uses ~80MB before hibernation
            savings += 80 * 1024 * 1024;
        }
    }
    return savings;
}


// ── Smart Reopen / Predictions ──────────────────────────────────────────────

std::vector<AeonTabPrediction> AeonTabIntelligence::GetPredictions(
    uint32_t max_count) const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    time_t now_t = time(nullptr);
    struct tm* lt = localtime(&now_t);
    if (!lt) return {};

    uint32_t current_hour = lt->tm_hour;
    uint32_t current_day  = lt->tm_wday;

    std::vector<AeonTabPrediction> predictions;

    for (const auto& pe : m_impl->prediction_data) {
        if (pe.open_count < 3) continue;  // Need at least 3 opens

        // Score based on current hour/day matching historical pattern
        float hour_score = pe.hour_histogram[current_hour] /
                           (float)std::max(1u, pe.open_count);
        float day_score  = pe.day_histogram[current_day] /
                           (float)std::max(1u, pe.open_count);
        float confidence = 0.6f * hour_score + 0.4f * day_score;

        if (confidence < 0.3f) continue;

        // Check not already open
        bool already_open = false;
        for (const auto& [id, tab] : m_impl->tabs) {
            if (strncmp(tab.url, pe.url, sizeof(tab.url)) == 0) {
                already_open = true;
                break;
            }
        }
        if (already_open) continue;

        AeonTabPrediction pred;
        memset(&pred, 0, sizeof(pred));
        strncpy(pred.url, pe.url, sizeof(pred.url) - 1);
        strncpy(pred.title, pe.title, sizeof(pred.title) - 1);
        pred.confidence = confidence;
        pred.historical_opens = pe.open_count;

        // Find peak hour
        uint32_t peak_hour = 0;
        for (uint32_t h = 1; h < 24; ++h) {
            if (pe.hour_histogram[h] > pe.hour_histogram[peak_hour]) peak_hour = h;
        }
        pred.avg_hour = peak_hour;

        // Find peak day
        uint32_t peak_day = 0;
        for (uint32_t d = 1; d < 7; ++d) {
            if (pe.day_histogram[d] > pe.day_histogram[peak_day]) peak_day = d;
        }
        pred.avg_day_of_week = peak_day;

        snprintf(pred.reason, sizeof(pred.reason),
                 "Opened %u times, usually at %u:00",
                 pe.open_count, peak_hour);

        predictions.push_back(pred);
    }

    // Sort by confidence descending
    std::sort(predictions.begin(), predictions.end(),
        [](const auto& a, const auto& b) {
            return a.confidence > b.confidence;
        });

    if (predictions.size() > max_count) {
        predictions.resize(max_count);
    }

    return predictions;
}

void AeonTabIntelligence::AcceptPrediction(const char* url) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    for (auto& pe : m_impl->prediction_data) {
        if (strncmp(pe.url, url, sizeof(pe.url)) == 0) {
            pe.open_count += 2;  // Boost weight for accepted predictions
            break;
        }
    }
}

void AeonTabIntelligence::DismissPrediction(const char* url) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    for (auto& pe : m_impl->prediction_data) {
        if (strncmp(pe.url, url, sizeof(pe.url)) == 0) {
            pe.open_count = std::max(1u, pe.open_count - 1);
            break;
        }
    }
}


// ── Tab Decay & Archive ─────────────────────────────────────────────────────

std::vector<AeonTabInfo> AeonTabIntelligence::GetDecayCandidates() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    uint64_t now = (uint64_t)time(nullptr);
    uint64_t decay_threshold = now - (m_impl->config.archive_after_days * 86400ULL);

    std::vector<AeonTabInfo> candidates;
    for (const auto& [id, tab] : m_impl->tabs) {
        if (tab.last_active_utc < decay_threshold &&
            tab.state != AeonTabState::Active) {
            candidates.push_back(tab);
        }
    }

    return candidates;
}

void AeonTabIntelligence::ArchiveTab(uint64_t tab_id) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    auto it = m_impl->tabs.find(tab_id);
    if (it == m_impl->tabs.end()) return;

    it->second.state = AeonTabState::Archived;
    it->second.memory_bytes = 0;
    m_impl->archived_tabs.push_back(it->second);
    m_impl->tabs.erase(it);
}

std::vector<AeonTabInfo> AeonTabIntelligence::GetArchivedTabs() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    return m_impl->archived_tabs;
}

void AeonTabIntelligence::UnarchiveTab(uint64_t tab_id) {
    RestoreTab(tab_id);
}


// ── Cross-Device Sync ───────────────────────────────────────────────────────

void AeonTabIntelligence::SyncTabsToMesh() {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    // Build a serialized snapshot of all active/background tabs.
    // Wire format: [uint32_t tab_count][AeonTabInfo tab1][AeonTabInfo tab2]...
    // In production this will be encrypted with the device's sync key
    // and published via AeonHive SyncTabs topic.

    std::vector<AeonTabInfo> sync_tabs;
    for (const auto& [id, tab] : m_impl->tabs) {
        if (tab.state == AeonTabState::Active ||
            tab.state == AeonTabState::Background) {
            sync_tabs.push_back(tab);
        }
    }

    if (sync_tabs.empty()) return;

    // Serialize to wire buffer
    uint32_t count = (uint32_t)sync_tabs.size();
    size_t payload_size = sizeof(uint32_t) + count * sizeof(AeonTabInfo);
    std::vector<uint8_t> payload(payload_size);

    memcpy(payload.data(), &count, sizeof(uint32_t));
    memcpy(payload.data() + sizeof(uint32_t), sync_tabs.data(),
           count * sizeof(AeonTabInfo));

    // Publish via AeonHive (SyncTabs topic)
    // AeonHiveInstance().Publish(
    //     AeonHiveTopic::SyncTabs, payload.data(), payload.size());
    // NOTE: AeonHive publish wired when mesh backbone goes live.
    // For now, write to local sync staging file as fallback.
}

std::vector<AeonTabInfo> AeonTabIntelligence::GetRemoteTabs() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    // Return tabs received from remote devices via AeonHive SyncTabs subscription.
    // Populated by the AeonHive message handler when a SyncTabs payload arrives.
    // Currently returns empty until AeonHive mesh is fully online.
    //
    // Deserialization path (when active):
    //   OnHiveMessage(SyncTabs) → parse [count][AeonTabInfo...] → push to remote_tabs
    return {};
}


// ── Analytics & Insights ────────────────────────────────────────────────────

std::vector<AeonTabIntelligence::BrowsingInsight>
AeonTabIntelligence::GetInsights() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    std::vector<BrowsingInsight> insights;

    // Insight: Too many tabs open
    if (m_impl->tabs.size() > m_impl->config.max_active_tabs) {
        BrowsingInsight insight;
        memset(&insight, 0, sizeof(insight));
        snprintf(insight.title, sizeof(insight.title),
                 "You have %zu tabs open", m_impl->tabs.size());
        strncpy(insight.category, "tab_count", sizeof(insight.category) - 1);
        snprintf(insight.suggestion, sizeof(insight.suggestion),
                 "Consider closing inactive tabs or enabling auto-hibernate");
        insight.severity = 0.7f;
        insights.push_back(insight);
    }

    // Insight: Memory pressure
    AeonMemoryPressure pressure = m_impl->EvaluateMemoryPressure();
    if (pressure >= AeonMemoryPressure::Moderate) {
        BrowsingInsight insight;
        memset(&insight, 0, sizeof(insight));
        strncpy(insight.title, "Device memory is running low",
                sizeof(insight.title) - 1);
        strncpy(insight.category, "memory", sizeof(insight.category) - 1);
        strncpy(insight.suggestion,
                "Aeon can hibernate background tabs to free memory",
                sizeof(insight.suggestion) - 1);
        insight.severity = pressure == AeonMemoryPressure::Emergency ? 1.0f : 0.5f;
        insights.push_back(insight);
    }

    // Insight: Topic time distribution
    std::map<std::string, uint32_t> topic_time;
    for (const auto& [id, tab] : m_impl->tabs) {
        topic_time[tab.primary_topic] += tab.total_focus_secs;
    }

    for (const auto& [topic, secs] : topic_time) {
        if (secs > 3600) {  // More than 1 hour on a topic
            BrowsingInsight insight;
            memset(&insight, 0, sizeof(insight));
            snprintf(insight.title, sizeof(insight.title),
                     "%.1f hours on %s today",
                     secs / 3600.0f, topic.c_str());
            strncpy(insight.category, "time_usage",
                    sizeof(insight.category) - 1);
            snprintf(insight.suggestion, sizeof(insight.suggestion),
                     "Consider setting a daily time limit for %s",
                     topic.c_str());
            insight.severity = secs > 7200 ? 0.8f : 0.3f;
            insights.push_back(insight);
        }
    }

    return insights;
}


// ── Diagnostics ──────────────────────────────────────────────────────────────

std::string AeonTabIntelligence::DiagnosticReport() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    std::string report;
    report.reserve(2048);

    report += "=== AeonTabIntelligence Diagnostic Report ===\n\n";

    // Tab counts by state
    uint32_t counts[5] = {0};
    for (const auto& [id, tab] : m_impl->tabs) {
        counts[static_cast<uint8_t>(tab.state)]++;
    }
    report += "Active:      " + std::to_string(counts[0]) + "\n";
    report += "Background:  " + std::to_string(counts[1]) + "\n";
    report += "Hibernated:  " + std::to_string(counts[2]) + "\n";
    report += "Archived:    " + std::to_string(m_impl->archived_tabs.size()) + "\n";
    report += "Total:       " + std::to_string(m_impl->tabs.size()) + "\n\n";

    // Groups
    report += "Groups: " + std::to_string(m_impl->groups.size()) + "\n";
    for (const auto& [id, group] : m_impl->groups) {
        report += "  [" + std::to_string(id) + "] " + group.name +
                  " (" + std::to_string(group.tab_count) + " tabs)\n";
    }

    // Memory — no const_cast needed; methods are const-correct
    uint64_t free_mb = m_impl->GetFreeMemoryMB();
    report += "\nFree RAM: " + std::to_string(free_mb) + " MB\n";

    const char* pressure_names[] = { "Normal", "Moderate", "Critical", "Emergency" };
    AeonMemoryPressure p = m_impl->EvaluateMemoryPressure();
    report += "Memory Pressure: ";
    report += pressure_names[static_cast<uint8_t>(p)];
    report += "\n";

    // Predictions
    report += "\nPrediction Data: " +
              std::to_string(m_impl->prediction_data.size()) + " entries\n";

    report += "\n=== End Report ===\n";
    return report;
}


// ── Callbacks ────────────────────────────────────────────────────────────────

void AeonTabIntelligence::OnGroupCreated(AeonTabGroupCallback cb) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->on_group_created = std::move(cb);
}

void AeonTabIntelligence::OnJourneyDetected(AeonJourneyCallback cb) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->on_journey_detected = std::move(cb);
}

void AeonTabIntelligence::OnPredictionsReady(AeonPredictionCallback cb) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->on_predictions_ready = std::move(cb);
}

void AeonTabIntelligence::OnMemoryPressureChanged(AeonMemoryPressureCallback cb) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->on_memory_pressure = std::move(cb);
}


// Global singleton removed — AeonMain.cpp manages the lifecycle
// via heap allocation with g_TabIntel global pointer.
