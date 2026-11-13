/**
 * Aeon Task Planner — The Brain
 *
 * Autonomous agent loop that decomposes natural language goals into
 * executable browser actions using the existing 23 MCP tools.
 *
 * Architecture:
 *   1. PLAN  — LLM decomposes goal into ordered steps
 *   2. ACT   — Execute each step via internal tool dispatch
 *   3. OBSERVE — Snapshot current page state
 *   4. VALIDATE — Check expected conditions, replan on failure
 *
 * @module planner
 */

import { llmChat, llmHealthCheck, extractJSON, type ChatMessage } from "./llm-client.js";
import { generateSnapshot, formatSnapshotForLLM, type AeonSnapshot } from "./snapshot.js";
import { recordReplan, recordTaskComplete, recordTaskFailed, recordLlmError } from "./feedback-ledger.js";

// ── Configuration ───────────────────────────────────────────

const MAX_STEPS = parseInt(process.env.AEON_MAX_STEPS ?? "30", 10);
const MAX_REPLANS = parseInt(process.env.AEON_MAX_REPLANS ?? "3", 10);
const STEP_DELAY_MS = parseInt(process.env.AEON_STEP_DELAY ?? "500", 10);

// ── Types ───────────────────────────────────────────────────

export interface PlannedStep {
  action: string;          // MCP tool name suffix (e.g. "navigate", "click", "type")
  description: string;     // Human-readable description of what this step does
  // Action-specific params
  url?: string;
  ref?: number;
  text?: string;
  submit?: boolean;        // Press Enter after typing
  key?: string;
  direction?: string;
  condition?: string;
  value?: string;
  fields?: Array<{ ref: number; value: string }>;
  expectUrl?: string;
  expectTitle?: string;
  expectText?: string;
}

export interface StepResult {
  step: PlannedStep;
  status: "success" | "failed" | "skipped";
  output: string;
  snapshotSummary?: string;
  timestamp: string;
}

export interface TaskState {
  id: string;
  goal: string;
  status: "planning" | "executing" | "validating" | "replanning" | "completed" | "failed";
  steps: PlannedStep[];
  results: StepResult[];
  currentStep: number;
  replanCount: number;
  startTime: number;
  endTime?: number;
  finalResult?: string;
}

// ── Active Tasks Registry ───────────────────────────────────

const activeTasks = new Map<string, TaskState>();

export function getTaskState(taskId: string): TaskState | undefined {
  return activeTasks.get(taskId);
}

// ── System Prompt ───────────────────────────────────────────

const SYSTEM_PROMPT = `You are AeonPlanner, the autonomous browsing agent for Aeon Browser.
You operate in an iterative Plan→Act→Observe→Validate loop to accomplish the user's goal.

You receive:
1. The user's goal
2. The current browser state — a Snapshot+Refs showing interactive elements with [ref] numbers
3. Step history from previous actions (if any)

Your job: decide the NEXT sequence of browser actions. Each action maps to an Aeon MCP tool.

## Available Actions
- "navigate" — Go to a URL. Params: url
- "click" — Click an element by ref. Params: ref, description
- "type" — Type text into an element. Params: ref, text, submit (true = press Enter after)
- "press_key" — Press a key. Params: key (Enter, Tab, Escape, ArrowDown, etc.)
- "scroll" — Scroll the page. Params: direction (up/down/top/bottom) OR ref (scroll to element)
- "wait" — Wait for a condition. Params: condition (load/idle/text/element), value
- "snapshot" — Take a new snapshot to see current page state
- "select_option" — Select a dropdown option. Params: ref, value or text
- "fill_form" — Fill multiple fields. Params: fields [{ref, value}]
- "hover" — Hover over an element. Params: ref
- "done" — Goal achieved. Put the final answer/result in description

## Critical Rules
1. ALWAYS start with a "snapshot" if you haven't seen the page yet.
2. After navigation or clicking links, add "wait" (condition: "load") then "snapshot".
3. Use ref numbers from the MOST RECENT snapshot ONLY — refs change between snapshots.
4. Keep plans SHORT (5-15 steps). You will get chances to replan after observing results.
5. For search: type in the search box with submit=true rather than clicking a search button.
6. For dropdowns that aren't <select> elements: click to expand first, then snapshot, then click the option.
7. DATA GROUNDING: Only report data you observe in the browser state. Do NOT fabricate values from training knowledge.
8. Before returning "done": re-read the goal, verify every requirement is met from the actual page state.

## Response Format
Respond with ONLY a JSON object:
{
  "memory": "1-3 sentences: Was the previous step successful? What do I see in the current state? What is my plan to achieve the goal?",
  "steps": [
    { "action": "navigate", "url": "https://...", "description": "Go to the website" },
    { "action": "wait", "condition": "load", "description": "Wait for page" },
    { "action": "snapshot", "description": "See the page" },
    { "action": "click", "ref": 5, "description": "Click search box" },
    { "action": "type", "ref": 5, "text": "query", "submit": true, "description": "Search" },
    { "action": "wait", "condition": "load", "description": "Wait for results" },
    { "action": "snapshot", "description": "See results" },
    { "action": "done", "description": "Found the answer: ..." }
  ]
}`;

const REPLAN_PROMPT = `The previous plan failed at a step. You need to recover and achieve the original goal.

FAILED STEP: {failedStep}
ERROR: {error}

CURRENT PAGE STATE:
{snapshot}

STEP HISTORY (what has been done so far):
{history}

ORIGINAL GOAL: {goal}

Instructions:
1. Analyze what went wrong — did the ref change? Did the page not load? Was the element not interactable?
2. Use the CURRENT snapshot's ref numbers (they may have changed from before).
3. Create a NEW plan from the current state to achieve the goal.
4. If the page looks different than expected, adapt your approach.

Respond with ONLY a JSON object with "memory" and "steps" fields.`;

// Step format from LLM now includes memory field
interface LLMPlanResponse {
  memory?: string;
  steps: PlannedStep[];
  thinking?: string; // backward compat
}

// ── Tool Executor ───────────────────────────────────────────

/**
 * The tool executor is injected by index.ts to avoid circular imports.
 * It calls handleToolCall() with the appropriate tool name and args.
 */
export type ToolExecutor = (
  toolName: string,
  args: Record<string, unknown>
) => Promise<{ content: Array<{ type: string; text?: string; data?: string }> }>;

let _executor: ToolExecutor | null = null;

export function setToolExecutor(executor: ToolExecutor): void {
  _executor = executor;
}

async function executeTool(
  toolName: string,
  args: Record<string, unknown>
): Promise<string> {
  if (!_executor) {
    throw new Error("Tool executor not initialized. Call setToolExecutor() first.");
  }

  const result = await _executor(toolName, args);
  const textContent = result.content
    .filter((c) => c.type === "text" && c.text)
    .map((c) => c.text!)
    .join("\n");

  return textContent || "(no text output)";
}

// ── Step Execution ──────────────────────────────────────────

/**
 * Map a planned step to an MCP tool call and execute it.
 */
async function executeStep(step: PlannedStep): Promise<string> {
  switch (step.action) {
    case "navigate":
      return executeTool("aeon_navigate", { url: step.url });

    case "page_navigate":
      return executeTool("aeon_page_navigate", { url: step.url });

    case "click":
      return executeTool("aeon_page_click", { ref: step.ref });

    case "type": {
      const result = await executeTool("aeon_page_type", {
        ref: step.ref,
        text: step.text,
        clearFirst: true,
      });
      // If submit=true, press Enter after typing
      if (step.submit) {
        await delay(200);
        await executeTool("aeon_page_press_key", { key: "Enter" });
      }
      return result;
    }

    case "press_key":
      return executeTool("aeon_page_press_key", { key: step.key ?? "Enter" });

    case "scroll":
      if (step.ref !== undefined) {
        return executeTool("aeon_page_scroll", { ref: step.ref });
      }
      return executeTool("aeon_page_scroll", {
        direction: step.direction ?? "down",
      });

    case "wait":
      return executeTool("aeon_page_wait", {
        condition: step.condition ?? "load",
        value: step.value,
        timeoutMs: 10000,
      });

    case "snapshot": {
      const snap = await generateSnapshot();
      return formatSnapshotForLLM(snap);
    }

    case "hover":
      return executeTool("aeon_page_hover", { ref: step.ref });

    case "select_option":
      return executeTool("aeon_page_select_option", {
        ref: step.ref,
        value: step.value,
        text: step.text,
      });

    case "fill_form":
      return executeTool("aeon_page_fill_form", { fields: step.fields });

    case "validate":
      return executeTool("aeon_validate", {
        expectUrl: step.expectUrl,
        expectTitle: step.expectTitle,
        expectText: step.expectText,
      });

    case "done":
      return `TASK COMPLETE: ${step.description}`;

    default:
      return `Unknown action: ${step.action}`;
  }
}

// ── Planning ────────────────────────────────────────────────

/**
 * Ask the LLM to create a step-by-step plan for the given goal.
 */
async function planSteps(
  goal: string,
  currentSnapshot: string,
  history?: string
): Promise<PlannedStep[]> {
  const messages: ChatMessage[] = [
    { role: "system", content: SYSTEM_PROMPT },
    {
      role: "user",
      content: `GOAL: ${goal}\n\nCURRENT BROWSER STATE:\n${currentSnapshot}${history ? `\n\nPREVIOUS STEP RESULTS:\n${history}` : ""}`,
    },
  ];

  const response = await llmChat(messages);

  const parsed = extractJSON<LLMPlanResponse>(response.content);
  if (!parsed?.steps || !Array.isArray(parsed.steps) || parsed.steps.length === 0) {
    throw new Error(
      `LLM returned invalid plan. Expected { steps: [...] }, got: ${response.content.slice(0, 300)}`
    );
  }

  // Validate each step has an action
  for (const step of parsed.steps) {
    if (!step.action) {
      throw new Error(`Step missing 'action' field: ${JSON.stringify(step)}`);
    }
    if (!step.description) {
      step.description = step.action;
    }
  }

  return parsed.steps;
}

/**
 * Ask the LLM to replan after a step failure.
 */
async function replanSteps(
  goal: string,
  failedStep: PlannedStep,
  error: string,
  currentSnapshot: string,
  stepHistory: StepResult[]
): Promise<PlannedStep[]> {
  const historyStr = stepHistory
    .map((r, i) => `  ${i + 1}. [${r.status}] ${r.step.description}: ${r.output.slice(0, 100)}`)
    .join("\n");

  const prompt = REPLAN_PROMPT
    .replace("{failedStep}", JSON.stringify(failedStep))
    .replace("{error}", error)
    .replace("{snapshot}", currentSnapshot)
    .replace("{history}", historyStr)
    .replace("{goal}", goal);

  const messages: ChatMessage[] = [
    { role: "system", content: SYSTEM_PROMPT },
    { role: "user", content: prompt },
  ];

  const response = await llmChat(messages);

  const parsed = extractJSON<{ steps?: PlannedStep[] }>(response.content);
  if (!parsed?.steps || parsed.steps.length === 0) {
    throw new Error("LLM failed to produce a recovery plan");
  }

  for (const step of parsed.steps) {
    if (!step.description) step.description = step.action;
  }

  return parsed.steps;
}

// ── Main Task Runner ────────────────────────────────────────

/**
 * Execute an autonomous browsing task.
 *
 * This is the main entry point — it runs the full
 * Plan → Act → Observe → Validate loop until the goal
 * is achieved or max steps/replans are exhausted.
 *
 * @param goal - Natural language description of what to accomplish
 * @returns Task result summary
 */
export async function runTask(goal: string): Promise<TaskState> {
  const taskId = `task_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 6)}`;

  const state: TaskState = {
    id: taskId,
    goal,
    status: "planning",
    steps: [],
    results: [],
    currentStep: 0,
    replanCount: 0,
    startTime: Date.now(),
  };

  activeTasks.set(taskId, state);

  try {
    // ── 1. Check LLM availability ──
    const health = await llmHealthCheck();
    if (!health.available) {
      state.status = "failed";
      state.finalResult = `LLM not available at ${health.url}: ${health.error}. Start Ollama: 'ollama serve' then 'ollama pull ${health.model}'`;
      state.endTime = Date.now();
      return state;
    }

    // ── 2. Get initial snapshot ──
    const initialSnap = await generateSnapshot();
    const snapText = formatSnapshotForLLM(initialSnap);

    // ── 3. Plan ──
    state.status = "planning";
    state.steps = await planSteps(goal, snapText);

    // ── 4. Execute loop ──
    let totalStepsExecuted = 0;

    while (state.currentStep < state.steps.length && totalStepsExecuted < MAX_STEPS) {
      const step = state.steps[state.currentStep];
      state.status = "executing";
      totalStepsExecuted++;

      // Check for "done" action
      if (step.action === "done") {
        state.results.push({
          step,
          status: "success",
          output: step.description,
          timestamp: new Date().toISOString(),
        });
        state.status = "completed";
        state.finalResult = step.description;
        state.endTime = Date.now();
        recordTaskComplete(state.id, state.results.length);
        return state;
      }

      try {
        // Execute the step
        const output = await executeStep(step);

        // Record result
        const result: StepResult = {
          step,
          status: "success",
          output: output.slice(0, 2000), // Cap output size for context
          timestamp: new Date().toISOString(),
        };

        // If this was a snapshot, include it for context
        if (step.action === "snapshot") {
          result.snapshotSummary = output.slice(0, 3000);
        }

        state.results.push(result);
        state.currentStep++;

        // Small delay between steps to let pages settle
        await delay(STEP_DELAY_MS);

      } catch (err: any) {
        const errorMsg = err.message ?? String(err);

        state.results.push({
          step,
          status: "failed",
          output: `ERROR: ${errorMsg}`,
          timestamp: new Date().toISOString(),
        });

        // ── Replan ──
        if (state.replanCount >= MAX_REPLANS) {
          state.status = "failed";
          state.finalResult = `Failed after ${MAX_REPLANS} replan attempts. Last error: ${errorMsg}`;
          state.endTime = Date.now();
          recordTaskFailed(state.id, state.finalResult);
          return state;
        }

        state.status = "replanning";
        state.replanCount++;
        recordReplan(state.id, step.action, errorMsg);

        try {
          // Get fresh snapshot for replanning
          const freshSnap = await generateSnapshot();
          const freshSnapText = formatSnapshotForLLM(freshSnap);

          const newSteps = await replanSteps(
            goal,
            step,
            errorMsg,
            freshSnapText,
            state.results
          );

          // Replace remaining steps with new plan
          state.steps = [...state.steps.slice(0, state.currentStep), ...newSteps];
          // Don't increment currentStep — retry from current position with new plan

        } catch (replanErr: any) {
          state.status = "failed";
          state.finalResult = `Replan failed: ${replanErr.message}`;
          state.endTime = Date.now();
          recordTaskFailed(state.id, state.finalResult);
          return state;
        }
      }
    }

    // If we exhausted steps without a "done" action, try to summarize
    if ((state.status as string) !== "completed") {
      const lastSnapshot = state.results
        .filter((r) => r.snapshotSummary)
        .pop();

      if (lastSnapshot?.snapshotSummary) {
        // Ask LLM to summarize what was accomplished
        try {
          const summaryResponse = await llmChat([
            {
              role: "system",
              content: "You are a concise summarizer. Given the browsing history and final page state, answer the user's original question or explain what was accomplished. Be brief and direct.",
            },
            {
              role: "user",
              content: `Original goal: ${goal}\n\nFinal page state:\n${lastSnapshot.snapshotSummary}\n\nSteps taken:\n${state.results.map((r, i) => `${i + 1}. [${r.status}] ${r.step.description}`).join("\n")}`,
            },
          ]);
          state.finalResult = summaryResponse.content;
        } catch {
          state.finalResult = `Completed ${totalStepsExecuted} steps but could not determine if goal was fully achieved.`;
        }
      } else {
        state.finalResult = `Executed ${totalStepsExecuted} steps. Max steps reached without explicit completion.`;
      }

      state.status = totalStepsExecuted >= MAX_STEPS ? "failed" : "completed";
    }

  } catch (err: any) {
    state.status = "failed";
    state.finalResult = `Fatal error: ${err.message}`;
  }

  state.endTime = Date.now();
  return state;
}

// ── Formatting ──────────────────────────────────────────────

/**
 * Format a TaskState into a human-readable summary.
 */
export function formatTaskResult(state: TaskState): string {
  const duration = state.endTime
    ? `${((state.endTime - state.startTime) / 1000).toFixed(1)}s`
    : "in progress";

  const lines: string[] = [
    `# Task: ${state.goal}`,
    ``,
    `**Status:** ${state.status.toUpperCase()}`,
    `**Duration:** ${duration}`,
    `**Steps:** ${state.results.length}/${state.steps.length} (${state.replanCount} replans)`,
    `**Task ID:** ${state.id}`,
    ``,
  ];

  if (state.finalResult) {
    lines.push(`## Result`);
    lines.push(state.finalResult);
    lines.push(``);
  }

  if (state.results.length > 0) {
    lines.push(`## Step History`);
    for (let i = 0; i < state.results.length; i++) {
      const r = state.results[i];
      const icon = r.status === "success" ? "✅" : r.status === "failed" ? "❌" : "⏭️";
      const output = r.output.length > 120
        ? r.output.slice(0, 120) + "..."
        : r.output;
      lines.push(`${i + 1}. ${icon} **${r.step.action}** — ${r.step.description}`);
      if (r.status === "failed") {
        lines.push(`   → ${output}`);
      }
    }
  }

  return lines.join("\n");
}

// ── Utility ─────────────────────────────────────────────────

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}
