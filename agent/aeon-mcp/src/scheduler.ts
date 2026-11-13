/**
 * Task Scheduler — Cron-Like Browser Automation
 *
 * Allows the agent to schedule recurring or one-shot tasks:
 *   - Full cron expressions (e.g. every-5-min, weekday schedules)
 *   - Human-readable intervals ("every 5 minutes", "daily at 9am")
 *   - One-shot delayed execution ("in 30 minutes")
 *   - Persisted to disk so schedules survive restarts
 *   - Each scheduled task fires the planner with a goal string
 *   - Worker thread execution prevents blocking the MCP main thread
 *
 * Data lives at ~/.aeon/scheduler/schedules.json
 *
 * Cron parser ported from node-cron patterns.
 * Worker thread execution inspired by node-cron BackgroundScheduledTask.
 *
 * @module scheduler
 */

import {
  readFileSync,
  writeFileSync,
  existsSync,
  mkdirSync,
} from "node:fs";
import { join } from "node:path";
import { homedir } from "node:os";
import { executeWithIsolation } from "./task-worker.js";

// ── Paths ───────────────────────────────────────────────────

const AEON_HOME = join(homedir(), ".aeon");
const SCHEDULER_DIR = join(AEON_HOME, "scheduler");
const SCHEDULES_FILE = join(SCHEDULER_DIR, "schedules.json");

// ── Types ───────────────────────────────────────────────────

export type ScheduleType = "interval" | "once" | "daily" | "cron";

export interface ScheduledTask {
  id: string;
  goal: string;           // What to tell the planner
  type: ScheduleType;
  intervalMs?: number;     // For "interval" type
  executeAt?: string;      // ISO 8601 for "once" type
  dailyTime?: string;      // "HH:MM" for "daily" type
  cronExpression?: string; // For "cron" type (5-field: min hour dom month dow)
  timezone?: string;       // Default: EST
  enabled: boolean;
  createdAt: string;
  lastRunAt?: string;
  lastResult?: string;
  runCount: number;
  maxRuns?: number;        // Stop after N runs (optional)
  tags?: string[];
}

// ── Cron Expression Parser ──────────────────────────────────

interface CronField {
  values: Set<number>;
}

interface ParsedCron {
  minute: CronField;
  hour: CronField;
  dayOfMonth: CronField;
  month: CronField;
  dayOfWeek: CronField;
}

/**
 * Parse a single cron field (e.g., step: "[star]/5", range: "1-5", list: "1,3,5", wildcard: "*").
 * @param field - The cron field string
 * @param min - Minimum valid value
 * @param max - Maximum valid value
 */
function parseCronField(field: string, min: number, max: number): CronField {
  const values = new Set<number>();

  for (const part of field.split(",")) {
    const stepMatch = part.match(/^(\*|\d+-\d+)\/(\d+)$/);
    const rangeMatch = part.match(/^(\d+)-(\d+)$/);

    if (part === "*") {
      for (let i = min; i <= max; i++) values.add(i);
    } else if (stepMatch) {
      const step = parseInt(stepMatch[2], 10);
      let start = min;
      let end = max;
      if (stepMatch[1] !== "*") {
        const rangeParts = stepMatch[1].split("-").map(Number);
        start = rangeParts[0];
        end = rangeParts[1];
      }
      for (let i = start; i <= end; i += step) values.add(i);
    } else if (rangeMatch) {
      const start = parseInt(rangeMatch[1], 10);
      const end = parseInt(rangeMatch[2], 10);
      for (let i = start; i <= end; i++) values.add(i);
    } else {
      const num = parseInt(part, 10);
      if (!isNaN(num) && num >= min && num <= max) values.add(num);
    }
  }

  return { values };
}

//
// Parse a 5-field cron expression.
// Format: minute hour day-of-month month day-of-week
// Examples:
//   "0/5 * * * *"     -> every 5 minutes (also: "*/5")
//   "0 9 * * 1-5"     -> 9 AM weekdays
//   "30 2 1 * *"      -> 2:30 AM on the 1st of every month
//   "0 0/2 * * *"     -> every 2 hours
//
export function parseCronExpression(expr: string): ParsedCron | null {
  const fields = expr.trim().split(/\s+/);
  if (fields.length !== 5) return null;

  try {
    return {
      minute: parseCronField(fields[0], 0, 59),
      hour: parseCronField(fields[1], 0, 23),
      dayOfMonth: parseCronField(fields[2], 1, 31),
      month: parseCronField(fields[3], 1, 12),
      dayOfWeek: parseCronField(fields[4], 0, 6), // 0 = Sunday
    };
  } catch {
    return null;
  }
}

/**
 * Validate a cron expression string.
 */
export function validateCron(expr: string): { valid: boolean; error?: string } {
  const fields = expr.trim().split(/\s+/);
  if (fields.length !== 5) {
    return { valid: false, error: `Expected 5 fields, got ${fields.length}. Format: minute hour day-of-month month day-of-week` };
  }
  const parsed = parseCronExpression(expr);
  if (!parsed) {
    return { valid: false, error: "Invalid cron field syntax" };
  }
  if (parsed.minute.values.size === 0 || parsed.hour.values.size === 0) {
    return { valid: false, error: "Cron expression produces no valid fire times" };
  }
  return { valid: true };
}

/**
 * Calculate the next fire time for a parsed cron expression.
 * Scans forward from `from` up to 48 hours.
 */
function nextCronFire(parsed: ParsedCron, from: Date = new Date()): Date | null {
  const candidate = new Date(from.getTime() + 60_000); // Start 1 minute in the future
  candidate.setSeconds(0, 0);

  // Scan up to 48 hours forward (2880 minutes)
  for (let i = 0; i < 2880; i++) {
    const minute = candidate.getMinutes();
    const hour = candidate.getHours();
    const dom = candidate.getDate();
    const month = candidate.getMonth() + 1; // JS months are 0-based
    const dow = candidate.getDay();

    if (
      parsed.minute.values.has(minute) &&
      parsed.hour.values.has(hour) &&
      parsed.dayOfMonth.values.has(dom) &&
      parsed.month.values.has(month) &&
      parsed.dayOfWeek.values.has(dow)
    ) {
      return candidate;
    }

    candidate.setMinutes(candidate.getMinutes() + 1);
  }

  return null; // No match within 48 hours
}

/**
 * Get a human-readable description of a cron expression.
 */
export function describeCron(expr: string): string {
  const parsed = parseCronExpression(expr);
  if (!parsed) return "Invalid cron expression";

  const fields = expr.trim().split(/\s+/);
  const parts: string[] = [];

  if (fields[0] === "*" && fields[1] === "*") parts.push("Every minute");
  else if (fields[0].startsWith("*/")) parts.push(`Every ${fields[0].slice(2)} minutes`);
  else if (fields[1].startsWith("*/")) parts.push(`Every ${fields[1].slice(2)} hours at minute ${fields[0]}`);
  else parts.push(`At ${fields[1].padStart(2, "0")}:${fields[0].padStart(2, "0")}`);

  if (fields[4] !== "*") {
    const days = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
    const dayList = [...parsed.dayOfWeek.values].map(d => days[d] ?? `${d}`).join(", ");
    parts.push(`on ${dayList}`);
  }

  if (fields[2] !== "*") parts.push(`on day ${fields[2]}`);
  if (fields[3] !== "*") {
    const months = ["", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
    const monthList = [...parsed.month.values].map(m => months[m] ?? `${m}`).join(", ");
    parts.push(`in ${monthList}`);
  }

  return parts.join(" ");
}

interface SchedulerState {
  tasks: ScheduledTask[];
  version: number;
}

// ── In-Memory State ─────────────────────────────────────────

let _state: SchedulerState = { tasks: [], version: 1 };
let _timers: Map<string, ReturnType<typeof setTimeout | typeof setInterval>> = new Map();
let _taskExecutor: ((goal: string) => Promise<string>) | null = null;
let _loaded = false;

function ensureSchedulerDir(): void {
  if (!existsSync(SCHEDULER_DIR)) {
    mkdirSync(SCHEDULER_DIR, { recursive: true });
  }
}

function loadState(): void {
  if (_loaded) return;
  ensureSchedulerDir();

  if (existsSync(SCHEDULES_FILE)) {
    try {
      const raw = readFileSync(SCHEDULES_FILE, "utf-8");
      _state = JSON.parse(raw);
    } catch {
      _state = { tasks: [], version: 1 };
    }
  }
  _loaded = true;
}

function saveState(): void {
  ensureSchedulerDir();
  writeFileSync(SCHEDULES_FILE, JSON.stringify(_state, null, 2), "utf-8");
}

function generateId(): string {
  return `sched_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 6)}`;
}

// ── Task Execution ──────────────────────────────────────────

async function executeScheduledTask(task: ScheduledTask): Promise<void> {
  if (!task.enabled) return;
  if (task.maxRuns && task.runCount >= task.maxRuns) {
    task.enabled = false;
    saveState();
    return;
  }

  process.stderr.write(`[Scheduler] Executing task '${task.id}': ${task.goal.slice(0, 80)}\n`);

  task.lastRunAt = new Date().toISOString();
  task.runCount++;

  if (_taskExecutor) {
    // Execute with crash isolation — if the task throws, the scheduler
    // stays alive and logs the error instead of crashing the server
    const workerResult = await executeWithIsolation(task.goal, _taskExecutor);
    task.lastResult = workerResult.success
      ? workerResult.result?.slice(0, 500) ?? "OK"
      : `Error: ${workerResult.error}`;

    process.stderr.write(
      `[Scheduler] Task '${task.id}' ${workerResult.success ? "succeeded" : "failed"} in ${workerResult.durationMs}ms\n`
    );
  } else {
    task.lastResult = "No task executor registered.";
  }

  // Disable one-shot tasks after execution
  if (task.type === "once") {
    task.enabled = false;
  }

  saveState();
}

// ── Timer Management ────────────────────────────────────────

function scheduleTimer(task: ScheduledTask): void {
  // Clear existing timer
  const existing = _timers.get(task.id);
  if (existing) {
    clearTimeout(existing as any);
    clearInterval(existing as any);
    _timers.delete(task.id);
  }

  if (!task.enabled) return;

  switch (task.type) {
    case "interval": {
      if (!task.intervalMs || task.intervalMs < 60_000) return; // Min 1 minute
      const timer = setInterval(() => {
        executeScheduledTask(task).catch(() => {});
      }, task.intervalMs);
      _timers.set(task.id, timer);
      break;
    }

    case "once": {
      if (!task.executeAt) return;
      const delay = new Date(task.executeAt).getTime() - Date.now();
      if (delay <= 0) {
        // Already past — execute immediately
        executeScheduledTask(task).catch(() => {});
        return;
      }
      const timer = setTimeout(() => {
        executeScheduledTask(task).catch(() => {});
        _timers.delete(task.id);
      }, delay);
      _timers.set(task.id, timer);
      break;
    }

    case "daily": {
      if (!task.dailyTime) return;
      const [hours, minutes] = task.dailyTime.split(":").map(Number);
      if (isNaN(hours) || isNaN(minutes)) return;

      // Calculate next fire time
      const now = new Date();
      const target = new Date();
      target.setHours(hours, minutes, 0, 0);

      if (target.getTime() <= now.getTime()) {
        target.setDate(target.getDate() + 1);
      }

      const delay = target.getTime() - now.getTime();

      const timer = setTimeout(() => {
        executeScheduledTask(task).catch(() => {});
        scheduleTimer(task);
      }, delay);
      _timers.set(task.id, timer);
      break;
    }

    case "cron": {
      if (!task.cronExpression) return;
      const parsed = parseCronExpression(task.cronExpression);
      if (!parsed) return;

      const nextFire = nextCronFire(parsed);
      if (!nextFire) {
        process.stderr.write(`[Scheduler] Cron '${task.cronExpression}' has no fire time within 48h\n`);
        return;
      }

      const delay = nextFire.getTime() - Date.now();
      const timer = setTimeout(() => {
        executeScheduledTask(task).catch(() => {});
        // Re-schedule for next cron match
        scheduleTimer(task);
      }, Math.max(delay, 1000));
      _timers.set(task.id, timer);

      process.stderr.write(
        `[Scheduler] Cron '${task.cronExpression}' next fire: ${nextFire.toLocaleString()}\n`
      );
      break;
    }
  }
}

// ── Public API ──────────────────────────────────────────────

/**
 * Register the task executor function (called from index.ts).
 */
export function setSchedulerExecutor(
  executor: (goal: string) => Promise<string>
): void {
  _taskExecutor = executor;
}

/**
 * Initialize the scheduler — load persisted tasks and set up timers.
 */
export function initScheduler(): void {
  loadState();

  let scheduled = 0;
  for (const task of _state.tasks) {
    if (task.enabled) {
      scheduleTimer(task);
      scheduled++;
    }
  }

  if (scheduled > 0) {
    process.stderr.write(
      `[Scheduler] Loaded ${_state.tasks.length} tasks, ${scheduled} active\n`
    );
  }
}

/**
 * Create a new scheduled task.
 */
export function createSchedule(
  goal: string,
  type: ScheduleType,
  options?: {
    intervalMs?: number;
    executeAt?: string;
    dailyTime?: string;
    cronExpression?: string;
    maxRuns?: number;
    tags?: string[];
  }
): ScheduledTask | { error: string } {
  loadState();

  // Validate cron expression if provided
  if (type === "cron") {
    if (!options?.cronExpression) {
      return { error: "cronExpression is required for cron-type schedules" };
    }
    const validation = validateCron(options.cronExpression);
    if (!validation.valid) {
      return { error: `Invalid cron expression: ${validation.error}` };
    }
  }

  const task: ScheduledTask = {
    id: generateId(),
    goal,
    type,
    intervalMs: options?.intervalMs,
    executeAt: options?.executeAt,
    dailyTime: options?.dailyTime,
    cronExpression: options?.cronExpression,
    enabled: true,
    createdAt: new Date().toISOString(),
    runCount: 0,
    maxRuns: options?.maxRuns,
    tags: options?.tags,
  };

  _state.tasks.push(task);
  saveState();
  scheduleTimer(task);

  return task;
}

/**
 * Delete a scheduled task.
 */
export function deleteSchedule(id: string): boolean {
  loadState();

  const existing = _timers.get(id);
  if (existing) {
    clearTimeout(existing as any);
    clearInterval(existing as any);
    _timers.delete(id);
  }

  const before = _state.tasks.length;
  _state.tasks = _state.tasks.filter((t) => t.id !== id);

  if (_state.tasks.length < before) {
    saveState();
    return true;
  }
  return false;
}

/**
 * Enable or disable a scheduled task.
 */
export function toggleSchedule(id: string, enabled: boolean): ScheduledTask | null {
  loadState();

  const task = _state.tasks.find((t) => t.id === id);
  if (!task) return null;

  task.enabled = enabled;
  saveState();

  if (enabled) {
    scheduleTimer(task);
  } else {
    const existing = _timers.get(id);
    if (existing) {
      clearTimeout(existing as any);
      clearInterval(existing as any);
      _timers.delete(id);
    }
  }

  return task;
}

/**
 * List all scheduled tasks.
 */
export function listSchedules(): ScheduledTask[] {
  loadState();
  return [..._state.tasks].sort(
    (a, b) => b.createdAt.localeCompare(a.createdAt)
  );
}

/**
 * Get a specific scheduled task by ID.
 */
export function getSchedule(id: string): ScheduledTask | null {
  loadState();
  return _state.tasks.find((t) => t.id === id) ?? null;
}

/**
 * Get scheduler statistics.
 */
export function schedulerStats(): {
  totalTasks: number;
  activeTasks: number;
  totalRuns: number;
  activeTimers: number;
} {
  loadState();
  return {
    totalTasks: _state.tasks.length,
    activeTasks: _state.tasks.filter((t) => t.enabled).length,
    totalRuns: _state.tasks.reduce((sum, t) => sum + t.runCount, 0),
    activeTimers: _timers.size,
  };
}

/**
 * Stop all timers (called on server shutdown).
 */
export function stopScheduler(): void {
  for (const [id, timer] of _timers) {
    clearTimeout(timer as any);
    clearInterval(timer as any);
  }
  _timers.clear();
}

/**
 * Parse a human-readable interval string to milliseconds.
 * Examples: "5 minutes", "1 hour", "30 seconds", "2 hours"
 */
export function parseInterval(input: string): number | null {
  const match = input.trim().match(/^(\d+)\s*(s|sec|second|seconds|m|min|minute|minutes|h|hr|hour|hours|d|day|days)$/i);
  if (!match) return null;

  const value = parseInt(match[1], 10);
  const unit = match[2].toLowerCase();

  switch (unit) {
    case "s": case "sec": case "second": case "seconds":
      return value * 1000;
    case "m": case "min": case "minute": case "minutes":
      return value * 60_000;
    case "h": case "hr": case "hour": case "hours":
      return value * 3_600_000;
    case "d": case "day": case "days":
      return value * 86_400_000;
    default:
      return null;
  }
}
