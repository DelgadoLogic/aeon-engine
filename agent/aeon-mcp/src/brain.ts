/**
 * Brain File Manager — Self-Modifying Agent Identity
 *
 * Manages Aeon's "brain files" — Markdown documents in ~/.aeon/brain/
 * that shape how the AI agent behaves. The RSI engine writes improvements
 * to these files based on feedback analysis, creating a recursive
 * self-improvement loop.
 *
 * Brain File Taxonomy:
 *   SOUL.md     — How the model BEHAVES (response style, reasoning patterns)
 *   TOOLS.md    — How TOOLS are used (arg formats, common pitfalls)
 *   USER.md     — User preferences (corrections, frustrations)
 *   MEMORY.md   — Persistent knowledge (facts, project state)
 *
 * @module brain
 */

import { existsSync, mkdirSync, readFileSync, writeFileSync, appendFileSync } from "node:fs";
import { join } from "node:path";
import { AEON_HOME_DIR, RSI_DIR_PATH, recordImprovement } from "./feedback-ledger.js";

// ── Constants ───────────────────────────────────────────────

const BRAIN_DIR = join(AEON_HOME_DIR, "brain");

/** Brain files the RSI engine is allowed to read/modify */
export const ALLOWED_BRAIN_FILES = [
  "SOUL.md",
  "TOOLS.md",
  "USER.md",
  "MEMORY.md",
  "CODE.md",
  "SECURITY.md",
  "AGENTS.md",
] as const;

export type BrainFile = (typeof ALLOWED_BRAIN_FILES)[number];

// ── Default Brain Files ─────────────────────────────────────

const DEFAULT_SOUL = `# Aeon Soul — Agent Behavioral Identity

You are Aeon, the sovereign AI agent embedded in the Aeon Browser.
You think locally, act autonomously, and improve yourself.

## Core Directives

- **Execute, don't narrate.** When you need to take action, use tools directly.
  Never describe what you "would do" — do it.
- **Be concise.** Respect the user's time. Short, direct responses win.
- **Data grounding.** Only report information you observe on the page.
  Never fabricate data from training knowledge.
- **Self-heal.** If a step fails, analyze why and adapt. Don't repeat
  the same failed approach.
- **Chain actions.** Don't stop after one tool call when the goal requires
  multiple steps. Plan ahead, execute sequentially.
`;

const DEFAULT_TOOLS = `# Aeon Tools — Usage Patterns & Pitfalls

## Snapshot+Refs

- ALWAYS take a snapshot before clicking, typing, or selecting.
- Ref numbers change between snapshots — never reuse old refs.
- After navigation or page changes, wait for load then re-snapshot.

## Common Pitfalls

- \`aeon_page_click\`: If the element is behind a modal/overlay, close it first.
- \`aeon_page_type\`: Always set clearFirst=true to avoid appending to existing text.
- \`aeon_page_select_option\`: For custom dropdowns (not native <select>), click to
  expand first, snapshot, then click the option by ref.
- \`aeon_navigate\`: Always follow with aeon_page_wait(condition='load').
`;

const DEFAULT_USER = `# Aeon User Profile

## Preferences

- Role: Sovereign Aeon Browser User
- Timezone: Local System Time
- Style: Direct, no fluff, production-grade output

## Corrections History

(RSI engine will append learned preferences here)
`;

const DEFAULT_MEMORY = `# Aeon Persistent Memory

## System Facts

- Engine: Aeon Browser (C++ core, WebView2, Windows)
- Agent: aeon-mcp (TypeScript, node-llama-cpp embedded inference)
- Architecture: Sovereign — no external APIs required

## Session Knowledge

(RSI engine and agent will append persistent facts here)
`;

const DEFAULT_CODE = `# Aeon Code Standards

## General Principles

- Write concise, production-quality code. No boilerplate.
- Always include error handling — never let exceptions pass silently.
- Prefer const over let. Avoid any unless truly needed.
- Use descriptive variable names, not single letters.

## TypeScript Conventions

- Use strict mode. No implicit any.
- Interface over type for object shapes.
- Export functions, not classes, for tool modules.

## Code Quality Patterns

(RSI engine will append learned coding patterns here)
`;

const DEFAULT_SECURITY = `# Aeon Security Policies

## Core Principles

- All data stays local. No external telemetry or data exfiltration.
- Brain files are owned by the user. RSI writes are logged and auditable.
- Never execute arbitrary code from untrusted sources.
- File system access is limited to ~/.aeon/ and explicit user directories.

## Boundaries

- No unauthorized network requests beyond configured LLM endpoints.
- Scheduled tasks execute within the agent sandbox.
- RSI improvements are capped at 8 per cycle.

## Security Events

(RSI engine will append security-related learnings here)
`;

const DEFAULT_AGENTS = `# Aeon Agent Configuration

## Workspace Rules

- One active session at a time. No parallel conflicting tasks.
- Scheduled tasks queue if the agent is busy with a user task.
- Brain context is prepended to every LLM prompt.

## Approval Flow

- Tool calls are auto-approved during planner execution.
- RSI improvements are auto-applied if confidence >= threshold.
- Self-update requires explicit user trigger (safety valve).

## Agent Behavior

(RSI engine will append agent-level configuration learnings here)
`;

const DEFAULTS: Record<BrainFile, string> = {
  "SOUL.md": DEFAULT_SOUL,
  "TOOLS.md": DEFAULT_TOOLS,
  "USER.md": DEFAULT_USER,
  "MEMORY.md": DEFAULT_MEMORY,
  "CODE.md": DEFAULT_CODE,
  "SECURITY.md": DEFAULT_SECURITY,
  "AGENTS.md": DEFAULT_AGENTS,
};

// ── Directory Management ────────────────────────────────────

/** Ensure ~/.aeon/brain/ exists and initialize default brain files */
export function ensureBrainDir(): void {
  if (!existsSync(BRAIN_DIR)) {
    mkdirSync(BRAIN_DIR, { recursive: true });
  }

  // Create default brain files only if they don't exist
  for (const [filename, content] of Object.entries(DEFAULTS)) {
    const filepath = join(BRAIN_DIR, filename);
    if (!existsSync(filepath)) {
      writeFileSync(filepath, content, "utf-8");
    }
  }
}

// ── Read Operations ─────────────────────────────────────────

/**
 * Read a brain file's contents.
 * Returns the file content or null if it doesn't exist.
 */
export function readBrainFile(filename: BrainFile): string | null {
  ensureBrainDir();
  const filepath = join(BRAIN_DIR, filename);
  if (!existsSync(filepath)) return null;
  return readFileSync(filepath, "utf-8");
}

/**
 * Read all brain files and return a concatenated system prompt addition.
 * This gets prepended to the LLM system prompt so the agent's behavior
 * is shaped by the brain files.
 */
export function loadBrainContext(): string {
  ensureBrainDir();
  const sections: string[] = [];

  for (const filename of ALLOWED_BRAIN_FILES) {
    const content = readBrainFile(filename);
    if (content && content.trim().length > 0) {
      sections.push(`<!-- Brain: ${filename} -->\n${content.trim()}`);
    }
  }

  if (sections.length === 0) return "";
  return `\n\n--- AEON BRAIN FILES ---\n\n${sections.join("\n\n")}\n\n--- END BRAIN ---\n\n`;
}

// ── Write Operations ────────────────────────────────────────

/**
 * Append new content to a brain file.
 * Used by the RSI engine when a genuinely new instruction is needed.
 */
export function appendToBrainFile(
  filename: BrainFile,
  content: string,
  description: string,
  rationale: string
): { success: boolean; error?: string } {
  try {
    ensureBrainDir();

    if (!ALLOWED_BRAIN_FILES.includes(filename)) {
      return { success: false, error: `Invalid brain file: ${filename}` };
    }

    const filepath = join(BRAIN_DIR, filename);
    appendFileSync(filepath, `\n${content.trim()}\n`, "utf-8");

    // Log to RSI improvements
    logImprovement("Applied", filename, description, rationale);
    recordImprovement(filename, description);

    return { success: true };
  } catch (err: any) {
    return { success: false, error: err.message };
  }
}

/**
 * Surgically update a section in a brain file.
 * Finds the exact `oldContent` substring and replaces it with `newContent`.
 * Used by the RSI engine when refining an existing instruction.
 */
export function updateBrainFile(
  filename: BrainFile,
  oldContent: string,
  newContent: string,
  description: string,
  rationale: string
): { success: boolean; error?: string } {
  try {
    ensureBrainDir();

    if (!ALLOWED_BRAIN_FILES.includes(filename)) {
      return { success: false, error: `Invalid brain file: ${filename}` };
    }

    const filepath = join(BRAIN_DIR, filename);
    if (!existsSync(filepath)) {
      return {
        success: false,
        error: `${filename} does not exist — use append instead.`,
      };
    }

    const existing = readFileSync(filepath, "utf-8");

    if (!existing.includes(oldContent)) {
      return {
        success: false,
        error: `old_content not found in ${filename}. Read the file first to get exact current content.`,
      };
    }

    // Replace first occurrence only
    const updated = existing.replace(oldContent, newContent.trim());
    writeFileSync(filepath, updated, "utf-8");

    // Log to RSI improvements
    logImprovement("Updated", filename, description, rationale, oldContent, newContent);
    recordImprovement(filename, description);

    return { success: true };
  } catch (err: any) {
    return { success: false, error: err.message };
  }
}

// ── Improvement Logging ─────────────────────────────────────

const IMPROVEMENTS_PATH = join(RSI_DIR_PATH, "improvements.md");

function logImprovement(
  action: "Applied" | "Updated",
  targetFile: string,
  description: string,
  rationale: string,
  oldContent?: string,
  newContent?: string
): void {
  try {
    if (!existsSync(RSI_DIR_PATH)) {
      mkdirSync(RSI_DIR_PATH, { recursive: true });
    }

    const now = new Date().toISOString().replace("T", " ").slice(0, 19) + " UTC";

    let entry = `\n## [${action}] ${description}\n\n`;
    entry += `**Date:** ${now}\n`;
    entry += `**Target:** ${targetFile}\n`;
    entry += `**Rationale:** ${rationale || "(none)"}\n`;
    entry += `**Status:** ${action}\n`;

    if (action === "Updated" && oldContent && newContent) {
      entry += `\n**Replaced:**\n\`\`\`\n${oldContent.slice(0, 500)}\n\`\`\`\n`;
      entry += `**With:**\n\`\`\`\n${newContent.slice(0, 500)}\n\`\`\`\n`;
    }

    appendFileSync(IMPROVEMENTS_PATH, entry, "utf-8");

    // Also archive to daily history
    const today = new Date().toISOString().slice(0, 10);
    const historyDir = join(RSI_DIR_PATH, "history");
    if (!existsSync(historyDir)) mkdirSync(historyDir, { recursive: true });
    const historyPath = join(historyDir, `${today}.md`);
    appendFileSync(historyPath, entry, "utf-8");
  } catch {
    // Non-critical — never crash over logging
  }
}

/**
 * List all previously applied improvements.
 */
export function listImprovements(): string {
  if (!existsSync(IMPROVEMENTS_PATH)) {
    return "No improvements recorded yet.";
  }
  return readFileSync(IMPROVEMENTS_PATH, "utf-8");
}

/** Export brain directory path */
export const BRAIN_DIR_PATH = BRAIN_DIR;
