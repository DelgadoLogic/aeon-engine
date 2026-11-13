/**
 * Persistent Agent Memory — Cross-Session Knowledge Store
 *
 * Provides Aeon with persistent memory that survives across server restarts:
 *   - Store facts, context, and knowledge snippets with tags
 *   - Search memory by keyword, tag, or recency
 *   - Auto-expire old memories (configurable TTL)
 *   - Memory entries are indexed for fast retrieval
 *   - Hash-based deduplication (mem0 pattern)
 *   - Full mutation history tracking (ADD/UPDATE/DELETE)
 *   - Get + Update by ID for surgical memory refinement
 *
 * Data lives at ~/.aeon/memory/memories.jsonl
 * History lives at ~/.aeon/memory/history.jsonl
 *
 * Inspired by BrowserOS persistent memory, mem0 agent memory, and OpenCrabs store.
 *
 * @module memory
 */

import {
  appendFileSync,
  readFileSync,
  writeFileSync,
  existsSync,
  mkdirSync,
} from "node:fs";
import { join } from "node:path";
import { homedir } from "node:os";
import { createHash } from "node:crypto";

// ── Paths ───────────────────────────────────────────────────

const AEON_HOME = join(homedir(), ".aeon");
const MEMORY_DIR = join(AEON_HOME, "memory");
const MEMORY_FILE = join(MEMORY_DIR, "memories.jsonl");
const HISTORY_FILE = join(MEMORY_DIR, "history.jsonl");

// ── Types ───────────────────────────────────────────────────

export interface MemoryEntry {
  id: string;
  content: string;
  tags: string[];
  source: string; // "user" | "agent" | "rsi" | "system"
  importance: number; // 0.0–1.0, higher = more important
  timestamp: string; // ISO 8601
  expiresAt?: string; // ISO 8601, optional TTL
  accessCount: number;
  lastAccessed?: string;
  contentHash?: string; // MD5 hash for dedup
  embedding?: number[]; // Semantic vector (when available)
  entities?: string[]; // Extracted named entities
}

export interface MemoryHistoryEntry {
  id: string;
  memoryId: string;
  oldContent: string | null;
  newContent: string | null;
  event: "ADD" | "UPDATE" | "DELETE";
  timestamp: string;
}

// ── In-Memory Index ─────────────────────────────────────────

let _memories: MemoryEntry[] = [];
let _loaded = false;
const _hashIndex = new Set<string>(); // MD5 hash dedup index

/** Pluggable embedding function — set via registerEmbeddingFn() */
let _embedFn: ((text: string) => Promise<number[]>) | null = null;

/**
 * Register an embedding function for semantic search.
 * Call this once after the LLM engine initializes.
 * If never called, memory search falls back to keyword-only.
 */
export function registerEmbeddingFn(fn: (text: string) => Promise<number[]>): void {
  _embedFn = fn;
  process.stderr.write(`[memory] Embedding function registered — semantic search enabled\n`);
}

/**
 * Check if semantic search is available.
 */
export function hasSemanticSearch(): boolean {
  return _embedFn !== null;
}

// ── Vector Math ─────────────────────────────────────────────

/**
 * Cosine similarity between two vectors.
 * Returns a value between -1 and 1 (1 = identical, 0 = orthogonal).
 */
function cosineSimilarity(a: number[], b: number[]): number {
  if (a.length !== b.length || a.length === 0) return 0;
  let dot = 0, magA = 0, magB = 0;
  for (let i = 0; i < a.length; i++) {
    dot += a[i] * b[i];
    magA += a[i] * a[i];
    magB += b[i] * b[i];
  }
  const denom = Math.sqrt(magA) * Math.sqrt(magB);
  return denom === 0 ? 0 : dot / denom;
}

// ── Entity Extraction ───────────────────────────────────────

/**
 * Extract named entities from text (lightweight heuristic).
 * Extracts: PascalCase identifiers, quoted strings, URLs, file paths.
 * This provides a foundation for relationship-aware memory retrieval.
 */
function extractEntities(text: string): string[] {
  const entities = new Set<string>();

  // PascalCase / camelCase identifiers (likely class/project names)
  const identifiers = text.match(/\b[A-Z][a-zA-Z0-9]{2,}(?:[A-Z][a-z]+)+\b/g);
  if (identifiers) identifiers.forEach(id => entities.add(id));

  // Quoted strings (likely named entities)
  const quoted = text.match(/["']([^"']{2,50})["']/g);
  if (quoted) quoted.forEach(q => entities.add(q.replace(/["']/g, '')));

  // URLs
  const urls = text.match(/https?:\/\/[^\s)]+/g);
  if (urls) urls.forEach(u => entities.add(u));

  // File paths (Windows or Unix)
  const paths = text.match(/(?:[A-Z]:\\|~\/|\.\/|\.\.\/)[^\s,;)]+/g);
  if (paths) paths.forEach(p => entities.add(p));

  // Technical terms: kebab-case or snake_case identifiers (3+ chars)
  const technical = text.match(/\b[a-z][a-z0-9]*(?:[-_][a-z0-9]+){1,}\b/g);
  if (technical) technical.forEach(t => entities.add(t));

  return [...entities].slice(0, 20); // Cap at 20 entities per memory
}

function ensureMemoryDir(): void {
  if (!existsSync(MEMORY_DIR)) {
    mkdirSync(MEMORY_DIR, { recursive: true });
  }
}

function loadMemories(): void {
  if (_loaded) return;
  ensureMemoryDir();

  if (!existsSync(MEMORY_FILE)) {
    _memories = [];
    _loaded = true;
    return;
  }

  try {
    const raw = readFileSync(MEMORY_FILE, "utf-8").trim();
    if (!raw) {
      _memories = [];
      _loaded = true;
      return;
    }
    _memories = raw
      .split("\n")
      .filter((line) => line.trim())
      .map((line) => {
        try {
          return JSON.parse(line) as MemoryEntry;
        } catch {
          return null;
        }
      })
      .filter((m): m is MemoryEntry => m !== null);
  } catch {
    _memories = [];
  }

  // Build hash index for dedup
  _hashIndex.clear();
  for (const m of _memories) {
    if (m.contentHash) _hashIndex.add(m.contentHash);
  }

  // Purge expired memories
  const now = new Date().toISOString();
  const before = _memories.length;
  _memories = _memories.filter(
    (m) => !m.expiresAt || m.expiresAt > now
  );
  if (_memories.length < before) {
    flushMemories();
  }

  _loaded = true;
}

function flushMemories(): void {
  ensureMemoryDir();
  const data = _memories.map((m) => JSON.stringify(m)).join("\n") + "\n";
  writeFileSync(MEMORY_FILE, data, "utf-8");
}

function generateId(): string {
  return `mem_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 6)}`;
}

function hashContent(content: string): string {
  return createHash("md5").update(content).digest("hex");
}

// ── History Tracking ────────────────────────────────────────

function recordHistory(
  memoryId: string,
  event: MemoryHistoryEntry["event"],
  oldContent: string | null,
  newContent: string | null
): void {
  try {
    ensureMemoryDir();
    const entry: MemoryHistoryEntry = {
      id: `hist_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 6)}`,
      memoryId,
      oldContent: oldContent ? oldContent.slice(0, 2000) : null,
      newContent: newContent ? newContent.slice(0, 2000) : null,
      event,
      timestamp: new Date().toISOString(),
    };
    appendFileSync(HISTORY_FILE, JSON.stringify(entry) + "\n", "utf-8");
  } catch {
    // Non-critical — never crash over history logging
  }
}

// ── Public API ──────────────────────────────────────────────

/**
 * Store a new memory entry.
 */
export function storeMemory(
  content: string,
  options?: {
    tags?: string[];
    source?: string;
    importance?: number;
    ttlHours?: number;
  }
): MemoryEntry | { duplicate: true; existingId: string } {
  loadMemories();

  // Hash-based deduplication (mem0 pattern)
  const hash = hashContent(content);
  if (_hashIndex.has(hash)) {
    const existing = _memories.find((m) => m.contentHash === hash);
    if (existing) {
      return { duplicate: true, existingId: existing.id };
    }
  }

  const entry: MemoryEntry = {
    id: generateId(),
    content,
    tags: options?.tags ?? [],
    source: options?.source ?? "agent",
    importance: Math.max(0, Math.min(1, options?.importance ?? 0.5)),
    timestamp: new Date().toISOString(),
    accessCount: 0,
    contentHash: hash,
  };

  if (options?.ttlHours && options.ttlHours > 0) {
    const expires = new Date(Date.now() + options.ttlHours * 3600_000);
    entry.expiresAt = expires.toISOString();
  }

  _memories.push(entry);
  _hashIndex.add(hash);

  // Generate embedding + extract entities asynchronously (non-blocking)
  (async () => {
    try {
      // Entity extraction (synchronous, fast)
      entry.entities = extractEntities(content);

      // Embedding generation (async, may fail if no model loaded)
      if (_embedFn) {
        entry.embedding = await _embedFn(content);
      }

      // Flush with enriched data
      flushMemories();
    } catch {
      // Non-critical — memory was already stored without enrichment
    }
  })();

  // Append to file (fast path — without embedding/entities initially)
  try {
    ensureMemoryDir();
    appendFileSync(MEMORY_FILE, JSON.stringify(entry) + "\n", "utf-8");
  } catch {
    // Fall back to full flush
    flushMemories();
  }

  // Record history
  recordHistory(entry.id, "ADD", null, content);

  return entry;
}

/**
 * Search memories by keyword (content + tags) with BM25-style ranking.
 * When embeddings are available, performs hybrid search:
 *   - BM25 keyword scoring (exact match boosting)
 *   - Cosine similarity on vector embeddings
 *   - Reciprocal Rank Fusion (RRF) to merge both ranked lists
 */
export async function searchMemory(
  query: string,
  options?: {
    tags?: string[];
    source?: string;
    limit?: number;
    minImportance?: number;
    semantic?: boolean; // Force semantic search (default: auto)
  }
): Promise<MemoryEntry[]> {
  loadMemories();

  const limit = options?.limit ?? 10;
  const minImp = options?.minImportance ?? 0;
  const queryTerms = query.toLowerCase().split(/\s+/).filter(Boolean);
  const useSemantic = (options?.semantic ?? true) && _embedFn !== null;

  // Pre-filter by source, importance, tags
  let candidates = _memories.filter((m) => {
    if (options?.source && m.source !== options.source) return false;
    if (m.importance < minImp) return false;
    if (options?.tags && options.tags.length > 0) {
      const hasTag = options.tags.some((t) =>
        m.tags.map((mt) => mt.toLowerCase()).includes(t.toLowerCase())
      );
      if (!hasTag) return false;
    }
    return true;
  });

  // ── BM25 Keyword Scoring ──────────────────────────────
  const keywordScored = candidates.map((m) => {
    const contentLower = m.content.toLowerCase();
    const tagsLower = m.tags.join(" ").toLowerCase();
    const entitiesLower = (m.entities ?? []).join(" ").toLowerCase();

    let score = 0;

    for (const term of queryTerms) {
      if (contentLower.includes(term)) score += 2;
      if (tagsLower.includes(term)) score += 3;
      if (entitiesLower.includes(term)) score += 2.5; // Entity match boost
    }

    // Boost by importance
    score += m.importance * 2;

    // Boost by recency (last 24h gets +1)
    const age = Date.now() - new Date(m.timestamp).getTime();
    if (age < 86400_000) score += 1;

    // Boost by access count
    score += Math.min(m.accessCount * 0.1, 1);

    return { memory: m, score };
  });

  keywordScored.sort((a, b) => {
    if (b.score !== a.score) return b.score - a.score;
    return b.memory.timestamp.localeCompare(a.memory.timestamp);
  });

  // ── Semantic Vector Scoring (when available) ──────────
  let finalResults: MemoryEntry[];

  if (useSemantic) {
    try {
      const queryEmbedding = await _embedFn!(query);
      const embeddableCandidates = candidates.filter(m => m.embedding && m.embedding.length > 0);

      if (embeddableCandidates.length > 0) {
        const vectorScored = embeddableCandidates.map(m => ({
          memory: m,
          score: cosineSimilarity(queryEmbedding, m.embedding!),
        }));

        vectorScored.sort((a, b) => b.score - a.score);

        // Reciprocal Rank Fusion (k=60)
        // RRF(d) = sum(1 / (k + rank_i)) for each ranking system
        const rrfScores = new Map<string, number>();
        const k = 60;

        keywordScored.forEach(({ memory }, rank) => {
          rrfScores.set(memory.id, (rrfScores.get(memory.id) ?? 0) + 1 / (k + rank + 1));
        });

        vectorScored.forEach(({ memory }, rank) => {
          rrfScores.set(memory.id, (rrfScores.get(memory.id) ?? 0) + 1 / (k + rank + 1));
        });

        // Merge and sort by RRF score
        const allIds = new Set([...keywordScored.map(s => s.memory.id), ...vectorScored.map(s => s.memory.id)]);
        const rrfResults = [...allIds]
          .map(id => ({
            memory: candidates.find(m => m.id === id)!,
            score: rrfScores.get(id) ?? 0,
          }))
          .filter(r => r.memory)
          .sort((a, b) => b.score - a.score);

        finalResults = rrfResults.slice(0, limit).map(r => r.memory);
      } else {
        // No embeddings stored yet — fall back to keyword
        finalResults = keywordScored.slice(0, limit).map(s => s.memory);
      }
    } catch {
      // Embedding generation failed — fall back to keyword
      finalResults = keywordScored.slice(0, limit).map(s => s.memory);
    }
  } else {
    finalResults = keywordScored.slice(0, limit).map(s => s.memory);
  }

  // Update access counts for returned results
  for (const mem of finalResults) {
    mem.accessCount++;
    mem.lastAccessed = new Date().toISOString();
  }

  // Flush access count updates periodically
  if (finalResults.length > 0) {
    try {
      flushMemories();
    } catch {
      // Non-critical
    }
  }

  return finalResults;
}

/**
 * Get a single memory by ID.
 */
export function getMemory(id: string): MemoryEntry | null {
  loadMemories();
  return _memories.find((m) => m.id === id) ?? null;
}

/**
 * Update an existing memory's content, tags, or importance.
 * Tracks the mutation in history.jsonl (mem0 pattern).
 */
export function updateMemory(
  id: string,
  newContent: string,
  options?: {
    tags?: string[];
    importance?: number;
    ttlHours?: number;
  }
): MemoryEntry | null {
  loadMemories();

  const idx = _memories.findIndex((m) => m.id === id);
  if (idx === -1) return null;

  const old = _memories[idx];
  const oldContent = old.content;

  // Remove old hash, add new one
  if (old.contentHash) _hashIndex.delete(old.contentHash);
  const newHash = hashContent(newContent);

  // Check dedup against new content
  if (_hashIndex.has(newHash)) {
    const existing = _memories.find((m) => m.contentHash === newHash && m.id !== id);
    if (existing) {
      // Re-add old hash since we're not proceeding
      if (old.contentHash) _hashIndex.add(old.contentHash);
      return null; // Would create duplicate
    }
  }

  old.content = newContent;
  old.contentHash = newHash;
  old.timestamp = new Date().toISOString();
  if (options?.tags !== undefined) old.tags = options.tags;
  if (options?.importance !== undefined) old.importance = Math.max(0, Math.min(1, options.importance));
  if (options?.ttlHours !== undefined && options.ttlHours > 0) {
    old.expiresAt = new Date(Date.now() + options.ttlHours * 3600_000).toISOString();
  }

  _hashIndex.add(newHash);
  flushMemories();

  // Record history
  recordHistory(id, "UPDATE", oldContent, newContent);

  return old;
}

/**
 * Delete a memory by ID.
 */
export function deleteMemory(id: string): boolean {
  loadMemories();

  const target = _memories.find((m) => m.id === id);
  if (!target) return false;

  // Remove from hash index
  if (target.contentHash) _hashIndex.delete(target.contentHash);

  // Record history before deletion
  recordHistory(id, "DELETE", target.content, null);

  _memories = _memories.filter((m) => m.id !== id);
  flushMemories();
  return true;
}

/**
 * Get memory mutation history for a specific memory or all memories.
 */
export function memoryHistory(memoryId?: string, limit = 20): MemoryHistoryEntry[] {
  try {
    if (!existsSync(HISTORY_FILE)) return [];
    const raw = readFileSync(HISTORY_FILE, "utf-8").trim();
    if (!raw) return [];

    let entries = raw
      .split("\n")
      .filter((l) => l.trim())
      .map((l) => {
        try { return JSON.parse(l) as MemoryHistoryEntry; } catch { return null; }
      })
      .filter((e): e is MemoryHistoryEntry => e !== null);

    if (memoryId) {
      entries = entries.filter((e) => e.memoryId === memoryId);
    }

    return entries.reverse().slice(0, limit);
  } catch {
    return [];
  }
}

/**
 * List all memories, optionally filtered.
 */
export function listMemories(options?: {
  tags?: string[];
  source?: string;
  limit?: number;
}): MemoryEntry[] {
  loadMemories();

  let results = [..._memories];

  if (options?.source) {
    results = results.filter((m) => m.source === options.source);
  }

  if (options?.tags && options.tags.length > 0) {
    results = results.filter((m) =>
      options.tags!.some((t) =>
        m.tags.map((mt) => mt.toLowerCase()).includes(t.toLowerCase())
      )
    );
  }

  // Sort by timestamp (most recent first)
  results.sort((a, b) => b.timestamp.localeCompare(a.timestamp));

  const limit = options?.limit ?? 20;
  return results.slice(0, limit);
}

/**
 * Get memory statistics.
 */
export function memoryStats(): {
  totalMemories: number;
  bySource: Record<string, number>;
  byTag: Record<string, number>;
  oldestTimestamp: string | null;
  newestTimestamp: string | null;
  avgImportance: number;
} {
  loadMemories();

  const bySource: Record<string, number> = {};
  const byTag: Record<string, number> = {};
  let totalImportance = 0;

  for (const m of _memories) {
    bySource[m.source] = (bySource[m.source] ?? 0) + 1;
    for (const t of m.tags) {
      byTag[t] = (byTag[t] ?? 0) + 1;
    }
    totalImportance += m.importance;
  }

  const sorted = [..._memories].sort((a, b) =>
    a.timestamp.localeCompare(b.timestamp)
  );

  return {
    totalMemories: _memories.length,
    bySource,
    byTag,
    oldestTimestamp: sorted[0]?.timestamp ?? null,
    newestTimestamp: sorted[sorted.length - 1]?.timestamp ?? null,
    avgImportance:
      _memories.length > 0 ? totalImportance / _memories.length : 0,
  };
}

/**
 * Load brain context from memory for system prompt injection.
 * Returns the most important + most accessed memories formatted for LLM context.
 */
export function loadMemoryContext(maxEntries = 15): string {
  loadMemories();

  if (_memories.length === 0) return "";

  // Get top memories by importance + access frequency
  const scored = _memories.map((m) => ({
    memory: m,
    score: m.importance * 3 + Math.min(m.accessCount * 0.5, 2),
  }));

  scored.sort((a, b) => b.score - a.score);

  const top = scored.slice(0, maxEntries);

  const lines = ["## Agent Memory", ""];

  for (const { memory: m } of top) {
    const tags = m.tags.length > 0 ? ` [${m.tags.join(", ")}]` : "";
    lines.push(`- ${m.content}${tags}`);
  }

  return lines.join("\n");
}
