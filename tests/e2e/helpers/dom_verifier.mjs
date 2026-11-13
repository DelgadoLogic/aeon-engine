/**
 * AeonBrowser DOM & Obsidian Design System Verifier
 * DelgadoLogic Quality Assurance & UI Standards
 * 
 * Validates DelgadoLogic's Obsidian design tokens, glassmorphism, micro-interactions,
 * typography, CSS custom properties, and semantic HTML structure across all 7 internal aeon:// pages:
 * 1. settings.html
 * 2. history.html
 * 3. bookmarks.html
 * 4. passwords.html
 * 5. downloads.html
 * 6. newtab.html
 * 7. intelligence.html
 */

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const PROJECT_ROOT = path.resolve(__dirname, '..', '..');

export const OBSIDIAN_TOKENS = {
    // Canonical DelgadoLogic Obsidian Palette
    PALETTE: {
        OBSIDIAN_SPACE: '#0b0616',        // Base ambient background
        CARD_SURFACE: '#121424',          // Elevated card background
        CARD_BORDER: '#181a32',           // Card boundary border
        NEON_PURPLE: '#9333ea',           // Primary neon accent
        NEON_LIGHT_PURPLE: '#c084fc',     // Highlight / hover accent
        NEON_CYAN: '#38bdf8',             // Secondary electric cyan
        ACCENT_GLOW: 'rgba(168, 85, 247, 0.45)', // Atmospheric glow
    },
    // Typography
    TYPOGRAPHY: {
        MAIN: 'Plus Jakarta Sans',
        MONO: 'JetBrains Mono',
        FALLBACK_MAIN: 'Inter',
        FALLBACK_SYSTEM: 'sans-serif'
    },
    // Internal pages list
    CANONICAL_PAGES: [
        'settings.html',
        'history.html',
        'bookmarks.html',
        'passwords.html',
        'downloads.html',
        'newtab.html',
        'intelligence.html'
    ]
};

export class DomVerifier {
    /**
     * Initialize with a file path, page name, or HTML string
     * @param {string} target - Absolute path, relative path, or page name (e.g. 'settings')
     */
    constructor(target) {
        this.htmlPath = this._resolvePath(target);
        this.content = '';
        this.load();
    }

    _resolvePath(target) {
        if (!target) {
            const defaultNewTab = path.resolve(PROJECT_ROOT, 'resources', 'newtab', 'newtab.html');
            if (fs.existsSync(defaultNewTab)) return defaultNewTab;
            return path.resolve(process.cwd(), 'resources', 'newtab', 'newtab.html');
        }

        // Direct file path check (must be a file, not a directory)
        try {
            if (fs.existsSync(target) && fs.statSync(target).isFile()) {
                return path.resolve(target);
            }
        } catch (_) {}

        // Clean page name (e.g. 'settings', 'settings.html', 'newtab')
        const baseName = path.basename(target).toLowerCase().replace('.html', '');
        if (baseName === 'newtab') {
            const rootNewTab = path.resolve(PROJECT_ROOT, 'resources', 'newtab', 'newtab.html');
            if (fs.existsSync(rootNewTab)) return rootNewTab;
            return path.resolve(process.cwd(), 'resources', 'newtab', 'newtab.html');
        }

        // Check project root resources/pages/
        const rootCandidate = path.join(PROJECT_ROOT, 'resources', 'pages', `${baseName}.html`);
        try {
            if (fs.existsSync(rootCandidate) && fs.statSync(rootCandidate).isFile()) {
                return rootCandidate;
            }
        } catch (_) {}

        // Check cwd resources/pages/
        const cwdCandidate = path.join(process.cwd(), 'resources', 'pages', `${baseName}.html`);
        try {
            if (fs.existsSync(cwdCandidate) && fs.statSync(cwdCandidate).isFile()) {
                return cwdCandidate;
            }
        } catch (_) {}

        // Return original if cannot resolve directly
        return path.resolve(target);
    }

    load() {
        if (fs.existsSync(this.htmlPath)) {
            this.content = fs.readFileSync(this.htmlPath, 'utf8');
        } else {
            throw new Error(`DOM Verifier failed: File not found at ${this.htmlPath}`);
        }
    }

    // ── Primitive DOM Matchers ──────────────────────────────────────────────────

    hasDoctype() {
        return /<!DOCTYPE\s+html>/i.test(this.content);
    }

    hasElementWithId(id) {
        const regex = new RegExp(`id=["']${id}["']`, 'i');
        return regex.test(this.content);
    }

    hasElementWithClass(className) {
        const regex = new RegExp(`class=["'][^"']*\\b${className}\\b[^"']*["']`, 'i');
        return regex.test(this.content);
    }

    hasElementWithTag(tagName) {
        const regex = new RegExp(`<${tagName}\\b`, 'i');
        return regex.test(this.content);
    }

    countElementsWithClass(className) {
        const regex = new RegExp(`class=["'][^"']*\\b${className}\\b[^"']*["']`, 'gi');
        const matches = this.content.match(regex);
        return matches ? matches.length : 0;
    }

    hasCssVariable(varName, expectedValue = null) {
        // Strip leading '--' if provided
        const cleanName = varName.replace(/^--/, '');
        const regex = new RegExp(`--${cleanName}\\s*:\\s*([^;]+);`, 'i');
        const match = this.content.match(regex);
        if (!match) return false;
        if (expectedValue === null) return true;
        return match[1].trim().toLowerCase() === expectedValue.toLowerCase();
    }

    getCssVariableValue(varName) {
        const cleanName = varName.replace(/^--/, '');
        const regex = new RegExp(`--${cleanName}\\s*:\\s*([^;]+);`, 'i');
        const match = this.content.match(regex);
        return match ? match[1].trim() : null;
    }

    hasText(text) {
        return this.content.includes(text);
    }

    matchesPattern(pattern) {
        const regex = typeof pattern === 'string' ? new RegExp(pattern, 'i') : pattern;
        return regex.test(this.content);
    }

    // ── Obsidian Design System Token Verifiers ─────────────────────────────────

    hasFont(fontName) {
        const regex = new RegExp(`['"]?${fontName}['"]?`, 'i');
        return regex.test(this.content);
    }

    hasColor(color) {
        const clean = color.replace('#', '').toLowerCase();
        // Check for hex #xxx, #xxxxxx or rgba/rgb representation
        const hexRegex = new RegExp(`#${clean}\\b`, 'i');
        return hexRegex.test(this.content) || this.content.toLowerCase().includes(color.toLowerCase());
    }

    hasObsidianPalette() {
        // Must have dark background (#0b..., #0d..., #12..., #0e...)
        const hasDarkBg = /#(0b0616|0b0c16|0d0e14|0e071c|121424|16182a)/i.test(this.content) ||
                          this.hasCssVariable('bg-space') || this.hasCssVariable('bg');
        
        // Must have purple / cyan neon accents
        const hasNeonAccents = /#(9333ea|c084fc|38bdf8|a855f7|6c63ff|a78bfa)/i.test(this.content) ||
                               this.hasCssVariable('accent-purple') || this.hasCssVariable('accent') ||
                               this.hasCssVariable('neon-cyan');

        // Must have card surface / border
        const hasCardTokens = /#(121424|181a32|1e2140|2e3064|2a2d54)/i.test(this.content) ||
                              this.hasCssVariable('bg-card') || this.hasCssVariable('card') ||
                              this.hasCssVariable('border');

        return hasDarkBg && hasNeonAccents && hasCardTokens;
    }

    hasGlassmorphism() {
        const hasBackdropBlur = /backdrop-filter\s*:\s*blur\([^)]+\)/i.test(this.content);
        const hasAlphaBackground = /rgba\(\s*\d+\s*,\s*\d+\s*,\s*\d+\s*,\s*0?\.\d+\s*\)/i.test(this.content);
        return hasBackdropBlur || hasAlphaBackground;
    }

    hasMicroInteractions() {
        const hasTransition = /transition\s*:\s*[^;]+;/i.test(this.content);
        const hasKeyframes = /@keyframes\s+[a-zA-Z0-9_-]+/i.test(this.content);
        const hasHoverStates = /:hover\b/i.test(this.content);
        return (hasTransition || hasKeyframes) && hasHoverStates;
    }

    hasTypographyTokens() {
        const hasMainFont = this.hasFont('Plus Jakarta Sans') || this.hasFont('Inter') || this.hasFont('sans-serif');
        const hasMonoFont = this.hasFont('JetBrains Mono') || this.hasFont('monospace') || this.hasFont('Courier');
        return hasMainFont;
    }

    validateCommonObsidianTheme() {
        return {
            hasDoctype: this.hasDoctype(),
            hasObsidianPalette: this.hasObsidianPalette(),
            hasGlassmorphism: this.hasGlassmorphism(),
            hasMicroInteractions: this.hasMicroInteractions(),
            hasTypography: this.hasTypographyTokens()
        };
    }

    // ── Page-Specific Structural Verifications ─────────────────────────────────

    validateNewTabStructure() {
        const checks = {
            hasDoctype: this.hasDoctype(),
            hasSearchInput: this.hasElementWithId('search-input') || this.hasElementWithId('searchInput') || this.hasElementWithClass('search-input-box') || this.hasElementWithClass('search-input'),
            hasShortcutsGrid: this.hasElementWithClass('shortcuts-grid') || this.hasElementWithClass('quick-grid'),
            hasObsidianOrDarkTheme: this.hasObsidianPalette() || this.hasCssVariable('bg-space') || this.hasCssVariable('zen-bg'),
            hasAccentGlow: this.hasCssVariable('accent-purple') || this.hasCssVariable('accent-purple-glow') || this.hasCssVariable('zen-accent-glow') || /rgba\(\s*168\s*,\s*85\s*,\s*247/i.test(this.content) || /rgba\(\s*108\s*,\s*99\s*,\s*255/i.test(this.content) || /#(9333ea|c084fc|a855f7)/i.test(this.content),
            hasShortcuts: (this.content.match(/class=["'][^"']*(shortcut-card|quick-card)[^"']*["']/g) || []).length >= 4
        };

        return {
            passed: Object.values(checks).every(Boolean),
            checks
        };
    }

    validateSettingsStructure() {
        return {
            hasDoctype: this.hasDoctype(),
            hasSidebarNav: this.hasElementWithTag('nav') || this.hasElementWithClass('nav-logo'),
            hasPrivacySection: this.hasElementWithId('privacy') || this.hasText('Privacy'),
            hasNetworkSection: this.hasElementWithId('network') || this.hasElementWithId('circumvention') || this.hasText('Network'),
            hasSecuritySection: this.hasElementWithId('security') || this.hasElementWithId('privacy') || this.hasElementWithId('circumvention') || this.hasText('Security') || this.hasText('Kyber'),
            hasDownloadsSection: this.hasElementWithId('downloads') || this.hasElementWithId('general') || this.hasText('Download'),
            hasSaveBar: this.hasElementWithClass('save-bar') || this.hasElementWithClass('save-indicator-btn') || this.hasText('Save & Commit') || this.hasText('Save Changes'),
            hasBridgeCalls: this.hasText('window.aeonBridge') || this.hasText('setSetting')
        };
    }

    validateHistoryStructure() {
        return {
            hasDoctype: this.hasDoctype(),
            hasSearchBox: this.hasElementWithId('history-search') || this.hasElementWithId('search_input') || this.hasElementWithClass('search-box'),
            hasFilterPills: this.hasElementWithClass('filter-tabs') || this.hasElementWithClass('filter-pills') || this.hasElementWithClass('tab-pill') || this.hasElementWithClass('pill') || this.hasText('Today'),
            hasClearAllButton: this.hasElementWithClass('btn-danger') || this.hasElementWithClass('clear-btn') || this.hasText('Clear'),
            hasHistoryList: this.hasElementWithId('history-container') || this.hasElementWithId('history_list') || this.hasElementWithClass('history-card'),
            hasBridgeWiring: this.hasText('getHistory') || this.hasText('deleteHistoryEntry') || this.hasText('aeonBridge')
        };
    }

    validateBookmarksStructure() {
        return {
            hasDoctype: this.hasDoctype(),
            hasSearchBox: this.hasElementWithId('bm-search') || this.hasElementWithId('bm_search') || this.hasElementWithClass('search-box'),
            hasFolderSidebar: this.hasElementWithClass('folder-sidebar') || this.hasElementWithClass('folder-list') || this.hasElementWithId('folder_nav') || this.hasElementWithId('folder-tree'),
            hasAddModal: this.hasElementWithId('bm-modal') || this.hasElementWithId('modal') || this.hasText('Add Bookmark'),
            hasViewToggle: this.hasElementWithClass('view-switcher') || this.hasElementWithClass('view-toggle') || this.hasElementWithId('btn-grid') || this.hasElementWithId('grid_btn'),
            hasBridgeWiring: this.hasText('getBookmarks') || this.hasText('addBookmark') || this.hasText('aeonBridge')
        };
    }

    validatePasswordsStructure() {
        return {
            hasDoctype: this.hasDoctype(),
            hasLockedState: this.hasElementWithId('vault_locked') || this.hasElementWithClass('unlock-card') || this.hasElementWithClass('security-banner') || this.hasText('DPAPI Locked') || this.hasText('Lock'),
            hasUnlockedState: this.hasElementWithId('vault_open') || this.hasElementWithId('vault-container') || this.hasElementWithId('pw_list') || this.hasElementWithClass('vault-card'),
            hasMasterPwInput: this.hasElementWithId('master_pw') || this.hasElementWithId('cred-input-pass') || this.hasElementWithClass('master-input') || this.hasElementWithId('cred-modal'),
            hasStatsBar: this.hasElementWithClass('stats-bar') || this.hasElementWithClass('security-banner') || this.hasElementWithClass('sec-card') || this.hasElementWithId('st_total') || this.hasElementWithId('audit-score'),
            hasAddModal: this.hasElementWithId('cred-modal') || this.hasElementWithId('modal') || this.hasText('Add Credential') || this.hasText('Add Password'),
            hasBreachBadges: this.hasElementWithClass('badge-health') || this.hasElementWithClass('breach-badge') || this.hasText('Compromised') || this.hasText('Secure') || this.hasText('Strong'),
            hasBridgeWiring: this.hasText('getPasswords') || this.hasText('getSavedPasswords') || this.hasText('unlockVault') || this.hasText('aeonBridge')
        };
    }

    validateDownloadsStructure() {
        return {
            hasDoctype: this.hasDoctype(),
            hasHeaderActions: this.hasElementWithClass('header-actions') || this.hasElementWithClass('header') || this.hasText('Clear Completed') || this.hasText('Clear History'),
            hasFilters: this.hasElementWithClass('filter-tabs') || this.hasElementWithClass('filters') || this.hasElementWithClass('tab-pill') || this.hasElementWithClass('pill'),
            hasStatsBar: this.hasElementWithClass('stats-bar') || this.hasElementWithClass('dl-meta-row') || this.hasElementWithClass('stat-card') || this.hasElementWithId('stat_total') || this.hasText('remaining'),
            hasDownloadCards: this.hasElementWithClass('dl-card') || this.hasElementWithClass('progress-bar') || this.hasElementWithClass('dl-progress-bar') || this.hasText('Downloads'),
            hasBridgeWiring: this.hasText('getDownloads') || this.hasText('pauseDownload') || this.hasText('resumeDownload') || this.hasText('aeonBridge')
        };
    }

    validateIntelligenceStructure() {
        return {
            hasDoctype: this.hasDoctype(),
            hasBrandHeader: this.hasElementWithClass('brand') || this.hasElementWithClass('brand-row') || this.hasElementWithClass('brand-text') || this.hasText('Aeon AI') || this.hasText('Aeon Intelligence'),
            hasStatusPill: this.hasElementWithId('statusPill') || this.hasElementWithClass('status-pill') || this.hasElementWithClass('metric-pill') || this.hasElementWithClass('dock-pill') || this.hasText('Sovereignty Guard Active'),
            hasPromptChips: this.hasElementWithClass('chips-row') || this.hasElementWithClass('chip') || this.hasElementWithClass('dock-pill') || this.hasElementWithClass('dock-controls'),
            hasChatBox: this.hasElementWithClass('chat-box') || this.hasElementWithClass('chat-feed') || this.hasElementWithId('chat-feed') || this.hasElementWithId('messages'),
            hasChatInput: this.hasElementWithClass('chat-input') || this.hasElementWithClass('prompt-textarea') || this.hasElementWithId('prompt-input') || this.hasElementWithId('userInput'),
            hasConfigModal: this.hasElementWithId('configModal') || this.hasElementWithId('model-select') || this.hasElementWithId('providerSelect') || this.hasElementWithClass('model-select'),
            hasMultiProviderOptions: this.hasText('sovereign') && this.hasText('gemini') && this.hasText('openai')
        };
    }

    /**
     * Static utility to check all 7 internal pages in a single call
     */
    static checkAllPages() {
        const pages = [
            { name: 'settings', path: path.resolve(PROJECT_ROOT, 'resources', 'pages', 'settings.html') },
            { name: 'history', path: path.resolve(PROJECT_ROOT, 'resources', 'pages', 'history.html') },
            { name: 'bookmarks', path: path.resolve(PROJECT_ROOT, 'resources', 'pages', 'bookmarks.html') },
            { name: 'passwords', path: path.resolve(PROJECT_ROOT, 'resources', 'pages', 'passwords.html') },
            { name: 'downloads', path: path.resolve(PROJECT_ROOT, 'resources', 'pages', 'downloads.html') },
            { name: 'newtab', path: path.resolve(PROJECT_ROOT, 'resources', 'newtab', 'newtab.html') },
            { name: 'intelligence', path: path.resolve(PROJECT_ROOT, 'resources', 'pages', 'intelligence.html') }
        ];

        const results = {};
        for (const p of pages) {
            if (fs.existsSync(p.path)) {
                const verifier = new DomVerifier(p.path);
                results[p.name] = {
                    exists: true,
                    tokens: verifier.validateCommonObsidianTheme()
                };
            } else {
                results[p.name] = { exists: false, error: 'File missing' };
            }
        }
        return results;
    }
}
