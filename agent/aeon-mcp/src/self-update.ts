/**
 * Self-Update Engine for Aeon MCP Server
 *
 * Inspired by opencrabs' self_update.rs pattern.
 * Allows the agent to rebuild itself from source after RSI modifications
 * or manual code changes.
 *
 * Safety controls:
 *   - max_restarts_per_hour (default: 3) — prevents infinite restart loops
 *   - Build verification before signaling restart
 *   - Dry-run mode for testing builds without triggering restart
 *   - Lineage tracking via AEON_EVOLVED_FROM env var
 *
 * On Windows (our target), we can't exec() to replace the process.
 * Instead, we:
 *   1. Run `npm run build` in the MCP source directory
 *   2. Write a restart signal file (~/.aeon/restart-signal)
 *   3. The host process (Aeon Browser or IDE) can watch this file
 *      and restart the MCP server when it appears
 *
 * @module self-update
 */

import { execSync } from "node:child_process";
import {
  existsSync,
  readFileSync,
  writeFileSync,
  mkdirSync,
  appendFileSync,
} from "node:fs";
import { join, dirname } from "node:path";
import { homedir } from "node:os";
import { fileURLToPath } from "node:url";

// ── Paths ───────────────────────────────────────────────────

const AEON_HOME = join(homedir(), ".aeon");
const RESTART_SIGNAL = join(AEON_HOME, "restart-signal");
const UPDATE_LOG = join(AEON_HOME, "update-history.jsonl");
const MAX_RESTARTS_PER_HOUR = 3;

// Find the project root (walk up from this file to find package.json)
function findProjectRoot(): string | null {
  const __filename = fileURLToPath(import.meta.url);
  let dir = dirname(__filename);

  // Walk up max 5 levels to find package.json
  for (let i = 0; i < 5; i++) {
    if (existsSync(join(dir, "package.json"))) {
      return dir;
    }
    const parent = dirname(dir);
    if (parent === dir) break; // Root reached
    dir = parent;
  }

  return null;
}

// ── Types ───────────────────────────────────────────────────

export interface SelfUpdateResult {
  success: boolean;
  buildDurationMs: number;
  stdout?: string;
  error?: string;
  projectRoot?: string;
  restartSignaled?: boolean;
  evolutionGeneration: number;
}

interface UpdateHistoryEntry {
  timestamp: string;
  success: boolean;
  buildDurationMs: number;
  generation: number;
  error?: string;
  triggeredBy: string;
}

// ── Rate Limiting ───────────────────────────────────────────

function getRecentUpdateCount(): number {
  if (!existsSync(UPDATE_LOG)) return 0;

  try {
    const lines = readFileSync(UPDATE_LOG, "utf-8")
      .trim()
      .split("\n")
      .filter(Boolean);

    const oneHourAgo = Date.now() - 3_600_000;
    let count = 0;

    for (const line of lines.slice(-20)) {
      // Only check recent entries
      try {
        const entry: UpdateHistoryEntry = JSON.parse(line);
        if (new Date(entry.timestamp).getTime() > oneHourAgo && entry.success) {
          count++;
        }
      } catch {
        // Skip malformed entries
      }
    }

    return count;
  } catch {
    return 0;
  }
}

function getEvolutionGeneration(): number {
  const envGen = process.env.AEON_EVOLVED_FROM;
  if (envGen) {
    const gen = parseInt(envGen, 10);
    if (!isNaN(gen)) return gen + 1;
  }

  // Count successful updates from log
  if (!existsSync(UPDATE_LOG)) return 1;

  try {
    const lines = readFileSync(UPDATE_LOG, "utf-8")
      .trim()
      .split("\n")
      .filter(Boolean);

    let maxGen = 0;
    for (const line of lines.slice(-50)) {
      try {
        const entry: UpdateHistoryEntry = JSON.parse(line);
        if (entry.generation > maxGen) maxGen = entry.generation;
      } catch {
        // Skip
      }
    }
    return maxGen + 1;
  } catch {
    return 1;
  }
}

function recordUpdateHistory(entry: UpdateHistoryEntry): void {
  try {
    if (!existsSync(AEON_HOME)) {
      mkdirSync(AEON_HOME, { recursive: true });
    }
    appendFileSync(UPDATE_LOG, JSON.stringify(entry) + "\n", "utf-8");
  } catch {
    // Non-critical
  }
}

// ── Main Self-Update Function ───────────────────────────────

export async function selfUpdate(options?: {
  dryRun?: boolean;
}): Promise<SelfUpdateResult> {
  const dryRun = options?.dryRun ?? false;
  const generation = getEvolutionGeneration();

  // Rate limiting — prevent infinite rebuild loops
  const recentCount = getRecentUpdateCount();
  if (recentCount >= MAX_RESTARTS_PER_HOUR) {
    return {
      success: false,
      buildDurationMs: 0,
      error: `Rate limited: ${recentCount} updates in the last hour (max: ${MAX_RESTARTS_PER_HOUR}). Wait before updating again.`,
      evolutionGeneration: generation,
    };
  }

  // Find project root
  const projectRoot = findProjectRoot();
  if (!projectRoot) {
    return {
      success: false,
      buildDurationMs: 0,
      error: "Could not find project root (no package.json found walking up from source).",
      evolutionGeneration: generation,
    };
  }

  process.stderr.write(
    `[self-update] Generation ${generation} | Project: ${projectRoot} | Mode: ${dryRun ? "dry-run" : "full"}\\n`
  );

  // Execute build
  const startTime = Date.now();
  let stdout = "";
  let success = false;
  let error: string | undefined;

  try {
    stdout = execSync("npm run build", {
      cwd: projectRoot,
      encoding: "utf-8",
      timeout: 60_000, // 60 second timeout
      env: {
        ...process.env,
        AEON_EVOLVED_FROM: String(generation),
      },
    });

    success = true;
    process.stderr.write(`[self-update] Build succeeded in ${Date.now() - startTime}ms\\n`);
  } catch (err: any) {
    success = false;
    error = err.stderr || err.message || "Unknown build error";
    stdout = err.stdout || "";
    process.stderr.write(`[self-update] Build FAILED: ${error?.slice(0, 200)}\\n`);
  }

  const buildDurationMs = Date.now() - startTime;

  // Record history
  recordUpdateHistory({
    timestamp: new Date().toISOString(),
    success,
    buildDurationMs,
    generation,
    error,
    triggeredBy: dryRun ? "dry-run" : "self-update",
  });

  // Signal restart (only if build succeeded and not dry run)
  let restartSignaled = false;
  if (success && !dryRun) {
    try {
      if (!existsSync(AEON_HOME)) {
        mkdirSync(AEON_HOME, { recursive: true });
      }

      writeFileSync(
        RESTART_SIGNAL,
        JSON.stringify({
          timestamp: new Date().toISOString(),
          generation,
          buildDurationMs,
          projectRoot,
        }),
        "utf-8"
      );

      restartSignaled = true;
      process.stderr.write(`[self-update] Restart signal written to ${RESTART_SIGNAL}\\n`);
    } catch (sigErr: any) {
      process.stderr.write(`[self-update] Failed to write restart signal: ${sigErr.message}\\n`);
    }
  }

  return {
    success,
    buildDurationMs,
    stdout: stdout.slice(-2000), // Last 2KB of output
    error,
    projectRoot,
    restartSignaled,
    evolutionGeneration: generation,
  };
}

/**
 * Check if a restart signal is pending.
 */
export function hasPendingRestart(): boolean {
  return existsSync(RESTART_SIGNAL);
}

/**
 * Clear the restart signal (call after restart is handled).
 */
export function clearRestartSignal(): void {
  try {
    if (existsSync(RESTART_SIGNAL)) {
      const { unlinkSync } = require("node:fs");
      unlinkSync(RESTART_SIGNAL);
    }
  } catch {
    // Non-critical
  }
}

/**
 * Get self-update history (last N entries).
 */
export function updateHistory(limit: number = 10): UpdateHistoryEntry[] {
  if (!existsSync(UPDATE_LOG)) return [];

  try {
    const lines = readFileSync(UPDATE_LOG, "utf-8")
      .trim()
      .split("\n")
      .filter(Boolean);

    return lines
      .slice(-limit)
      .map((line) => {
        try {
          return JSON.parse(line) as UpdateHistoryEntry;
        } catch {
          return null;
        }
      })
      .filter((e): e is UpdateHistoryEntry => e !== null)
      .reverse();
  } catch {
    return [];
  }
}
