/**
 * Chrome DevTools Protocol Client for Aeon Browser
 *
 * Connects to the browser's CDP endpoint (ws://localhost:9222)
 * to control web content: DOM manipulation, JavaScript execution,
 * navigation, screenshots, etc.
 *
 * Architecture:
 *   1. GET http://localhost:9222/json/version   → browser-level WS URL
 *   2. GET http://localhost:9222/json/list      → per-tab WS URLs
 *   3. WebSocket connect to target              → send CDP methods
 */

import WebSocket from "ws";

const CDP_HTTP = "http://localhost:9222";
const WS_TIMEOUT = 5000;

let nextId = 1;

interface CDPTarget {
  id: string;
  type: string;
  title: string;
  url: string;
  webSocketDebuggerUrl: string;
}

interface CDPResponse {
  id: number;
  result?: Record<string, unknown>;
  error?: { code: number; message: string };
}

/**
 * Fetch the list of inspectable targets (tabs/pages) from the CDP HTTP endpoint.
 */
export async function cdpListTargets(): Promise<CDPTarget[]> {
  const resp = await fetch(`${CDP_HTTP}/json/list`);
  if (!resp.ok) throw new Error(`CDP /json/list failed: ${resp.status}`);
  return (await resp.json()) as CDPTarget[];
}

/**
 * Fetch browser-level version info from CDP.
 */
export async function cdpVersion(): Promise<Record<string, unknown>> {
  const resp = await fetch(`${CDP_HTTP}/json/version`);
  if (!resp.ok) throw new Error(`CDP /json/version failed: ${resp.status}`);
  return (await resp.json()) as Record<string, unknown>;
}

/**
 * Send a single CDP command to a target via WebSocket.
 *
 * Opens a WS connection, sends the method, waits for the matching response,
 * then closes. This is the simplest approach for MCP tool calls where we
 * need request-response semantics.
 *
 * @param wsUrl   - The WebSocket debugger URL for the target
 * @param method  - CDP method name (e.g., "Page.navigate", "Runtime.evaluate")
 * @param params  - CDP method parameters
 * @returns       - The CDP result object
 */
export function cdpSend(
  wsUrl: string,
  method: string,
  params: Record<string, unknown> = {}
): Promise<Record<string, unknown>> {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(wsUrl, { handshakeTimeout: WS_TIMEOUT });
    const msgId = nextId++;
    let settled = false;

    const timeout = setTimeout(() => {
      if (!settled) {
        settled = true;
        ws.close();
        reject(new Error(`CDP timeout after ${WS_TIMEOUT}ms for ${method}`));
      }
    }, WS_TIMEOUT);

    ws.on("open", () => {
      ws.send(JSON.stringify({ id: msgId, method, params }));
    });

    ws.on("message", (data) => {
      try {
        const msg: CDPResponse = JSON.parse(data.toString());
        if (msg.id === msgId) {
          clearTimeout(timeout);
          settled = true;
          ws.close();
          if (msg.error) {
            reject(new Error(`CDP error [${msg.error.code}]: ${msg.error.message}`));
          } else {
            resolve(msg.result ?? {});
          }
        }
        // Ignore events and other message IDs
      } catch {
        // Non-JSON message, ignore
      }
    });

    ws.on("error", (err) => {
      if (!settled) {
        clearTimeout(timeout);
        settled = true;
        reject(new Error(`CDP WebSocket error: ${err.message}. Is Aeon running with --remote-debugging-port=9222?`));
      }
    });

    ws.on("close", () => {
      if (!settled) {
        clearTimeout(timeout);
        settled = true;
        reject(new Error("CDP WebSocket closed unexpectedly"));
      }
    });
  });
}

/**
 * Convenience: send a CDP command to the first available page target.
 * Finds the first "page" type target and sends the command to it.
 */
export async function cdpSendToFirstPage(
  method: string,
  params: Record<string, unknown> = {}
): Promise<Record<string, unknown>> {
  const targets = await cdpListTargets();
  const page = targets.find((t) => t.type === "page");
  if (!page) throw new Error("No page targets available");
  return cdpSend(page.webSocketDebuggerUrl, method, params);
}

/**
 * Send a CDP command to a specific target by its target ID.
 */
export async function cdpSendToTarget(
  targetId: string,
  method: string,
  params: Record<string, unknown> = {}
): Promise<Record<string, unknown>> {
  const targets = await cdpListTargets();
  const target = targets.find((t) => t.id === targetId);
  if (!target) throw new Error(`Target ${targetId} not found`);
  return cdpSend(target.webSocketDebuggerUrl, method, params);
}

/**
 * Quick health check — verifies CDP is responding.
 */
export async function cdpHealthCheck(): Promise<boolean> {
  try {
    await cdpVersion();
    return true;
  } catch {
    return false;
  }
}
