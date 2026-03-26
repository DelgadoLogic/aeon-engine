// =============================================================================
// aeon_translate.h — AeonTranslate Public API
// Graduated from: OpenNMT/CTranslate2 (MIT License)
//
// What we improve over CTranslate2:
//   [+] Pre-converted .ctm2 model binaries — no Python pipeline at runtime
//   [+] RAM-aware loader — never loads more than budget allows; swaps model pages
//   [+] Streaming decode — first words appear in <200ms, rest streams in
//   [+] Hive peer offload — XP/low-RAM nodes delegate to capable peer
//   [+] Language auto-detection — detects input language, selects correct model
//   [+] Sovereign model updates — .ctm2 weights pushed via Ed25519 manifest
//   [+] XP-safe: SSE2-only code path, no AVX required for base inference
//   [+] OPUS-MT (CC-BY 4.0) for Legacy Edition, MADLAD-400 (Apache 2.0) Standard
// =============================================================================

#pragma once

#include "../core/aeon_component.h"
#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// AeonTranslateResult — result of one translation
// ---------------------------------------------------------------------------
struct AeonTranslateResult {
    std::string translated_text;
    std::string detected_lang;   // source language detected
    std::string target_lang;
    float       confidence;      // 0.0–1.0
    bool        used_hive_peer;  // true = offloaded to network peer
    bool        is_complete;     // false = streaming (more tokens coming)
    uint32_t    latency_ms;
};

// ---------------------------------------------------------------------------
// Language pair availability
// ---------------------------------------------------------------------------
enum class AeonModelTier {
    LegacyEdition  = 0,  // OPUS-MT pairs (CC-BY 4.0), 50–300MB, XP-safe
    StandardEdition = 1, // MADLAD-400 (Apache 2.0), 280MB quantized, 400 langs
    HivePeer        = 2, // No local model — fully offloaded to capable peer
};

struct AeonLangPair {
    char src_lang[16];   // ISO 639 e.g. "en", "es", "zh"
    char tgt_lang[16];
    AeonModelTier tier;
    bool available;      // model file exists locally
    size_t model_ram_kb; // RAM cost when loaded
};

// ---------------------------------------------------------------------------
// Streaming callback — fires for each decoded token or sentence fragment.
// is_final: true = translation is complete
// ---------------------------------------------------------------------------
using AeonTranslateStreamCallback = std::function<void(
    const std::string& fragment,
    bool               is_final,
    float              confidence
)>;

// ---------------------------------------------------------------------------
// One-shot callback — fires with complete result
// ---------------------------------------------------------------------------
using AeonTranslateCallback = std::function<void(
    const AeonTranslateResult& result
)>;

// ---------------------------------------------------------------------------
// AeonTranslate — main translation engine
// ---------------------------------------------------------------------------
class AeonTranslateImpl;

class AeonTranslate final : public AeonComponentBase {
public:
    AeonTranslate();
    ~AeonTranslate() override;

    // ── IAeonComponent identity ───────────────────────────────────────────
    const char* ComponentId()      const override { return "aeon.translate"; }
    const char* ComponentVersion() const override { return "1.0.0"; }
    const char* UpstreamRef()      const override {
        return "OpenNMT/CTranslate2@MIT + OPUS-MT/CC-BY + MADLAD-400/Apache2";
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────
    bool Initialize(const ResourceBudget& budget) override;
    void Shutdown() override;

    // ── Model Management ─────────────────────────────────────────────────

    // Returns all pairs supported given current resources
    std::vector<AeonLangPair> AvailablePairs() const;

    // Preload a specific model into RAM (call on browser start for popular pairs)
    bool PreloadModel(const char* src_lang, const char* tgt_lang);

    // Unload a model from RAM (called when memory pressure detected)
    void UnloadModel(const char* src_lang, const char* tgt_lang);

    // Returns the tier that would be used for a given pair
    AeonModelTier GetTierFor(const char* src_lang, const char* tgt_lang) const;

    // ── Language Detection ────────────────────────────────────────────────

    // Detect language of text. Fast: uses compact n-gram model (<1MB).
    // Returns ISO 639-1 code e.g. "en", "fr", "zh"
    std::string DetectLanguage(const char* text, size_t len = 0) const;

    // ── Translation API ───────────────────────────────────────────────────

    // Asynchronous streaming translation.
    // stream_cb fires for each decoded fragment (fast first-token delivery).
    // done_cb fires once with the complete result.
    void TranslateAsync(
        const char*                   text,
        const char*                   src_lang,       // nullptr = auto-detect
        const char*                   tgt_lang,
        AeonTranslateStreamCallback   stream_cb,
        AeonTranslateCallback         done_cb = nullptr,
        int                           max_tokens = 512
    );

    // Synchronous translation (blocks; use only for short strings, not UI)
    AeonTranslateResult TranslateSync(
        const char* text,
        const char* src_lang,
        const char* tgt_lang,
        int         max_tokens = 512
    );

    // Translate a full web page.
    // Extracts text nodes, batches requests, reassembles the DOM.
    // dom_cb fires with modified HTML incrementally.
    void TranslatePage(
        const char*           html,
        const char*           tgt_lang,
        AeonTranslateCallback dom_cb
    );

    // ── Hive Offload ─────────────────────────────────────────────────────

    // Returns true if this node should offload translations to a Hive peer.
    // Criteria: RAM budget < 256MB OR requested model not locally available.
    bool CanOffloadToHive() const override;

    // Request translation from a Hive peer.
    bool RequestHiveTranslation(
        const char*           text,
        const char*           src_lang,
        const char*           tgt_lang,
        AeonTranslateCallback callback
    );

    // ── Sovereign Model Updates ───────────────────────────────────────────
    // ApplyUpdate() (inherited) handles .ctm2 model file delivery.
    // After update: UnloadModel + reload on next translation request.

private:
    AeonTranslateImpl* m_impl = nullptr;
};

// ── Global singleton ──────────────────────────────────────────────────────────
AeonTranslate& AeonTranslateInstance();
