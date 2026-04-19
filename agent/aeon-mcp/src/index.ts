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

import { pipeSend, pipeHealthCheck } from "./pipe-client.js";
import { cdpSend, cdpListTargets, cdpSendToTarget, cdpSendToFirstPage, cdpHealthCheck, cdpVersion } from "./cdp-client.js";
import { generateSnapshot, formatSnapshotForLLM } from "./snapshot.js";

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
    version: "0.3.0",
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
  try {
    return await handleToolCall(name, args as Record<string, unknown>);
  } catch (error: any) {
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
  const transport = new StdioServerTransport();
  await server.connect(transport);
  console.error("[aeon-mcp] Server started on stdio transport");
  console.error("[aeon-mcp] Tools registered:", tools.map((t) => t.name).join(", "));
}

main().catch((err) => {
  console.error("[aeon-mcp] Fatal error:", err);
  process.exit(1);
});
