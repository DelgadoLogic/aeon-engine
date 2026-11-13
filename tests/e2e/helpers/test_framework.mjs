/**
 * AeonBrowser E2E Test Framework
 * Lightweight, robust, async test runner with rich assertions and reporting.
 */

export class Expectation {
    constructor(actual, isNot = false) {
        this.actual = actual;
        this.isNot = isNot;
    }

    get not() {
        return new Expectation(this.actual, !this.isNot);
    }

    _assert(condition, message) {
        const passed = this.isNot ? !condition : condition;
        if (!passed) {
            throw new Error(message);
        }
    }

    toBe(expected) {
        const cond = this.actual === expected;
        this._assert(
            cond,
            `Expected ${JSON.stringify(this.actual)} ${this.isNot ? 'NOT to be' : 'to be'} ${JSON.stringify(expected)}`
        );
    }

    toEqual(expected) {
        const cond = JSON.stringify(this.actual) === JSON.stringify(expected);
        this._assert(
            cond,
            `Expected deep equality: ${JSON.stringify(this.actual)} ${this.isNot ? 'NOT to equal' : 'to equal'} ${JSON.stringify(expected)}`
        );
    }

    toBeTruthy() {
        const cond = Boolean(this.actual);
        this._assert(
            cond,
            `Expected ${JSON.stringify(this.actual)} ${this.isNot ? 'NOT to be truthy' : 'to be truthy'}`
        );
    }

    toBeFalsy() {
        const cond = !Boolean(this.actual);
        this._assert(
            cond,
            `Expected ${JSON.stringify(this.actual)} ${this.isNot ? 'NOT to be falsy' : 'to be falsy'}`
        );
    }

    toBeNull() {
        const cond = this.actual === null;
        this._assert(
            cond,
            `Expected ${JSON.stringify(this.actual)} ${this.isNot ? 'NOT to be null' : 'to be null'}`
        );
    }

    toBeDefined() {
        const cond = typeof this.actual !== 'undefined';
        this._assert(
            cond,
            `Expected value ${this.isNot ? 'to be undefined' : 'to be defined'}`
        );
    }

    toBeGreaterThan(expected) {
        const cond = this.actual > expected;
        this._assert(
            cond,
            `Expected ${this.actual} ${this.isNot ? 'NOT to be >' : 'to be >'} ${expected}`
        );
    }

    toBeGreaterThanOrEqual(expected) {
        const cond = this.actual >= expected;
        this._assert(
            cond,
            `Expected ${this.actual} ${this.isNot ? 'NOT to be >=' : 'to be >='} ${expected}`
        );
    }

    toBeLessThan(expected) {
        const cond = this.actual < expected;
        this._assert(
            cond,
            `Expected ${this.actual} ${this.isNot ? 'NOT to be <' : 'to be <'} ${expected}`
        );
    }

    toBeLessThanOrEqual(expected) {
        const cond = this.actual <= expected;
        this._assert(
            cond,
            `Expected ${this.actual} ${this.isNot ? 'NOT to be <=' : 'to be <='} ${expected}`
        );
    }

    toBeCloseTo(expected, delta = 0.001) {
        const diff = Math.abs(this.actual - expected);
        const cond = diff <= delta;
        this._assert(
            cond,
            `Expected ${this.actual} to be within ${delta} of ${expected} (diff: ${diff})`
        );
    }

    toContain(item) {
        let cond = false;
        if (typeof this.actual === 'string' || Array.isArray(this.actual)) {
            cond = this.actual.includes(item);
        } else if (this.actual && typeof this.actual === 'object') {
            cond = item in this.actual;
        }
        this._assert(
            cond,
            `Expected ${JSON.stringify(this.actual)} ${this.isNot ? 'NOT to contain' : 'to contain'} ${JSON.stringify(item)}`
        );
    }

    toMatch(regex) {
        const r = typeof regex === 'string' ? new RegExp(regex) : regex;
        const cond = r.test(String(this.actual));
        this._assert(
            cond,
            `Expected "${this.actual}" ${this.isNot ? 'NOT to match' : 'to match'} regex ${regex}`
        );
    }

    toThrow(expectedMessageOrRegex) {
        if (typeof this.actual !== 'function') {
            throw new Error('Target must be a function to test toThrow()');
        }
        let threw = false;
        let thrownError = null;
        try {
            this.actual();
        } catch (err) {
            threw = true;
            thrownError = err;
        }

        if (this.isNot) {
            if (threw) {
                throw new Error(`Expected function NOT to throw, but it threw: ${thrownError?.message}`);
            }
            return;
        }

        if (!threw) {
            throw new Error('Expected function to throw an error, but it did not throw.');
        }

        if (expectedMessageOrRegex) {
            const msg = thrownError?.message || String(thrownError);
            if (typeof expectedMessageOrRegex === 'string') {
                if (!msg.includes(expectedMessageOrRegex)) {
                    throw new Error(`Expected thrown error to contain "${expectedMessageOrRegex}", got "${msg}"`);
                }
            } else if (expectedMessageOrRegex instanceof RegExp) {
                if (!expectedMessageOrRegex.test(msg)) {
                    throw new Error(`Expected thrown error to match ${expectedMessageOrRegex}, got "${msg}"`);
                }
            }
        }
    }
}

export function expect(actual) {
    return new Expectation(actual);
}

class TestSuite {
    constructor(name) {
        this.name = name;
        this.tests = [];
        this.beforeAllFns = [];
        this.afterAllFns = [];
        this.beforeEachFns = [];
        this.afterEachFns = [];
    }

    addTest(name, fn, options = {}) {
        this.tests.push({ name, fn, timeout: options.timeout || 30000 });
    }

    beforeAll(fn) { this.beforeAllFns.push(fn); }
    afterAll(fn) { this.afterAllFns.push(fn); }
    beforeEach(fn) { this.beforeEachFns.push(fn); }
    afterEach(fn) { this.afterEachFns.push(fn); }
}

class TestRunner {
    constructor() {
        this.suites = [];
        this.currentSuite = null;
        this.globalBeforeAllFns = [];
        this.globalAfterAllFns = [];
        this.globalBeforeEachFns = [];
        this.globalAfterEachFns = [];
    }

    describe(name, fn) {
        const suite = new TestSuite(name);
        this.suites.push(suite);
        const prev = this.currentSuite;
        this.currentSuite = suite;
        fn();
        this.currentSuite = prev;
    }

    test(name, fn, options) {
        if (!this.currentSuite) {
            this.describe('Default Suite', () => {
                this.currentSuite.addTest(name, fn, options);
            });
        } else {
            this.currentSuite.addTest(name, fn, options);
        }
    }

    beforeAll(fn) {
        if (this.currentSuite) {
            this.currentSuite.beforeAll(fn);
        } else {
            this.globalBeforeAllFns.push(fn);
        }
    }

    afterAll(fn) {
        if (this.currentSuite) {
            this.currentSuite.afterAll(fn);
        } else {
            this.globalAfterAllFns.push(fn);
        }
    }

    beforeEach(fn) {
        if (this.currentSuite) {
            this.currentSuite.beforeEach(fn);
        } else {
            this.globalBeforeEachFns.push(fn);
        }
    }

    afterEach(fn) {
        if (this.currentSuite) {
            this.currentSuite.afterEach(fn);
        } else {
            this.globalAfterEachFns.push(fn);
        }
    }

    async run(options = {}) {
        const filter = options.filter ? new RegExp(options.filter, 'i') : null;
        const verbose = options.verbose ?? true;
        let total = 0, passed = 0, failed = 0, skipped = 0;
        const suiteResults = [];

        console.log('\n\x1b[1m\x1b[35m══════════════════════════════════════════════════════════════════════\x1b[0m');
        console.log(`\x1b[1m\x1b[36m  AeonBrowser Test Runner — Executing Test Suites\x1b[0m`);
        console.log('\x1b[1m\x1b[35m══════════════════════════════════════════════════════════════════════\x1b[0m\n');

        const startTime = Date.now();

        // Run global beforeAll
        for (const fn of this.globalBeforeAllFns) {
            try {
                await fn();
            } catch (err) {
                console.error(`  \x1b[31m✖ globalBeforeAll failed: ${err.message}\x1b[0m`);
            }
        }

        for (const suite of this.suites) {
            console.log(`\x1b[1m\x1b[34m▶ Suite: ${suite.name}\x1b[0m`);
            const suiteResult = {
                name: suite.name,
                tests: [],
                passed: 0,
                failed: 0,
                durationMs: 0
            };
            const suiteStart = Date.now();

            try {
                for (const fn of suite.beforeAllFns) await fn();
            } catch (err) {
                console.error(`  \x1b[31m✖ beforeAll failed: ${err.message}\x1b[0m`);
            }

            for (const t of suite.tests) {
                if (filter && !filter.test(t.name) && !filter.test(suite.name)) {
                    skipped++;
                    continue;
                }

                total++;
                const testStart = Date.now();
                let error = null;
                let testPassed = false;

                try {
                    for (const fn of this.globalBeforeEachFns) await fn();
                    for (const fn of suite.beforeEachFns) await fn();

                    // Timeout wrapper
                    await Promise.race([
                        t.fn(),
                        new Promise((_, reject) =>
                            setTimeout(() => reject(new Error(`Test timed out after ${t.timeout}ms`)), t.timeout)
                        )
                    ]);

                    for (const fn of suite.afterEachFns) await fn();
                    for (const fn of this.globalAfterEachFns) await fn();
                    testPassed = true;
                    passed++;
                    suiteResult.passed++;
                } catch (err) {
                    error = err;
                    failed++;
                    suiteResult.failed++;
                }

                const duration = Date.now() - testStart;
                suiteResult.tests.push({
                    name: t.name,
                    passed: testPassed,
                    durationMs: duration,
                    error: error ? error.message : null,
                    stack: error ? error.stack : null
                });

                if (testPassed) {
                    if (verbose) {
                        console.log(`  \x1b[32m✔\x1b[0m ${t.name} \x1b[2m(${duration}ms)\x1b[0m`);
                    }
                } else {
                    console.log(`  \x1b[31m✖\x1b[0m ${t.name} \x1b[2m(${duration}ms)\x1b[0m`);
                    console.log(`    \x1b[31mError: ${error.message}\x1b[0m`);
                    if (error.stack && options.showStack) {
                        console.log(`    \x1b[2m${error.stack.split('\n').slice(1, 3).join('\n    ')}\x1b[0m`);
                    }
                }
            }

            try {
                for (const fn of suite.afterAllFns) await fn();
            } catch (err) {
                console.error(`  \x1b[31m✖ afterAll failed: ${err.message}\x1b[0m`);
            }

            suiteResult.durationMs = Date.now() - suiteStart;
            suiteResults.push(suiteResult);
            console.log();
        }

        // Run global afterAll
        for (const fn of this.globalAfterAllFns) {
            try {
                await fn();
            } catch (err) {
                console.error(`  \x1b[31m✖ globalAfterAll failed: ${err.message}\x1b[0m`);
            }
        }

        const totalDuration = Date.now() - startTime;

        console.log('\x1b[1m\x1b[35m══════════════════════════════════════════════════════════════════════\x1b[0m');
        console.log(`\x1b[1m  Summary: Total: ${total} | \x1b[32mPassed: ${passed}\x1b[0m\x1b[1m | \x1b[31mFailed: ${failed}\x1b[0m\x1b[1m | Skipped: ${skipped} | Time: ${(totalDuration / 1000).toFixed(2)}s\x1b[0m`);
        console.log('\x1b[1m\x1b[35m══════════════════════════════════════════════════════════════════════\x1b[0m\n');

        return {
            total,
            passed,
            failed,
            skipped,
            durationMs: totalDuration,
            suites: suiteResults,
            success: failed === 0
        };
    }
}

export const runner = new TestRunner();
export const describe = (name, fn) => runner.describe(name, fn);
export const test = (name, fn, options) => runner.test(name, fn, options);
export const it = test;
export const beforeAll = (fn) => runner.beforeAll(fn);
export const afterAll = (fn) => runner.afterAll(fn);
export const beforeEach = (fn) => runner.beforeEach(fn);
export const afterEach = (fn) => runner.afterEach(fn);
