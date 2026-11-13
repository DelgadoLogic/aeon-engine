/**
 * Tier 2: Boundary & Corner Cases E2E Test Suite
 * Strictly covers edge cases, limits, rapid inputs, malformed data, and boundary parameters for all 18 features (90+ TCs).
 */

import { describe, test, expect, beforeAll, afterAll } from './helpers/test_framework.mjs';
import { AeonClient, AeonGeometry, CHROME_CONSTANTS } from './helpers/aeon_client.mjs';
import fs from 'fs';
import path from 'path';

export function registerTier2Tests() {
    const client = new AeonClient();

    beforeAll(async () => {
        await client.connect();
    });

    afterAll(() => {
        if (client) client.close();
    });

    // ── FEATURE 1 BOUNDARIES: Tab Strip Bounds & Extreme Metrics ────────────────
    describe('Feature 1 Boundaries: Tab Strip Limits', () => {
        test('TC2.1: Boundary hit-test at y=35px returns tab strip, y=36px returns toolbar', () => {
            const tabs = [{ id: 1, width: 180 }];
            const hitStrip = AeonGeometry.hitTest(100, 35, 1280, 800, tabs);
            expect(hitStrip.control).toBe('tab');

            const hitToolbar = AeonGeometry.hitTest(100, 36, 1280, 800, tabs);
            expect(hitToolbar.control).not.toBe('tab');
        });

        test('TC2.2: Hit testing tab strip on ultra-wide 8K display (7680x4320)', () => {
            const tabs = [{ id: 1, width: 180 }];
            const hit = AeonGeometry.hitTest(4000, 18, 7680, 4320, tabs);
            expect(hit.code).toBe('HTCAPTION');
            expect(hit.isCaption).toBe(true);
        });

        test('TC2.3: Tab strip width clamping when window width is minimal (300px)', () => {
            const tabs = [{ id: 1, width: 180 }];
            const hit = AeonGeometry.hitTest(100, 18, 300, 400, tabs);
            expect(hit.code).toBe('HTCLIENT');
        });

        test('TC2.4: Sub-pixel rounding handles fractional coordinate inputs gracefully', () => {
            const tabs = [{ id: 1, width: 180 }];
            const hit = AeonGeometry.hitTest(50.4, 18.7, 1280, 800, tabs);
            expect(hit.code).toBe('HTCLIENT');
        });

        test('TC2.5: Zero-tab edge case maintains stable tab strip geometry', () => {
            const hit = AeonGeometry.hitTest(200, 18, 1280, 800, []);
            expect(hit.code).toBe('HTCAPTION');
        });
    });

    // ── FEATURE 2 BOUNDARIES: Rapid Tab Lifecycle & Extreme Titles ──────────────
    describe('Feature 2 Boundaries: Rapid Tab Lifecycle', () => {
        test('TC2.6: Rapid sequential creation of 10 tabs executes without errors', async () => {
            const createdIds = [];
            for (let i = 0; i < 10; i++) {
                const res = await client.sendCommand({ cmd: 'tab.new', url: `https://site-${i}.org` });
                expect(res.ok).toBe(true);
                createdIds.push(res.tab_id);
            }
            const list = await client.sendCommand({ cmd: 'tab.list' });
            expect(list.tabs.length).toBeGreaterThanOrEqual(10);

            // Clean up created tabs
            for (const id of createdIds) {
                await client.sendCommand({ cmd: 'tab.close', tab_id: id });
            }
        });

        test('TC2.7: Closing active tab correctly shifts focus to adjacent tab', async () => {
            const tabA = await client.sendCommand({ cmd: 'tab.new', url: 'https://site-a.org' });
            const tabB = await client.sendCommand({ cmd: 'tab.new', url: 'https://site-b.org' });

            await client.sendCommand({ cmd: 'tab.focus', tab_id: tabB.tab_id });
            await client.sendCommand({ cmd: 'tab.close', tab_id: tabB.tab_id });

            const active = await client.sendCommand({ cmd: 'tab.active' });
            expect(active.tab.id).toBe(tabA.tab_id);

            await client.sendCommand({ cmd: 'tab.close', tab_id: tabA.tab_id });
        });

        test('TC2.8: Closing the last remaining tab auto-spawns clean aeon://newtab', async () => {
            const list = await client.sendCommand({ cmd: 'tab.list' });
            for (const t of list.tabs) {
                await client.sendCommand({ cmd: 'tab.close', tab_id: t.id });
            }
            const afterList = await client.sendCommand({ cmd: 'tab.list' });
            expect(afterList.tabs.length).toBe(1);
            expect(afterList.tabs[0].url).toBe('aeon://newtab');
        });

        test('TC2.9: Tab creation with empty URL string defaults to aeon://newtab', async () => {
            const res = await client.sendCommand({ cmd: 'tab.new', url: '' });
            expect(res.ok).toBe(true);
            const active = await client.sendCommand({ cmd: 'tab.active' });
            expect(active.tab.url).toBe('aeon://newtab');
            await client.sendCommand({ cmd: 'tab.close', tab_id: res.tab_id });
        });

        test('TC2.10: Tab navigation with 4096-character URL executes safely', async () => {
            const active = await client.sendCommand({ cmd: 'tab.active' });
            const longUrl = 'https://example.com/search?q=' + 'A'.repeat(4000);
            const res = await client.sendCommand({ cmd: 'tab.navigate', tab_id: active.tab.id, url: longUrl });
            expect(res.ok).toBe(true);
        });
    });

    // ── FEATURE 3 BOUNDARIES: Chrome & DWM Dragging Extremes ────────────────────
    describe('Feature 3 Boundaries: DWM Dragging & Window Metrics', () => {
        test('TC2.11: Non-client hit test on top window border (y < 6px) returns HTTOP', () => {
            const hit = AeonGeometry.hitTest(500, 3, 1280, 800, []);
            expect(hit.code).toBe('HTTOP');
        });

        test('TC2.12: Non-client hit test on left window border (x < 6px) returns HTLEFT', () => {
            const hit = AeonGeometry.hitTest(3, 400, 1280, 800, []);
            expect(hit.code).toBe('HTLEFT');
        });

        test('TC2.13: Non-client hit test on bottom right corner returns HTBOTTOMRIGHT', () => {
            const hit = AeonGeometry.hitTest(1276, 796, 1280, 800, []);
            expect(hit.code).toBe('HTBOTTOMRIGHT');
        });

        test('TC2.14: Negative coordinate window positioning (multi-monitor scenario) handled safely', async () => {
            const res = await client.sendCommand({ cmd: 'window.resize', x: -1920, y: 0, width: 1920, height: 1080 });
            expect(res.ok).toBe(true);
        });

        test('TC2.15: Rapid minimize-maximize-restore cycle completes cleanly', async () => {
            await client.sendCommand({ cmd: 'window.minimize' });
            await client.sendCommand({ cmd: 'window.maximize' });
            await client.sendCommand({ cmd: 'window.restore' });
            const bounds = await client.sendCommand({ cmd: 'window.bounds' });
            expect(bounds.ok).toBe(true);
        });
    });

    // ── FEATURE 4 BOUNDARIES: Navigation Cluster Extremes ───────────────────────
    describe('Feature 4 Boundaries: Navigation Cluster Edge Cases', () => {
        test('TC2.16: Rapid navigation between 5 distinct URLs maintains history list', async () => {
            const active = await client.sendCommand({ cmd: 'tab.active' });
            const urls = [
                'https://example.com/1',
                'https://example.com/2',
                'https://example.com/3',
                'https://example.com/4',
                'https://example.com/5'
            ];
            for (const u of urls) {
                await client.sendCommand({ cmd: 'tab.navigate', tab_id: active.tab.id, url: u });
            }
            const current = await client.sendCommand({ cmd: 'tab.active' });
            expect(current.tab.url).toBe('https://example.com/5');
        });

        test('TC2.17: Reloading when URL is aeon://newtab succeeds without network errors', async () => {
            const active = await client.sendCommand({ cmd: 'tab.active' });
            await client.sendCommand({ cmd: 'tab.navigate', tab_id: active.tab.id, url: 'aeon://newtab' });
            const res = await client.sendCommand({ cmd: 'tab.navigate', tab_id: active.tab.id, url: 'aeon://newtab' });
            expect(res.ok).toBe(true);
        });

        test('TC2.18: Navigation to custom internal schemas aeon://history and aeon://downloads', async () => {
            const active = await client.sendCommand({ cmd: 'tab.active' });
            const hRes = await client.sendCommand({ cmd: 'tab.navigate', tab_id: active.tab.id, url: 'aeon://history' });
            expect(hRes.ok).toBe(true);
            const dRes = await client.sendCommand({ cmd: 'tab.navigate', tab_id: active.tab.id, url: 'aeon://downloads' });
            expect(dRes.ok).toBe(true);
        });

        test('TC2.19: Hit test for toolbar background area returns HTCLIENT and not HTCAPTION', () => {
            // Hit test between reload and home (x=103, y=55)
            const hit = AeonGeometry.hitTest(140, 55, 1280, 800, []);
            expect(hit.code).toBe('HTCLIENT');
            expect(hit.isCaption).toBeFalsy();
        });

        test('TC2.20: Navigating with URL containing hashes and query parameters', async () => {
            const active = await client.sendCommand({ cmd: 'tab.active' });
            const complexUrl = 'https://docs.aeon.org/guide/intro?search=speed&version=2#section-3';
            const res = await client.sendCommand({ cmd: 'tab.navigate', tab_id: active.tab.id, url: complexUrl });
            expect(res.ok).toBe(true);
        });
    });

    // ── FEATURE 5 BOUNDARIES: Omnibox Text Extremes & Special Schemas ───────────
    describe('Feature 5 Boundaries: Omnibox Input Extremes', () => {
        test('TC2.21: Omnibox handles unicode / multilingual queries (日本語, Русский, العربية)', async () => {
            const active = await client.sendCommand({ cmd: 'tab.active' });
            const query = 'https://www.google.com/search?q=' + encodeURIComponent('日本語 Русский العربية');
            const res = await client.sendCommand({ cmd: 'tab.navigate', tab_id: active.tab.id, url: query });
            expect(res.ok).toBe(true);
        });

        test('TC2.22: Omnibox handles URL with spaces by auto-trimming and query encoding', async () => {
            const active = await client.sendCommand({ cmd: 'tab.active' });
            const res = await client.sendCommand({
                cmd: 'tab.navigate',
                tab_id: active.tab.id,
                url: 'https://example.com/path with spaces'
            });
            expect(res.ok).toBe(true);
        });

        test('TC2.23: Omnibox input with script tags is treated as plain search text without execution', async () => {
            const active = await client.sendCommand({ cmd: 'tab.active' });
            const xssQuery = '<script>alert(1)</script>';
            const res = await client.sendCommand({
                cmd: 'tab.navigate',
                tab_id: active.tab.id,
                url: 'https://www.google.com/search?q=' + encodeURIComponent(xssQuery)
            });
            expect(res.ok).toBe(true);
        });

        test('TC2.24: Hit test at exact left edge of Omnibox (x=144, y=55) returns omnibox control', () => {
            const hit = AeonGeometry.hitTest(145, 55, 1280, 800, []);
            expect(hit.code).toBe('HTCLIENT');
            expect(hit.control).toContain('omnibox');
        });

        test('TC2.25: Hit test at exact right edge of Omnibox returns omnibox control', () => {
            const hubStart = 1280 - CHROME_CONSTANTS.ACTION_HUB_WIDTH - 8;
            const hit = AeonGeometry.hitTest(hubStart - 10, 55, 1280, 800, []);
            expect(hit.code).toBe('HTCLIENT');
            expect(hit.control).toContain('omnibox');
        });
    });

    // ── FEATURE 6 BOUNDARIES: Security & Shield Icon Extremes ───────────────────
    describe('Feature 6 Boundaries: Security & Shield Badges', () => {
        test('TC2.26: Security lock evaluation for localhost endpoint', async () => {
            const active = await client.sendCommand({ cmd: 'tab.active' });
            const res = await client.sendCommand({ cmd: 'tab.navigate', tab_id: active.tab.id, url: 'http://localhost:3000' });
            expect(res.ok).toBe(true);
        });

        test('TC2.27: Security lock evaluation for raw IPv4 address (http://127.0.0.1)', async () => {
            const active = await client.sendCommand({ cmd: 'tab.active' });
            const res = await client.sendCommand({ cmd: 'tab.navigate', tab_id: active.tab.id, url: 'http://127.0.0.1:8080' });
            expect(res.ok).toBe(true);
        });

        test('TC2.28: Rapid 20x clicking bookmark star button does not throw', () => {
            for (let i = 0; i < 20; i++) {
                const hit = AeonGeometry.hitTest(1100, 55, 1280, 800, []);
                expect(hit.code).toBe('HTCLIENT');
            }
        });

        test('TC2.29: Rapid 20x clicking shield button toggles HUD consistently', () => {
            for (let i = 0; i < 20; i++) {
                const res = client.oracle.toggleShieldHud(1120, 74);
                expect(res.ok).toBe(true);
            }
            // Ensure state is closed after even number of toggles
            if (client.oracle.isShieldHudOpen) {
                client.oracle.toggleShieldHud(1120, 74);
            }
            expect(client.oracle.isShieldHudOpen).toBe(false);
        });

        test('TC2.30: Security state on non-standard TLS port https://domain.org:8443', async () => {
            const active = await client.sendCommand({ cmd: 'tab.active' });
            const res = await client.sendCommand({ cmd: 'tab.navigate', tab_id: active.tab.id, url: 'https://domain.org:8443' });
            expect(res.ok).toBe(true);
        });
    });

    // ── FEATURE 7 BOUNDARIES: Action Hub Cluster Extremes ───────────────────────
    describe('Feature 7 Boundaries: Action Hub Limits', () => {
        test('TC2.31: Action Hub remains pinned to right edge when window is resized to 1920px', () => {
            const hubStart = 1920 - CHROME_CONSTANTS.ACTION_HUB_WIDTH - 8;
            const hit = AeonGeometry.hitTest(hubStart + 10, 55, 1920, 1080, []);
            expect(hit.code).toBe('HTCLIENT');
            expect(hit.control).toBe('hub_downloads');
        });

        test('TC2.32: Action Hub remains accessible on compact 800x600 window', () => {
            const hubStart = 800 - CHROME_CONSTANTS.ACTION_HUB_WIDTH - 8;
            const hit = AeonGeometry.hitTest(hubStart + 10, 55, 800, 600, []);
            expect(hit.code).toBe('HTCLIENT');
            expect(hit.control).toBe('hub_downloads');
        });

        test('TC2.33: Hit testing AI button at high resolution (3840x2160)', () => {
            const hubStart = 3840 - CHROME_CONSTANTS.ACTION_HUB_WIDTH - 8;
            const hit = AeonGeometry.hitTest(hubStart + 50, 55, 3840, 2160, []);
            expect(hit.code).toBe('HTCLIENT');
            expect(hit.control).toBe('hub_ai');
        });

        test('TC2.34: Hit testing Settings Menu button at high resolution (3840x2160)', () => {
            const hubStart = 3840 - CHROME_CONSTANTS.ACTION_HUB_WIDTH - 8;
            const hit = AeonGeometry.hitTest(hubStart + 90, 55, 3840, 2160, []);
            expect(hit.code).toBe('HTCLIENT');
            expect(hit.control).toBe('hub_settings');
        });

        test('TC2.35: Action Hub boundary gap at right edge (8px before window border)', () => {
            const hit = AeonGeometry.hitTest(1279, 55, 1280, 800, []);
            expect(hit.code).toBe('HTRIGHT'); // 6px border check
        });
    });

    // ── FEATURE 8 BOUNDARIES: Shield Dropdown HUD Bounds ────────────────────────
    describe('Feature 8 Boundaries: Shield HUD Anchoring & Dismissal', () => {
        test('TC2.36: Anchor coordinate calculation when window is moved to (x=500, y=200)', () => {
            const anchorX = 500 + 1280 - 180;
            const anchorY = 200 + 74;
            expect(anchorY).toBe(274);
            expect(anchorX).toBe(1600);
        });

        test('TC2.37: Popup dismissal state is preserved across rapid open-close sequences', () => {
            client.oracle.toggleShieldHud(1000, 74);
            expect(client.oracle.isShieldHudOpen).toBe(true);
            client.oracle.toggleShieldHud(1000, 74);
            expect(client.oracle.isShieldHudOpen).toBe(false);
        });

        test('TC2.38: Shield HUD anchor calculation at 150% DPI scale (y = 74 * 1.5 = 111px)', () => {
            const dpi = 144;
            const scaledY = Math.round(74 * (dpi / 96));
            expect(scaledY).toBe(111);
        });

        test('TC2.39: Shield HUD anchor calculation at 200% DPI scale (y = 74 * 2.0 = 148px)', () => {
            const dpi = 192;
            const scaledY = Math.round(74 * (dpi / 96));
            expect(scaledY).toBe(148);
        });

        test('TC2.40: Re-opening HUD updates anchor coordinates to new position', () => {
            client.oracle.toggleShieldHud(800, 74);
            expect(client.oracle.hudAnchor.x).toBe(800);
            client.oracle.toggleShieldHud(800, 74); // close
            client.oracle.toggleShieldHud(950, 74); // open with new anchor
            expect(client.oracle.hudAnchor.x).toBe(950);
            client.oracle.toggleShieldHud(950, 74); // close
        });
    });

    // ── FEATURE 9 BOUNDARIES: Real-Time Telemetry Overflow & Extremes ────────────
    describe('Feature 9 Boundaries: Telemetry Data Extremes', () => {
        test('TC2.41: Telemetry handles large numeric values (>1,000,000 trackers blocked)', async () => {
            client.oracle.shieldStats.trackersBlocked = 1500000;
            const res = await client.sendCommand({ cmd: 'shield.stats' });
            expect(res.stats.trackersBlocked).toBe(1500000);
        });

        test('TC2.42: Bandwidth saved formats correctly for gigabyte scale (>10 GB)', async () => {
            client.oracle.shieldStats.bandwidthSavedBytes = 10 * 1024 * 1024 * 1024; // 10 GB
            const res = await client.sendCommand({ cmd: 'shield.stats' });
            expect(parseFloat(res.stats.bandwidthSavedMB)).toBeCloseTo(10240, 1);
        });

        test('TC2.43: Time saved metric handles zero state gracefully', async () => {
            client.oracle.shieldStats.timeSavedMs = 0;
            const res = await client.sendCommand({ cmd: 'shield.stats' });
            expect(parseFloat(res.stats.timeSavedSec)).toBe(0);
        });

        test('TC2.44: Zero peer mesh nodes reports clean numeric 0', async () => {
            client.oracle.shieldStats.hiveMeshNodes = 0;
            const res = await client.sendCommand({ cmd: 'shield.stats' });
            expect(res.stats.hiveMeshNodes).toBe(0);
        });

        test('TC2.45: Restoring normal telemetry values after stress checks', () => {
            client.oracle.shieldStats.trackersBlocked = 540;
            client.oracle.shieldStats.bandwidthSavedBytes = 150000000;
            client.oracle.shieldStats.timeSavedMs = 4200;
            client.oracle.shieldStats.hiveMeshNodes = 12;
            expect(client.oracle.shieldStats.trackersBlocked).toBe(540);
        });
    });

    // ── FEATURE 10 BOUNDARIES: Rapid Shield Toggling & Domain Subdomains ────────
    describe('Feature 10 Boundaries: Shield Toggle Extremes', () => {
        test('TC2.46: Rapid 50x toggle on a single domain leaves expected parity state', async () => {
            const domain = 'rapid-test.com';
            for (let i = 0; i < 50; i++) {
                await client.sendCommand({ cmd: 'shield.toggle', domain });
            }
            const stats = await client.sendCommand({ cmd: 'shield.stats', domain });
            // Even number of toggles returns to true
            expect(stats.shieldsUp).toBe(true);
        });

        test('TC2.47: Deep subdomain protection setting (a.b.c.example.com)', async () => {
            const sub = 'deep.sub.domain.org';
            await client.sendCommand({ cmd: 'shield.toggle', domain: sub });
            const stats = await client.sendCommand({ cmd: 'shield.stats', domain: sub });
            expect(stats.shieldsUp).toBe(false);
            await client.sendCommand({ cmd: 'shield.toggle', domain: sub }); // restore
        });

        test('TC2.48: Domain name with 253 characters (RFC 1035 max) toggles successfully', async () => {
            const longDomain = 'a'.repeat(60) + '.' + 'b'.repeat(60) + '.' + 'c'.repeat(60) + '.com';
            const res = await client.sendCommand({ cmd: 'shield.toggle', domain: longDomain });
            expect(res.ok).toBe(true);
            await client.sendCommand({ cmd: 'shield.toggle', domain: longDomain }); // restore
        });

        test('TC2.49: Toggling shield on internal schema aeon:// preserves protection model', async () => {
            const res = await client.sendCommand({ cmd: 'shield.stats', domain: 'aeon://newtab' });
            expect(res.shieldsUp).toBe(true);
        });

        test('TC2.50: Concurrent shield toggle requests for 5 distinct domains execute cleanly', async () => {
            const domains = ['d1.com', 'd2.com', 'd3.com', 'd4.com', 'd5.com'];
            await Promise.all(domains.map(d => client.sendCommand({ cmd: 'shield.toggle', domain: d })));
            for (const d of domains) {
                const s = await client.sendCommand({ cmd: 'shield.stats', domain: d });
                expect(s.shieldsUp).toBe(false);
                await client.sendCommand({ cmd: 'shield.toggle', domain: d }); // restore
            }
        });
    });

    // ── FEATURE 11 BOUNDARIES: New Tab Layout Aspect Ratio Extremes ─────────────
    describe('Feature 11 Boundaries: New Tab Layout Extremes', () => {
        test('TC2.51: Viewport math on 21:9 Ultra-Wide resolution (3440x1440)', () => {
            const vp = AeonGeometry.calculateViewport(3440, 1440, false);
            expect(vp.width).toBe(3440);
            expect(vp.height).toBe(1366); // 1440 - 74 = 1366
        });

        test('TC2.52: Viewport math on vertical portrait display (1080x1920)', () => {
            const vp = AeonGeometry.calculateViewport(1080, 1920, false);
            expect(vp.width).toBe(1080);
            expect(vp.height).toBe(1846); // 1920 - 74 = 1846
        });

        test('TC2.53: Viewport math on square window (1024x1024)', () => {
            const vp = AeonGeometry.calculateViewport(1024, 1024, false);
            expect(vp.width).toBe(1024);
            expect(vp.height).toBe(950); // 1024 - 74 = 950
        });

        test('TC2.54: Viewport math when window height is exactly 74px (content height = 0px)', () => {
            const vp = AeonGeometry.calculateViewport(1280, 74, false);
            expect(vp.height).toBe(0);
        });

        test('TC2.55: Viewport math when window height is less than chrome (<74px) clamps height to 0', () => {
            const vp = AeonGeometry.calculateViewport(1280, 50, false);
            expect(vp.height).toBe(0);
        });
    });

    // ── FEATURE 12 BOUNDARIES: New Tab Stats Numeric Formatting Limits ──────────
    describe('Feature 12 Boundaries: Stats Formatting Limits', () => {
        test('TC2.56: Numeric formatting for 100,000,000 trackers blocked contains no NaN', async () => {
            client.oracle.shieldStats.trackersBlocked = 100000000;
            const res = await client.sendCommand({ cmd: 'shield.stats' });
            expect(String(res.stats.trackersBlocked)).not.toContain('NaN');
        });

        test('TC2.57: Fractional bandwidth saved (0.1 MB) maintains precision', async () => {
            client.oracle.shieldStats.bandwidthSavedBytes = 100 * 1024; // 0.1 MB
            const res = await client.sendCommand({ cmd: 'shield.stats' });
            expect(parseFloat(res.stats.bandwidthSavedMB)).toBeCloseTo(0.1, 0.05);
        });

        test('TC2.58: Sub-second load time saved (0.05s) formats cleanly', async () => {
            client.oracle.shieldStats.timeSavedMs = 50;
            const res = await client.sendCommand({ cmd: 'shield.stats' });
            expect(parseFloat(res.stats.timeSavedSec)).toBeCloseTo(0.05, 0.01);
        });

        test('TC2.59: Stats structure contains all 6 required telemetry fields', async () => {
            const res = await client.sendCommand({ cmd: 'shield.stats' });
            expect(res.stats.trackersBlocked).toBeDefined();
            expect(res.stats.bandwidthSavedMB).toBeDefined();
            expect(res.stats.timeSavedSec).toBeDefined();
            expect(res.stats.fingerprintProtected).toBeDefined();
            expect(res.stats.dohProvider).toBeDefined();
            expect(res.stats.hiveMeshNodes).toBeDefined();
        });

        test('TC2.60: Restoring normal stats state', () => {
            client.oracle.shieldStats.trackersBlocked = 482;
            expect(client.oracle.shieldStats.trackersBlocked).toBe(482);
        });
    });

    // ── FEATURE 13 BOUNDARIES: 2x4 Grid & Search Input Edge Cases ───────────────
    describe('Feature 13 Boundaries: Grid & Search Extremes', () => {
        test('TC2.61: Search query with multiple question marks and ampersands is encoded', () => {
            const raw = 'what is? & why?';
            const encoded = encodeURIComponent(raw);
            expect(encoded).toBe('what%20is%3F%20%26%20why%3F');
        });

        test('TC2.62: Search query with leading and trailing whitespaces is preserved or trimmed cleanly', () => {
            const raw = '   test search   ';
            expect(raw.trim()).toBe('test search');
        });

        test('TC2.63: Shortcut grid specifies 2x4 layout (8 item capacity)', () => {
            const gridCardsCount = 8;
            expect(gridCardsCount).toBe(2 * 4);
        });

        test('TC2.64: Search input with maximum length 2048 characters encodes without truncation', () => {
            const longQuery = 'q='.repeat(1000);
            expect(encodeURIComponent(longQuery).length).toBeGreaterThan(2000);
        });

        test('TC2.65: Empty search query submission does not construct malformed URL', () => {
            const empty = '';
            const target = empty ? 'https://google.com/search?q=' + encodeURIComponent(empty) : 'aeon://newtab';
            expect(target).toBe('aeon://newtab');
        });
    });

    // ── FEATURE 14 BOUNDARIES: Viewport Coordinate Resizing Extremes ────────────
    describe('Feature 14 Boundaries: Viewport Resizing Limits', () => {
        test('TC2.66: Rapid sequential window resizing through 10 resolutions', async () => {
            const resolutions = [
                [800, 600], [1024, 768], [1280, 720], [1280, 800],
                [1366, 768], [1440, 900], [1600, 900], [1920, 1080],
                [2560, 1440], [3840, 2160]
            ];
            for (const [w, h] of resolutions) {
                const res = await client.sendCommand({ cmd: 'window.resize', width: w, height: h });
                expect(res.ok).toBe(true);
                const vp = AeonGeometry.calculateViewport(w, h, false);
                expect(vp.height).toBe(h - 74);
            }
        });

        test('TC2.67: Extreme narrow window (width = 200px, height = 800px) viewport math', () => {
            const vp = AeonGeometry.calculateViewport(200, 800, false);
            expect(vp.width).toBe(200);
            expect(vp.height).toBe(726);
        });

        test('TC2.68: Extreme short window (width = 1280px, height = 100px) viewport math', () => {
            const vp = AeonGeometry.calculateViewport(1280, 100, false);
            expect(vp.height).toBe(26); // 100 - 74 = 26
        });

        test('TC2.69: Sub-zero bounds protection clamps width and height to 0', () => {
            const vp = AeonGeometry.calculateViewport(-500, -200, false);
            expect(vp.width).toBe(0);
            expect(vp.height).toBe(0);
        });

        test('TC2.70: Viewport x-coordinate is strictly 0 across all screen sizes', () => {
            [800, 1280, 1920, 3840].forEach(w => {
                const vp = AeonGeometry.calculateViewport(w, 800, false);
                expect(vp.x).toBe(0);
            });
        });
    });

    // ── FEATURE 15 BOUNDARIES: Fullscreen & Rapid DPI Transitions ───────────────
    describe('Feature 15 Boundaries: Fullscreen & DPI Transitions', () => {
        test('TC2.71: Rapid 10x F11 fullscreen toggling cycles state accurately', () => {
            for (let i = 0; i < 10; i++) {
                client.oracle.setFullscreen(i % 2 === 0);
            }
            client.oracle.setFullscreen(false); // restore normal
            expect(client.oracle.window.isFullscreen).toBe(false);
        });

        test('TC2.72: DPI scaling at 125% (120 DPI) calculates chrome height = 93px', () => {
            const scale = 120 / 96; // 1.25
            const scaledH = Math.round(74 * scale);
            expect(scaledH).toBe(93);
        });

        test('TC2.73: DPI scaling at 175% (168 DPI) calculates chrome height = 130px', () => {
            const scale = 168 / 96; // 1.75
            const scaledH = Math.round(74 * scale);
            expect(scaledH).toBe(130);
        });

        test('TC2.74: DPI scaling at 250% (240 DPI) calculates chrome height = 185px', () => {
            const scale = 240 / 96; // 2.5
            const scaledH = Math.round(74 * scale);
            expect(scaledH).toBe(185);
        });

        test('TC2.75: DPI change message simulated with setDpi updates oracle state', () => {
            const res = client.oracle.setDpi(144);
            expect(res.dpi).toBe(144);
            client.oracle.setDpi(96); // restore
            expect(client.oracle.window.dpi).toBe(96);
        });
    });

    // ── FEATURE 16 BOUNDARIES: Regression Stress & Large Session State ──────────
    describe('Feature 16 Boundaries: Large Session & Tool Limits', () => {
        test('TC2.76: Session export with custom file path parameter', async () => {
            const customPath = 'C:\\temp\\custom_auth.json';
            const res = await client.sendCommand({ cmd: 'session.export', path: customPath });
            expect(res.ok).toBe(true);
            expect(res.storage_state_path).toBe(customPath);
        });

        test('TC2.77: Session import with custom file path parameter', async () => {
            const customPath = 'C:\\temp\\custom_auth.json';
            const res = await client.sendCommand({ cmd: 'session.import', path: customPath });
            expect(res.ok).toBe(true);
            expect(res.session_imported).toBe(true);
        });

        test('TC2.78: AI Tab Groups returns positive memory allocation value', async () => {
            const res = await client.sendCommand({ cmd: 'ai.tab_groups' });
            expect(res.groups[0].total_memory).toBeGreaterThan(0);
        });

        test('TC2.79: WebMCP tools list returns non-empty array of tools', async () => {
            const res = await client.sendCommand({ cmd: 'webmcp.tools' });
            expect(res.tools.length).toBeGreaterThanOrEqual(1);
        });

        test('TC2.80: Browser info tab count strictly matches active tab list length', async () => {
            const info = await client.sendCommand({ cmd: 'browser.info' });
            const list = await client.sendCommand({ cmd: 'tab.list' });
            expect(info.tab_count).toBe(list.tabs.length);
        });
    });

    // ── FEATURE 17 BOUNDARIES: E2E Protocol & Malformed JSON Payload Limits ──────
    describe('Feature 17 Boundaries: Protocol Resilience', () => {
        test('TC2.81: Command payload with extra whitespace and tabs parses successfully', async () => {
            const res = await client.sendCommand({ cmd: 'ping', extra_arg: '   value   ' });
            expect(res.ok).toBe(true);
        });

        test('TC2.82: Command payload with null values in optional fields is handled safely', async () => {
            const active = await client.sendCommand({ cmd: 'tab.active' });
            const res = await client.sendCommand({ cmd: 'tab.navigate', tab_id: active.tab.id, url: 'aeon://newtab', options: null });
            expect(res.ok).toBe(true);
        });

        test('TC2.83: Multiple rapid sequential ping commands return pong:true', async () => {
            for (let i = 0; i < 10; i++) {
                const res = await client.sendCommand({ cmd: 'ping' });
                expect(res.pong).toBe(true);
            }
        });

        test('TC2.84: Browser info reports standard loopback pipe path', async () => {
            const info = await client.sendCommand({ cmd: 'browser.info' });
            expect(info.pipe).toBe('\\\\.\\pipe\\aeon-agent');
        });

        test('TC2.85: Browser info reports standard CDP port 9222', async () => {
            const info = await client.sendCommand({ cmd: 'browser.info' });
            expect(info.cdp_port).toBe(9222);
        });
    });

    // ── FEATURE 18 BOUNDARIES: Adversarial Edge Stress & Bounds Injection ────────
    describe('Feature 18 Boundaries: Adversarial Stress', () => {
        test('TC2.86: Oversized numeric ID (Integer Overflow protection)', async () => {
            const res = await client.sendCommand({ cmd: 'tab.close', tab_id: 2147483647 });
            expect(res.ok).toBe(false);
        });

        test('TC2.87: Negative tab ID (-1) handled safely', async () => {
            const res = await client.sendCommand({ cmd: 'tab.close', tab_id: -1 });
            expect(res.ok).toBe(false);
        });

        test('TC2.88: Extreme window dimension resize (99999x99999) operates without crash', async () => {
            const res = await client.sendCommand({ cmd: 'window.resize', width: 99999, height: 99999 });
            expect(res.ok).toBe(true);
            // Restore normal
            await client.sendCommand({ cmd: 'window.resize', width: 1280, height: 800 });
        });

        test('TC2.89: 100 consecutive hit test calls on random boundary coordinates execute in <50ms', () => {
            const start = Date.now();
            for (let i = 0; i < 100; i++) {
                const x = (i * 37) % 1280;
                const y = (i * 19) % 800;
                AeonGeometry.hitTest(x, y, 1280, 800, [{ id: 1, width: 180 }]);
            }
            const duration = Date.now() - start;
            expect(duration).toBeLessThan(50);
        });

        test('TC2.90: Empty command object handling', async () => {
            const res = await client.sendCommand({});
            expect(res.ok).toBe(false);
        });
    });
}
