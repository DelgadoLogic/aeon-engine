/**
 * LLM Client — Embedded Inference via node-llama-cpp
 *
 * Zero-dependency local LLM inference. No Ollama, no external apps.
 * Uses node-llama-cpp to run GGUF models in-process with native
 * Metal/CUDA/Vulkan GPU acceleration.
 *
 * Fallback: If AEON_LLM_URL is set, routes to an external OpenAI-compatible
 * endpoint (Ollama, LM Studio, etc.) for dev/testing flexibility.
 *
 * @module llm-client
 */

import { getLlama, LlamaChatSession, type Llama, type LlamaModel, type LlamaContext, type LlamaEmbeddingContext } from "node-llama-cpp";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { existsSync, readdirSync, statSync } from "node:fs";

// ── Configuration ───────────────────────────────────────────

/** External LLM endpoint — if set, bypasses embedded inference */
const EXTERNAL_URL = process.env.AEON_LLM_URL ?? "";
const EXTERNAL_KEY = process.env.AEON_LLM_KEY ?? "";
const EXTERNAL_MODEL = process.env.AEON_MODEL ?? "llama3.2:3b";

/** Path to the GGUF model file for embedded inference */
const MODEL_PATH = process.env.AEON_MODEL_PATH ?? "";
const EMBEDDING_MODEL_PATH = process.env.AEON_EMBEDDING_MODEL_PATH ?? "";
const REQUEST_TIMEOUT = 120_000;

// ── Types ───────────────────────────────────────────────────

export interface ChatMessage {
  role: "system" | "user" | "assistant";
  content: string;
}

/**
 * Task complexity hint — controls maxTokens budget per-tool.
 * - quick:     Simple actions (click, navigate, extract) → 64 tokens
 * - standard:  Normal planning & analysis → 512 tokens
 * - deep:      Complex reasoning, multi-step plans → 2048 tokens
 * - unlimited: Full context generation → 4096 tokens
 */
export type TaskComplexity = "quick" | "standard" | "deep" | "unlimited";

const TOKEN_BUDGETS: Record<TaskComplexity, number> = {
  quick: 64,
  standard: 512,
  deep: 2048,
  unlimited: 4096,
};

/** Callback for progressive/streaming token delivery */
export type StreamCallback = (chunk: string) => void;

export interface ChatOptions {
  /** Task complexity hint for token budget */
  complexity?: TaskComplexity;
  /** Override maxTokens directly (takes priority over complexity) */
  maxTokens?: number;
  /** Temperature override (default: 0.2) */
  temperature?: number;
  /** Streaming callback — receives tokens as they generate */
  onToken?: StreamCallback;
}

export interface ChatResponse {
  content: string;
  model: string;
  totalDurationMs: number;
  promptTokens?: number;
  completionTokens?: number;
  tokensPerSecond?: number;
}

export interface LLMModel {
  name: string;
  size: number;
  modifiedAt: string;
}

// ── Embedding Engine ────────────────────────────────────────

let _embLlama: Llama | null = null;
let _embModel: LlamaModel | null = null;
let _embContext: LlamaEmbeddingContext | null = null;
let _embReady = false;

/** Patterns that identify embedding models (not agent brains) */
const EMBEDDING_PATTERNS = ["minilm", "embed", "nomic-embed", "bge-", "gte-", "e5-"];

function isEmbeddingModel(filename: string): boolean {
  const lower = filename.toLowerCase();
  return EMBEDDING_PATTERNS.some(p => lower.includes(p));
}

// ── Embedded Engine Singleton ───────────────────────────────

let _llama: Llama | null = null;
let _model: LlamaModel | null = null;
let _context: LlamaContext | null = null;
let _modelName = "none";
let _initPromise: Promise<void> | null = null;
let _initError: string | null = null;

/**
 * Resolve the GGUF model path for a specific purpose.
 * @param purpose - "agent" picks the largest non-embedding model; "embedding" picks the embedding model
 */
function resolveModelPath(purpose: "agent" | "embedding" = "agent"): string | null {
  // Check explicit env vars first
  if (purpose === "agent" && MODEL_PATH && existsSync(MODEL_PATH)) return MODEL_PATH;
  if (purpose === "embedding" && EMBEDDING_MODEL_PATH && existsSync(EMBEDDING_MODEL_PATH)) return EMBEDDING_MODEL_PATH;

  // Auto-discover from common locations
  const __dirname = dirname(fileURLToPath(import.meta.url));
  const searchDirs = [
    resolve(__dirname, "..", "models"),
    resolve(__dirname, "..", "..", "models"),
    resolve(__dirname, "..", "..", "..", "models"),
  ];

  for (const dir of searchDirs) {
    if (!existsSync(dir)) continue;
    try {
      const ggufFiles = readdirSync(dir)
        .filter((f: string) => f.endsWith(".gguf"))
        .map((f: string) => ({ name: f, size: statSync(resolve(dir, f)).size, path: resolve(dir, f) }))
        .sort((a, b) => b.size - a.size); // Largest first

      if (purpose === "embedding") {
        const embFile = ggufFiles.find(f => isEmbeddingModel(f.name));
        if (embFile) return embFile.path;
      } else {
        // Agent brain = largest non-embedding model
        const agentFile = ggufFiles.find(f => !isEmbeddingModel(f.name));
        if (agentFile) return agentFile.path;
      }
    } catch { /* continue */ }
  }

  return null;
}


/**
 * Initialize the embedded LLM engine (lazy, called once).
 */
async function initEmbeddedEngine(): Promise<void> {
  if (_model) return;
  if (_initError) return;

  const modelPath = resolveModelPath("agent");
  if (!modelPath) {
    _initError = "No GGUF agent model found. Set AEON_MODEL_PATH or place a .gguf file in the models/ directory.";
    console.error(`[llm-client] ${_initError}`);
    return;
  }

  try {
    console.error(`[llm-client] Initializing embedded inference engine...`);
    console.error(`[llm-client] Agent model: ${modelPath}`);

    // Smart GPU selection:
    // 1. Try local Vulkan build first and check if VRAM is sufficient
    // 2. Fall back to CPU build (faster on integrated GPUs with <4GB VRAM)
    try {
      // Try Vulkan build
      const vulkanLlama = await getLlama({
        gpu: "vulkan",
        cmakeOptions: {
          Vulkan_GLSLC_EXECUTABLE: "C:/VulkanSDK/1.4.341.1/Bin/glslc.exe",
          Vulkan_INCLUDE_DIR: "C:/VulkanSDK/1.4.341.1/Include",
          Vulkan_LIBRARY: "C:/VulkanSDK/1.4.341.1/Lib/vulkan-1.lib"
        }
      });
      const vramState = vulkanLlama.getVramState();
      const totalVram = (vramState as any)?.total ?? 0;

      if (totalVram >= 4 * 1024 * 1024 * 1024) {
        // Sufficient VRAM (>=4GB) — use Vulkan GPU
        _llama = vulkanLlama;
        console.error(`[llm-client] GPU: Vulkan (${Math.round(totalVram / 1024 / 1024)} MB VRAM)`);
      } else {
        // Insufficient VRAM — CPU is faster on integrated GPUs
        console.error(`[llm-client] ⚠ VRAM insufficient (${Math.round(totalVram / 1024 / 1024)} MB) — using CPU build`);
        _llama = await getLlama("lastBuild");
      }
    } catch {
      // Vulkan not available — use lastBuild (CPU)
      try {
        _llama = await getLlama("lastBuild");
        console.error(`[llm-client] Build: ${_llama.buildType} | GPU: ${_llama.gpu}`);
      } catch {
        console.error(`[llm-client] ⚠ lastBuild unavailable, falling back to default`);
        _llama = await getLlama();
      }
    }

    const gpuLayerOpts = _llama.gpu === "vulkan" ? { gpuLayers: "max" as const } : {};
    _model = await _llama.loadModel({ modelPath, ...gpuLayerOpts });
    _context = await _model.createContext({
      contextSize: 16384, // Gemma 4 E4B supports 128K; use 16K for balance
      sequences: 8
    });
    _modelName = modelPath.split(/[\\/]/).pop() ?? "embedded";

    console.error(`[llm-client] ✅ Agent engine ready — ${_modelName} (${_llama.gpu || 'CPU'})`);
  } catch (e: any) {
    _initError = e.message ?? "Failed to initialize embedded engine";
    console.error(`[llm-client] ❌ Agent engine init failed: ${_initError}`);
    _model = null;
    _context = null;
  }
}

/**
 * Initialize the embedding engine (separate from agent brain).
 * Returns an embedding function compatible with memory.registerEmbeddingFn().
 */
export async function initEmbeddingEngine(): Promise<((text: string) => Promise<number[]>) | null> {
  const embPath = resolveModelPath("embedding");
  if (!embPath) {
    console.error(`[llm-client] No embedding model found — semantic search will use keyword-only mode`);
    return null;
  }

  try {
    console.error(`[llm-client] Loading embedding model: ${embPath}`);
    _embLlama = _llama ?? await getLlama("lastBuild");
    _embModel = await _embLlama.loadModel({ modelPath: embPath });
    _embContext = await _embModel.createEmbeddingContext();
    _embReady = true;

    const embName = embPath.split(/[\\/]/).pop() ?? "embedding";
    console.error(`[llm-client] ✅ Embedding engine ready — ${embName}`);

    // Return the embedding function using the correct node-llama-cpp v3 API
    return async (text: string): Promise<number[]> => {
      if (!_embContext) throw new Error("Embedding engine not initialized");
      try {
        const embedding = await _embContext.getEmbeddingFor(text);
        return Array.from(embedding.vector);
      } catch {
        // Fallback if the model doesn't support embeddings natively
        return simpleHashEmbedding(text);
      }
    };
  } catch (e: any) {
    console.error(`[llm-client] ⚠ Embedding engine failed: ${e.message} — falling back to keyword search`);
    return null;
  }
}

/**
 * Simple hash-based embedding fallback.
 * Produces a deterministic 384-dim vector from text (same as MiniLM output dim).
 * Not as good as real embeddings but enables basic semantic similarity.
 */
function simpleHashEmbedding(text: string): number[] {
  const dim = 384;
  const vec = new Float64Array(dim);
  const words = text.toLowerCase().split(/\s+/);
  for (const word of words) {
    let hash = 0;
    for (let i = 0; i < word.length; i++) {
      hash = ((hash << 5) - hash + word.charCodeAt(i)) | 0;
    }
    // Distribute word influence across multiple dimensions
    for (let d = 0; d < 8; d++) {
      const idx = Math.abs((hash + d * 7919) % dim);
      vec[idx] += 1.0 / (1 + d);
    }
  }
  // Normalize
  let mag = 0;
  for (let i = 0; i < dim; i++) mag += vec[i] * vec[i];
  mag = Math.sqrt(mag);
  if (mag > 0) for (let i = 0; i < dim; i++) vec[i] /= mag;
  return Array.from(vec);
}

function ensureInit(): Promise<void> {
  if (!_initPromise && !EXTERNAL_URL) {
    _initPromise = initEmbeddedEngine();
  }
  return _initPromise ?? Promise.resolve();
}

// ── Mode Detection ──────────────────────────────────────────

function useExternalBackend(): boolean {
  return !!EXTERNAL_URL;
}

// ── Health Check ────────────────────────────────────────────

export async function llmHealthCheck(): Promise<{
  available: boolean;
  url: string;
  model: string;
  backend: "embedded" | "external";
  error?: string;
}> {
  // External mode (Ollama / OpenAI-compatible)
  if (useExternalBackend()) {
    try {
      const controller = new AbortController();
      const timeout = setTimeout(() => controller.abort(), 5000);
      const resp = await fetch(`${EXTERNAL_URL}/api/tags`, { signal: controller.signal });
      clearTimeout(timeout);

      if (!resp.ok) {
        return { available: false, url: EXTERNAL_URL, model: EXTERNAL_MODEL, backend: "external", error: `HTTP ${resp.status}` };
      }

      const data = (await resp.json()) as { models?: Array<{ name: string }> };
      const models = data.models?.map((m) => m.name.split(":")[0]) ?? [];
      const modelBase = EXTERNAL_MODEL.split(":")[0];
      const hasModel = models.some((m) => m === modelBase || m === EXTERNAL_MODEL);

      return {
        available: true,
        url: EXTERNAL_URL,
        model: EXTERNAL_MODEL,
        backend: "external",
        ...(!hasModel ? { error: `Model '${EXTERNAL_MODEL}' not found. Available: ${models.join(", ")}` } : {}),
      };
    } catch (e: any) {
      return { available: false, url: EXTERNAL_URL, model: EXTERNAL_MODEL, backend: "external", error: e.message ?? "Connection failed" };
    }
  }

  // Embedded mode
  await ensureInit();
  if (_initError || !_model) {
    return {
      available: false,
      url: "embedded://node-llama-cpp",
      model: _modelName,
      backend: "embedded",
      error: _initError ?? "Model not loaded",
    };
  }

  return {
    available: true,
    url: "embedded://node-llama-cpp",
    model: _modelName,
    backend: "embedded",
  };
}

// ── Chat Completion ─────────────────────────────────────────

/**
 * Send a chat completion request.
 * Routes to embedded engine or external endpoint based on config.
 *
 * @param messages - Chat messages (system, user, assistant)
 * @param modelOrOpts - Model name string (external) or ChatOptions (embedded)
 */
export async function llmChat(
  messages: ChatMessage[],
  modelOrOpts?: string | ChatOptions,
): Promise<ChatResponse> {
  const opts: ChatOptions = typeof modelOrOpts === "string" ? {} : (modelOrOpts ?? {});
  const model = typeof modelOrOpts === "string" ? modelOrOpts : undefined;

  if (useExternalBackend()) {
    return externalChat(messages, model);
  }
  return embeddedChat(messages, opts);
}

/**
 * Embedded inference via node-llama-cpp (in-process, zero HTTP).
 * Now supports task-aware token budgets and streaming.
 */
async function embeddedChat(messages: ChatMessage[], opts: ChatOptions = {}): Promise<ChatResponse> {
  await ensureInit();

  if (!_model || !_context) {
    throw new Error(`Embedded engine not available: ${_initError ?? "Model not loaded"}`);
  }

  const startMs = Date.now();

  // Resolve token budget: explicit maxTokens > complexity hint > default
  const maxTokens = opts.maxTokens
    ?? TOKEN_BUDGETS[opts.complexity ?? "standard"]
    ?? 512;
  const temperature = opts.temperature ?? 0.2;

  // Create a fresh session for each request (stateless planning)
  const sequence = _context.getSequence();
  const session = new LlamaChatSession({
    contextSequence: sequence,
  });

  try {
    // Build the prompt: system message becomes the session system prompt,
    // then we feed user/assistant turns as history
    const systemMsg = messages.find((m) => m.role === "system");
    const conversationMsgs = messages.filter((m) => m.role !== "system");

    // Compose the user prompt with system context
    let userPrompt = "";
    if (systemMsg) {
      userPrompt += systemMsg.content + "\n\n";
    }
    for (const msg of conversationMsgs) {
      if (msg.role === "user") {
        userPrompt += msg.content;
      } else if (msg.role === "assistant") {
        userPrompt += `\n\nPrevious assistant response:\n${msg.content}\n\n`;
      }
    }

    let response = "";

    // Streaming mode: deliver tokens progressively via callback
    if (opts.onToken) {
      const onToken = opts.onToken;
      response = await session.prompt(userPrompt, {
        temperature,
        maxTokens,
        onTextChunk(chunk: string) {
          onToken(chunk);
        },
      });
    } else {
      // Non-streaming mode
      response = await session.prompt(userPrompt, {
        temperature,
        maxTokens,
      });
    }

    const durationMs = Date.now() - startMs;

    // Estimate tokens for throughput tracking
    const estTokens = Math.ceil(response.length / 4);
    const tokPerSec = durationMs > 0 ? (estTokens / (durationMs / 1000)) : 0;

    return {
      content: response,
      model: _modelName,
      totalDurationMs: durationMs,
      completionTokens: estTokens,
      tokensPerSecond: Math.round(tokPerSec * 10) / 10,
    };
  } finally {
    session.dispose();
    sequence.dispose();
  }
}

/**
 * External inference via HTTP (Ollama / OpenAI-compatible — fallback).
 */
async function externalChat(
  messages: ChatMessage[],
  model?: string,
): Promise<ChatResponse> {
  const targetModel = model ?? EXTERNAL_MODEL;
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), REQUEST_TIMEOUT);

  try {
    const resp = await fetch(`${EXTERNAL_URL}/api/chat`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        ...(EXTERNAL_KEY ? { Authorization: `Bearer ${EXTERNAL_KEY}` } : {}),
      },
      body: JSON.stringify({
        model: targetModel,
        messages,
        stream: false,
        options: {
          temperature: 0.2,
          num_ctx: 8192,
          num_predict: 2048,
        },
      }),
      signal: controller.signal,
    });
    clearTimeout(timeout);

    if (!resp.ok) {
      const body = await resp.text().catch(() => "");
      throw new Error(`External LLM returned HTTP ${resp.status}: ${body.slice(0, 200)}`);
    }

    const data = (await resp.json()) as {
      message?: { content: string };
      model: string;
      total_duration?: number;
      prompt_eval_count?: number;
      eval_count?: number;
    };

    const content = data.message?.content ?? "";
    if (!content) {
      throw new Error("External LLM returned empty response");
    }

    return {
      content,
      model: data.model ?? targetModel,
      totalDurationMs: Math.round((data.total_duration ?? 0) / 1_000_000),
      promptTokens: data.prompt_eval_count,
      completionTokens: data.eval_count,
    };
  } catch (e: any) {
    clearTimeout(timeout);
    if (e.name === "AbortError") {
      throw new Error(`LLM request timed out after ${REQUEST_TIMEOUT / 1000}s`);
    }
    throw e;
  }
}

// ── List Models ─────────────────────────────────────────────

export async function llmListModels(): Promise<LLMModel[]> {
  // Embedded mode — return the loaded model
  if (!useExternalBackend()) {
    await ensureInit();
    if (_model) {
      return [{
        name: _modelName,
        size: 0, // node-llama-cpp doesn't expose file size directly
        modifiedAt: new Date().toISOString(),
      }];
    }
    return [];
  }

  // External mode — query Ollama API
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 5000);
  try {
    const resp = await fetch(`${EXTERNAL_URL}/api/tags`, { signal: controller.signal });
    clearTimeout(timeout);
    if (!resp.ok) return [];

    const data = (await resp.json()) as {
      models?: Array<{ name: string; size: number; modified_at: string }>;
    };

    return (data.models ?? []).map((m) => ({
      name: m.name,
      size: m.size,
      modifiedAt: m.modified_at,
    }));
  } catch {
    clearTimeout(timeout);
    return [];
  }
}

// ── JSON Extraction Helper ──────────────────────────────────

/**
 * Extract a JSON object from an LLM response that may contain
 * markdown code fences or extra text around the JSON.
 */
export function extractJSON<T = Record<string, unknown>>(text: string): T | null {
  // Try direct parse first
  try {
    return JSON.parse(text) as T;
  } catch { /* continue */ }

  // Try extracting from markdown code fence
  const fenceMatch = text.match(/```(?:json)?\s*\n?([\s\S]*?)\n?\s*```/);
  if (fenceMatch) {
    try {
      return JSON.parse(fenceMatch[1].trim()) as T;
    } catch { /* continue */ }
  }

  // Try finding first { ... } or [ ... ] block
  const braceStart = text.indexOf("{");
  const bracketStart = text.indexOf("[");
  const start = braceStart === -1 ? bracketStart
    : bracketStart === -1 ? braceStart
    : Math.min(braceStart, bracketStart);

  if (start === -1) return null;

  const isArray = text[start] === "[";
  const closeChar = isArray ? "]" : "}";
  let depth = 0;

  for (let i = start; i < text.length; i++) {
    const c = text[i];
    if (c === text[start]) depth++;
    else if (c === closeChar) {
      depth--;
      if (depth === 0) {
        try {
          return JSON.parse(text.slice(start, i + 1)) as T;
        } catch {
          return null;
        }
      }
    }
  }

  return null;
}
