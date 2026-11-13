/**
 * RSI Engine — Recursive Self-Improvement Background Loop
 *
 * Runs as a background timer after MCP server startup:
 *   1. Writes a digest of feedback stats to ~/.aeon/rsi/digest.md
 *   2. Every RSI_CYCLE_MS, analyzes feedback for actionable patterns
 *   3. When opportunities are found, uses the embedded LLM to analyze
 *      and apply targeted improvements to brain files
 *   4. Logs every improvement to rsi/improvements.md and daily archives
 *
 * The RSI engine is the meta-loop on top of Aeon's Plan→Act→Observe→Validate:
 *   "After enough task executions, analyze what went wrong and rewrite
 *    the system prompt to prevent it from happening again."
 *
 * Architecture modeled after OpenCrabs' rsi.rs, adapted for TypeScript/JSONL.
 *
 * @module rsi-engine
 */

import { writeFileSync, existsSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { z } from "zod";
import {
  totalCount,
  detectOpportunities,
  formatDigest,
  toolStats,
  entriesByType,
  RSI_DIR_PATH,
  type FeedbackEventType,
} from "./feedback-ledger.js";
import {
  readBrainFile,
  appendToBrainFile,
  updateBrainFile,
  listImprovements,
  ALLOWED_BRAIN_FILES,
  type BrainFile,
  ensureBrainDir,
} from "./brain.js";
import { llmChat, llmHealthCheck, type ChatMessage } from "./llm-client.js";

// ── Configuration ───────────────────────────────────────────

/** Interval between RSI cycles (ms). Default: 1 hour */
const RSI_CYCLE_MS = parseInt(process.env.AEON_RSI_INTERVAL_MS ?? "3600000", 10);

/** Minimum feedback entries before RSI attempts improvements */
const RSI_MIN_ENTRIES = parseInt(process.env.AEON_RSI_MIN_ENTRIES ?? "25", 10);

/** Maximum LLM tool iterations per RSI cycle */
const RSI_MAX_ITERATIONS = 8;

/** Path to last cycle timestamp file */
const LAST_CYCLE_PATH = join(RSI_DIR_PATH, "last_cycle");

// ── State ───────────────────────────────────────────────────

let _rsiTimer: ReturnType<typeof setInterval> | null = null;
let _lastSeenCount = 0;
let _cycleCount = 0;
let _lastCycleResult: string | null = null;
let _rsiRunning = false;

// ── RSI System Prompt ───────────────────────────────────────

const RSI_SYSTEM_PROMPT = `You are the RSI (Recursive Self-Improvement) engine for Aeon Browser.
Your job is to analyze system feedback and apply targeted improvements to brain files.

## Brain File Taxonomy

Each brain file controls a different aspect of the agent. Route improvements to the RIGHT file:

- **SOUL.md** — How the model BEHAVES: response style, reasoning patterns.
  Fix here when: the agent narrates instead of acting, gives verbose responses,
  or fails to chain actions properly.
- **TOOLS.md** — How TOOLS are used: argument formats, common pitfalls, usage patterns.
  Fix here when: the same tool fails repeatedly with similar args.
- **USER.md** — How to interact with THIS USER: preferences, corrections.
  Fix here when: user corrections show a repeated preference the agent violates.
- **MEMORY.md** — Persistent KNOWLEDGE: facts, context, project state.
  Fix here when: the agent repeatedly lacks context it should have retained.
- **CODE.md** — Coding STANDARDS: TypeScript conventions, error handling patterns.
  Fix here when: generated code repeatedly violates style or quality rules.
- **SECURITY.md** — Security POLICIES: boundaries, data handling, access limits.
  Fix here when: the agent attempts unauthorized actions or violates sandboxing.
- **AGENTS.md** — Agent CONFIGURATION: workspace rules, approval flows, session management.
  Fix here when: agent behavior patterns need reconfiguration (parallelism, queuing, etc).

## Rules

1. Be conservative — only apply improvements when you have CLEAR evidence.
2. Quality over quantity — one well-reasoned improvement beats many speculative ones.
3. Never duplicate an existing instruction in a brain file.
4. Focus on the highest-impact issues first (highest failure rate, most frequent).
5. Each improvement must include:
   - target_file: which brain file to modify
   - action: "append" (new instruction) or "update" (refine existing)
   - content: the actual text to add or replace
   - old_content: (for "update" only) the exact text being replaced
   - description: what this improvement does
   - rationale: why, referencing specific feedback data

## Response Format

Respond with a JSON object:
{
  "analysis": "Brief analysis of the feedback patterns",
  "improvements": [
    {
      "target_file": "TOOLS.md",
      "action": "append",
      "content": "## New instruction text here",
      "description": "Add guidance for aeon_page_click failures",
      "rationale": "Tool has 35% failure rate due to stale refs"
    }
  ],
  "skipped_reason": "Why no improvements were needed (if improvements is empty)"
}`;

// ── Zod Schemas (replaces fragile regex JSON parsing) ───────

const RSIImprovementSchema = z.object({
  target_file: z.string(),
  action: z.enum(["append", "update"]),
  content: z.string(),
  old_content: z.string().optional(),
  description: z.string(),
  rationale: z.string(),
});

const RSIResponseSchema = z.object({
  analysis: z.string().optional(),
  improvements: z.array(RSIImprovementSchema).optional().default([]),
  skipped_reason: z.string().optional(),
});

type RSIImprovement = z.infer<typeof RSIImprovementSchema>;
type RSIResponse = z.infer<typeof RSIResponseSchema>;

// ── RSI Routing Matrix ──────────────────────────────────────
// Maps feedback event categories to their primary brain file target.
// When the RSI engine detects a pattern, it uses this matrix to suggest
// which brain file to route the improvement to.

const RSI_ROUTING_MATRIX: Record<string, BrainFile> = {
  // Tool execution patterns
  tool_success: "TOOLS.md",
  tool_failure: "TOOLS.md",

  // Agent behavior patterns
  phantom_tool_call: "SOUL.md",
  replan: "SOUL.md",

  // User interaction patterns
  user_correction: "USER.md",

  // Knowledge gaps
  task_complete: "MEMORY.md",
  task_failed: "MEMORY.md",

  // Infrastructure errors
  llm_error: "AGENTS.md",
  provider_error: "AGENTS.md",
  context_compaction: "AGENTS.md",

  // Self-improvement
  improvement_applied: "MEMORY.md",
};

function getRoutingSuggestions(opportunities: string[]): string {
  const suggestions: string[] = [];
  for (const opp of opportunities) {
    for (const [eventType, brainFile] of Object.entries(RSI_ROUTING_MATRIX)) {
      if (opp.toLowerCase().includes(eventType.replace(/_/g, " ")) ||
          opp.toLowerCase().includes(eventType.replace(/_/g, "_"))) {
        suggestions.push(`- Pattern "${eventType}" → route to **${brainFile}**`);
        break;
      }
    }
  }
  return suggestions.length > 0
    ? `### Routing Suggestions\n${suggestions.join("\n")}`
    : "";
}

/**
 * Run a single RSI analysis + improvement cycle.
 * Uses the embedded LLM to analyze feedback and generate improvements.
 */
async function runRsiCycle(opportunities: string[]): Promise<string> {
  // Check LLM availability
  const health = await llmHealthCheck();
  if (!health.available) {
    return `RSI cycle skipped — LLM not available: ${health.error}`;
  }

  // Build context: current brain file contents + feedback data
  const brainContext: string[] = [];
  for (const file of ALLOWED_BRAIN_FILES) {
    const content = readBrainFile(file);
    if (content) {
      brainContext.push(`### ${file} (current content)\n\`\`\`\n${content.slice(0, 1500)}\n\`\`\``);
    }
  }

  // Tool stats
  const stats = toolStats();
  const statsText = stats.length > 0
    ? stats.map(s => `- ${s.dimension}: ${s.totalEvents} calls, ${(s.successRate * 100).toFixed(0)}% success (${s.failures} failures)`).join("\n")
    : "No tool execution data yet.";

  // Recent failures
  const failures = entriesByType("tool_failure", 10);
  const failuresText = failures.length > 0
    ? failures.map(f => `- [${f.timestamp}] ${f.dimension}: ${f.metadata ?? "(no details)"}`).join("\n")
    : "No recent failures.";

  // Previous improvements
  const prevImprovements = listImprovements();

  const userPrompt = `## Feedback Analysis Request

### Detected Opportunities
${opportunities.map(o => `- ${o}`).join("\n")}

${getRoutingSuggestions(opportunities)}

### Tool Statistics
${statsText}

### Recent Failures
${failuresText}

### Current Brain Files
${brainContext.join("\n\n")}

### Previously Applied Improvements
${prevImprovements.slice(0, 1000)}

Analyze the feedback data and respond with targeted improvements. Only apply improvements that are clearly justified by the data.`;

  const messages: ChatMessage[] = [
    { role: "system", content: RSI_SYSTEM_PROMPT },
    { role: "user", content: userPrompt },
  ];

  const response = await llmChat(messages);

  // Parse the LLM response using Zod (replaces fragile regex)
  let parsed: RSIResponse;
  try {
    // Extract JSON from response (handle markdown code blocks)
    const jsonMatch = response.content.match(/\{[\s\S]*\}/);
    if (!jsonMatch) {
      return `RSI cycle: LLM did not return valid JSON. Response: ${response.content.slice(0, 200)}`;
    }
    const rawJson = JSON.parse(jsonMatch[0]);
    parsed = RSIResponseSchema.parse(rawJson);
  } catch (err) {
    if (err instanceof z.ZodError) {
      const issues = err.issues.map(i => `${i.path.join(".")}: ${i.message}`).join("; ");
      return `RSI cycle: LLM response failed schema validation: ${issues}`;
    }
    return `RSI cycle: Failed to parse LLM response as JSON: ${(err as Error).message}`;
  }

  const results: string[] = [];

  if (parsed.analysis) {
    results.push(`Analysis: ${parsed.analysis}`);
  }

  if (!parsed.improvements || parsed.improvements.length === 0) {
    results.push(`No improvements applied: ${parsed.skipped_reason ?? "data insufficient"}`);
    return results.join("\n");
  }

  // Apply each improvement
  for (const imp of parsed.improvements.slice(0, RSI_MAX_ITERATIONS)) {
    const targetFile = imp.target_file as BrainFile;

    if (!ALLOWED_BRAIN_FILES.includes(targetFile)) {
      results.push(`⚠ Skipped: invalid target file '${imp.target_file}'`);
      continue;
    }

    if (imp.action === "append") {
      const result = appendToBrainFile(
        targetFile,
        imp.content,
        imp.description,
        imp.rationale
      );
      if (result.success) {
        results.push(`✅ Applied to ${targetFile}: ${imp.description}`);
      } else {
        results.push(`❌ Failed to apply to ${targetFile}: ${result.error}`);
      }
    } else if (imp.action === "update" && imp.old_content) {
      const result = updateBrainFile(
        targetFile,
        imp.old_content,
        imp.content,
        imp.description,
        imp.rationale
      );
      if (result.success) {
        results.push(`✅ Updated ${targetFile}: ${imp.description}`);
      } else {
        results.push(`❌ Failed to update ${targetFile}: ${result.error}`);
      }
    } else {
      results.push(`⚠ Skipped: action '${imp.action}' requires old_content for update`);
    }
  }

  return results.join("\n");
}

// ── Background Engine ───────────────────────────────────────

/**
 * Run a single tick of the RSI engine.
 * Called by the background timer or manually via the MCP tool.
 */
export async function rsiTick(): Promise<string> {
  if (_rsiRunning) {
    return "RSI cycle already in progress.";
  }

  _rsiRunning = true;

  try {
    const total = totalCount();

    if (total < RSI_MIN_ENTRIES) {
      _lastCycleResult = `Skipped — only ${total} entries (need ${RSI_MIN_ENTRIES})`;
      return _lastCycleResult;
    }

    // Skip if no new feedback since last cycle
    if (total === _lastSeenCount) {
      _lastCycleResult = `Skipped — feedback count unchanged (${total})`;
      stampLastCycle();
      return _lastCycleResult;
    }

    _lastSeenCount = total;
    _cycleCount++;

    process.stderr.write(`[RSI] Cycle #${_cycleCount}: analyzing ${total} feedback entries\n`);

    // Write digest
    try {
      const digest = formatDigest();
      const digestPath = join(RSI_DIR_PATH, "digest.md");
      writeFileSync(digestPath, digest, "utf-8");
    } catch {
      // Non-critical
    }

    // Detect opportunities
    const opportunities = detectOpportunities();

    if (opportunities.length === 0) {
      _lastCycleResult = `Cycle #${_cycleCount}: No actionable patterns found in ${total} entries.`;
      stampLastCycle();
      return _lastCycleResult;
    }

    process.stderr.write(`[RSI] ${opportunities.length} opportunities found, running analysis...\n`);

    // Run the LLM-powered improvement cycle
    const result = await runRsiCycle(opportunities);

    _lastCycleResult = `Cycle #${_cycleCount}: ${result}`;
    stampLastCycle();

    process.stderr.write(`[RSI] Cycle complete: ${result.slice(0, 200)}\n`);

    return _lastCycleResult;
  } catch (err: any) {
    _lastCycleResult = `Cycle #${_cycleCount} failed: ${err.message}`;
    process.stderr.write(`[RSI] Error: ${err.message}\n`);
    return _lastCycleResult;
  } finally {
    _rsiRunning = false;
  }
}

function stampLastCycle(): void {
  try {
    writeFileSync(LAST_CYCLE_PATH, new Date().toISOString(), "utf-8");
  } catch {
    // Non-critical
  }
}

// ── Engine Lifecycle ────────────────────────────────────────

/**
 * Start the background RSI engine.
 * Called once during MCP server initialization.
 */
export function startRsiEngine(): void {
  // Initialize brain files on first boot
  ensureBrainDir();

  // Calculate initial delay: if we restarted recently, don't wait the full interval
  let initialDelayMs = RSI_CYCLE_MS;
  if (existsSync(LAST_CYCLE_PATH)) {
    try {
      const lastStr = readFileSync(LAST_CYCLE_PATH, "utf-8").trim();
      const lastTime = new Date(lastStr).getTime();
      const elapsed = Date.now() - lastTime;
      if (elapsed >= RSI_CYCLE_MS) {
        initialDelayMs = 30_000; // Overdue — run soon
      } else {
        initialDelayMs = RSI_CYCLE_MS - elapsed;
      }
    } catch {
      // Fall through to full interval
    }
  }

  process.stderr.write(
    `[RSI] Engine started. First cycle in ${Math.round(initialDelayMs / 60_000)}m. ` +
    `Interval: ${Math.round(RSI_CYCLE_MS / 60_000)}m. Min entries: ${RSI_MIN_ENTRIES}\n`
  );

  // Initial delayed cycle
  setTimeout(() => {
    rsiTick().catch(() => {});

    // Subsequent cycles on interval
    _rsiTimer = setInterval(() => {
      rsiTick().catch(() => {});
    }, RSI_CYCLE_MS);
  }, initialDelayMs);
}

/**
 * Stop the background RSI engine.
 */
export function stopRsiEngine(): void {
  if (_rsiTimer) {
    clearInterval(_rsiTimer);
    _rsiTimer = null;
  }
}

/**
 * Get current RSI engine status.
 */
export function rsiStatus(): {
  running: boolean;
  cycleCount: number;
  lastResult: string | null;
  totalFeedback: number;
  minEntries: number;
  intervalMinutes: number;
  opportunities: string[];
} {
  return {
    running: _rsiTimer !== null,
    cycleCount: _cycleCount,
    lastResult: _lastCycleResult,
    totalFeedback: totalCount(),
    minEntries: RSI_MIN_ENTRIES,
    intervalMinutes: Math.round(RSI_CYCLE_MS / 60_000),
    opportunities: detectOpportunities(),
  };
}
