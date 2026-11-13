/**
 * TTL Cache — In-Memory Key-Value Store with Auto-Expiry
 *
 * Inspired by Aide's Redis-like storage wrapper:
 *   - Set keys with optional TTL (time-to-live) in seconds
 *   - Automatic cleanup of expired entries on a configurable interval
 *   - Functional caching via `cached()` higher-order function
 *   - LRU eviction when max capacity is reached
 *
 * Use for:
 *   - Caching CDP target lists to avoid redundant lookups
 *   - Caching snapshot data between rapid tool calls
 *   - Caching LLM responses during RSI analysis
 *   - Any ephemeral data that doesn't need to survive a restart
 *
 * @module cache
 */

// ── Types ───────────────────────────────────────────────────

interface CacheEntry<T = unknown> {
  value: T;
  expiresAt: number; // Unix timestamp (ms). 0 = never expires
  createdAt: number;
  accessCount: number;
  lastAccessed: number;
}

interface CacheOptions {
  /** Maximum number of entries. Default: 500 */
  maxEntries?: number;
  /** Cleanup interval in ms. Default: 60_000 (1 minute) */
  cleanupIntervalMs?: number;
}

interface CacheStats {
  totalEntries: number;
  maxEntries: number;
  hitRate: number;
  missRate: number;
  totalHits: number;
  totalMisses: number;
  oldestEntryAge: number; // seconds
}

// ── Cache Implementation ────────────────────────────────────

const DEFAULT_MAX_ENTRIES = 500;
const DEFAULT_CLEANUP_INTERVAL_MS = 60_000;

const _cache = new Map<string, CacheEntry>();
let _cleanupTimer: ReturnType<typeof setInterval> | null = null;
let _maxEntries = DEFAULT_MAX_ENTRIES;
let _hits = 0;
let _misses = 0;

/**
 * Initialize the cache cleanup timer.
 * Call once during server startup.
 */
export function initCache(options?: CacheOptions): void {
  _maxEntries = options?.maxEntries ?? DEFAULT_MAX_ENTRIES;
  const interval = options?.cleanupIntervalMs ?? DEFAULT_CLEANUP_INTERVAL_MS;

  if (_cleanupTimer) clearInterval(_cleanupTimer);

  _cleanupTimer = setInterval(() => {
    evictExpired();
  }, interval);

  // Don't block process exit for cache cleanup
  if (_cleanupTimer.unref) _cleanupTimer.unref();
}

/**
 * Get a value from the cache.
 * Returns undefined if the key doesn't exist or has expired.
 */
export function cacheGet<T = unknown>(key: string): T | undefined {
  const entry = _cache.get(key);
  if (!entry) {
    _misses++;
    return undefined;
  }

  // Check expiry
  if (entry.expiresAt > 0 && Date.now() > entry.expiresAt) {
    _cache.delete(key);
    _misses++;
    return undefined;
  }

  entry.accessCount++;
  entry.lastAccessed = Date.now();
  _hits++;
  return entry.value as T;
}

/**
 * Set a value in the cache.
 * @param key - Cache key
 * @param value - Value to store
 * @param ttlSeconds - Time-to-live in seconds. 0 = never expires. Default: 300 (5 min)
 */
export function cacheSet<T = unknown>(
  key: string,
  value: T,
  ttlSeconds = 300
): void {
  // Evict if at capacity
  if (_cache.size >= _maxEntries && !_cache.has(key)) {
    evictLRU();
  }

  const now = Date.now();
  _cache.set(key, {
    value,
    expiresAt: ttlSeconds > 0 ? now + ttlSeconds * 1000 : 0,
    createdAt: now,
    accessCount: 0,
    lastAccessed: now,
  });
}

/**
 * Delete a key from the cache.
 */
export function cacheDel(key: string): boolean {
  return _cache.delete(key);
}

/**
 * Check if a key exists and is not expired.
 */
export function cacheHas(key: string): boolean {
  const entry = _cache.get(key);
  if (!entry) return false;
  if (entry.expiresAt > 0 && Date.now() > entry.expiresAt) {
    _cache.delete(key);
    return false;
  }
  return true;
}

/**
 * Clear all cache entries.
 */
export function cacheClear(): void {
  _cache.clear();
  _hits = 0;
  _misses = 0;
}

/**
 * Get cache statistics.
 */
export function cacheStats(): CacheStats {
  evictExpired(); // Clean first for accurate count

  let oldestAge = 0;
  const now = Date.now();
  for (const entry of _cache.values()) {
    const age = (now - entry.createdAt) / 1000;
    if (age > oldestAge) oldestAge = age;
  }

  const totalRequests = _hits + _misses;
  return {
    totalEntries: _cache.size,
    maxEntries: _maxEntries,
    hitRate: totalRequests > 0 ? _hits / totalRequests : 0,
    missRate: totalRequests > 0 ? _misses / totalRequests : 0,
    totalHits: _hits,
    totalMisses: _misses,
    oldestEntryAge: Math.round(oldestAge),
  };
}

/**
 * Higher-order function: wrap an async function with caching.
 * First call executes and caches the result; subsequent calls return cached value.
 *
 * @example
 * const cachedTargets = cached("cdp_targets", 30, () => cdpListTargets());
 * const targets = await cachedTargets(); // cached for 30s
 */
export function cached<T>(
  key: string,
  ttlSeconds: number,
  fn: () => T | Promise<T>
): () => Promise<T> {
  return async () => {
    const existing = cacheGet<T>(key);
    if (existing !== undefined) return existing;

    const result = await fn();
    cacheSet(key, result, ttlSeconds);
    return result;
  };
}

/**
 * Functional cache wrapper with dynamic key generation.
 *
 * @example
 * const getUser = cachedFn(
 *   (id: string) => `user_${id}`,
 *   60,
 *   (id: string) => fetchUser(id)
 * );
 * const user = await getUser("abc123"); // cached for 60s by key "user_abc123"
 */
export function cachedFn<TArgs extends unknown[], TResult>(
  keyFn: (...args: TArgs) => string,
  ttlSeconds: number,
  fn: (...args: TArgs) => TResult | Promise<TResult>
): (...args: TArgs) => Promise<TResult> {
  return async (...args: TArgs) => {
    const key = keyFn(...args);
    const existing = cacheGet<TResult>(key);
    if (existing !== undefined) return existing;

    const result = await fn(...args);
    cacheSet(key, result, ttlSeconds);
    return result;
  };
}

// ── Internal ────────────────────────────────────────────────

function evictExpired(): void {
  const now = Date.now();
  for (const [key, entry] of _cache) {
    if (entry.expiresAt > 0 && now > entry.expiresAt) {
      _cache.delete(key);
    }
  }
}

function evictLRU(): void {
  let oldestKey: string | null = null;
  let oldestAccess = Infinity;

  for (const [key, entry] of _cache) {
    if (entry.lastAccessed < oldestAccess) {
      oldestAccess = entry.lastAccessed;
      oldestKey = key;
    }
  }

  if (oldestKey) {
    _cache.delete(oldestKey);
  }
}

/**
 * Stop the cache cleanup timer.
 */
export function stopCache(): void {
  if (_cleanupTimer) {
    clearInterval(_cleanupTimer);
    _cleanupTimer = null;
  }
}
