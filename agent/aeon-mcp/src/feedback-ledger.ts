/**
 * Feedback Ledger — Event Log for Recursive Self-Improvement
 *
 * Records every tool execution, failure, replan event, and user correction
 * to a local JSON-line log file. This data feeds the RSI engine so it can
 * identify patterns and autonomously improve brain files.
 *
 * Storage: ~/.aeon/rsi/feedback_ledger.jsonl (append-only)
 * Format: One JSON object per line (JSONL) for fast append + streaming reads.
 *
 * Inspired by OpenCrabs' feedback_ledger SQLite pattern, adapted for Aeon's
 * zero-dependency TypeScript architecture (no native SQLite bindings needed).
 *
 * @module feedback-ledger
 */

import { existsSync, mkdirSync, appendFileSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { homedir } from "node:os";

// ── Types ───────────────────────────────────────────────────

export type FeedbackEventType =
  | "tool_success"       // Tool executed successfully
  | "tool_failure"       // Tool threw an error
  | "replan"             // Planner had to replan after step failure
  | "task_complete"      // Task finished successfully
  | "task_failed"        // Task exhausted replans / fatal error
  | "user_correction"    // User gave negative feedback / correction
  | "llm_error"          // LLM inference failed
  | "improvement_applied" // RSI engine applied a brain file improvement
  | "phantom_tool_call"  // Model narrated instead of executing tools
  | "context_compaction" // Context exceeded budget, had to be compacted
  | "provider_error";    // LLM provider returned an error (distinct from llm_error)

export interface FeedbackEntry {
  timestamp: string;      // ISO 8601
  eventType: FeedbackEventType;
  dimension: string;      // Tool name, task category, or general dimension
  success: boolean;
  sessionId: string;      // Task ID or session identifier
  metadata?: string;      // Error messages, user feedback text, etc.
}

export interface ToolStats {
  dimension: string;
  totalEvents: number;
  successes: number;
  failures: number;
  successRate: number;
}

export interface LedgerSummary {
  totalEntries: number;
  eventBreakdown: Record<string, number>;
  toolStats: ToolStats[];
  recentFailures: FeedbackEntry[];
  recentCorrections: FeedbackEntry[];
  recentImprovements: FeedbackEntry[];
}

// ── Ledger Path ─────────────────────────────────────────────

const AEON_HOME = join(homedir(), ".aeon");
const RSI_DIR = join(AEON_HOME, "rsi");
const HISTORY_DIR = join(RSI_DIR, "history");
const LEDGER_PATH = join(RSI_DIR, "feedback_ledger.jsonl");

/** Ensure ~/.aeon/rsi/history/ exists */
function ensureDirs(): void {
  if (!existsSync(HISTORY_DIR)) {
    mkdirSync(HISTORY_DIR, { recursive: true });
  }
}

// ── Write Operations ────────────────────────────────────────

/**
 * Record a feedback event to the ledger.
 */
export function recordFeedback(
  eventType: FeedbackEventType,
  dimension: string,
  success: boolean,
  sessionId: string,
  metadata?: string
): void {
  try {
    ensureDirs();

    const entry: FeedbackEntry = {
      timestamp: new Date().toISOString(),
      eventType,
      dimension,
      success,
      sessionId,
      metadata: metadata ? metadata.slice(0, 2000) : undefined, // cap metadata size
    };

    appendFileSync(LEDGER_PATH, JSON.stringify(entry) + "\n", "utf-8");
  } catch (err) {
    // Silent fail — ledger is non-critical. Never crash the agent over logging.
    process.stderr.write(`[feedback-ledger] write error: ${err}\n`);
  }
}

// ── Convenience Recorders ───────────────────────────────────

/** Record a successful tool execution */
export function recordToolSuccess(toolName: string, sessionId: string): void {
  recordFeedback("tool_success", toolName, true, sessionId);
}

/** Record a failed tool execution */
export function recordToolFailure(
  toolName: string,
  sessionId: string,
  errorMessage: string
): void {
  recordFeedback("tool_failure", toolName, false, sessionId, errorMessage);
}

/** Record a planner replan event */
export function recordReplan(
  sessionId: string,
  failedAction: string,
  errorMessage: string
): void {
  recordFeedback(
    "replan",
    failedAction,
    false,
    sessionId,
    `Replan triggered: ${errorMessage}`
  );
}

/** Record task completion */
export function recordTaskComplete(sessionId: string, stepsExecuted: number): void {
  recordFeedback(
    "task_complete",
    "planner",
    true,
    sessionId,
    `Completed in ${stepsExecuted} steps`
  );
}

/** Record task failure */
export function recordTaskFailed(
  sessionId: string,
  reason: string
): void {
  recordFeedback("task_failed", "planner", false, sessionId, reason);
}

/** Record an LLM inference error */
export function recordLlmError(sessionId: string, errorMessage: string): void {
  recordFeedback("llm_error", "llm", false, sessionId, errorMessage);
}

/** Record an RSI improvement application */
export function recordImprovement(
  targetFile: string,
  description: string
): void {
  recordFeedback(
    "improvement_applied",
    targetFile,
    true,
    "rsi_engine",
    description
  );
}

/** Record a phantom tool call (model narrated instead of acting) */
export function recordPhantomToolCall(
  sessionId: string,
  details: string
): void {
  recordFeedback(
    "phantom_tool_call",
    "agent_behavior",
    false,
    sessionId,
    details
  );
}

/** Record a context compaction event */
export function recordContextCompaction(
  sessionId: string,
  beforeTokens: number,
  afterTokens: number
): void {
  recordFeedback(
    "context_compaction",
    "context_management",
    true,
    sessionId,
    `Compacted from ${beforeTokens} to ${afterTokens} tokens`
  );
}

/** Record a provider error (LLM provider returned an HTTP/API error) */
export function recordProviderError(
  sessionId: string,
  provider: string,
  errorMessage: string
): void {
  recordFeedback(
    "provider_error",
    provider,
    false,
    sessionId,
    errorMessage
  );
}

// ── Read Operations ─────────────────────────────────────────

/**
 * Read all entries from the ledger.
 * Returns empty array if ledger doesn't exist yet.
 */
export function readAllEntries(): FeedbackEntry[] {
  try {
    if (!existsSync(LEDGER_PATH)) return [];

    const raw = readFileSync(LEDGER_PATH, "utf-8");
    const lines = raw.split("\n").filter((l) => l.trim().length > 0);

    return lines
      .map((line) => {
        try {
          return JSON.parse(line) as FeedbackEntry;
        } catch {
          return null;
        }
      })
      .filter((e): e is FeedbackEntry => e !== null);
  } catch {
    return [];
  }
}

/**
 * Get total entry count without loading all entries into memory.
 */
export function totalCount(): number {
  try {
    if (!existsSync(LEDGER_PATH)) return 0;
    const raw = readFileSync(LEDGER_PATH, "utf-8");
    return raw.split("\n").filter((l) => l.trim().length > 0).length;
  } catch {
    return 0;
  }
}

/**
 * Get entries filtered by event type, most recent first.
 */
export function entriesByType(
  eventType: FeedbackEventType,
  limit = 10
): FeedbackEntry[] {
  return readAllEntries()
    .filter((e) => e.eventType === eventType)
    .reverse()
    .slice(0, limit);
}

/**
 * Compute per-tool success/failure statistics.
 */
export function toolStats(): ToolStats[] {
  const entries = readAllEntries().filter(
    (e) => e.eventType === "tool_success" || e.eventType === "tool_failure"
  );

  const byTool = new Map<string, { success: number; fail: number }>();
  for (const e of entries) {
    const existing = byTool.get(e.dimension) ?? { success: 0, fail: 0 };
    if (e.success) existing.success++;
    else existing.fail++;
    byTool.set(e.dimension, existing);
  }

  return Array.from(byTool.entries())
    .map(([dim, stats]) => ({
      dimension: dim,
      totalEvents: stats.success + stats.fail,
      successes: stats.success,
      failures: stats.fail,
      successRate:
        stats.success + stats.fail > 0
          ? stats.success / (stats.success + stats.fail)
          : 1.0,
    }))
    .sort((a, b) => a.successRate - b.successRate); // worst tools first
}

/**
 * Event type breakdown — count per event type.
 */
export function eventBreakdown(): Record<string, number> {
  const entries = readAllEntries();
  const counts: Record<string, number> = {};
  for (const e of entries) {
    counts[e.eventType] = (counts[e.eventType] ?? 0) + 1;
  }
  return counts;
}

/**
 * Generate a full summary of the feedback ledger.
 */
export function generateSummary(): LedgerSummary {
  const total = totalCount();
  const breakdown = eventBreakdown();
  const stats = toolStats();
  const failures = entriesByType("tool_failure", 10);
  const corrections = entriesByType("user_correction", 10);
  const improvements = entriesByType("improvement_applied", 10);

  return {
    totalEntries: total,
    eventBreakdown: breakdown,
    toolStats: stats,
    recentFailures: failures,
    recentCorrections: corrections,
    recentImprovements: improvements,
  };
}

/**
 * Identify actionable improvement opportunities.
 * Returns descriptions of patterns worth investigating.
 */
export function detectOpportunities(): string[] {
  const opportunities: string[] = [];
  const stats = toolStats();

  // Tools with >20% failure rate and >=5 total executions
  for (const s of stats) {
    if (s.totalEvents >= 5 && s.successRate < 0.8) {
      const failRate = ((1 - s.successRate) * 100).toFixed(0);
      const recentFails = entriesByType("tool_failure", 10)
        .filter((e) => e.dimension === s.dimension)
        .slice(0, 3);

      let detail = `Tool '${s.dimension}' has ${failRate}% failure rate (${s.failures} failures out of ${s.totalEvents}).`;
      if (recentFails.length > 0) {
        detail += "\n  Recent failures:";
        for (const f of recentFails) {
          detail += `\n  - ${f.timestamp}: ${f.metadata ?? "(no details)"}`;
        }
      }
      opportunities.push(detail);
    }
  }

  // Repeated user corrections
  const corrections = entriesByType("user_correction", 50);
  if (corrections.length >= 3) {
    let desc = `${corrections.length} user corrections recorded.`;
    desc += "\n  Recent corrections:";
    for (const c of corrections.slice(0, 5)) {
      desc += `\n  - ${c.timestamp} [${c.dimension}]: ${c.metadata ?? "(no details)"}`;
    }
    opportunities.push(desc);
  }

  // LLM errors
  const llmErrors = entriesByType("llm_error", 20);
  if (llmErrors.length >= 3) {
    let desc = `${llmErrors.length} LLM errors recorded.`;
    desc += "\n  Recent errors:";
    for (const e of llmErrors.slice(0, 5)) {
      desc += `\n  - ${e.timestamp}: ${e.metadata ?? "(no details)"}`;
    }
    opportunities.push(desc);
  }

  return opportunities;
}

/**
 * Format the ledger summary as a Markdown digest.
 */
export function formatDigest(): string {
  const summary = generateSummary();

  let out = `# Aeon RSI Digest\n\n`;
  out += `**Generated:** ${new Date().toISOString()}\n`;
  out += `**Total events:** ${summary.totalEntries}\n\n`;

  // Event breakdown
  if (Object.keys(summary.eventBreakdown).length > 0) {
    out += `## Event Breakdown\n\n`;
    for (const [type, count] of Object.entries(summary.eventBreakdown)) {
      const pct = ((count / summary.totalEntries) * 100).toFixed(1);
      out += `- **${type}**: ${count} (${pct}%)\n`;
    }
    out += "\n";
  }

  // Tool performance
  const failing = summary.toolStats.filter((s) => s.failures > 0);
  if (failing.length > 0) {
    out += `## Tool Performance\n\n`;
    out += `| Tool | Total | OK | Fail | Rate |\n`;
    out += `|------|------:|---:|-----:|-----:|\n`;
    for (const s of failing) {
      out += `| ${s.dimension} | ${s.totalEvents} | ${s.successes} | ${s.failures} | ${(s.successRate * 100).toFixed(0)}% |\n`;
    }
    out += "\n";
  }

  // Recent failures
  if (summary.recentFailures.length > 0) {
    out += `## Recent Failures\n\n`;
    for (const f of summary.recentFailures) {
      const short = (f.metadata ?? "(no details)").slice(0, 120);
      out += `- \`${f.timestamp}\` — ${f.dimension} — ${short}\n`;
    }
    out += "\n";
  }

  // Improvements
  if (summary.recentImprovements.length > 0) {
    out += `## Applied Improvements\n\n`;
    for (const imp of summary.recentImprovements) {
      out += `- \`${imp.timestamp}\` — ${imp.dimension} — ${imp.metadata ?? ""}\n`;
    }
    out += "\n";
  }

  return out;
}

/** Export the ledger path for external use */
export const LEDGER_FILE_PATH = LEDGER_PATH;
export const AEON_HOME_DIR = AEON_HOME;
export const RSI_DIR_PATH = RSI_DIR;
