/**
 * Named Pipe Client for Aeon Browser Shell Control
 *
 * Communicates with AeonAgentPipe (C++) over \\.\pipe\aeon-agent
 * Protocol: newline-delimited JSON
 *   Send:    { "cmd": "tab.list", ... }\n
 *   Receive: { "ok": true, "tabs": [...] }\n
 */

import * as net from "node:net";

const PIPE_PATH = "\\\\.\\pipe\\aeon-agent";
const CONNECT_TIMEOUT = 3000;
const RESPONSE_TIMEOUT = 5000;

export interface PipeCommand {
  cmd: string;
  [key: string]: unknown;
}

/**
 * Send a command to the Aeon browser shell via Named Pipe.
 * Opens a connection, sends the command, reads the response, then closes.
 *
 * The C++ side (AeonAgentPipe) handles one command per connection,
 * so we connect fresh each time. This is fast on localhost Named Pipes
 * (sub-millisecond connect time on Windows).
 */
export function pipeSend(command: PipeCommand): Promise<Record<string, unknown>> {
  return new Promise((resolve, reject) => {
    const client = net.createConnection(PIPE_PATH);
    let responseData = "";
    let settled = false;

    const responseTimeout = setTimeout(() => {
      if (!settled) {
        settled = true;
        client.destroy();
        reject(new Error(`Pipe response timeout after ${RESPONSE_TIMEOUT}ms`));
      }
    }, RESPONSE_TIMEOUT);

    const connectTimeout = setTimeout(() => {
      if (!settled && !client.connecting) return;
      if (!settled) {
        settled = true;
        clearTimeout(responseTimeout);
        client.destroy();
        reject(new Error(`Pipe connect timeout after ${CONNECT_TIMEOUT}ms. Is Aeon Browser running?`));
      }
    }, CONNECT_TIMEOUT);

    client.on("connect", () => {
      clearTimeout(connectTimeout);
      const payload = JSON.stringify(command) + "\n";
      client.write(payload);
    });

    client.on("data", (chunk) => {
      responseData += chunk.toString();

      // Check for complete JSON response (newline-delimited)
      const newlineIdx = responseData.indexOf("\n");
      if (newlineIdx !== -1) {
        clearTimeout(responseTimeout);
        clearTimeout(connectTimeout);
        settled = true;
        const jsonStr = responseData.slice(0, newlineIdx).trim();
        client.destroy();

        try {
          resolve(JSON.parse(jsonStr));
        } catch (e) {
          reject(new Error(`Invalid JSON from pipe: ${jsonStr.slice(0, 200)}`));
        }
      }
    });

    client.on("error", (err) => {
      if (!settled) {
        clearTimeout(responseTimeout);
        clearTimeout(connectTimeout);
        settled = true;
        reject(new Error(`Pipe connection failed: ${err.message}. Is Aeon Browser running?`));
      }
    });

    client.on("close", () => {
      if (!settled) {
        clearTimeout(responseTimeout);
        clearTimeout(connectTimeout);
        settled = true;

        // Try to parse whatever we got
        if (responseData.trim()) {
          try {
            resolve(JSON.parse(responseData.trim()));
          } catch {
            reject(new Error(`Pipe closed with incomplete response: ${responseData.slice(0, 200)}`));
          }
        } else {
          reject(new Error("Pipe closed without response"));
        }
      }
    });
  });
}

/**
 * Quick health check — attempts to connect to pipe and immediately disconnects.
 * Returns true if the pipe exists and accepts connections.
 */
export function pipeHealthCheck(): Promise<boolean> {
  return new Promise((resolve) => {
    const client = net.createConnection(PIPE_PATH);
    const timeout = setTimeout(() => {
      client.destroy();
      resolve(false);
    }, 1000);

    client.on("connect", () => {
      clearTimeout(timeout);
      client.destroy();
      resolve(true);
    });

    client.on("error", () => {
      clearTimeout(timeout);
      resolve(false);
    });
  });
}

export function summarizePage(tabId?: number, maxBullets?: number): Promise<Record<string, unknown>> {
  return pipeSend({ cmd: "ai.summarize", tab_id: tabId ?? -1, max_bullets: maxBullets ?? 5 });
}

export function navigateIntent(intent: string, tabId?: number): Promise<Record<string, unknown>> {
  return pipeSend({ cmd: "ai.navigate_intent", intent, tab_id: tabId ?? -1 });
}

export function getWebMCPTools(tabId?: number): Promise<Record<string, unknown>> {
  return pipeSend({ cmd: "webmcp.tools", tab_id: tabId ?? -1 });
}

export function stagehandObserve(tabId?: number): Promise<Record<string, unknown>> {
  return pipeSend({ cmd: "stagehand.observe", tab_id: tabId ?? -1 });
}

export function stagehandAct(action: string, ref?: number, text?: string): Promise<Record<string, unknown>> {
  return pipeSend({ cmd: "stagehand.act", action, ref: ref ?? -1, text: text ?? "" });
}

export function stagehandExtract(instruction: string): Promise<Record<string, unknown>> {
  return pipeSend({ cmd: "stagehand.extract", instruction });
}

export function stagehandAgent(goal: string): Promise<Record<string, unknown>> {
  return pipeSend({ cmd: "stagehand.agent", goal });
}

export function gemmaProcess(prompt: string, imageBase64?: string): Promise<Record<string, unknown>> {
  return pipeSend({ cmd: "gemma.process", prompt, image_base64: imageBase64 ?? "" });
}

export function multionWorkflow(goal: string, authJson?: string): Promise<Record<string, unknown>> {
  return pipeSend({ cmd: "multion.workflow", goal, auth_json: authJson ?? "" });
}
