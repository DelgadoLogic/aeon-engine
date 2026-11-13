#!/usr/bin/env node
/**
 * AeonBrowser Master E2E Test Runner
 * Executes all 6 tiers of E2E tests and outputs structured JSON and Markdown summary reports.
 * 
 * Usage:
 *   node tests/e2e/run_all_e2e.mjs
 *   node tests/e2e/run_all_e2e.mjs --tier=1
 *   node tests/e2e/run_all_e2e.mjs --tier=2
 *   node tests/e2e/run_all_e2e.mjs --tier=3
 *   node tests/e2e/run_all_e2e.mjs --tier=4
 *   node tests/e2e/run_all_e2e.mjs --tier=5
 *   node tests/e2e/run_all_e2e.mjs --tier=6
 *   node tests/e2e/run_all_e2e.mjs --filter=Obsidian
 */

import { runner } from './helpers/test_framework.mjs';
import { registerTier1Tests } from './tier1_feature_coverage.mjs';
import { registerTier2Tests } from './tier2_boundary_cases.mjs';
import { registerTier3Tests } from './tier3_cross_feature.mjs';
import { registerTier4Tests } from './tier4_real_world_workloads.mjs';
import { registerTier5Tests } from './tier5_obsidian_pages.mjs';
import { registerTier6Tests } from './tier6_bridge_contracts.mjs';
import fs from 'fs';
import path from 'path';

// Parse CLI Arguments
const args = process.argv.slice(2);
let selectedTier = null;
let filterPattern = null;

for (const arg of args) {
    if (arg.startsWith('--tier=')) {
        selectedTier = parseInt(arg.split('=')[1], 10);
    } else if (arg.startsWith('--filter=')) {
        filterPattern = arg.split('=')[1];
    }
}

// Register requested suites
if (!selectedTier || selectedTier === 1) {
    registerTier1Tests();
}
if (!selectedTier || selectedTier === 2) {
    registerTier2Tests();
}
if (!selectedTier || selectedTier === 3) {
    registerTier3Tests();
}
if (!selectedTier || selectedTier === 4) {
    registerTier4Tests();
}
if (!selectedTier || selectedTier === 5) {
    registerTier5Tests();
}
if (!selectedTier || selectedTier === 6) {
    registerTier6Tests();
}

async function main() {
    const results = await runner.run({ filter: filterPattern });

    // Ensure reports directory exists
    const reportsDir = path.resolve(process.cwd(), 'tests', 'e2e', 'reports');
    if (!fs.existsSync(reportsDir)) {
        fs.mkdirSync(reportsDir, { recursive: true });
    }

    // 1. Write JSON result report
    const jsonReportPath = path.join(reportsDir, 'test_results.json');
    fs.writeFileSync(jsonReportPath, JSON.stringify(results, null, 2), 'utf8');

    // 2. Generate Markdown Summary Report
    const mdReportPath = path.join(reportsDir, 'test_summary.md');
    let md = `# AeonBrowser E2E Test Execution Summary\n\n`;
    md += `**Execution Time**: ${new Date().toISOString()}\n`;
    md += `**Status**: ${results.success ? '✅ ALL TESTS PASSED' : '❌ SOME TESTS FAILED'}\n`;
    md += `**Total Tests**: ${results.total}\n`;
    md += `**Passed**: ${results.passed}\n`;
    md += `**Failed**: ${results.failed}\n`;
    md += `**Skipped**: ${results.skipped}\n`;
    md += `**Duration**: ${(results.durationMs / 1000).toFixed(2)}s\n\n`;

    md += `## Suite Breakdown\n\n`;
    md += `| Suite | Tests | Passed | Failed | Duration |\n`;
    md += `|-------|-------|--------|--------|----------|\n`;
    for (const s of results.suites) {
        md += `| ${s.name} | ${s.tests.length} | ${s.passed} | ${s.failed} | ${(s.durationMs / 1000).toFixed(2)}s |\n`;
    }
    md += `\n`;

    if (results.failed > 0) {
        md += `## Failed Tests Details\n\n`;
        for (const s of results.suites) {
            for (const t of s.tests) {
                if (!t.passed) {
                    md += `### ❌ ${s.name} → ${t.name}\n`;
                    md += `**Error**: \`${t.error}\`\n\n`;
                }
            }
        }
    }

    fs.writeFileSync(mdReportPath, md, 'utf8');
    console.log(`\x1b[32m✔ JSON Report saved to:\x1b[0m ${jsonReportPath}`);
    console.log(`\x1b[32m✔ Markdown Summary saved to:\x1b[0m ${mdReportPath}\n`);

    if (!results.success) {
        process.exit(1);
    } else {
        process.exit(0);
    }
}

main().catch((err) => {
    console.error('Fatal Test Runner Error:', err);
    process.exit(1);
});
