// =============================================================================
// aeon_spell.h — AeonSpell Public API
// Graduated from: hunspell/hunspell (LGPL 2.1) — linked as .dll, never embedded
//
// What we improve over Hunspell:
//   1. Async suggestion engine — never blocks the UI thread
//   2. Context-ranked suggestions — frequency + edit distance combined
//   3. Lazy dictionary loading — only loads ranges needed, XP-safe
//   4. Personal word list — learns from user corrections
//   5. Hive word-frequency — peer network improves ranking communally
//   6. Sovereign dictionary updates — .bdic pushed via Ed25519-signed manifest
//   7. Compact Unicode table — compressed trie vs Hunspell's 2.6MB static array
// =============================================================================

#pragma once

#include "../core/aeon_component.h"
#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
class AeonSpellImpl;

// ---------------------------------------------------------------------------
// AeonSpellSuggestion — ranked spelling suggestion
// ---------------------------------------------------------------------------
struct AeonSpellSuggestion {
    std::string word;
    int         edit_distance;      // Levenshtein distance from input
    uint32_t    frequency_rank;     // 0 = most common; higher = rarer
    float       hive_boost;         // 0.0–1.0 boost from Hive word popularity
    float       personal_boost;     // 0.0–1.0 boost if user has accepted before
    float       final_score;        // combined ranking score (lower = better)
};

// ---------------------------------------------------------------------------
// AeonSpellResult — result of a check() call
// ---------------------------------------------------------------------------
struct AeonSpellResult {
    bool is_correct;
    // Only populated if is_correct == false and suggestions were requested
    std::vector<AeonSpellSuggestion> suggestions;
};

// ---------------------------------------------------------------------------
// Suggestion callback — async delivery
// callback(word, results) called from background thread
// UI must marshal to main thread before displaying
// ---------------------------------------------------------------------------
using AeonSpellCallback = std::function<void(
    const std::string&               original_word,
    std::vector<AeonSpellSuggestion> suggestions
)>;

// ---------------------------------------------------------------------------
// AeonSpellLanguage — loaded language context
// ---------------------------------------------------------------------------
struct AeonSpellLanguage {
    char   lang_code[16];      // e.g. "en_US", "es", "zh_Hans"
    char   dict_version[32];   // from sovereign manifest
    size_t loaded_ram_bytes;
    bool   lazy_loaded;        // true = only partial trie in memory
};

// ---------------------------------------------------------------------------
// AeonSpell — main class
// ---------------------------------------------------------------------------
class AeonSpell final : public AeonComponentBase {
public:
    AeonSpell();
    ~AeonSpell() override;

    // ── IAeonComponent identity ───────────────────────────────────────────
    const char* ComponentId()      const override { return "aeon.spell"; }
    const char* ComponentVersion() const override { return "1.0.0"; }
    const char* UpstreamRef()      const override { return "hunspell/hunspell@2.1.0-lgpl"; }

    // ── Lifecycle ─────────────────────────────────────────────────────────
    bool Initialize(const ResourceBudget& budget) override;
    void Shutdown() override;

    // ── Language Management ───────────────────────────────────────────────

    // Load a .bdic dictionary for the given language.
    // If budget.max_ram_bytes is tight, loads only the most-used character ranges.
    // dict_path: absolute path to .bdic file (sovereign-pushed or bundled)
    bool LoadLanguage(const char* lang_code, const char* dict_path);

    // Unload a language (frees RAM). Called when a tab with that language is closed.
    void UnloadLanguage(const char* lang_code);

    // Returns the currently loaded languages
    std::vector<AeonSpellLanguage> LoadedLanguages() const;

    // ── Spell Check API ───────────────────────────────────────────────────

    // Synchronous: check a single word. No suggestions generated.
    // Fast — runs on current thread. Uses bloom filter first, then full check.
    bool IsCorrect(const char* word, const char* lang_code = "en_US") const;

    // Asynchronous: check a word AND generate ranked suggestions.
    // Returns immediately. Callback is called from a background thread.
    // The callback is guaranteed to fire even if no suggestions are found.
    void CheckAsync(
        const char*          word,
        const char*          lang_code,
        AeonSpellCallback    callback,
        int                  max_suggestions = 8
    );

    // Synchronous version for testing / simple integrations
    AeonSpellResult CheckSync(
        const char* word,
        const char* lang_code = "en_US",
        int         max_suggestions = 8
    );

    // ── Personal Word List ────────────────────────────────────────────────

    // Add a word to the user's personal dictionary (persisted locally)
    void AddPersonalWord(const char* word);

    // Remove a word from the personal dictionary
    void RemovePersonalWord(const char* word);

    // Mark a correction: user rejected suggestion A in favor of B
    // This boosts B's personal_boost score for future suggestions
    void RecordAcceptedCorrection(const char* original, const char* accepted);

    // ── Hive Integration ─────────────────────────────────────────────────

    // Called by AeonHive when updated word-frequency data arrives.
    // freq_data: compact binary frequency map (anonymized, no user data)
    void UpdateHiveFrequencyData(const uint8_t* freq_data, size_t len);

    // ── Resource Awareness ────────────────────────────────────────────────
    bool CanOffloadToHive() const override;

    // On XP with <256MB RAM: offload suggestion generation to a Hive peer.
    // Returns false if no peer available (falls back to local fast-path only)
    bool RequestHiveSuggestions(
        const char*       word,
        const char*       lang_code,
        AeonSpellCallback callback
    );

private:
    AeonSpellImpl* m_impl = nullptr;
};

// ---------------------------------------------------------------------------
// Global singleton access (browser uses one AeonSpell instance)
// ---------------------------------------------------------------------------
AeonSpell& AeonSpellInstance();
