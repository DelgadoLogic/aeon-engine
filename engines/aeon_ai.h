// =============================================================================
// aeon_ai.h — AeonAI Public API
// Graduated from: llama.cpp (MIT) + whisper.cpp (MIT) + ggml (MIT)
//
// AeonAI is the in-browser AI inference engine.
// Runs entirely locally — no cloud, no API keys, no phone-home.
//
// What we improve over upstream llama.cpp:
//   [+] Browser-first API: async streaming tokens via callback (not blocking)
//   [+] Context window manager: auto-summarizes when context fills
//   [+] Tab context injection: can read current page content into prompt
//   [+] Hive routing: heavy inference optionally offloaded to capable peer
//   [+] Model budget manager: never exceeds RAM ceiling, evicts LRU model
//   [+] Sovereign model updates: .gguf weights delivered via signed manifest
//   [+] XP-safe SSE2 path: Q4_0 quantized models run on Pentium 4-era CPUs
//   [+] Whisper integration: voice input → text → AeonAI prompt (hands-free)
//   [+] GPU acceleration: CUDA/Metal/Vulkan path on capable hardware (auto-detected)
//
// Commercially licensed models only:
//   Tier 0 (XP/256MB):  TinyLlama 1.1B Q4_0 — Apache 2.0
//   Tier 1 (512MB+):    Phi-3 Mini Q4_K_M   — MIT
//   Tier 2 (1GB+):      SmolLM2-1.7B        — Apache 2.0
//   Tier 3 (2GB+):      MiniCPM-2B          — Apache 2.0
//   Voice (all tiers):  Whisper Tiny/Base    — MIT
//
//   DO NOT USE: Any GPL/LGPL model weights.
// =============================================================================

#pragma once

#include "../core/aeon_component.h"
#include <functional>
#include <string>
#include <vector>

#ifdef _WIN32
    #ifdef AEON_AI_EXPORTS
        #define AEON_AI_API __declspec(dllexport)
    #else
        #define AEON_AI_API __declspec(dllimport)
    #endif
#else
    #define AEON_AI_API
#endif

// ---------------------------------------------------------------------------
// AeonAIModelTier
// ---------------------------------------------------------------------------
enum class AeonAIModelTier : uint8_t {
    Micro      = 0,   // TinyLlama Q4_0 (~600MB disk, ~256MB RAM)
    Mini       = 1,   // Phi-3 Mini Q4_K (~2.4GB disk, ~512MB RAM)
    Small      = 2,   // SmolLM2-1.7B Q4 (~900MB disk, ~1GB RAM)
    Mid        = 3,   // MiniCPM-2B Q4   (~1.5GB disk, ~2GB RAM)
    HivePeer   = 4,   // Offload to capable Hive node
    Gemma4_2B  = 5,   // gemma4:e2b (Apache 2.0 2B parameters)
    Gemma4_12B = 6,   // gemma4:12b (Apache 2.0 12B parameters)
};

// ---------------------------------------------------------------------------
// AeonAIModel — loaded model descriptor
// ---------------------------------------------------------------------------
struct AeonAIModel {
    char   model_id[64];       // e.g. "tinyllama-1.1b-q4"
    char   model_version[32];
    AeonAIModelTier tier;
    size_t ram_bytes_loaded;
    int    context_window;     // max tokens (512 XP, 4096 modern)
    bool   gpu_accelerated;
    bool   is_active;          // currently serving requests
};

// ---------------------------------------------------------------------------
// AeonAIMessage — one message in a conversation
// ---------------------------------------------------------------------------
struct AeonAIMessage {
    enum class Role { System, User, Assistant } role;
    std::string content;
};

// ---------------------------------------------------------------------------
// AeonAIStreamCallback — fires for each decoded token
// fragment: next token(s) to display (may be multiple chars for byte-pair tokens)
// is_final: true = generation complete
// ---------------------------------------------------------------------------
using AeonAIStreamCallback = std::function<void(
    const std::string& fragment,
    bool               is_final
)>;

// ---------------------------------------------------------------------------
// AeonAIVoiceCallback — fires when voice input is transcribed
// ---------------------------------------------------------------------------
using AeonAIVoiceCallback = std::function<void(
    const std::string& transcription,
    float              confidence
)>;

// ---------------------------------------------------------------------------
// AeonAI — main AI engine
// ---------------------------------------------------------------------------
class AeonAIImpl;

class AEON_AI_API AeonAI final : public AeonComponentBase {
public:
    AeonAI();
    ~AeonAI() override;

    // ── IAeonComponent identity ───────────────────────────────────────────
    const char* ComponentId()      const override { return "aeon.ai"; }
    const char* ComponentVersion() const override { return "1.0.0"; }
    const char* UpstreamRef()      const override {
        return "ggerganov/llama.cpp@MIT + whisper.cpp@MIT + ggerganov/ggml@MIT";
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────
    bool Initialize(const ResourceBudget& budget) override;
    void Shutdown() override;

    // ── Model Management ─────────────────────────────────────────────────

    // Auto-select and load the best model for current RAM budget
    bool AutoLoadModel();

    // Load a specific model tier
    bool LoadModel(AeonAIModelTier tier);

    // Unload current model (frees RAM)
    void UnloadModel();

    // Returns currently loaded model info (by value)
    AeonAIModel GetLoadedModel() const;

    // Returns number of loaded model tensors (0 if unloaded / lightweight mode)
    uint64_t GetLoadedTensorCount() const;

    // ── Inference API ─────────────────────────────────────────────────────

    // Single-turn prompt (async streaming)
    void PromptAsync(
        const std::string&    prompt,
        AeonAIStreamCallback  callback,
        int                   max_tokens = 512
    );

    // Multi-turn conversation
    void ChatAsync(
        const std::vector<AeonAIMessage>& messages,
        AeonAIStreamCallback             callback,
        int                              max_tokens = 512
    );

    // Page-aware prompt: automatically injects current page content
    // page_text: extracted text of current tab's page
    void PageAwarePrompt(
        const std::string&   user_question,
        const std::string&   page_text,
        AeonAIStreamCallback callback,
        int                  max_tokens = 512
    );

    // Topic classification via Gemma 4 / Local AI GGUF engine
    std::string ClassifyTopicLLM(const std::string& url, const std::string& title);

    // Intent detection via Gemma 4 / Local AI GGUF engine
    std::string DetectIntentLLM(const std::string& url, const std::string& title, const std::string& referrer);

    // Predictive prefetching candidates
    std::vector<std::string> PredictPrefetchURLs(const std::string& current_url, const std::string& journey_context);

    // Synchronous webpage summarization returning structured JSON
    std::string SummarizeText(const std::string& page_text, int max_bullets = 5);

    // Intent prompt parser returning target URL and classification
    std::string ParseIntent(const std::string& intent_prompt);

    // Gemma 4 local model text/vision processing
    std::string ProcessGemma(const std::string& input_prompt, const std::string& image_base64 = "");

    // MultiOn-grade autonomous workflow motor cortex execution
    std::string RunMultiOnWorkflow(const std::string& goal, const std::string& auth_json_path = "");

    // Cancel current generation
    void CancelGeneration();

    // ── Context Management ────────────────────────────────────────────────

    // When conversation history grows near context window limit,
    // AeonAI automatically summarizes older messages to stay within budget.
    // Call this to get the current effective context used.
    int ContextTokensUsed() const;
    int ContextWindowSize() const;

    // Manually clear conversation history
    void ClearContext();

    // ── Voice Input (Whisper integration) ─────────────────────────────────

    // Start recording microphone input.
    // When user stops speaking (VAD silence > 1.5s), fires callback with transcription.
    // Transcription is automatically fed into the next PromptAsync call if
    // auto_prompt is true.
    bool StartVoiceInput(AeonAIVoiceCallback callback, bool auto_prompt = true);
    void StopVoiceInput();

    // Transcribe an audio buffer (PCM 16kHz mono)
    void TranscribeAsync(
        const float*         pcm_data,
        size_t               sample_count,
        AeonAIVoiceCallback  callback
    );

    // ── Hive Offload ─────────────────────────────────────────────────────
    bool CanOffloadToHive() const override;

    // Send inference request to a capable Hive peer
    bool RequestHiveInference(
        const std::vector<AeonAIMessage>& messages,
        AeonAIStreamCallback             callback
    );

    // ── Aeon Browser Integration Hooks ────────────────────────────────────

    // Called by the browser when user opens AI sidebar on a page
    void OnSidebarOpened(const std::string& page_url, const std::string& page_text);

    // Called when user selects text and right-clicks "Ask AI"
    void OnSelectionQuery(const std::string& selected_text,
                          const std::string& context_text,
                          AeonAIStreamCallback callback);

    // Called when autofill wants AI assistance
    void OnAutofillHint(const std::string& field_label,
                        const std::string& page_context,
                        std::function<void(const std::string&)> callback);

private:
    AeonAIImpl* m_impl = nullptr;
};

// ── Global singleton ──────────────────────────────────────────────────────────
AEON_AI_API AeonAI& AeonAIInstance();
