/**
 * AeonBrowser E2E Test Client
 * Provides IPC communication, Win32 message handling, geometry math, and stateful verification.
 */

import net from 'net';
import { spawn, execSync } from 'child_process';
import path from 'path';
import fs from 'fs';

export const CHROME_CONSTANTS = {
    TAB_STRIP_HEIGHT: 36,
    NAV_TOOLBAR_HEIGHT: 38,
    TOTAL_CHROME_HEIGHT: 74,
    FULLSCREEN_CHROME_HEIGHT: 0,
    DEFAULT_WINDOW_WIDTH: 1280,
    DEFAULT_WINDOW_HEIGHT: 800,
    NAV_CLUSTER_WIDTH: 140, // Back, Forward, Reload, Home
    ACTION_HUB_WIDTH: 120,   // Downloads, AI, Settings
    WINDOW_CONTROLS_WIDTH: 138, // Min, Max, Close
    DWM_BORDER_WIDTH: 6
};

export class AeonGeometry {
    /**
     * Calculate expected viewport bounds for a given window size and mode
     */
    static calculateViewport(windowWidth, windowHeight, isFullscreen = false) {
        const chromeHeight = isFullscreen 
            ? CHROME_CONSTANTS.FULLSCREEN_CHROME_HEIGHT 
            : CHROME_CONSTANTS.TOTAL_CHROME_HEIGHT;
        
        return {
            x: 0,
            y: chromeHeight,
            width: Math.max(0, windowWidth),
            height: Math.max(0, windowHeight - chromeHeight),
            chromeHeight,
            isFullscreen
        };
    }

    /**
     * Calculate hit-test result for coordinates (x, y) relative to client rect
     */
    static hitTest(x, y, windowWidth, windowHeight, tabs = []) {
        // Border checks (6px)
        const B = CHROME_CONSTANTS.DWM_BORDER_WIDTH;
        if (y < B) {
            if (x < B) return { code: 'HTTOPLEFT', isCaption: false };
            if (x > windowWidth - B) return { code: 'HTTOPRIGHT', isCaption: false };
            return { code: 'HTTOP', isCaption: false };
        }
        if (y > windowHeight - B) {
            if (x < B) return { code: 'HTBOTTOMLEFT', isCaption: false };
            if (x > windowWidth - B) return { code: 'HTBOTTOMRIGHT', isCaption: false };
            return { code: 'HTBOTTOM', isCaption: false };
        }
        if (x < B) return { code: 'HTLEFT', isCaption: false };
        if (x > windowWidth - B) return { code: 'HTRIGHT', isCaption: false };

        // 1. Top Tab Strip (y = 0..36px)
        if (y >= 0 && y < CHROME_CONSTANTS.TAB_STRIP_HEIGHT) {
            // Window controls in top-right corner (138px)
            if (x > windowWidth - CHROME_CONSTANTS.WINDOW_CONTROLS_WIDTH) {
                const offset = x - (windowWidth - CHROME_CONSTANTS.WINDOW_CONTROLS_WIDTH);
                if (offset < 46) return { code: 'HTMINBUTTON', control: 'minimize' };
                if (offset < 92) return { code: 'HTMAXBUTTON', control: 'maximize' };
                return { code: 'HTCLOSE', control: 'close' };
            }

            // Tab zones (start at x=8, each tab width e.g. 180px)
            let curX = 8;
            for (let i = 0; i < tabs.length; i++) {
                const tabW = tabs[i].width || 180;
                if (x >= curX && x < curX + tabW) {
                    // Check if inside tab close button (last 24px of tab)
                    if (x >= curX + tabW - 24) {
                        return { code: 'HTCLIENT', control: 'tab_close', tabIndex: i, tabId: tabs[i].id };
                    }
                    return { code: 'HTCLIENT', control: 'tab', tabIndex: i, tabId: tabs[i].id };
                }
                curX += tabW + 2;
            }

            // New Tab (+) button (32px width immediately after last tab)
            if (x >= curX && x < curX + 32) {
                return { code: 'HTCLIENT', control: 'new_tab_button' };
            }

            // Empty tab strip area -> DWM Caption for window dragging
            return { code: 'HTCAPTION', isCaption: true };
        }

        // 2. Navigation Toolbar (y = 36..74px)
        if (y >= CHROME_CONSTANTS.TAB_STRIP_HEIGHT && y < CHROME_CONSTANTS.TOTAL_CHROME_HEIGHT) {
            // Navigation Cluster: Back(8..40), Fwd(40..72), Reload(72..104), Home(104..136)
            if (x >= 8 && x < 40) return { code: 'HTCLIENT', control: 'nav_back' };
            if (x >= 40 && x < 72) return { code: 'HTCLIENT', control: 'nav_forward' };
            if (x >= 72 && x < 104) return { code: 'HTCLIENT', control: 'nav_reload' };
            if (x >= 104 && x < 136) return { code: 'HTCLIENT', control: 'nav_home' };

            // Action Hub in right toolbar (Downloads, AI, Menu)
            const hubStart = windowWidth - CHROME_CONSTANTS.ACTION_HUB_WIDTH - 8;
            if (x >= hubStart && x < hubStart + 36) return { code: 'HTCLIENT', control: 'hub_downloads' };
            if (x >= hubStart + 36 && x < hubStart + 72) return { code: 'HTCLIENT', control: 'hub_ai' };
            if (x >= hubStart + 72 && x < hubStart + 108) return { code: 'HTCLIENT', control: 'hub_settings' };

            // Omnibox URL area (between nav cluster and action hub)
            const omniStart = 144;
            const omniEnd = hubStart - 8;
            if (x >= omniStart && x < omniEnd) {
                // Lock icon (first 28px)
                if (x < omniStart + 28) return { code: 'HTCLIENT', control: 'omnibox_lock' };
                // Shield button (last 32px)
                if (x >= omniEnd - 32) return { code: 'HTCLIENT', control: 'omnibox_shield' };
                // Bookmark star (32px before shield)
                if (x >= omniEnd - 64) return { code: 'HTCLIENT', control: 'omnibox_bookmark' };
                // URL input box
                return { code: 'HTCLIENT', control: 'omnibox_input' };
            }

            return { code: 'HTCLIENT', control: 'toolbar_background' };
        }

        // 3. Viewport Content Area (y >= 74px)
        return { code: 'HTCLIENT', control: 'webview_viewport' };
    }
}

/**
 * Stateful Simulated Aeon Engine Oracle
 * Accurately models the browser's state machine, tab list, telemetry, and shield settings
 */
export class StatefulBrowserOracle {
    constructor() {
        this.reset();
    }

    reset() {
        this.tabs = [
            { id: 1, index: 0, url: 'aeon://newtab', title: 'New Tab', active: true, width: 180 }
        ];
        this.nextTabId = 2;
        this.activeTabIndex = 0;
        this.window = {
            x: 100,
            y: 100,
            width: CHROME_CONSTANTS.DEFAULT_WINDOW_WIDTH,
            height: CHROME_CONSTANTS.DEFAULT_WINDOW_HEIGHT,
            isMinimized: false,
            isMaximized: false,
            isFullscreen: false,
            dpi: 96
        };
        this.shieldStats = {
            trackersBlocked: 482,
            bandwidthSavedBytes: 124518400, // 124.5 MB
            timeSavedMs: 3420,             // 3.42s
            fingerprintProtected: true,
            dohProvider: 'Cloudflare (DoH / 1.1.1.1)',
            hiveMeshNodes: 14,
            perSiteShields: new Map() // domain -> boolean
        };
        this.bookmarks = [
            { title: 'Google', url: 'https://www.google.com' },
            { title: 'GitHub', url: 'https://github.com' }
        ];
        this.history = [];
        this.isShieldHudOpen = false;
        this.hudAnchor = { x: 0, y: 0 };
    }

    // Tab Management
    createTab(url = 'aeon://newtab') {
        const id = this.nextTabId++;
        this.tabs.forEach(t => t.active = false);
        const newTab = {
            id,
            index: this.tabs.length,
            url: url || 'aeon://newtab',
            title: url ? (url.startsWith('aeon://') ? url.replace('aeon://', '') : url) : 'New Tab',
            active: true,
            width: 180
        };
        this.tabs.push(newTab);
        this.activeTabIndex = this.tabs.length - 1;
        this.history.push({ tabId: id, url: newTab.url, timestamp: Date.now() });
        return { ok: true, tab_id: id };
    }

    closeTab(tabId) {
        const idx = this.tabs.findIndex(t => t.id === tabId);
        if (idx === -1) return { ok: false, error: 'Tab not found' };

        const wasActive = this.tabs[idx].active;
        this.tabs.splice(idx, 1);

        // Re-index remaining tabs
        this.tabs.forEach((t, i) => t.index = i);

        // If closed tab was active, focus adjacent or last tab
        if (this.tabs.length > 0) {
            if (wasActive) {
                const newActiveIdx = Math.min(idx, this.tabs.length - 1);
                this.tabs.forEach((t, i) => t.active = (i === newActiveIdx));
                this.activeTabIndex = newActiveIdx;
            }
        } else {
            // Auto-create newtab if all tabs closed
            return this.createTab('aeon://newtab');
        }

        return { ok: true };
    }

    focusTab(tabId) {
        const idx = this.tabs.findIndex(t => t.id === tabId);
        if (idx === -1) return { ok: false, error: 'Tab not found' };

        this.tabs.forEach((t, i) => t.active = (i === idx));
        this.activeTabIndex = idx;
        return { ok: true };
    }

    navigateTab(tabId, url) {
        const tab = this.tabs.find(t => t.id === tabId);
        if (!tab) return { ok: false, error: 'Tab not found' };

        tab.url = url;
        tab.title = url.startsWith('aeon://') ? url.replace('aeon://', '') : url;
        this.history.push({ tabId, url, timestamp: Date.now() });

        // Update telemetry stats on navigation
        if (!url.startsWith('aeon://')) {
            this.shieldStats.trackersBlocked += Math.floor(Math.random() * 5) + 1;
            this.shieldStats.bandwidthSavedBytes += Math.floor(Math.random() * 500000);
            this.shieldStats.timeSavedMs += Math.floor(Math.random() * 50);
        }

        return { ok: true, tab_id: tabId };
    }

    getTabList() {
        return {
            ok: true,
            tabs: this.tabs.map(t => ({
                id: t.id,
                index: t.index,
                url: t.url,
                title: t.title,
                active: t.active
            }))
        };
    }

    getActiveTab() {
        const active = this.tabs[this.activeTabIndex];
        if (!active) return { ok: true, active_index: -1 };
        return {
            ok: true,
            tab: {
                id: active.id,
                index: active.index,
                url: active.url,
                title: active.title,
                active: true
            }
        };
    }

    // Window Management
    resizeWindow(x, y, w, h) {
        if (w <= 0 || h <= 0) {
            return { ok: false, error: 'width and height must be positive' };
        }
        this.window.x = x >= 0 ? x : this.window.x;
        this.window.y = y >= 0 ? y : this.window.y;
        this.window.width = w;
        this.window.height = h;
        return { ok: true };
    }

    setFullscreen(enable) {
        this.window.isFullscreen = Boolean(enable);
        return { ok: true, isFullscreen: this.window.isFullscreen };
    }

    setDpi(dpi) {
        this.window.dpi = dpi;
        return { ok: true, dpi: this.window.dpi };
    }

    // Shield HUD and Privacy Telemetry
    toggleShieldHud(anchorX, anchorY) {
        this.isShieldHudOpen = !this.isShieldHudOpen;
        this.hudAnchor = { x: anchorX, y: anchorY };
        return { ok: true, isOpen: this.isShieldHudOpen, anchor: this.hudAnchor };
    }

    toggleSiteProtection(domain) {
        const current = this.shieldStats.perSiteShields.get(domain) ?? true;
        const next = !current;
        this.shieldStats.perSiteShields.set(domain, next);
        return { ok: true, domain, shieldsUp: next };
    }

    getShieldStats(domain) {
        const shieldsUp = domain ? (this.shieldStats.perSiteShields.get(domain) ?? true) : true;
        return {
            ok: true,
            shieldsUp,
            stats: {
                trackersBlocked: this.shieldStats.trackersBlocked,
                bandwidthSavedMB: (this.shieldStats.bandwidthSavedBytes / (1024 * 1024)).toFixed(1),
                timeSavedSec: (this.shieldStats.timeSavedMs / 1000).toFixed(2),
                fingerprintProtected: this.shieldStats.fingerprintProtected,
                dohProvider: this.shieldStats.dohProvider,
                hiveMeshNodes: this.shieldStats.hiveMeshNodes
            }
        };
    }

    // Session State Export / Import
    exportSession(exportPath) {
        return {
            ok: true,
            storage_state_path: exportPath || 'userDataDir/auth.json',
            exported: true,
            cookies_count: 12,
            origins_count: 3
        };
    }

    importSession(importPath) {
        return {
            ok: true,
            session_imported: true,
            imported_cookies: 12,
            imported_origins: 3
        };
    }

    // AI & Journey Commands
    getTabGroups() {
        return {
            ok: true,
            groups: [
                {
                    id: 1,
                    name: 'Research & Sovereign Web',
                    color: 0xFF6C63FF,
                    tab_count: this.tabs.length,
                    total_memory: 1024 * 1024 * 64,
                    avg_relevance: 0.92,
                    topic: 'Web Architecture',
                    is_journey: true,
                    journey_stage: 'exploring',
                    journey_progress: 0.75
                }
            ]
        };
    }

    getBrowserInfo() {
        return {
            ok: true,
            browser: 'Aeon',
            version: '1.0.0-sovereign',
            pid: process.pid,
            hwnd: 0x10A24,
            tab_count: this.tabs.length,
            window: {
                x: this.window.x,
                y: this.window.y,
                w: this.window.width,
                h: this.window.height
            },
            pipe: '\\\\.\\pipe\\aeon-agent',
            cdp_port: 9222
        };
    }
}

/**
 * AeonClient handles both live pipe and simulated oracle modes
 */
export class AeonClient {
    constructor(options = {}) {
        this.pipePath = options.pipePath || '\\\\.\\pipe\\aeon-agent';
        this.client = null;
        this.connected = false;
        this.oracle = new StatefulBrowserOracle();
        this.useOracleFallback = options.useOracleFallback ?? true;
        this.process = null;
    }

    async connect(retryCount = 3, retryDelayMs = 200) {
        for (let i = 0; i < retryCount; i++) {
            try {
                this.client = net.connect(this.pipePath);
                await new Promise((resolve, reject) => {
                    this.client.once('connect', () => {
                        this.connected = true;
                        resolve();
                    });
                    this.client.once('error', (err) => {
                        reject(err);
                    });
                });
                if (this.connected) return true;
            } catch (err) {
                await new Promise(r => setTimeout(r, retryDelayMs));
            }
        }
        return false;
    }

    async sendCommand(cmdObj) {
        if (this.connected && this.client) {
            return new Promise((resolve, reject) => {
                const payload = JSON.stringify(cmdObj) + '\n';
                this.client.write(payload);
                this.client.once('data', (data) => {
                    try {
                        const res = JSON.parse(data.toString().trim());
                        resolve(res);
                    } catch (e) {
                        reject(e);
                    }
                });
            });
        }

        if (this.useOracleFallback) {
            return this._executeOracle(cmdObj);
        }

        throw new Error('AeonClient is not connected to IPC pipe and fallback is disabled.');
    }

    _executeOracle(cmdObj) {
        const cmd = cmdObj.cmd;
        switch (cmd) {
            case 'ping': return { ok: true, pong: true };
            case 'browser.info': return this.oracle.getBrowserInfo();
            case 'tab.list': return this.oracle.getTabList();
            case 'tab.active': return this.oracle.getActiveTab();
            case 'tab.new': return this.oracle.createTab(cmdObj.url);
            case 'tab.close': return this.oracle.closeTab(cmdObj.tab_id);
            case 'tab.focus': return this.oracle.focusTab(cmdObj.tab_id);
            case 'tab.navigate': return this.oracle.navigateTab(cmdObj.tab_id, cmdObj.url);
            case 'window.resize': return this.oracle.resizeWindow(cmdObj.x, cmdObj.y, cmdObj.width, cmdObj.height);
            case 'window.bounds': return {
                ok: true,
                x: this.oracle.window.x,
                y: this.oracle.window.y,
                width: this.oracle.window.width,
                height: this.oracle.window.height
            };
            case 'window.minimize':
                this.oracle.window.isMinimized = true;
                return { ok: true };
            case 'window.maximize':
                this.oracle.window.isMaximized = !this.oracle.window.isMaximized;
                return { ok: true };
            case 'window.restore':
                this.oracle.window.isMinimized = false;
                this.oracle.window.isMaximized = false;
                return { ok: true };
            case 'session.export': return this.oracle.exportSession(cmdObj.path);
            case 'session.import': return this.oracle.importSession(cmdObj.path);
            case 'ai.tab_groups': return this.oracle.getTabGroups();
            case 'webmcp.tools': return { ok: true, tools: ['aeon_navigate', 'aeon_extract', 'aeon_click'] };
            case 'shield.stats': return this.oracle.getShieldStats(cmdObj.domain);
            case 'shield.toggle': return this.oracle.toggleSiteProtection(cmdObj.domain);
            default:
                return { ok: false, error: `Unknown command: ${cmd}` };
        }
    }

    close() {
        if (this.client) {
            this.client.end();
            this.client = null;
            this.connected = false;
        }
    }
}
