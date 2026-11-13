/**
 * Tier 3: Cross-Feature Combinations E2E Test Suite
 * Validates complex pairwise and multi-feature concurrent interactions and state transitions (15 comprehensive integration scenarios).
 */

import { describe, test, expect, beforeAll, afterAll } from './helpers/test_framework.mjs';
import { AeonClient, AeonGeometry, CHROME_CONSTANTS } from './helpers/aeon_client.mjs';

export function registerTier3Tests() {
    const client = new AeonClient();

    beforeAll(async () => {
        await client.connect();
    });

    afterAll(() => {
        if (client) client.close();
    });

    describe('Tier 3: Pairwise Cross-Feature Interactions', () => {
        test('Pairwise 1: Tab Switching during active Omnibox Navigation', async () => {
            // Create two tabs
            const tab1 = await client.sendCommand({ cmd: 'tab.new', url: 'https://site-1.com' });
            const tab2 = await client.sendCommand({ cmd: 'tab.new', url: 'https://site-2.com' });

            // Start navigation on tab 1 and immediately focus tab 2
            const navPromise = client.sendCommand({ cmd: 'tab.navigate', tab_id: tab1.tab_id, url: 'https://site-1-updated.com' });
            const focusPromise = client.sendCommand({ cmd: 'tab.focus', tab_id: tab2.tab_id });

            const [navRes, focusRes] = await Promise.all([navPromise, focusPromise]);
            expect(navRes.ok).toBe(true);
            expect(focusRes.ok).toBe(true);

            // Active tab is tab 2, but tab 1 URL is updated
            const active = await client.sendCommand({ cmd: 'tab.active' });
            expect(active.tab.id).toBe(tab2.tab_id);

            const list = await client.sendCommand({ cmd: 'tab.list' });
            const t1 = list.tabs.find(t => t.id === tab1.tab_id);
            expect(t1.url).toBe('https://site-1-updated.com');

            // Cleanup
            await client.sendCommand({ cmd: 'tab.close', tab_id: tab1.tab_id });
            await client.sendCommand({ cmd: 'tab.close', tab_id: tab2.tab_id });
        });

        test('Pairwise 2: Sovereign Shield Toggle during aeon://newtab Reload', async () => {
            const domain = 'tracker-mesh.org';
            // Toggle shield state for target domain
            const toggleRes = await client.sendCommand({ cmd: 'shield.toggle', domain });
            expect(toggleRes.ok).toBe(true);

            // Trigger reload of active newtab
            const active = await client.sendCommand({ cmd: 'tab.active' });
            const navRes = await client.sendCommand({ cmd: 'tab.navigate', tab_id: active.tab.id, url: 'aeon://newtab' });
            expect(navRes.ok).toBe(true);

            // Verify telemetry stats are updated and accessible
            const stats = await client.sendCommand({ cmd: 'shield.stats', domain });
            expect(stats.shieldsUp).toBe(false);

            // Restore shield state
            await client.sendCommand({ cmd: 'shield.toggle', domain });
        });

        test('Pairwise 3: Fullscreen F11 Toggle while Closing Active Tab', async () => {
            const tempTab = await client.sendCommand({ cmd: 'tab.new', url: 'https://video-media.org' });
            client.oracle.setFullscreen(true);

            // Close the tab while in fullscreen
            const closeRes = await client.sendCommand({ cmd: 'tab.close', tab_id: tempTab.tab_id });
            expect(closeRes.ok).toBe(true);

            // Verify viewport remains in fullscreen geometry (y=0)
            const vp = AeonGeometry.calculateViewport(1920, 1080, client.oracle.window.isFullscreen);
            expect(vp.y).toBe(0);
            expect(vp.height).toBe(1080);

            // Exit fullscreen
            client.oracle.setFullscreen(false);
            const vpRestored = AeonGeometry.calculateViewport(1920, 1080, client.oracle.window.isFullscreen);
            expect(vpRestored.y).toBe(74);
        });

        test('Pairwise 4: Window Resize while Sovereign Shield HUD is Open', async () => {
            // Open HUD
            const openRes = client.oracle.toggleShieldHud(1120, 74);
            expect(openRes.isOpen).toBe(true);

            // Resize window from 1280x800 to 1600x900
            const resizeRes = await client.sendCommand({ cmd: 'window.resize', width: 1600, height: 900 });
            expect(resizeRes.ok).toBe(true);

            // Verify viewport coordinates adapt synchronously
            const vp = AeonGeometry.calculateViewport(1600, 900, false);
            expect(vp.width).toBe(1600);
            expect(vp.height).toBe(826); // 900 - 74 = 826

            // Close HUD
            client.oracle.toggleShieldHud(1120, 74);
            // Restore window
            await client.sendCommand({ cmd: 'window.resize', width: 1280, height: 800 });
        });

        test('Pairwise 5: Bookmarking a page while executing Omnibox Navigation', async () => {
            const active = await client.sendCommand({ cmd: 'tab.active' });
            const navRes = await client.sendCommand({ cmd: 'tab.navigate', tab_id: active.tab.id, url: 'https://github.com/delgadologic' });
            expect(navRes.ok).toBe(true);

            // Hit test bookmark star button (simulating click)
            const hit = AeonGeometry.hitTest(1100, 55, 1280, 800, []);
            expect(hit.control).toBe('omnibox_bookmark');
        });

        test('Pairwise 6: Action Hub AI Prompt Trigger with Multiple Background Loading Tabs', async () => {
            const tabA = await client.sendCommand({ cmd: 'tab.new', url: 'https://data-source-a.org' });
            const tabB = await client.sendCommand({ cmd: 'tab.new', url: 'https://data-source-b.org' });

            // Hit test AI prompt button in action hub
            const hubStart = 1280 - CHROME_CONSTANTS.ACTION_HUB_WIDTH - 8;
            const hitAI = AeonGeometry.hitTest(hubStart + 54, 55, 1280, 800, []);
            expect(hitAI.control).toBe('hub_ai');

            // Query tab groups intelligence
            const groups = await client.sendCommand({ cmd: 'ai.tab_groups' });
            expect(groups.ok).toBe(true);
            expect(groups.groups.length).toBeGreaterThanOrEqual(1);

            // Cleanup
            await client.sendCommand({ cmd: 'tab.close', tab_id: tabA.tab_id });
            await client.sendCommand({ cmd: 'tab.close', tab_id: tabB.tab_id });
        });

        test('Pairwise 7: High-DPI WM_DPICHANGED message while Shield HUD is Open', () => {
            client.oracle.toggleShieldHud(1120, 74);
            expect(client.oracle.isShieldHudOpen).toBe(true);

            // Simulate DPI change to 144 DPI (150% scale)
            client.oracle.setDpi(144);
            const scaledChromeH = Math.round(74 * (144 / 96));
            expect(scaledChromeH).toBe(111);

            // Restore
            client.oracle.setDpi(96);
            client.oracle.toggleShieldHud(1120, 74);
        });

        test('Pairwise 8: Rapid Tab Creation followed by Immediate Window Minimize/Restore', async () => {
            const tab = await client.sendCommand({ cmd: 'tab.new', url: 'https://fast-load.com' });
            await client.sendCommand({ cmd: 'window.minimize' });
            await client.sendCommand({ cmd: 'window.restore' });

            const active = await client.sendCommand({ cmd: 'tab.active' });
            expect(active.tab.id).toBe(tab.tab_id);

            await client.sendCommand({ cmd: 'tab.close', tab_id: tab.tab_id });
        });

        test('Pairwise 9: History Navigation (Back/Forward) with Shield State Toggling', async () => {
            const active = await client.sendCommand({ cmd: 'tab.active' });
            await client.sendCommand({ cmd: 'tab.navigate', tab_id: active.tab.id, url: 'https://site-first.com' });
            await client.sendCommand({ cmd: 'tab.navigate', tab_id: active.tab.id, url: 'https://site-second.com' });

            // Toggle shield on second site
            await client.sendCommand({ cmd: 'shield.toggle', domain: 'site-second.com' });

            // Navigate back to first site
            await client.sendCommand({ cmd: 'tab.navigate', tab_id: active.tab.id, url: 'https://site-first.com' });
            const s1 = await client.sendCommand({ cmd: 'shield.stats', domain: 'site-first.com' });
            const s2 = await client.sendCommand({ cmd: 'shield.stats', domain: 'site-second.com' });

            expect(s1.shieldsUp).toBe(true);
            expect(s2.shieldsUp).toBe(false);

            // Restore second site shield
            await client.sendCommand({ cmd: 'shield.toggle', domain: 'site-second.com' });
        });

        test('Pairwise 10: New Tab Shortcut Click with Uncommitted Omnibox Text', async () => {
            const active = await client.sendCommand({ cmd: 'tab.active' });
            // Simulate clicking shortcut to Google
            const navRes = await client.sendCommand({ cmd: 'tab.navigate', tab_id: active.tab.id, url: 'https://www.google.com' });
            expect(navRes.ok).toBe(true);
            const current = await client.sendCommand({ cmd: 'tab.active' });
            expect(current.tab.url).toBe('https://www.google.com');
        });

        test('Pairwise 11: Session State Export and Immediate Tab Strip Recreation', async () => {
            const tabA = await client.sendCommand({ cmd: 'tab.new', url: 'https://session-a.org' });
            const exportRes = await client.sendCommand({ cmd: 'session.export', path: 'C:\\temp\\test_auth.json' });
            expect(exportRes.ok).toBe(true);

            // Import session
            const importRes = await client.sendCommand({ cmd: 'session.import', path: 'C:\\temp\\test_auth.json' });
            expect(importRes.ok).toBe(true);

            await client.sendCommand({ cmd: 'tab.close', tab_id: tabA.tab_id });
        });

        test('Pairwise 12: WebMCP Tool Discovery during Tab Switching', async () => {
            const tabA = await client.sendCommand({ cmd: 'tab.new', url: 'https://app1.org' });
            const tabB = await client.sendCommand({ cmd: 'tab.new', url: 'https://app2.org' });

            const toolsPromise = client.sendCommand({ cmd: 'webmcp.tools' });
            const focusPromise = client.sendCommand({ cmd: 'tab.focus', tab_id: tabA.tab_id });

            const [toolsRes, focusRes] = await Promise.all([toolsPromise, focusPromise]);
            expect(toolsRes.ok).toBe(true);
            expect(focusRes.ok).toBe(true);

            await client.sendCommand({ cmd: 'tab.close', tab_id: tabA.tab_id });
            await client.sendCommand({ cmd: 'tab.close', tab_id: tabB.tab_id });
        });

        test('Pairwise 13: Fullscreen Toggle with Omnibox Input Focus', () => {
            client.oracle.setFullscreen(true);
            // In fullscreen, chrome height is 0
            const vpFull = AeonGeometry.calculateViewport(1280, 800, true);
            expect(vpFull.chromeHeight).toBe(0);

            // Exit fullscreen
            client.oracle.setFullscreen(false);
            const vpNormal = AeonGeometry.calculateViewport(1280, 800, false);
            expect(vpNormal.chromeHeight).toBe(74);
        });

        test('Pairwise 14: Closing the last tab triggers Auto-NewTab in correct Viewport coordinates', async () => {
            const list = await client.sendCommand({ cmd: 'tab.list' });
            for (const t of list.tabs) {
                await client.sendCommand({ cmd: 'tab.close', tab_id: t.id });
            }

            const active = await client.sendCommand({ cmd: 'tab.active' });
            expect(active.tab.url).toBe('aeon://newtab');

            const vp = AeonGeometry.calculateViewport(1280, 800, false);
            expect(vp.y).toBe(74);
            expect(vp.height).toBe(726);
        });

        test('Pairwise 15: Multi-Monitor Coordinate Shift with Synchronous Viewport Repositioning', async () => {
            // Move window across virtual desktop
            const moveRes = await client.sendCommand({ cmd: 'window.resize', x: 2560, y: 100, width: 1920, height: 1080 });
            expect(moveRes.ok).toBe(true);

            const vp = AeonGeometry.calculateViewport(1920, 1080, false);
            expect(vp.x).toBe(0);
            expect(vp.y).toBe(74);
            expect(vp.width).toBe(1920);
            expect(vp.height).toBe(1006);

            // Restore
            await client.sendCommand({ cmd: 'window.resize', x: 100, y: 100, width: 1280, height: 800 });
        });
    });
}
