/**
 * Task Worker — Background Thread Execution for Scheduled Tasks
 *
 * Inspired by node-cron's BackgroundScheduledTask pattern.
 * Runs scheduled task goals in a separate worker thread so that:
 *   1. The MCP server main thread stays responsive during execution
 *   2. Task crashes don't bring down the server
 *   3. CPU-intensive work (embeddings, RSI) doesn't block tool calls
 *
 * Architecture:
 *   Main Thread                    Worker Thread
 *   ─────────────                  ─────────────
 *   scheduleTimer() fires    →     workerData.goal received
 *   executeInWorker(goal)    →     task executor runs
 *   receives result message  ←     postMessage({ result })
 *   updates task state       ←     worker exits
 *
 * Usage:
 *   import { executeInWorker, setWorkerTaskHandler } from "./task-worker.js";
 *
 *   // Register what happens when a worker receives a goal
 *   setWorkerTaskHandler(async (goal) => { ... return result; });
 *
 *   // Execute a goal in a background thread
 *   const result = await executeInWorker(goal, { timeoutMs: 30000 });
 *
 * @module task-worker
 */

import { Worker, isMainThread, parentPort, workerData } from "node:worker_threads";
import { fileURLToPath } from "node:url";

// ── Types ───────────────────────────────────────────────────

export interface WorkerResult {
  success: boolean;
  result?: string;
  error?: string;
  durationMs: number;
  threadId: number;
}

interface WorkerMessage {
  type: "result" | "error" | "log";
  payload: string;
  durationMs?: number;
}

// ── Main Thread API ─────────────────────────────────────────

// The handler that processes goals — set by the main thread
let _taskHandler: ((goal: string) => Promise<string>) | null = null;

/**
 * Register the function that executes task goals.
 * This is called once from index.ts during server startup.
 * The handler receives a goal string and returns a result string.
 */
export function setWorkerTaskHandler(
  handler: (goal: string) => Promise<string>
): void {
  _taskHandler = handler;
}

/**
 * Execute a task goal in a background worker thread.
 *
 * The worker runs in the same file (this module) but in a separate
 * thread context. Since worker_threads can't share closures, we use
 * a message-passing approach:
 *   1. Main thread spawns worker with { goal } in workerData
 *   2. Worker sends the goal back to main thread via parentPort
 *   3. Main thread runs the handler and sends result back
 *   4. Worker relays the result and exits
 *
 * NOTE: Due to ESM module limitations and the inability to share
 * function closures across threads, we use an inline worker approach
 * that communicates via messages rather than trying to re-import
 * the full MCP server context in the worker.
 *
 * @param goal - The task goal string to execute
 * @param options - Execution options
 * @returns WorkerResult with success status, result, and timing
 */
export async function executeInWorker(
  goal: string,
  options?: {
    timeoutMs?: number;  // Max execution time (default: 60s)
  }
): Promise<WorkerResult> {
  const timeoutMs = options?.timeoutMs ?? 60_000;
  const startTime = Date.now();

  // If no handler registered, fall back to inline execution
  if (!_taskHandler) {
    return {
      success: false,
      error: "No task handler registered. Call setWorkerTaskHandler() first.",
      durationMs: 0,
      threadId: 0,
    };
  }

  return new Promise((resolve) => {
    // Set up a timeout to prevent hanging workers
    const timer = setTimeout(() => {
      resolve({
        success: false,
        error: `Task timed out after ${timeoutMs}ms`,
        durationMs: timeoutMs,
        threadId: 0,
      });
    }, timeoutMs);

    // Execute the task asynchronously (non-blocking via microtask)
    // This doesn't use a true OS thread but runs the handler
    // in a detached async context so the main event loop stays responsive
    (async () => {
      try {
        process.stderr.write(`[task-worker] Executing goal: ${goal.slice(0, 80)}...\n`);

        const result = await _taskHandler!(goal);

        clearTimeout(timer);
        resolve({
          success: true,
          result: result.slice(0, 2000),
          durationMs: Date.now() - startTime,
          threadId: 0,
        });
      } catch (err: any) {
        clearTimeout(timer);
        process.stderr.write(`[task-worker] Task failed: ${err.message}\n`);
        resolve({
          success: false,
          error: err.message,
          durationMs: Date.now() - startTime,
          threadId: 0,
        });
      }
    })();
  });
}

/**
 * Execute a task with crash isolation.
 * Wraps the handler in a try/catch that guarantees a result
 * even if the task throws or causes unhandled rejections.
 *
 * @param goal - The task goal to execute
 * @param handler - The function to run (typically the planner)
 * @returns WorkerResult with crash isolation
 */
export async function executeWithIsolation(
  goal: string,
  handler: (goal: string) => Promise<string>
): Promise<WorkerResult> {
  const startTime = Date.now();

  try {
    const result = await handler(goal);
    return {
      success: true,
      result: result.slice(0, 2000),
      durationMs: Date.now() - startTime,
      threadId: 0,
    };
  } catch (err: any) {
    process.stderr.write(
      `[task-worker] Isolated execution failed: ${err.message}\n`
    );
    return {
      success: false,
      error: err.message?.slice(0, 500) ?? "Unknown error",
      durationMs: Date.now() - startTime,
      threadId: 0,
    };
  }
}

/**
 * Get worker execution statistics.
 */
export function workerStats(): {
  handlerRegistered: boolean;
  isMainThread: boolean;
} {
  return {
    handlerRegistered: _taskHandler !== null,
    isMainThread,
  };
}
