// =============================================================================
// aeon_translate.cpp — AeonTranslate Implementation
// Project: Aeon Browser (DelgadoLogic)
//
// Upstream reference: research/native-components/ctranslate2_src (MIT License)
// Models:
//   Legacy Edition  → OPUS-MT pairs     (CC-BY 4.0) — XP/SSE2-safe
//   Standard Edition→ MADLAD-400 quant  (Apache 2.0) — requires ~280MB RAM
//   DO NOT USE: NLLB-200 (Meta CC-BY-NC-4.0) — non-commercial, blocked
//
// Architecture:
//   • ModelRegistry tracks loaded .ctm2 binaries and their RAM cost
//   • RAM-aware loader evicts LRU model if new one would exceed budget
//   • Inference thread pool (1 thread XP / ncores-1 modern)
//   • Streaming decode: decoder posts partial output via stream_cb
//   • Lang-detect: 1MB compressed n-gram model always in RAM
//   • Hive offload: QUIC request to capable peer, response streamed back
// =============================================================================

#include "aeon_translate.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

// ---------------------------------------------------------------------------
// .ctm2 Model format (Aeon proprietary, CTranslate2-inspired)
// Magic: "CTM2" | version u32 | src_lang[16] | tgt_lang[16] | tier u8
// Followed by quantized weight tensors in our compact format
// ---------------------------------------------------------------------------
struct Ctm2Header {
    char     magic[4];     // "CTM2"
    uint32_t version;      // 0x00010000 = v1.0
    char     src_lang[16];
    char     tgt_lang[16];
    uint8_t  tier;         // 0=Legacy/OPUS-MT, 1=Standard/MADLAD-400
    uint8_t  quant;        // 0=f32, 1=f16, 2=int8, 3=int4
    uint32_t vocab_size;
    uint32_t enc_layers;
    uint32_t dec_layers;
    uint32_t model_dim;
    uint64_t payload_bytes;
    uint8_t  reserved[24];
};

static_assert(sizeof(Ctm2Header) == 100, "Ctm2Header size mismatch");

// ---------------------------------------------------------------------------
// LoadedModel — one model in RAM
// ---------------------------------------------------------------------------
struct LoadedModel {
    Ctm2Header          header{};
    std::vector<uint8_t> weights;  // quantized weight blob
    size_t               ram_bytes = 0;
    uint64_t             last_used = 0; // unix timestamp for LRU eviction

    std::string PairKey() const {
        return std::string(header.src_lang) + "_" + header.tgt_lang;
    }
};

// ---------------------------------------------------------------------------
// Simple language detector — n-gram frequency fingerprinting
// (1MB compressed model stored in aeon_langdetect_data.h)
// ---------------------------------------------------------------------------
struct LangDetector {
    // Real implementation: load compressed trigram frequency table per language.
    // Fast: 20–50ms for a typical sentence.
    // Placeholder: returns "en" for everything (stub)
    std::string Detect(const char* text, size_t len) const {
        (void)text; (void)len;
        // TODO: implement trigram scoring against n-gram model
        // See research/native-components/ctranslate2_src for reference tokenizer
        return "en";
    }
};

// ---------------------------------------------------------------------------
// Translation task for async queue
// ---------------------------------------------------------------------------
struct TranslateTask {
    std::string                 text;
    std::string                 src_lang;
    std::string                 tgt_lang;
    int                         max_tokens;
    AeonTranslateStreamCallback stream_cb;
    AeonTranslateCallback       done_cb;
};

// ---------------------------------------------------------------------------
// AeonTranslateImpl
// ---------------------------------------------------------------------------
struct AeonTranslateImpl {
    ResourceBudget      budget{};
    LangDetector        lang_detector;

    // Model registry
    std::unordered_map<std::string, std::unique_ptr<LoadedModel>> models;
    std::mutex models_mutex;
    size_t     loaded_ram_total = 0;

    // Models directory (sovereign updates deliver .ctm2 files here)
    char models_dir[512]{};

    // Async task queue (one worker thread on XP, ncores-1 on modern)
    std::vector<std::thread> workers;
    std::queue<TranslateTask> task_queue;
    std::mutex queue_mutex;
    std::atomic<bool> running{false};
#ifdef _WIN32
    HANDLE queue_event = nullptr;
#endif

    // Hive offload
    bool hive_offload_enabled = false;

    void WorkerLoop();
    AeonTranslateResult RunInference(
        LoadedModel& model,
        const std::string& text,
        int max_tokens,
        AeonTranslateStreamCallback* stream_cb
    );
    LoadedModel* GetOrLoadModel(const char* src_lang, const char* tgt_lang);
    bool LoadModelFile(const char* src_lang, const char* tgt_lang);
    void EvictLRU(size_t needed_bytes);
    std::string FindModelPath(const char* src, const char* tgt) const;
};

// ---------------------------------------------------------------------------
// WorkerLoop
// ---------------------------------------------------------------------------
void AeonTranslateImpl::WorkerLoop() {
    while (running.load()) {
#ifdef _WIN32
        if (queue_event) WaitForSingleObject(queue_event, 100);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
#endif
        for (;;) {
            TranslateTask task;
            {
                std::lock_guard<std::mutex> lk(queue_mutex);
                if (task_queue.empty()) break;
                task = std::move(task_queue.front());
                task_queue.pop();
            }

            // Auto-detect src_lang if not specified
            std::string src = task.src_lang;
            if (src.empty() || src == "auto")
                src = lang_detector.Detect(task.text.c_str(), task.text.size());

            AeonTranslateResult result;
            result.detected_lang   = src;
            result.target_lang     = task.tgt_lang;
            result.is_complete     = true;
            result.used_hive_peer  = false;

            LoadedModel* model = GetOrLoadModel(src.c_str(), task.tgt_lang.c_str());
            if (model) {
                result = RunInference(*model, task.text, task.max_tokens,
                                      task.stream_cb ? &task.stream_cb : nullptr);
            } else if (hive_offload_enabled) {
                // No local model → offload
                result.translated_text = task.text; // stub: echo
                result.used_hive_peer  = true;
                result.confidence      = 0.0f;
            } else {
                // Nothing available
                result.translated_text = task.text;
                result.confidence      = 0.0f;
            }

            if (task.done_cb) task.done_cb(result);
        }
    }
}

// ---------------------------------------------------------------------------
// Inference stub — real CTranslate2 call goes here
// ---------------------------------------------------------------------------
AeonTranslateResult AeonTranslateImpl::RunInference(
    LoadedModel&                  model,
    const std::string&            text,
    int                           max_tokens,
    AeonTranslateStreamCallback*  stream_cb)
{
    AeonTranslateResult result;
    result.detected_lang  = model.header.src_lang;
    result.target_lang    = model.header.tgt_lang;
    result.used_hive_peer = false;

    auto t0 = (uint32_t)(time(nullptr) * 1000);

    // ── Real CTranslate2 inference would happen here ──
    // Steps:
    //   1. Tokenize text using SentencePiece model bundled in .ctm2
    //   2. Feed token IDs to encoder layers (weight vectors in model.weights)
    //   3. Run beam search decoder (beam_size=4 on XP, 8 on modern)
    //   4. For each decoded token: call stream_cb if provided
    //   5. Decode token IDs back to text via SentencePiece
    //
    // For XP/SSE2: use int8 quantized path (fast, 2–3x vs f32)
    // For AVX2+:   use int4 path (fastest, ~4x vs f32)
    //
    // Streaming: every 3–5 tokens, fire stream_cb with partial result

    // Stub: simulate streaming output
    const std::string stub_result = "[" + std::string(model.header.tgt_lang) +
                                    "] " + text + " [translated]";

    // Simulate streaming by chunking every 8 chars
    if (stream_cb && *stream_cb) {
        size_t chunk = 8;
        for (size_t i = 0; i < stub_result.size(); i += chunk) {
            std::string frag = stub_result.substr(i, chunk);
            bool final = (i + chunk >= stub_result.size());
            (*stream_cb)(frag, final, 0.85f);
        }
    }

    result.translated_text = stub_result;
    result.confidence      = 0.85f;
    result.is_complete     = true;
    result.latency_ms      = (uint32_t)(time(nullptr) * 1000) - t0;

    model.last_used = (uint64_t)time(nullptr);
    return result;
}

// ---------------------------------------------------------------------------
// Model loading
// ---------------------------------------------------------------------------
LoadedModel* AeonTranslateImpl::GetOrLoadModel(
    const char* src_lang, const char* tgt_lang)
{
    std::string key = std::string(src_lang) + "_" + std::string(tgt_lang);
    std::lock_guard<std::mutex> lk(models_mutex);

    auto it = models.find(key);
    if (it != models.end()) {
        it->second->last_used = (uint64_t)time(nullptr);
        return it->second.get();
    }

    // Try loading from disk
    lk.~lock_guard(); // manual unlock before recursive call
    // (In real impl: use unique_lock + condition variable)
    if (LoadModelFile(src_lang, tgt_lang)) {
        std::lock_guard<std::mutex> lk2(models_mutex);
        auto it2 = models.find(key);
        if (it2 != models.end()) return it2->second.get();
    }
    return nullptr;
}

bool AeonTranslateImpl::LoadModelFile(const char* src_lang, const char* tgt_lang) {
    std::string path = FindModelPath(src_lang, tgt_lang);
    if (path.empty()) return false;

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    auto model = std::make_unique<LoadedModel>();
    if (fread(&model->header, sizeof(Ctm2Header), 1, f) != 1) {
        fclose(f); return false;
    }

    if (memcmp(model->header.magic, "CTM2", 4) != 0) {
        fclose(f); return false;
    }

    size_t payload = (size_t)model->header.payload_bytes;
    size_t needed  = payload;

    // RAM budget check: evict LRU if we'd exceed the ceiling
    {
        std::lock_guard<std::mutex> lk(models_mutex);
        if (loaded_ram_total + needed > budget.max_ram_bytes) {
            EvictLRU(needed);
        }
    }

    model->weights.resize(payload);
    if (fread(model->weights.data(), 1, payload, f) != payload) {
        fclose(f); return false;
    }
    fclose(f);

    model->ram_bytes  = sizeof(Ctm2Header) + payload;
    model->last_used  = (uint64_t)time(nullptr);

    std::string key = std::string(src_lang) + "_" + std::string(tgt_lang);
    std::lock_guard<std::mutex> lk(models_mutex);
    loaded_ram_total += model->ram_bytes;
    models[key] = std::move(model);
    return true;
}

void AeonTranslateImpl::EvictLRU(size_t needed_bytes) {
    // Already holding models_mutex from caller
    while (loaded_ram_total + needed_bytes > budget.max_ram_bytes
           && !models.empty())
    {
        // Find LRU
        auto lru = models.end();
        uint64_t oldest = UINT64_MAX;
        for (auto it = models.begin(); it != models.end(); ++it) {
            if (it->second->last_used < oldest) {
                oldest = it->second->last_used;
                lru    = it;
            }
        }
        if (lru == models.end()) break;
        loaded_ram_total -= lru->second->ram_bytes;
        models.erase(lru);
    }
}

std::string AeonTranslateImpl::FindModelPath(
    const char* src, const char* tgt) const
{
    // Look for exact pair: models_dir/en_es.ctm2
    char path[1024];
    snprintf(path, sizeof(path), "%s\\%s_%s.ctm2", models_dir, src, tgt);
    FILE* f = fopen(path, "rb");
    if (f) { fclose(f); return path; }

    // Look for generic target model: models_dir/madlad400_es.ctm2
    snprintf(path, sizeof(path), "%s\\madlad400_%s.ctm2", models_dir, tgt);
    f = fopen(path, "rb");
    if (f) { fclose(f); return path; }

    return "";
}

// =============================================================================
// AeonTranslate public API
// =============================================================================

AeonTranslate::AeonTranslate() : m_impl(new AeonTranslateImpl()) {}

AeonTranslate::~AeonTranslate() {
    Shutdown();
    delete m_impl;
}

bool AeonTranslate::Initialize(const ResourceBudget& budget) {
    m_budget = budget;
    m_impl->budget = budget;

    // Determine models directory
#ifdef _WIN32
    char appdata[MAX_PATH]{};
    SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appdata);
    snprintf(m_impl->models_dir, sizeof(m_impl->models_dir),
             "%s\\Aeon\\models", appdata);
    CreateDirectoryA(m_impl->models_dir, nullptr);
#else
    snprintf(m_impl->models_dir, sizeof(m_impl->models_dir),
             "%s/.aeon/models", getenv("HOME") ? getenv("HOME") : ".");
#endif

    // Hive offload enabled on low-RAM nodes
    m_impl->hive_offload_enabled = budget.hive_offload_ok
        && (budget.max_ram_bytes < 256 * 1024 * 1024);

    m_impl->running.store(true);

#ifdef _WIN32
    m_impl->queue_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
#endif

    // Thread pool: 1 on XP (cpu_class=0), 2 on Win7+, ncores-1 on modern
    int num_workers = 1;
    if (budget.cpu_class >= AEON_CPU_CLASS_SSE4)
        num_workers = std::max(1, (int)std::thread::hardware_concurrency() - 1);
    num_workers = std::min(num_workers, 4);

    m_impl->workers.resize(num_workers);
    for (int i = 0; i < num_workers; ++i)
        m_impl->workers[i] = std::thread([this]() { m_impl->WorkerLoop(); });

    m_healthy = true;
    return true;
}

void AeonTranslate::Shutdown() {
    if (!m_healthy) return;
    m_impl->running.store(false);
#ifdef _WIN32
    for (int i = 0; i < (int)m_impl->workers.size(); ++i)
        if (m_impl->queue_event) SetEvent(m_impl->queue_event);
    if (m_impl->queue_event) {
        CloseHandle(m_impl->queue_event);
        m_impl->queue_event = nullptr;
    }
#endif
    for (auto& w : m_impl->workers)
        if (w.joinable()) w.join();
    m_impl->workers.clear();
    m_impl->models.clear();
    m_healthy = false;
}

std::string AeonTranslate::DetectLanguage(const char* text, size_t len) const {
    return m_impl->lang_detector.Detect(text, len ? len : strlen(text));
}

std::vector<AeonLangPair> AeonTranslate::AvailablePairs() const {
    // Scan models directory for .ctm2 files
    std::vector<AeonLangPair> pairs;
#ifdef _WIN32
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s\\*.ctm2", m_impl->models_dir);
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            // Parse lang codes from filename: "en_es.ctm2"
            AeonLangPair lp{};
            sscanf(fd.cFileName, "%7[^_]_%7[^.]", lp.src_lang, lp.tgt_lang);
            lp.available = true;
            lp.tier      = AeonModelTier::LegacyEdition;
            pairs.push_back(lp);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#endif
    // Always add Hive offload as a fallback option
    if (m_impl->hive_offload_enabled) {
        AeonLangPair hive{};
        strncpy(hive.src_lang, "*", 15);
        strncpy(hive.tgt_lang, "*", 15);
        hive.tier      = AeonModelTier::HivePeer;
        hive.available = true;
        pairs.push_back(hive);
    }
    return pairs;
}

bool AeonTranslate::PreloadModel(const char* src_lang, const char* tgt_lang) {
    return m_impl->LoadModelFile(src_lang, tgt_lang);
}

void AeonTranslate::UnloadModel(const char* src_lang, const char* tgt_lang) {
    std::string key = std::string(src_lang) + "_" + std::string(tgt_lang);
    std::lock_guard<std::mutex> lk(m_impl->models_mutex);
    auto it = m_impl->models.find(key);
    if (it != m_impl->models.end()) {
        m_impl->loaded_ram_total -= it->second->ram_bytes;
        m_impl->models.erase(it);
    }
}

AeonModelTier AeonTranslate::GetTierFor(
    const char* src_lang, const char* tgt_lang) const
{
    std::string path = m_impl->FindModelPath(src_lang, tgt_lang);
    if (!path.empty()) {
        // Determine tier from filename
        if (path.find("madlad400") != std::string::npos)
            return AeonModelTier::StandardEdition;
        return AeonModelTier::LegacyEdition;
    }
    if (m_impl->hive_offload_enabled) return AeonModelTier::HivePeer;
    return AeonModelTier::LegacyEdition; // will fail at inference time gracefully
}

void AeonTranslate::TranslateAsync(
    const char*                   text,
    const char*                   src_lang,
    const char*                   tgt_lang,
    AeonTranslateStreamCallback   stream_cb,
    AeonTranslateCallback         done_cb,
    int                           max_tokens)
{
    TranslateTask task;
    task.text        = text;
    task.src_lang    = src_lang ? src_lang : "auto";
    task.tgt_lang    = tgt_lang;
    task.max_tokens  = max_tokens;
    task.stream_cb   = std::move(stream_cb);
    task.done_cb     = std::move(done_cb);

    {
        std::lock_guard<std::mutex> lk(m_impl->queue_mutex);
        m_impl->task_queue.push(std::move(task));
    }
#ifdef _WIN32
    if (m_impl->queue_event) SetEvent(m_impl->queue_event);
#endif
}

AeonTranslateResult AeonTranslate::TranslateSync(
    const char* text,
    const char* src_lang,
    const char* tgt_lang,
    int         max_tokens)
{
    std::string src = src_lang && strlen(src_lang) > 0 ? src_lang
        : m_impl->lang_detector.Detect(text, strlen(text));

    LoadedModel* model = m_impl->GetOrLoadModel(src.c_str(), tgt_lang);
    if (model) {
        return m_impl->RunInference(*model, text, max_tokens, nullptr);
    }

    AeonTranslateResult result;
    result.translated_text = text;
    result.detected_lang   = src;
    result.target_lang     = tgt_lang;
    result.confidence      = 0.0f;
    result.is_complete     = true;
    return result;
}

void AeonTranslate::TranslatePage(
    const char*           html,
    const char*           tgt_lang,
    AeonTranslateCallback dom_cb)
{
    // Full page translation:
    // 1. Parse HTML → extract text nodes (ignore script/style)
    // 2. Detect language from first 512 chars of aggregated text
    // 3. Batch text nodes into <512 token chunks
    // 4. Translate each chunk async, reassemble with original DOM positions
    // 5. Fire dom_cb with modified HTML once all chunks done
    //
    // Stub: just translate raw HTML as text
    TranslateAsync(html, nullptr, tgt_lang, nullptr, dom_cb, 4096);
}

bool AeonTranslate::CanOffloadToHive() const {
    return m_impl->hive_offload_enabled;
}

bool AeonTranslate::RequestHiveTranslation(
    const char*           text,
    const char*           src_lang,
    const char*           tgt_lang,
    AeonTranslateCallback callback)
{
    (void)text; (void)src_lang; (void)tgt_lang;
    // Full implementation: serialize request over QUIC to a capable Hive peer,
    // stream response back, call done_cb when complete.
    AeonTranslateResult result;
    result.translated_text = text;
    result.used_hive_peer  = true;
    result.confidence      = 0.0f;
    if (callback) callback(result);
    return false; // stub: no peer connected yet
}

// ── Singleton ─────────────────────────────────────────────────────────────────
AeonTranslate& AeonTranslateInstance() {
    static AeonTranslate instance;
    return instance;
}
