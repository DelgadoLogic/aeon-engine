/**
 * Snapshot+Refs Module for Aeon Agent
 *
 * Generates a compact, token-efficient snapshot of the current browser state
 * for LLM consumption. Merges:
 *   - Shell state (tabs, active tab, window bounds) via Named Pipe
 *   - Page accessibility tree via CDP (Accessibility.getFullAXTree)
 *   - Page metadata (URL, title) via CDP
 *
 * Each interactive element gets a stable [ref] number that the agent can
 * use to target actions (click, type, etc.) without needing CSS selectors
 * or XPath. This follows the Snapshot+Refs pattern used by modern agent
 * frameworks (similar to Anthropic's computer-use approach).
 */

import { pipeSend, pipeHealthCheck } from "./pipe-client.js";
import { cdpListTargets, cdpSend, cdpHealthCheck, cdpVersion } from "./cdp-client.js";

export interface AeonSnapshot {
  timestamp: string;
  shell: {
    connected: boolean;
    tabs: Array<{ id: number; title: string; url: string; active: boolean }>;
    activeTabId: number | null;
  };
  cdp: {
    connected: boolean;
    browserVersion: string | null;
    targetCount: number;
  };
  page: {
    url: string;
    title: string;
    interactiveElements: InteractiveElement[];
  } | null;
}

export interface InteractiveElement {
  ref: number;
  role: string;
  name: string;
  value?: string;
  description?: string;
  backendDOMNodeId?: number;
  focused?: boolean;
  disabled?: boolean;
  checked?: string;
}

// Roles that represent interactive/actionable elements
const INTERACTIVE_ROLES = new Set([
  "button",
  "link",
  "textbox",
  "checkbox",
  "radio",
  "combobox",
  "listbox",
  "menuitem",
  "menuitemcheckbox",
  "menuitemradio",
  "option",
  "searchbox",
  "slider",
  "spinbutton",
  "switch",
  "tab",
  "textbox",
  "treeitem",
]);

/**
 * Generate a full Snapshot+Refs for the current browser state.
 *
 * This is the primary tool for the agent's perception layer. The snapshot
 * is designed to be compact enough to fit in a single LLM context without
 * overwhelming the token budget.
 */
export async function generateSnapshot(): Promise<AeonSnapshot> {
  const timestamp = new Date().toISOString();

  // Gather shell and CDP state in parallel
  const [shellState, cdpState] = await Promise.allSettled([
    gatherShellState(),
    gatherCDPState(),
  ]);

  const shell =
    shellState.status === "fulfilled"
      ? shellState.value
      : { connected: false, tabs: [], activeTabId: null };

  const cdp =
    cdpState.status === "fulfilled"
      ? cdpState.value
      : { connected: false, browserVersion: null, targetCount: 0, page: null };

  return {
    timestamp,
    shell: {
      connected: shell.connected,
      tabs: shell.tabs,
      activeTabId: shell.activeTabId,
    },
    cdp: {
      connected: cdp.connected,
      browserVersion: cdp.browserVersion,
      targetCount: cdp.targetCount,
    },
    page: cdp.page,
  };
}

/**
 * Gather browser shell state via Named Pipe.
 */
async function gatherShellState() {
  const connected = await pipeHealthCheck();
  if (!connected) {
    return { connected: false, tabs: [] as any[], activeTabId: null };
  }

  try {
    const result = await pipeSend({ cmd: "tab.list" });
    const tabs = Array.isArray(result.tabs) ? result.tabs : [];
    const activeTab = tabs.find((t: any) => t.active);

    return {
      connected: true,
      tabs: tabs.map((t: any) => ({
        id: t.id,
        title: t.title || "",
        url: t.url || "",
        active: !!t.active,
      })),
      activeTabId: activeTab?.id ?? null,
    };
  } catch {
    return { connected: false, tabs: [] as any[], activeTabId: null };
  }
}

/**
 * Gather CDP state: version info, targets, and accessibility tree for
 * the active page.
 */
async function gatherCDPState() {
  const connected = await cdpHealthCheck();
  if (!connected) {
    return { connected: false, browserVersion: null, targetCount: 0, page: null };
  }

  let browserVersion: string | null = null;
  try {
    const ver = await cdpVersion();
    browserVersion = (ver["Browser"] as string) ?? null;
  } catch {
    // Non-critical
  }

  const targets = await cdpListTargets();
  const pageTarget = targets.find((t) => t.type === "page");

  let page: AeonSnapshot["page"] = null;

  if (pageTarget) {
    try {
      page = {
        url: pageTarget.url,
        title: pageTarget.title,
        interactiveElements: await extractInteractiveElements(pageTarget.webSocketDebuggerUrl),
      };
    } catch {
      page = {
        url: pageTarget.url,
        title: pageTarget.title,
        interactiveElements: [],
      };
    }
  }

  return {
    connected: true,
    browserVersion,
    targetCount: targets.length,
    page,
  };
}

/**
 * Extract interactive elements from the accessibility tree and assign
 * stable ref numbers. This is the core of the Snapshot+Refs system.
 */
async function extractInteractiveElements(wsUrl: string): Promise<InteractiveElement[]> {
  // Get the full accessibility tree
  const axResult = await cdpSend(wsUrl, "Accessibility.getFullAXTree", {});
  const nodes = (axResult.nodes as any[]) ?? [];

  const elements: InteractiveElement[] = [];
  let refCounter = 1;

  for (const node of nodes) {
    const role = node.role?.value?.toLowerCase();
    if (!role || !INTERACTIVE_ROLES.has(role)) continue;

    // Skip ignored/hidden nodes
    if (node.ignored) continue;

    const element: InteractiveElement = {
      ref: refCounter++,
      role,
      name: node.name?.value ?? "",
    };

    // Extract useful properties
    if (node.value?.value !== undefined) {
      element.value = String(node.value.value);
    }

    if (node.description?.value) {
      element.description = node.description.value;
    }

    if (node.backendDOMNodeId) {
      element.backendDOMNodeId = node.backendDOMNodeId;
    }

    // Check properties array for state
    if (Array.isArray(node.properties)) {
      for (const prop of node.properties) {
        if (prop.name === "focused" && prop.value?.value === true) {
          element.focused = true;
        }
        if (prop.name === "disabled" && prop.value?.value === true) {
          element.disabled = true;
        }
        if (prop.name === "checked") {
          element.checked = String(prop.value?.value);
        }
      }
    }

    elements.push(element);
  }

  return elements;
}

/**
 * Format a snapshot as a compact text representation suitable for
 * LLM consumption. Minimizes tokens while preserving all actionable info.
 */
export function formatSnapshotForLLM(snapshot: AeonSnapshot): string {
  const lines: string[] = [];

  lines.push(`# Browser State @ ${snapshot.timestamp}`);
  lines.push(``);

  // Shell status
  lines.push(`## Shell (Named Pipe)`);
  if (!snapshot.shell.connected) {
    lines.push(`Status: DISCONNECTED`);
  } else {
    lines.push(`Status: Connected | ${snapshot.shell.tabs.length} tab(s)`);
    for (const tab of snapshot.shell.tabs) {
      const marker = tab.active ? "→" : " ";
      lines.push(`${marker} [tab:${tab.id}] ${tab.title} — ${tab.url}`);
    }
  }

  lines.push(``);

  // CDP status
  lines.push(`## CDP (DevTools Protocol)`);
  if (!snapshot.cdp.connected) {
    lines.push(`Status: DISCONNECTED`);
  } else {
    lines.push(`Status: Connected | ${snapshot.cdp.browserVersion} | ${snapshot.cdp.targetCount} target(s)`);
  }

  // Page content
  if (snapshot.page) {
    lines.push(``);
    lines.push(`## Active Page`);
    lines.push(`URL: ${snapshot.page.url}`);
    lines.push(`Title: ${snapshot.page.title}`);
    lines.push(``);

    if (snapshot.page.interactiveElements.length > 0) {
      lines.push(`### Interactive Elements (${snapshot.page.interactiveElements.length})`);
      for (const el of snapshot.page.interactiveElements) {
        let desc = `[${el.ref}] ${el.role}: "${el.name}"`;
        if (el.value) desc += ` value="${el.value}"`;
        if (el.focused) desc += " [FOCUSED]";
        if (el.disabled) desc += " [DISABLED]";
        if (el.checked) desc += ` [checked=${el.checked}]`;
        lines.push(desc);
      }
    } else {
      lines.push(`No interactive elements found.`);
    }
  }

  return lines.join("\n");
}
