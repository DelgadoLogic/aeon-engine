#!/usr/bin/env node
/**
 * Aeon MCP Server — Agent Bridge for Aeon Browser
 *
 * This Model Context Protocol server exposes unified tools for AI agents to
 * control the Aeon Browser through three control surfaces:
 *
 *   1. Shell Control  — Named Pipe IPC → tab/window management
 *   2. Content Control — CDP WebSocket  → DOM, JS execution, screenshots
 *   3. Snapshot+Refs  — Merged state    → compact perception for LLMs
 *
 * Transport: stdio (launched as subprocess by MCP-compliant hosts)
 *
 * Implementation follows the MCP specification:
 *   https://spec.modelcontextprotocol.io/
 */

import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
  ListResourcesRequestSchema,
  ReadResourceRequestSchema,
  type Tool,
} from "@modelcontextprotocol/sdk/types.js";

import {
  pipeSend,
  pipeHealthCheck,
  summarizePage,
  navigateIntent,
  getWebMCPTools,
  stagehandObserve,
  stagehandAct,
  stagehandExtract,
  stagehandAgent,
  gemmaProcess,
  multionWorkflow,
} from "./pipe-client.js";
import { cdpSend, cdpListTargets, cdpSendToTarget, cdpSendToFirstPage, cdpHealthCheck, cdpVersion } from "./cdp-client.js";
import { generateSnapshot, formatSnapshotForLLM } from "./snapshot.js";
import { runTask, getTaskState, formatTaskResult, setToolExecutor } from "./planner.js";
import { llmHealthCheck, llmListModels, initEmbeddingEngine } from "./llm-client.js";
import { recordToolSuccess, recordToolFailure, recordTaskComplete, recordTaskFailed, recordReplan, generateSummary, formatDigest, toolStats, entriesByType, detectOpportunities } from "./feedback-ledger.js";
import { readBrainFile, appendToBrainFile, updateBrainFile, listImprovements, ALLOWED_BRAIN_FILES, type BrainFile, loadBrainContext } from "./brain.js";
import { startRsiEngine, rsiStatus, rsiTick } from "./rsi-engine.js";
import { storeMemory, searchMemory, deleteMemory, getMemory, updateMemory, memoryHistory, listMemories, memoryStats, loadMemoryContext, registerEmbeddingFn, hasSemanticSearch } from "./memory.js";
import { initScheduler, setSchedulerExecutor, createSchedule, deleteSchedule, toggleSchedule, listSchedules, getSchedule, schedulerStats, parseInterval, parseCronExpression, validateCron, describeCron, type ScheduleType } from "./scheduler.js";
import { initCache, cacheStats, cacheClear } from "./cache.js";

// ─────────────────────────────────────────────────────────────
// Tool Definitions
// ─────────────────────────────────────────────────────────────

const tools: Tool[] = [
  // ── Perception ─────────────────────────────────────────────
  {
    name: "aeon_snapshot",
    description:
      "Capture a full Snapshot+Refs of the current browser state. Returns " +
      "shell state (tabs, window) and page content (interactive elements " +
      "with [ref] numbers). Use this before deciding what action to take.",
    inputSchema: {
      type: "object" as const,
      properties: {
        format: {
          type: "string",
          enum: ["text", "json"],
          description: "Output format: 'text' for LLM-compact, 'json' for structured data. Default: text",
        },
      },
    },
  },

  // ── Shell Control (Named Pipe) ─────────────────────────────
  {
    name: "aeon_tab_list",
    description: "List all open tabs in Aeon Browser with their IDs, titles, and URLs.",
    inputSchema: { type: "object" as const, properties: {} },
  },
  {
    name: "aeon_tab_new",
    description: "Open a new tab in Aeon Browser, optionally navigating to a URL.",
    inputSchema: {
      type: "object" as const,
      properties: {
        url: { type: "string", description: "URL to navigate to. Default: new tab page." },
      },
    },
  },
  {
    name: "aeon_tab_close",
    description: "Close a tab by its ID.",
    inputSchema: {
      type: "object" as const,
      properties: {
        tabId: { type: "number", description: "Tab ID to close." },
      },
      required: ["tabId"],
    },
  },
  {
    name: "aeon_tab_activate",
    description: "Switch to (activate) a tab by its ID.",
    inputSchema: {
      type: "object" as const,
      properties: {
        tabId: { type: "number", description: "Tab ID to activate." },
      },
      required: ["tabId"],
    },
  },
  {
    name: "aeon_navigate",
    description: "Navigate the active tab to a URL via the shell (address bar).",
    inputSchema: {
      type: "object" as const,
      properties: {
        url: { type: "string", description: "URL to navigate to." },
      },
      required: ["url"],
    },
  },

  // ── Content Control (CDP) ──────────────────────────────────
  {
    name: "aeon_page_navigate",
    description: "Navigate a page target to a URL via CDP Page.navigate.",
    inputSchema: {
      type: "object" as const,
      properties: {
        url: { type: "string", description: "URL to navigate to." },
        targetId: { type: "string", description: "CDP target ID. Omit for active page." },
      },
      required: ["url"],
    },
  },
  {
    name: "aeon_page_evaluate",
    description:
      "Execute JavaScript in the page context via CDP Runtime.evaluate. " +
      "Returns the result. Use for reading page state or performing actions.",
    inputSchema: {
      type: "object" as const,
      properties: {
        expression: { type: "string", description: "JavaScript expression to evaluate." },
        targetId: { type: "string", description: "CDP target ID. Omit for active page." },
        awaitPromise: { type: "boolean", description: "Await promise result. Default: true." },
      },
      required: ["expression"],
    },
  },
  {
    name: "aeon_page_screenshot",
    description: "Take a screenshot of the page. Returns base64-encoded PNG.",
    inputSchema: {
      type: "object" as const,
      properties: {
        targetId: { type: "string", description: "CDP target ID. Omit for active page." },
        fullPage: { type: "boolean", description: "Capture full scrollable page. Default: false." },
      },
    },
  },
  {
    name: "aeon_page_click",
    description:
      "Click an interactive element by its [ref] number from a snapshot. " +
      "Uses CDP DOM node resolution and Input.dispatchMouseEvent.",
    inputSchema: {
      type: "object" as const,
      properties: {
        ref: { type: "number", description: "Element ref number from the snapshot." },
      },
      required: ["ref"],
    },
  },
  {
    name: "aeon_page_type",
    description:
      "Type text into an interactive element by its [ref] number. " +
      "First focuses the element, then dispatches key events.",
    inputSchema: {
      type: "object" as const,
      properties: {
        ref: { type: "number", description: "Element ref number from the snapshot." },
        text: { type: "string", description: "Text to type." },
        clearFirst: { type: "boolean", description: "Clear existing content before typing. Default: true." },
      },
      required: ["ref", "text"],
    },
  },
  {
    name: "aeon_page_scroll",
    description:
      "Scroll the page up, down, or to a specific element by ref number. " +
      "Uses CDP Input.dispatchMouseEvent with mouseWheel type.",
    inputSchema: {
      type: "object" as const,
      properties: {
        direction: {
          type: "string",
          enum: ["up", "down", "top", "bottom"],
          description: "Scroll direction. Default: down.",
        },
        amount: {
          type: "number",
          description: "Pixels to scroll. Default: 600 (roughly one viewport).",
        },
        ref: {
          type: "number",
          description: "Scroll to bring this element into view instead of directional scroll.",
        },
      },
    },
  },
  {
    name: "aeon_page_wait",
    description:
      "Wait for a condition to be true on the page. Polls up to a timeout. " +
      "Use after navigation or actions that trigger async page loads.",
    inputSchema: {
      type: "object" as const,
      properties: {
        condition: {
          type: "string",
          enum: ["load", "idle", "text", "element"],
          description:
            "'load' = wait for page load event. 'idle' = wait for network idle. " +
            "'text' = wait for text to appear. 'element' = wait for element by selector.",
        },
        value: {
          type: "string",
          description: "For 'text': the text to wait for. For 'element': CSS selector.",
        },
        timeoutMs: {
          type: "number",
          description: "Maximum wait time in milliseconds. Default: 10000.",
        },
      },
      required: ["condition"],
    },
  },
  {
    name: "aeon_page_press_key",
    description:
      "Press a keyboard key (Enter, Tab, Escape, Backspace, ArrowDown, etc). " +
      "Useful for form submission, navigation, and dropdown interaction.",
    inputSchema: {
      type: "object" as const,
      properties: {
        key: {
          type: "string",
          description:
            "Key name: Enter, Tab, Escape, Backspace, ArrowDown, ArrowUp, " +
            "ArrowLeft, ArrowRight, Space, Delete, Home, End, PageUp, PageDown.",
        },
        modifiers: {
          type: "number",
          description:
            "Modifier bitmask: 1=Alt, 2=Ctrl, 4=Meta/Cmd, 8=Shift. " +
            "Example: Ctrl+Shift = 10 (2+8). Default: 0.",
        },
      },
      required: ["key"],
    },
  },
  {
    name: "aeon_page_hover",
    description:
      "Hover over an element by ref number. Triggers CSS :hover states " +
      "and mouseover events. Useful for revealing menus or tooltips.",
    inputSchema: {
      type: "object" as const,
      properties: {
        ref: { type: "number", description: "Element ref number from the snapshot." },
      },
      required: ["ref"],
    },
  },
  {
    name: "aeon_page_select_option",
    description:
      "Select an option from a dropdown/select element. Targets the select " +
      "element by ref and selects by value or visible text.",
    inputSchema: {
      type: "object" as const,
      properties: {
        ref: { type: "number", description: "Ref number of the select/combobox element." },
        value: { type: "string", description: "Option value to select." },
        text: { type: "string", description: "Visible text of the option. Used if value not provided." },
      },
      required: ["ref"],
    },
  },
  {
    name: "aeon_page_fill_form",
    description:
      "Fill multiple form fields at once. Takes an array of {ref, value} pairs. " +
      "More efficient than calling aeon_page_type for each field.",
    inputSchema: {
      type: "object" as const,
      properties: {
        fields: {
          type: "array",
          items: {
            type: "object",
            properties: {
              ref: { type: "number", description: "Element ref number." },
              value: { type: "string", description: "Value to fill." },
            },
            required: ["ref", "value"],
          },
          description: "Array of {ref, value} pairs for each form field.",
        },
      },
      required: ["fields"],
    },
  },
  {
    name: "aeon_cdp_raw",
    description: "Send a raw CDP command. For advanced use when specific tools don't cover your need.",
    inputSchema: {
      type: "object" as const,
      properties: {
        method: { type: "string", description: "CDP method (e.g., 'DOM.getDocument')." },
        params: {
          type: "object",
          description: "CDP method parameters.",
          additionalProperties: true,
        },
        targetId: { type: "string", description: "CDP target ID. Omit for active page." },
      },
      required: ["method"],
    },
  },

  // ── Diagnostics ────────────────────────────────────────────
  {
    name: "aeon_health",
    description:
      "Check connectivity to both the Aeon shell (Named Pipe) and " +
      "CDP (WebSocket). Use as a quick diagnostic.",
    inputSchema: { type: "object" as const, properties: {} },
  },
  {
    name: "aeon_targets",
    description: "List all CDP-inspectable targets (pages, service workers, etc.).",
    inputSchema: { type: "object" as const, properties: {} },
  },

  // ── Validation ─────────────────────────────────────────────
  {
    name: "aeon_validate",
    description:
      "Validate that the last action succeeded by checking current page state. " +
      "Takes a snapshot and checks for expected conditions. Part of the " +
      "Planner-Actor-Validator loop.",
    inputSchema: {
      type: "object" as const,
      properties: {
        expectUrl: { type: "string", description: "Expected URL pattern (substring match)." },
        expectTitle: { type: "string", description: "Expected title pattern (substring match)." },
        expectText: { type: "string", description: "Text expected to be present on the page." },
        expectElementWithRef: {
          type: "number",
          description: "Ref number of an element expected to exist in the new snapshot.",
        },
      },
    },
  },

  // ── Autonomous Task Planner (Phase 3) ─────────────────────
  {
    name: "aeon_task",
    description:
      "Execute an autonomous browsing task. Takes a natural language goal " +
      "(e.g. 'find the cheapest PS5 on Amazon') and runs the full " +
      "Plan→Act→Observe→Validate loop using the local LLM (Ollama). " +
      "Returns a structured result with step history and final answer.",
    inputSchema: {
      type: "object" as const,
      properties: {
        goal: {
          type: "string",
          description: "Natural language description of what to accomplish.",
        },
      },
      required: ["goal"],
    },
  },
  {
    name: "aeon_task_status",
    description:
      "Check the status of an autonomous task by its ID. Returns current " +
      "state, step history, and result if completed.",
    inputSchema: {
      type: "object" as const,
      properties: {
        taskId: {
          type: "string",
          description: "Task ID returned by aeon_task.",
        },
      },
      required: ["taskId"],
    },
  },
  {
    name: "aeon_llm_status",
    description:
      "Check LLM availability and list loaded models. " +
      "Supports embedded inference (node-llama-cpp) or external endpoints (Ollama/OpenAI). " +
      "Use to verify the AI backend is ready before running tasks.",
    inputSchema: { type: "object" as const, properties: {} },
  },

  // ── RSI (Recursive Self-Improvement) ────────────────────────
  {
    name: "aeon_rsi_status",
    description:
      "Check the status of Aeon's Recursive Self-Improvement engine. " +
      "Shows feedback ledger stats, detected improvement opportunities, " +
      "cycle count, and last cycle result.",
    inputSchema: { type: "object" as const, properties: {} },
  },
  {
    name: "aeon_feedback_analyze",
    description:
      "Analyze the feedback ledger for tool performance patterns, " +
      "failure rates, user corrections, and improvement history. " +
      "Query types: 'summary', 'tool_stats', 'failures', 'improvements', 'digest', 'opportunities'.",
    inputSchema: {
      type: "object" as const,
      properties: {
        query: {
          type: "string",
          enum: ["summary", "tool_stats", "failures", "improvements", "digest", "opportunities"],
          description: "What to analyze. 'summary' for overview, 'tool_stats' for per-tool performance, etc.",
        },
      },
      required: ["query"],
    },
  },
  {
    name: "aeon_self_improve",
    description:
      "Manually trigger the RSI self-improvement engine or apply a " +
      "specific improvement to a brain file. Actions: 'trigger' to run " +
      "a full RSI cycle, 'read' to view a brain file, 'apply' to append " +
      "new content, 'update' to refine existing content.",
    inputSchema: {
      type: "object" as const,
      properties: {
        action: {
          type: "string",
          enum: ["trigger", "read", "apply", "update"],
          description: "What to do: trigger=run RSI cycle, read=view brain file, apply=append, update=replace",
        },
        target_file: {
          type: "string",
          enum: ["SOUL.md", "TOOLS.md", "USER.md", "MEMORY.md", "CODE.md", "SECURITY.md", "AGENTS.md"],
          description: "Brain file to read/modify (for read/apply/update actions).",
        },
        content: {
          type: "string",
          description: "For apply/update: the content to add or replace with.",
        },
        old_content: {
          type: "string",
          description: "For update only: the exact existing text to find and replace.",
        },
        description: {
          type: "string",
          description: "For apply/update: human-readable description of the improvement.",
        },
        rationale: {
          type: "string",
          description: "For apply/update: why this improvement is needed.",
        },
      },
      required: ["action"],
    },
  },

  // ── Persistent Memory ─────────────────────────────────────────
  {
    name: "aeon_memory_store",
    description:
      "Store a fact, context, or knowledge snippet in persistent memory. " +
      "Memory survives across server restarts and is searchable by keyword and tags. " +
      "Use for important context, user preferences, project knowledge, or anything the agent should remember.",
    inputSchema: {
      type: "object" as const,
      properties: {
        content: { type: "string", description: "The fact or knowledge to remember." },
        tags: {
          type: "array",
          items: { type: "string" },
          description: "Tags for categorization (e.g., ['user_preference', 'project_context']).",
        },
        importance: {
          type: "number",
          description: "How important this memory is (0.0–1.0). Higher = more likely to appear in context.",
        },
        ttl_hours: {
          type: "number",
          description: "Optional: auto-expire this memory after N hours.",
        },
      },
      required: ["content"],
    },
  },
  {
    name: "aeon_memory_search",
    description:
      "Search persistent memory by keyword, tag, or source. " +
      "Returns the most relevant memories ranked by content match, importance, and recency.",
    inputSchema: {
      type: "object" as const,
      properties: {
        query: { type: "string", description: "Search query (keywords)." },
        tags: {
          type: "array",
          items: { type: "string" },
          description: "Filter by tags.",
        },
        limit: { type: "number", description: "Max results (default 10)." },
      },
      required: ["query"],
    },
  },
  {
    name: "aeon_memory_list",
    description:
      "List all persistent memories, optionally filtered by tag or source. " +
      "Shows the most recent entries first.",
    inputSchema: {
      type: "object" as const,
      properties: {
        tags: {
          type: "array",
          items: { type: "string" },
          description: "Filter by tags.",
        },
        source: { type: "string", description: "Filter by source (user, agent, rsi, system)." },
        limit: { type: "number", description: "Max results (default 20)." },
      },
    },
  },
  {
    name: "aeon_memory_delete",
    description: "Delete a specific memory by its ID.",
    inputSchema: {
      type: "object" as const,
      properties: {
        id: { type: "string", description: "The memory ID to delete." },
      },
      required: ["id"],
    },
  },
  {
    name: "aeon_memory_get",
    description:
      "Get a specific memory by its ID. Returns the full memory entry " +
      "including content, tags, importance, access count, and history.",
    inputSchema: {
      type: "object" as const,
      properties: {
        id: { type: "string", description: "The memory ID to retrieve." },
      },
      required: ["id"],
    },
  },
  {
    name: "aeon_memory_update",
    description:
      "Update an existing memory's content, tags, or importance. " +
      "Tracks the mutation in history for auditability. " +
      "Use instead of delete+recreate for surgical memory refinement.",
    inputSchema: {
      type: "object" as const,
      properties: {
        id: { type: "string", description: "The memory ID to update." },
        content: { type: "string", description: "New content for the memory." },
        tags: {
          type: "array",
          items: { type: "string" },
          description: "New tags (replaces existing tags).",
        },
        importance: {
          type: "number",
          description: "New importance score (0.0–1.0).",
        },
      },
      required: ["id", "content"],
    },
  },
  {
    name: "aeon_memory_history",
    description:
      "View the mutation history of memories. Shows ADD, UPDATE, and DELETE " +
      "events with timestamps and content diffs. Useful for auditing RSI " +
      "memory changes and understanding how knowledge has evolved.",
    inputSchema: {
      type: "object" as const,
      properties: {
        memory_id: {
          type: "string",
          description: "Filter history to a specific memory ID. Omit for all history.",
        },
        limit: { type: "number", description: "Max results (default 20)." },
      },
    },
  },

  // ── Task Scheduler ────────────────────────────────────────────
  {
    name: "aeon_schedule_create",
    description:
      "Schedule a recurring or one-shot task. The agent will automatically execute " +
      "the goal at the specified time/interval using the planner. " +
      "Types: 'interval' (every N ms), 'once' (at specific time), 'daily' (at HH:MM), " +
      "'cron' (5-field cron expression: minute hour day-of-month month day-of-week).",
    inputSchema: {
      type: "object" as const,
      properties: {
        goal: { type: "string", description: "The task goal for the planner (e.g., 'Check email and summarize unread')." },
        type: {
          type: "string",
          enum: ["interval", "once", "daily", "cron"],
          description: "Schedule type.",
        },
        interval: {
          type: "string",
          description: "For 'interval': human-readable interval (e.g., '5 minutes', '1 hour', '30 seconds').",
        },
        execute_at: {
          type: "string",
          description: "For 'once': ISO 8601 timestamp when to execute.",
        },
        daily_time: {
          type: "string",
          description: "For 'daily': time in HH:MM format (e.g., '09:00', '17:30').",
        },
        cron_expression: {
          type: "string",
          description: "For 'cron': 5-field cron expression (e.g., '*/5 * * * *' = every 5 min, '0 9 * * 1-5' = 9 AM weekdays).",
        },
        max_runs: {
          type: "number",
          description: "Optional: stop after N executions.",
        },
      },
      required: ["goal", "type"],
    },
  },
  {
    name: "aeon_schedule_list",
    description: "List all scheduled tasks with their status, run count, and next execution time.",
    inputSchema: { type: "object" as const, properties: {} },
  },
  {
    name: "aeon_schedule_delete",
    description: "Delete a scheduled task by its ID.",
    inputSchema: {
      type: "object" as const,
      properties: {
        id: { type: "string", description: "Schedule ID to delete." },
      },
      required: ["id"],
    },
  },
  {
    name: "aeon_schedule_toggle",
    description: "Enable or disable a scheduled task.",
    inputSchema: {
      type: "object" as const,
      properties: {
        id: { type: "string", description: "Schedule ID to toggle." },
        enabled: { type: "boolean", description: "True to enable, false to disable." },
      },
      required: ["id", "enabled"],
    },
  },

  // ── Cache Diagnostics ──────────────────────────────────────────
  {
    name: "aeon_cache_stats",
    description:
      "View cache statistics: hit/miss rate, entry count, oldest entry age. " +
      "The cache accelerates repeated CDP lookups, snapshot data, and LLM responses.",
    inputSchema: {
      type: "object" as const,
      properties: {
        action: {
          type: "string",
          enum: ["stats", "clear"],
          description: "'stats' to view metrics, 'clear' to flush all cached data. Default: stats.",
        },
      },
    },
  },

  // ── Self-Update ────────────────────────────────────────────────
  {
    name: "aeon_self_update",
    description:
      "Trigger a self-update cycle: rebuild the MCP server from source (npm run build) " +
      "and report the result. The server will need to be restarted by the host to pick up changes. " +
      "Use after RSI has modified source files, or when manual code changes have been made.",
    inputSchema: {
      type: "object" as const,
      properties: {
        dry_run: {
          type: "boolean",
          description: "If true, only check if build succeeds without signaling restart. Default: false.",
        },
      },
    },
  },

  // ── Milestone 1 R1 & Competitive Agentic Tools ─────────────
  {
    name: "aeon_ai_summarize",
    description: "Extract active page text & AX-tree, call Gemma 4 GGUF local model, return structured JSON summary.",
    inputSchema: {
      type: "object" as const,
      properties: {
        tabId: { type: "number", description: "Tab ID to summarize. Default: active tab (-1)." },
        maxBullets: { type: "number", description: "Max bullet points. Default: 5." },
      },
    },
  },
  {
    name: "aeon_ai_navigate_intent",
    description: "Parse natural language intent prompt and trigger browser navigation.",
    inputSchema: {
      type: "object" as const,
      properties: {
        intent: { type: "string", description: "Natural language intent prompt." },
        tabId: { type: "number", description: "Tab ID. Default: active/new (-1)." },
      },
      required: ["intent"],
    },
  },
  {
    name: "aeon_webmcp_tools",
    description: "Discover WebMCP tools exposed by active tab DOM forms and actions.",
    inputSchema: {
      type: "object" as const,
      properties: {
        tabId: { type: "number", description: "Tab ID. Default: active tab (-1)." },
      },
    },
  },
  {
    name: "aeon_stagehand_observe",
    description: "Perception primitive: extract AX-tree element refs and bounds.",
    inputSchema: {
      type: "object" as const,
      properties: {
        tabId: { type: "number", description: "Tab ID. Default: active (-1)." },
      },
    },
  },
  {
    name: "aeon_stagehand_act",
    description: "Motor primitive: execute click, type, scroll, hover actions on ref elements.",
    inputSchema: {
      type: "object" as const,
      properties: {
        action: { type: "string", description: "Action type: click, type, scroll, hover." },
        ref: { type: "number", description: "Element ref number." },
        text: { type: "string", description: "Text for type action." },
      },
      required: ["action"],
    },
  },
  {
    name: "aeon_stagehand_extract",
    description: "Extraction primitive: extract structured JSON data using prompt or schema.",
    inputSchema: {
      type: "object" as const,
      properties: {
        instruction: { type: "string", description: "Extraction prompt/instruction." },
      },
      required: ["instruction"],
    },
  },
  {
    name: "aeon_stagehand_agent",
    description: "Autonomous primitive: execute Stagehand goal loop (Observe->Plan->Act->Validate).",
    inputSchema: {
      type: "object" as const,
      properties: {
        goal: { type: "string", description: "Goal string." },
      },
      required: ["goal"],
    },
  },
  {
    name: "aeon_gemma_process",
    description: "On-device Gemma 4 GGUF local model text and vision processing.",
    inputSchema: {
      type: "object" as const,
      properties: {
        prompt: { type: "string", description: "Prompt string." },
        imageBase64: { type: "string", description: "Optional base64 image." },
      },
      required: ["prompt"],
    },
  },
  {
    name: "aeon_multion_workflow",
    description: "MultiOn motor cortex autonomous multi-step workflow execution with auth.json session reuse.",
    inputSchema: {
      type: "object" as const,
      properties: {
        goal: { type: "string", description: "Workflow goal." },
        authJson: { type: "string", description: "Optional path to auth.json session file." },
      },
      required: ["goal"],
    },
  },
];

// ─────────────────────────────────────────────────────────────
// Tool Handlers
// ─────────────────────────────────────────────────────────────

// Cache the most recent snapshot for ref-based actions
let lastSnapshot: Awaited<ReturnType<typeof generateSnapshot>> | null = null;

async function handleToolCall(
  name: string,
  args: Record<string, unknown>
): Promise<{ content: Array<{ type: string; text?: string; data?: string; mimeType?: string }> }> {
  switch (name) {
    // ── Perception ──
    case "aeon_snapshot": {
      const snap = await generateSnapshot();
      lastSnapshot = snap;
      const format = (args.format as string) ?? "text";
      if (format === "json") {
        return text(JSON.stringify(snap, null, 2));
      }
      return text(formatSnapshotForLLM(snap));
    }

    // ── Shell ──
    case "aeon_tab_list": {
      const result = await pipeSend({ cmd: "tab.list" });
      return text(JSON.stringify(result, null, 2));
    }

    case "aeon_tab_new": {
      const result = await pipeSend({
        cmd: "tab.new",
        ...(args.url ? { url: args.url } : {}),
      });
      return text(JSON.stringify(result, null, 2));
    }

    case "aeon_tab_close": {
      const result = await pipeSend({ cmd: "tab.close", tabId: args.tabId });
      return text(JSON.stringify(result, null, 2));
    }

    case "aeon_tab_activate": {
      const result = await pipeSend({ cmd: "tab.activate", tabId: args.tabId });
      return text(JSON.stringify(result, null, 2));
    }

    case "aeon_navigate": {
      const result = await pipeSend({ cmd: "navigate", url: args.url });
      return text(JSON.stringify(result, null, 2));
    }

    // ── Milestone 1 AI & Agentic ──
    case "aeon_ai_summarize": {
      const result = await summarizePage(args.tabId as number | undefined, args.maxBullets as number | undefined);
      return text(JSON.stringify(result, null, 2));
    }
    case "aeon_ai_navigate_intent": {
      const result = await navigateIntent(args.intent as string, args.tabId as number | undefined);
      return text(JSON.stringify(result, null, 2));
    }
    case "aeon_webmcp_tools": {
      const result = await getWebMCPTools(args.tabId as number | undefined);
      return text(JSON.stringify(result, null, 2));
    }
    case "aeon_stagehand_observe": {
      const result = await stagehandObserve(args.tabId as number | undefined);
      return text(JSON.stringify(result, null, 2));
    }
    case "aeon_stagehand_act": {
      const result = await stagehandAct(args.action as string, args.ref as number | undefined, args.text as string | undefined);
      return text(JSON.stringify(result, null, 2));
    }
    case "aeon_stagehand_extract": {
      const result = await stagehandExtract(args.instruction as string);
      return text(JSON.stringify(result, null, 2));
    }
    case "aeon_stagehand_agent": {
      const result = await stagehandAgent(args.goal as string);
      return text(JSON.stringify(result, null, 2));
    }
    case "aeon_gemma_process": {
      const result = await gemmaProcess(args.prompt as string, args.imageBase64 as string | undefined);
      return text(JSON.stringify(result, null, 2));
    }
    case "aeon_multion_workflow": {
      const result = await multionWorkflow(args.goal as string, args.authJson as string | undefined);
      return text(JSON.stringify(result, null, 2));
    }

    // ── Content (CDP) ──
    case "aeon_page_navigate": {
      const result = args.targetId
        ? await cdpSendToTarget(args.targetId as string, "Page.navigate", { url: args.url })
        : await cdpSendToFirstPage("Page.navigate", { url: args.url });
      return text(JSON.stringify(result, null, 2));
    }

    case "aeon_page_evaluate": {
      const params: Record<string, unknown> = {
        expression: args.expression,
        returnByValue: true,
        awaitPromise: args.awaitPromise !== false,
      };
      const result = args.targetId
        ? await cdpSendToTarget(args.targetId as string, "Runtime.evaluate", params)
        : await cdpSendToFirstPage("Runtime.evaluate", params);
      return text(JSON.stringify(result, null, 2));
    }

    case "aeon_page_screenshot": {
      const params: Record<string, unknown> = {
        format: "png",
        ...(args.fullPage ? { captureBeyondViewport: true } : {}),
      };
      const result = args.targetId
        ? await cdpSendToTarget(args.targetId as string, "Page.captureScreenshot", params)
        : await cdpSendToFirstPage("Page.captureScreenshot", params);
      const data = (result as any).data as string;
      return {
        content: [{ type: "image", data, mimeType: "image/png" }],
      };
    }

    case "aeon_page_click": {
      const ref = args.ref as number;
      const element = findElementByRef(ref);
      if (!element) {
        return text(`Error: No element with ref [${ref}] in last snapshot. Take a new snapshot first.`);
      }
      if (!element.backendDOMNodeId) {
        return text(`Error: Element [${ref}] has no backend DOM node ID. Cannot click.`);
      }

      // Resolve the DOM node's position and click it
      const targets = await cdpListTargets();
      const page = targets.find((t) => t.type === "page");
      if (!page) return text("Error: No page target available.");

      // Get the box model for the node
      const boxResult = await cdpSend(page.webSocketDebuggerUrl, "DOM.getBoxModel", {
        backendNodeId: element.backendDOMNodeId,
      });
      const model = (boxResult as any).model;
      if (!model?.content) {
        return text(`Error: Could not get box model for element [${ref}].`);
      }

      // Click at center of the element
      const [x1, y1, x2, y2, x3, y3, x4, y4] = model.content;
      const cx = (x1 + x3) / 2;
      const cy = (y1 + y3) / 2;

      await cdpSend(page.webSocketDebuggerUrl, "Input.dispatchMouseEvent", {
        type: "mousePressed",
        x: cx,
        y: cy,
        button: "left",
        clickCount: 1,
      });
      await cdpSend(page.webSocketDebuggerUrl, "Input.dispatchMouseEvent", {
        type: "mouseReleased",
        x: cx,
        y: cy,
        button: "left",
        clickCount: 1,
      });

      return text(`Clicked element [${ref}] (${element.role}: "${element.name}") at (${cx}, ${cy})`);
    }

    case "aeon_page_type": {
      const ref = args.ref as number;
      const inputText = args.text as string;
      const clearFirst = args.clearFirst !== false;

      const element = findElementByRef(ref);
      if (!element) {
        return text(`Error: No element with ref [${ref}] in last snapshot. Take a new snapshot first.`);
      }

      const targets = await cdpListTargets();
      const page = targets.find((t) => t.type === "page");
      if (!page) return text("Error: No page target available.");

      // Focus the element
      if (element.backendDOMNodeId) {
        await cdpSend(page.webSocketDebuggerUrl, "DOM.focus", {
          backendNodeId: element.backendDOMNodeId,
        });
      }

      // Clear existing content if requested
      if (clearFirst) {
        await cdpSend(page.webSocketDebuggerUrl, "Input.dispatchKeyEvent", {
          type: "keyDown",
          key: "a",
          code: "KeyA",
          modifiers: 2, // Ctrl
        });
        await cdpSend(page.webSocketDebuggerUrl, "Input.dispatchKeyEvent", {
          type: "keyUp",
          key: "a",
          code: "KeyA",
          modifiers: 2,
        });
      }

      // Type each character
      for (const char of inputText) {
        await cdpSend(page.webSocketDebuggerUrl, "Input.dispatchKeyEvent", {
          type: "keyDown",
          text: char,
        });
        await cdpSend(page.webSocketDebuggerUrl, "Input.dispatchKeyEvent", {
          type: "keyUp",
          text: char,
        });
      }

      return text(`Typed "${inputText}" into element [${ref}] (${element.role}: "${element.name}")`);
    }

    case "aeon_page_scroll": {
      const targets = await cdpListTargets();
      const page = targets.find((t) => t.type === "page");
      if (!page) return text("Error: No page target available.");

      if (args.ref !== undefined) {
        // Scroll element into view
        const element = findElementByRef(args.ref as number);
        if (!element?.backendDOMNodeId) {
          return text(`Error: No element with ref [${args.ref}] found.`);
        }
        await cdpSend(page.webSocketDebuggerUrl, "DOM.scrollIntoViewIfNeeded", {
          backendNodeId: element.backendDOMNodeId,
        });
        return text(`Scrolled element [${args.ref}] (${element.role}: "${element.name}") into view.`);
      }

      const direction = (args.direction as string) ?? "down";
      const amount = (args.amount as number) ?? 600;

      if (direction === "top" || direction === "bottom") {
        const scrollY = direction === "top" ? 0 : 99999;
        await cdpSend(page.webSocketDebuggerUrl, "Runtime.evaluate", {
          expression: `window.scrollTo(0, ${scrollY})`,
          returnByValue: true,
        });
        return text(`Scrolled to ${direction} of page.`);
      }

      const deltaY = direction === "up" ? -amount : amount;
      await cdpSend(page.webSocketDebuggerUrl, "Input.dispatchMouseEvent", {
        type: "mouseWheel",
        x: 400,
        y: 300,
        deltaX: 0,
        deltaY,
      });
      return text(`Scrolled ${direction} by ${Math.abs(deltaY)}px.`);
    }

    case "aeon_page_wait": {
      const condition = args.condition as string;
      const value = args.value as string | undefined;
      const timeout = (args.timeoutMs as number) ?? 10000;
      const pollInterval = 250;
      const maxPolls = Math.ceil(timeout / pollInterval);

      const targets = await cdpListTargets();
      const page = targets.find((t) => t.type === "page");
      if (!page) return text("Error: No page target available.");

      for (let i = 0; i < maxPolls; i++) {
        let done = false;

        if (condition === "load") {
          const result = await cdpSend(page.webSocketDebuggerUrl, "Runtime.evaluate", {
            expression: "document.readyState",
            returnByValue: true,
          });
          done = (result as any)?.result?.value === "complete";
        } else if (condition === "idle") {
          // Simple heuristic: check if document is complete and no pending XHRs
          const result = await cdpSend(page.webSocketDebuggerUrl, "Runtime.evaluate", {
            expression:
              "document.readyState === 'complete' && " +
              "(typeof performance !== 'undefined' ? performance.getEntriesByType('resource').every(r => r.responseEnd > 0) : true)",
            returnByValue: true,
          });
          done = (result as any)?.result?.value === true;
        } else if (condition === "text" && value) {
          const result = await cdpSend(page.webSocketDebuggerUrl, "Runtime.evaluate", {
            expression: `document.body && document.body.innerText.includes(${JSON.stringify(value)})`,
            returnByValue: true,
          });
          done = (result as any)?.result?.value === true;
        } else if (condition === "element" && value) {
          const result = await cdpSend(page.webSocketDebuggerUrl, "Runtime.evaluate", {
            expression: `!!document.querySelector(${JSON.stringify(value)})`,
            returnByValue: true,
          });
          done = (result as any)?.result?.value === true;
        }

        if (done) {
          return text(`Wait condition '${condition}'${value ? ` ("${value}")` : ""} met after ${i * pollInterval}ms.`);
        }

        await new Promise((resolve) => setTimeout(resolve, pollInterval));
      }

      return text(`Timeout: condition '${condition}'${value ? ` ("${value}")` : ""} not met after ${timeout}ms.`);
    }

    case "aeon_page_press_key": {
      const key = args.key as string;
      const modifiers = (args.modifiers as number) ?? 0;

      const targets = await cdpListTargets();
      const page = targets.find((t) => t.type === "page");
      if (!page) return text("Error: No page target available.");

      // Map common key names to their CDP key/code values
      const keyMap: Record<string, { key: string; code: string }> = {
        Enter: { key: "Enter", code: "Enter" },
        Tab: { key: "Tab", code: "Tab" },
        Escape: { key: "Escape", code: "Escape" },
        Backspace: { key: "Backspace", code: "Backspace" },
        Delete: { key: "Delete", code: "Delete" },
        Space: { key: " ", code: "Space" },
        ArrowDown: { key: "ArrowDown", code: "ArrowDown" },
        ArrowUp: { key: "ArrowUp", code: "ArrowUp" },
        ArrowLeft: { key: "ArrowLeft", code: "ArrowLeft" },
        ArrowRight: { key: "ArrowRight", code: "ArrowRight" },
        Home: { key: "Home", code: "Home" },
        End: { key: "End", code: "End" },
        PageUp: { key: "PageUp", code: "PageUp" },
        PageDown: { key: "PageDown", code: "PageDown" },
      };

      const mapped = keyMap[key] ?? { key, code: key };

      await cdpSend(page.webSocketDebuggerUrl, "Input.dispatchKeyEvent", {
        type: "keyDown",
        key: mapped.key,
        code: mapped.code,
        modifiers,
      });
      await cdpSend(page.webSocketDebuggerUrl, "Input.dispatchKeyEvent", {
        type: "keyUp",
        key: mapped.key,
        code: mapped.code,
        modifiers,
      });

      const modStr = modifiers
        ? ` with modifiers=${modifiers}`
        : "";
      return text(`Pressed key: ${key}${modStr}`);
    }

    case "aeon_page_hover": {
      const ref = args.ref as number;
      const element = findElementByRef(ref);
      if (!element?.backendDOMNodeId) {
        return text(`Error: No element with ref [${ref}] in last snapshot.`);
      }

      const targets = await cdpListTargets();
      const page = targets.find((t) => t.type === "page");
      if (!page) return text("Error: No page target available.");

      const boxResult = await cdpSend(page.webSocketDebuggerUrl, "DOM.getBoxModel", {
        backendNodeId: element.backendDOMNodeId,
      });
      const model = (boxResult as any).model;
      if (!model?.content) {
        return text(`Error: Could not get box model for element [${ref}].`);
      }

      const [x1, y1, , , x3, , , y3] = model.content;
      const cx = (x1 + x3) / 2;
      const cy = (y1 + (y3 ?? model.content[5])) / 2;

      await cdpSend(page.webSocketDebuggerUrl, "Input.dispatchMouseEvent", {
        type: "mouseMoved",
        x: cx,
        y: cy,
      });

      return text(`Hovered over element [${ref}] (${element.role}: "${element.name}") at (${cx}, ${cy})`);
    }

    case "aeon_page_select_option": {
      const ref = args.ref as number;
      const element = findElementByRef(ref);
      if (!element?.backendDOMNodeId) {
        return text(`Error: No element with ref [${ref}] in last snapshot.`);
      }

      const targets = await cdpListTargets();
      const page = targets.find((t) => t.type === "page");
      if (!page) return text("Error: No page target available.");

      // Resolve the node to get its objectId
      const resolveResult = await cdpSend(page.webSocketDebuggerUrl, "DOM.resolveNode", {
        backendNodeId: element.backendDOMNodeId,
      });
      const objectId = (resolveResult as any)?.object?.objectId;
      if (!objectId) {
        return text(`Error: Could not resolve DOM node for element [${ref}].`);
      }

      const selectValue = args.value as string | undefined;
      const selectText = args.text as string | undefined;

      // Use JS to select the option
      const jsExpr = selectValue
        ? `(function(el){ for(let o of el.options){if(o.value===${JSON.stringify(selectValue)}){el.value=o.value;el.dispatchEvent(new Event('change',{bubbles:true}));return o.text;}} return null; })`
        : `(function(el){ for(let o of el.options){if(o.text.includes(${JSON.stringify(selectText ?? "")}))){el.value=o.value;el.dispatchEvent(new Event('change',{bubbles:true}));return o.text;}} return null; })`;

      const result = await cdpSend(page.webSocketDebuggerUrl, "Runtime.callFunctionOn", {
        objectId,
        functionDeclaration: jsExpr,
        arguments: [{ objectId }],
        returnByValue: true,
      });

      const selectedText = (result as any)?.result?.value;
      if (selectedText) {
        return text(`Selected "${selectedText}" in element [${ref}] (${element.role}: "${element.name}")`);
      }
      return text(`Error: Could not find matching option in element [${ref}].`);
    }

    case "aeon_page_fill_form": {
      const fields = args.fields as Array<{ ref: number; value: string }>;
      if (!fields || !Array.isArray(fields)) {
        return text("Error: 'fields' must be an array of {ref, value} objects.");
      }

      const targets = await cdpListTargets();
      const page = targets.find((t) => t.type === "page");
      if (!page) return text("Error: No page target available.");

      const results: string[] = [];

      for (const field of fields) {
        const element = findElementByRef(field.ref);
        if (!element?.backendDOMNodeId) {
          results.push(`[${field.ref}] ❌ Element not found`);
          continue;
        }

        try {
          // Focus
          await cdpSend(page.webSocketDebuggerUrl, "DOM.focus", {
            backendNodeId: element.backendDOMNodeId,
          });

          // Select all
          await cdpSend(page.webSocketDebuggerUrl, "Input.dispatchKeyEvent", {
            type: "keyDown", key: "a", code: "KeyA", modifiers: 2,
          });
          await cdpSend(page.webSocketDebuggerUrl, "Input.dispatchKeyEvent", {
            type: "keyUp", key: "a", code: "KeyA", modifiers: 2,
          });

          // Type each character
          for (const char of field.value) {
            await cdpSend(page.webSocketDebuggerUrl, "Input.dispatchKeyEvent", {
              type: "keyDown", text: char,
            });
            await cdpSend(page.webSocketDebuggerUrl, "Input.dispatchKeyEvent", {
              type: "keyUp", text: char,
            });
          }

          results.push(`[${field.ref}] ✅ Filled "${field.value}"`);
        } catch (e: any) {
          results.push(`[${field.ref}] ❌ ${e.message}`);
        }
      }

      return text(`Form fill results:\n${results.join("\n")}`);
    }

    case "aeon_cdp_raw": {
      const result = args.targetId
        ? await cdpSendToTarget(
            args.targetId as string,
            args.method as string,
            (args.params as Record<string, unknown>) ?? {}
          )
        : await cdpSendToFirstPage(
            args.method as string,
            (args.params as Record<string, unknown>) ?? {}
          );
      return text(JSON.stringify(result, null, 2));
    }

    // ── Diagnostics ──
    case "aeon_health": {
      const [pipe, cdp] = await Promise.allSettled([pipeHealthCheck(), cdpHealthCheck()]);
      const pipeOk = pipe.status === "fulfilled" && pipe.value;
      const cdpOk = cdp.status === "fulfilled" && cdp.value;

      let versionStr = "";
      if (cdpOk) {
        try {
          const ver = await cdpVersion();
          versionStr = ` (${ver["Browser"]})`;
        } catch { /* ignore */ }
      }

      return text(
        `Shell (Named Pipe): ${pipeOk ? "✅ Connected" : "❌ Disconnected"}\n` +
        `CDP (WebSocket):    ${cdpOk ? `✅ Connected${versionStr}` : "❌ Disconnected"}`
      );
    }

    case "aeon_targets": {
      const targets = await cdpListTargets();
      const lines = targets.map(
        (t) => `[${t.type}] ${t.title}\n  URL: ${t.url}\n  ID: ${t.id}`
      );
      return text(lines.join("\n\n") || "No targets found.");
    }

    // ── Validation ──
    case "aeon_validate": {
      const snap = await generateSnapshot();
      lastSnapshot = snap;
      const checks: string[] = [];
      let allPassed = true;

      if (args.expectUrl) {
        const match = snap.page?.url?.includes(args.expectUrl as string);
        checks.push(`URL contains "${args.expectUrl}": ${match ? "✅" : "❌"} (actual: ${snap.page?.url})`);
        if (!match) allPassed = false;
      }

      if (args.expectTitle) {
        const match = snap.page?.title?.includes(args.expectTitle as string);
        checks.push(`Title contains "${args.expectTitle}": ${match ? "✅" : "❌"} (actual: ${snap.page?.title})`);
        if (!match) allPassed = false;
      }

      if (args.expectText) {
        // Use Runtime.evaluate to search for text on the page
        try {
          const result = await cdpSendToFirstPage("Runtime.evaluate", {
            expression: `document.body.innerText.includes(${JSON.stringify(args.expectText)})`,
            returnByValue: true,
          });
          const found = (result as any)?.result?.value === true;
          checks.push(`Page contains text "${args.expectText}": ${found ? "✅" : "❌"}`);
          if (!found) allPassed = false;
        } catch (e) {
          checks.push(`Page text check failed: ${e}`);
          allPassed = false;
        }
      }

      if (args.expectElementWithRef !== undefined) {
        const el = snap.page?.interactiveElements?.find(
          (e) => e.ref === (args.expectElementWithRef as number)
        );
        checks.push(
          `Element with ref [${args.expectElementWithRef}] exists: ${el ? "✅" : "❌"}`
        );
        if (!el) allPassed = false;
      }

      const status = allPassed ? "✅ VALIDATION PASSED" : "❌ VALIDATION FAILED";
      return text(`${status}\n\n${checks.join("\n")}`);
    }

    case "aeon_llm_status": {
      const health = await llmHealthCheck();
      const models = health.available ? await llmListModels() : [];

      const lines = [
        `## LLM Status`,
        ``,
        `**Available:** ${health.available ? "✅ Yes" : "❌ No"}`,
        `**Backend:** ${(health as any).backend === "embedded" ? "🧠 Embedded (node-llama-cpp)" : "🌐 External"}`,
        `**Endpoint:** ${health.url}`,
        `**Model:** ${health.model}`,
      ];

      if (health.error) {
        lines.push(`**Error:** ${health.error}`);
      }

      if (models.length > 0) {
        lines.push(``, `### Loaded Models`);
        for (const m of models) {
          const sizeMB = m.size > 0 ? ` (${(m.size / 1_000_000).toFixed(0)} MB)` : ``;
          lines.push(`- **${m.name}**${sizeMB}`);
        }
      }

      return text(lines.join("\n"));
    }

    // ── RSI Tools ──
    case "aeon_rsi_status": {
      const status = rsiStatus();
      const lines = [
        `## 🧬 RSI Engine Status`,
        ``,
        `**Engine:** ${status.running ? "🟢 Running" : "🔴 Stopped"}`,
        `**Cycles Completed:** ${status.cycleCount}`,
        `**Feedback Entries:** ${status.totalFeedback} (min: ${status.minEntries})`,
        `**Cycle Interval:** ${status.intervalMinutes} minutes`,
      ];

      if (status.lastResult) {
        lines.push(``, `### Last Cycle Result`, status.lastResult);
      }

      if (status.opportunities.length > 0) {
        lines.push(``, `### Detected Opportunities`);
        for (const opp of status.opportunities) {
          lines.push(`- ${opp.split("\n")[0]}`);
        }
      } else {
        lines.push(``, `_No actionable patterns detected yet._`);
      }

      return text(lines.join("\n"));
    }

    case "aeon_feedback_analyze": {
      const query = (args.query as string) ?? "summary";

      switch (query) {
        case "summary": {
          const summary = generateSummary();
          const lines = [
            `## Feedback Ledger Summary`,
            ``,
            `**Total Events:** ${summary.totalEntries}`,
            ``,
            `### Event Breakdown`,
          ];
          for (const [type, count] of Object.entries(summary.eventBreakdown)) {
            lines.push(`- **${type}**: ${count}`);
          }
          return text(lines.join("\n"));
        }
        case "tool_stats": {
          const stats = toolStats();
          if (stats.length === 0) return text("No tool execution data yet.");
          const lines = [
            `## Tool Performance`,
            ``,
            `| Tool | Total | OK | Fail | Success Rate |`,
            `|------|------:|---:|-----:|-------------:|`,
          ];
          for (const s of stats) {
            lines.push(`| ${s.dimension} | ${s.totalEvents} | ${s.successes} | ${s.failures} | ${(s.successRate * 100).toFixed(0)}% |`);
          }
          return text(lines.join("\n"));
        }
        case "failures":
          return text(entriesByType("tool_failure", 20).map(f =>
            `- [${f.timestamp}] **${f.dimension}**: ${f.metadata ?? "(no details)"}`
          ).join("\n") || "No failures recorded.");
        case "improvements":
          return text(listImprovements());
        case "digest":
          return text(formatDigest());
        case "opportunities":
          const opps = detectOpportunities();
          return text(opps.length > 0 ? opps.join("\n\n") : "No actionable opportunities detected.");
        default:
          return text(`Unknown query: '${query}'. Use: summary, tool_stats, failures, improvements, digest, opportunities`);
      }
    }

    case "aeon_self_improve": {
      const action = (args.action as string) ?? "";

      switch (action) {
        case "trigger": {
          const result = await rsiTick();
          return text(`## RSI Cycle Result\n\n${result}`);
        }
        case "read": {
          const targetFile = args.target_file as BrainFile;
          if (!targetFile) return text("Error: target_file is required for 'read'.");
          if (!ALLOWED_BRAIN_FILES.includes(targetFile)) {
            return text(`Error: target_file must be one of: ${ALLOWED_BRAIN_FILES.join(", ")}`);
          }
          const content = readBrainFile(targetFile);
          if (!content) return text(`${targetFile} does not exist yet.`);
          return text(`## ${targetFile}\n\n${content}`);
        }
        case "apply": {
          const tf = args.target_file as BrainFile;
          const content = args.content as string;
          const desc = args.description as string;
          const rationale = (args.rationale as string) ?? "";
          if (!tf || !content || !desc) {
            return text("Error: target_file, content, and description are required for 'apply'.");
          }
          const result = appendToBrainFile(tf, content, desc, rationale);
          return text(result.success
            ? `✅ Applied to ${tf}: ${desc}`
            : `❌ Failed: ${result.error}`);
        }
        case "update": {
          const tf = args.target_file as BrainFile;
          const content = args.content as string;
          const oldContent = args.old_content as string;
          const desc = args.description as string;
          const rationale = (args.rationale as string) ?? "";
          if (!tf || !content || !oldContent || !desc) {
            return text("Error: target_file, content, old_content, and description are required for 'update'.");
          }
          const result = updateBrainFile(tf, oldContent, content, desc, rationale);
          return text(result.success
            ? `✅ Updated ${tf}: ${desc}`
            : `❌ Failed: ${result.error}`);
        }
        default:
          return text(`Unknown action: '${action}'. Use: trigger, read, apply, update`);
      }
    }

    // ── Memory Tools ──
    case "aeon_memory_store": {
      const content = args.content as string;
      if (!content) return text("Error: 'content' is required.");

      const result = storeMemory(content, {
        tags: args.tags as string[] | undefined,
        importance: args.importance as number | undefined,
        ttlHours: args.ttl_hours as number | undefined,
        source: "agent",
      });

      // Handle dedup response
      if ("duplicate" in result) {
        return text(`⚠️ Duplicate detected — existing memory: **${result.existingId}**\n\nUse aeon_memory_update to modify it, or aeon_memory_get to view it.`);
      }

      return text(`✅ Memory stored: **${result.id}**\n\nContent: ${result.content.slice(0, 200)}\nTags: ${result.tags.join(", ") || "(none)"}\nImportance: ${result.importance}`);
    }

    case "aeon_memory_search": {
      const query = args.query as string;
      if (!query) return text("Error: 'query' is required.");

      const results = await searchMemory(query, {
        tags: args.tags as string[] | undefined,
        limit: args.limit as number | undefined,
      });

      if (results.length === 0) return text(`No memories found for: "${query}"`);

      const lines = [`## Memory Search: "${query}"`, ``, `**${results.length} results:**`, ``];
      for (const m of results) {
        const tags = m.tags.length > 0 ? ` [${m.tags.join(", ")}]` : "";
        lines.push(`- **${m.id}** (${m.importance.toFixed(1)}⭐): ${m.content.slice(0, 150)}${tags}`);
      }
      return text(lines.join("\n"));
    }

    case "aeon_memory_list": {
      const results = listMemories({
        tags: args.tags as string[] | undefined,
        source: args.source as string | undefined,
        limit: args.limit as number | undefined,
      });

      if (results.length === 0) return text("No memories stored yet.");

      const stats = memoryStats();
      const lines = [
        `## Agent Memory (${stats.totalMemories} total)`, ``,
        `| ID | Content | Tags | Importance |`,
        `|----|---------|------|-----------|`,
      ];
      for (const m of results) {
        lines.push(`| ${m.id} | ${m.content.slice(0, 60)} | ${m.tags.join(", ") || "-"} | ${m.importance.toFixed(1)} |`);
      }
      return text(lines.join("\n"));
    }

    case "aeon_memory_delete": {
      const id = args.id as string;
      if (!id) return text("Error: 'id' is required.");
      const deleted = deleteMemory(id);
      return text(deleted ? `✅ Memory deleted: ${id}` : `❌ Memory not found: ${id}`);
    }

    case "aeon_memory_get": {
      const id = args.id as string;
      if (!id) return text("Error: 'id' is required.");
      const mem = getMemory(id);
      if (!mem) return text(`❌ Memory not found: ${id}`);

      const lines = [
        `## Memory: ${mem.id}`,
        ``,
        `**Content:** ${mem.content}`,
        `**Tags:** ${mem.tags.join(", ") || "(none)"}`,
        `**Source:** ${mem.source}`,
        `**Importance:** ${mem.importance.toFixed(1)}`,
        `**Created:** ${mem.timestamp}`,
        `**Access Count:** ${mem.accessCount}`,
      ];
      if (mem.lastAccessed) lines.push(`**Last Accessed:** ${mem.lastAccessed}`);
      if (mem.expiresAt) lines.push(`**Expires:** ${mem.expiresAt}`);
      if (mem.contentHash) lines.push(`**Hash:** ${mem.contentHash}`);
      return text(lines.join("\n"));
    }

    case "aeon_memory_update": {
      const id = args.id as string;
      const content = args.content as string;
      if (!id || !content) return text("Error: 'id' and 'content' are required.");

      const updated = updateMemory(id, content, {
        tags: args.tags as string[] | undefined,
        importance: args.importance as number | undefined,
      });

      if (!updated) return text(`❌ Memory not found or update would create duplicate: ${id}`);
      return text(
        `✅ Memory updated: **${updated.id}**\n\n` +
        `Content: ${updated.content.slice(0, 200)}\n` +
        `Tags: ${updated.tags.join(", ") || "(none)"}\n` +
        `Importance: ${updated.importance}`
      );
    }

    case "aeon_memory_history": {
      const memId = args.memory_id as string | undefined;
      const limit = (args.limit as number) ?? 20;
      const history = memoryHistory(memId, limit);

      if (history.length === 0) return text("No memory history recorded yet.");

      const lines = [
        `## Memory History${memId ? ` for ${memId}` : ""}`,
        ``,
        `| Timestamp | Event | Memory ID | Old → New |`,
        `|-----------|-------|-----------|-----------|`,
      ];
      for (const h of history) {
        const oldStr = h.oldContent ? h.oldContent.slice(0, 40) : "—";
        const newStr = h.newContent ? h.newContent.slice(0, 40) : "—";
        lines.push(`| ${h.timestamp.slice(0, 19)} | ${h.event} | ${h.memoryId} | ${oldStr} → ${newStr} |`);
      }
      return text(lines.join("\n"));
    }

    // ── Scheduler Tools ──
    case "aeon_schedule_create": {
      const goal = args.goal as string;
      const type = args.type as ScheduleType;
      if (!goal || !type) return text("Error: 'goal' and 'type' are required.");

      let intervalMs: number | undefined;
      if (type === "interval") {
        const intervalStr = args.interval as string;
        if (!intervalStr) return text("Error: 'interval' is required for type 'interval'.");
        intervalMs = parseInterval(intervalStr) ?? undefined;
        if (!intervalMs) return text(`Error: Could not parse interval '${intervalStr}'. Use format like '5 minutes', '1 hour'.`);
        if (intervalMs < 60_000) return text("Error: Minimum interval is 1 minute.");
      }

      const cronExpr = args.cron_expression as string | undefined;

      const result = createSchedule(goal, type, {
        intervalMs,
        executeAt: args.execute_at as string | undefined,
        dailyTime: args.daily_time as string | undefined,
        cronExpression: cronExpr,
        maxRuns: args.max_runs as number | undefined,
      });

      // Check if createSchedule returned an error
      if ('error' in result) {
        return text(`❌ ${result.error}`);
      }

      const task = result;
      const lines = [
        `✅ Schedule created: **${task.id}**`,
        ``,
        `**Goal:** ${task.goal}`,
        `**Type:** ${task.type}`,
      ];
      if (task.intervalMs) lines.push(`**Interval:** ${Math.round(task.intervalMs / 60_000)}m`);
      if (task.executeAt) lines.push(`**Execute At:** ${task.executeAt}`);
      if (task.dailyTime) lines.push(`**Daily At:** ${task.dailyTime}`);
      if (task.cronExpression) lines.push(`**Cron:** \`${task.cronExpression}\` (${describeCron(task.cronExpression)})`);
      if (task.maxRuns) lines.push(`**Max Runs:** ${task.maxRuns}`);

      return text(lines.join("\n"));
    }

    case "aeon_schedule_list": {
      const schedules = listSchedules();
      if (schedules.length === 0) return text("No scheduled tasks.");

      const stats = schedulerStats();
      const lines = [
        `## Scheduled Tasks (${stats.totalTasks} total, ${stats.activeTasks} active)`,
        ``,
        `| ID | Goal | Type | Status | Runs |`,
        `|----|------|------|--------|------|`,
      ];
      for (const s of schedules) {
        const status = s.enabled ? "🟢" : "⏸️";
        lines.push(`| ${s.id} | ${s.goal.slice(0, 40)} | ${s.type} | ${status} | ${s.runCount} |`);
      }
      return text(lines.join("\n"));
    }

    case "aeon_schedule_delete": {
      const id = args.id as string;
      if (!id) return text("Error: 'id' is required.");
      const deleted = deleteSchedule(id);
      return text(deleted ? `✅ Schedule deleted: ${id}` : `❌ Schedule not found: ${id}`);
    }

    case "aeon_schedule_toggle": {
      const id = args.id as string;
      const enabled = args.enabled as boolean;
      if (!id || enabled === undefined) return text("Error: 'id' and 'enabled' are required.");
      const task = toggleSchedule(id, enabled);
      if (!task) return text(`❌ Schedule not found: ${id}`);
      return text(`✅ Schedule ${task.id} ${enabled ? "enabled" : "disabled"}`);
    }

    // ── Cache Tools ──
    case "aeon_cache_stats": {
      const action = (args.action as string) ?? "stats";
      if (action === "clear") {
        cacheClear();
        return text("✅ Cache cleared.");
      }
      const stats = cacheStats();
      const lines = [
        `## 🗄️ Cache Stats`,
        ``,
        `**Entries:** ${stats.totalEntries} / ${stats.maxEntries}`,
        `**Hit Rate:** ${(stats.hitRate * 100).toFixed(1)}%`,
        `**Miss Rate:** ${(stats.missRate * 100).toFixed(1)}%`,
        `**Total Hits:** ${stats.totalHits}`,
        `**Total Misses:** ${stats.totalMisses}`,
        `**Oldest Entry:** ${stats.oldestEntryAge}s ago`,
      ];
      return text(lines.join("\n"));
    }

    // ── Self-Update ──
    case "aeon_self_update": {
      const dryRun = (args.dry_run as boolean) ?? false;

      try {
        const selfUpdateModule = await import("./self-update.js");
        const result = await selfUpdateModule.selfUpdate({ dryRun });

        const lines = [
          `## 🔄 Self-Update ${result.success ? "✅ Success" : "❌ Failed"}`,
          ``,
          `**Build Duration:** ${result.buildDurationMs}ms`,
          `**Mode:** ${dryRun ? "Dry Run (no restart signal)" : "Full Update"}`,
        ];

        if (result.stdout) lines.push(``, `**Build Output:**`, `\`\`\``, result.stdout.slice(-1000), `\`\`\``);
        if (result.error) lines.push(``, `**Error:**`, `\`\`\``, result.error, `\`\`\``);

        if (result.success && !dryRun) {
          lines.push(``, `> [!IMPORTANT]`, `> Server rebuild complete. The host process should restart the MCP server to pick up changes.`);
        }

        return text(lines.join("\n"));
      } catch (err: any) {
        return text(`❌ Self-update failed: ${err.message}`);
      }
    }

    // ── Autonomous Task Planner ──
    case "aeon_task": {
      const goal = args.goal as string;
      if (!goal?.trim()) {
        return text("❌ Missing required parameter: goal");
      }

      try {
        const taskState = await runTask(goal);
        return text(formatTaskResult(taskState));
      } catch (err: any) {
        return text(`❌ Task execution failed: ${err.message}`);
      }
    }

    case "aeon_task_status": {
      const taskId = args.taskId as string;
      if (!taskId?.trim()) {
        return text("❌ Missing required parameter: taskId");
      }

      const state = getTaskState(taskId);
      if (!state) {
        return text(`❌ Task not found: ${taskId}`);
      }
      return text(formatTaskResult(state));
    }

    default:
      return text(`Unknown tool: ${name}`);
  }
}

// ─────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────

function text(content: string) {
  return { content: [{ type: "text" as const, text: content }] };
}

function findElementByRef(ref: number) {
  if (!lastSnapshot?.page?.interactiveElements) return null;
  return lastSnapshot.page.interactiveElements.find((e) => e.ref === ref) ?? null;
}

// ─────────────────────────────────────────────────────────────
// Server Setup
// ─────────────────────────────────────────────────────────────

const server = new Server(
  {
    name: "aeon-mcp",
    version: "0.4.0",
  },
  {
    capabilities: {
      tools: {},
      resources: {},
    },
  }
);

// Register tool listing
server.setRequestHandler(ListToolsRequestSchema, async () => ({ tools }));

// Register tool execution
server.setRequestHandler(CallToolRequestSchema, async (request) => {
  const { name, arguments: args = {} } = request.params;
  const sessionId = `mcp_${Date.now().toString(36)}`;
  try {
    const result = await handleToolCall(name, args as Record<string, unknown>);
    // Record success to feedback ledger (skip internal RSI tools to avoid recursion)
    if (!name.startsWith("aeon_rsi") && !name.startsWith("aeon_feedback") && !name.startsWith("aeon_self_improve")) {
      recordToolSuccess(name, sessionId);
    }
    return result;
  } catch (error: any) {
    // Record failure to feedback ledger
    if (!name.startsWith("aeon_rsi") && !name.startsWith("aeon_feedback") && !name.startsWith("aeon_self_improve")) {
      recordToolFailure(name, sessionId, error.message);
    }
    return text(`Error executing ${name}: ${error.message}`);
  }
});

// Register resource listing (browser state resources)
server.setRequestHandler(ListResourcesRequestSchema, async () => ({
  resources: [
    {
      uri: "aeon://browser/state",
      name: "Browser State",
      description: "Current Aeon Browser state snapshot",
      mimeType: "application/json",
    },
  ],
}));

// Register resource reading
server.setRequestHandler(ReadResourceRequestSchema, async (request) => {
  if (request.params.uri === "aeon://browser/state") {
    const snap = await generateSnapshot();
    return {
      contents: [
        {
          uri: "aeon://browser/state",
          mimeType: "application/json",
          text: JSON.stringify(snap, null, 2),
        },
      ],
    };
  }
  throw new Error(`Unknown resource: ${request.params.uri}`);
});

// ─────────────────────────────────────────────────────────────
// Launch
// ─────────────────────────────────────────────────────────────

async function main() {
  // Register the tool executor for the planner (breaks circular dep)
  setToolExecutor(async (toolName: string, args: Record<string, unknown>) => {
    return handleToolCall(toolName, args);
  });

  // Register the scheduler executor (uses the planner via runTask)
  setSchedulerExecutor(async (goal: string) => {
    const { runTask, formatTaskResult } = await import("./planner.js");
    const taskState = await runTask(goal);
    return formatTaskResult(taskState);
  });

  // Initialize the TTL cache (cleanup timer)
  initCache();

  // Start the RSI (Recursive Self-Improvement) background engine
  startRsiEngine();

  // Initialize the task scheduler (loads persisted schedules)
  initScheduler();

  const transport = new StdioServerTransport();
  await server.connect(transport);
  console.error("[aeon-mcp] Server started on stdio transport");
  console.error(`[aeon-mcp] ${tools.length} tools registered: ${tools.map((t) => t.name).join(", ")}`);
  console.error("[aeon-mcp] RSI engine initialized — self-improvement active");
  console.error("[aeon-mcp] Task scheduler initialized");

  // Initialize embedding engine (non-blocking, for semantic memory search)
  initEmbeddingEngine().then((embedFn) => {
    if (embedFn) {
      registerEmbeddingFn(embedFn);
      console.error("[aeon-mcp] ✅ Semantic search activated — embedding engine wired to memory");
    } else {
      console.error("[aeon-mcp] ℹ Running in keyword-only memory search mode");
    }
  }).catch((err) => {
    console.error(`[aeon-mcp] ⚠ Embedding init error (non-fatal): ${err.message}`);
  });
}

main().catch((err) => {
  console.error("[aeon-mcp] Fatal error:", err);
  process.exit(1);
});
