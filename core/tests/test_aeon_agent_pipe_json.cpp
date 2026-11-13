#include <windows.h>
#include <iostream>
#include <string>
#include <cstdio>
#include <cstring>
#include <cassert>

// Replicate exact functions from core/agent/AeonAgentPipe.cpp for empirical testing
static std::string JsonGetString(const char* json, const char* key) {
    if (!json || !key) return "";
    char needle[128];
    _snprintf_s(needle, sizeof(needle), _TRUNCATE, "\"%s\"", key);
    size_t needleLen = strlen(needle);
    const char* pos = strstr(json, needle);
    while (pos) {
        const char* p = pos + needleLen;
        while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (*p == ':') {
            p++; // skip colon
            while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            if (*p != '"') return "";
            p++; // skip opening quote
            std::string val;
            for (; *p && *p != '"'; p++) {
                if (*p == '\\' && *(p + 1)) {
                    p++;
                    switch (*p) {
                        case 'n':  val += '\n'; break;
                        case 'r':  val += '\r'; break;
                        case 't':  val += '\t'; break;
                        case '"':  val += '"';  break;
                        case '\\': val += '\\'; break;
                        default:   val += *p;   break;
                    }
                } else {
                    val += *p;
                }
            }
            return val;
        }
        pos = strstr(pos + 1, needle);
    }
    return "";
}

static int JsonGetInt(const char* json, const char* key) {
    if (!json || !key) return -1;
    char needle[128];
    _snprintf_s(needle, sizeof(needle), _TRUNCATE, "\"%s\"", key);
    size_t needleLen = strlen(needle);
    const char* pos = strstr(json, needle);
    while (pos) {
        const char* p = pos + needleLen;
        while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (*p == ':') {
            p++; // skip colon
            while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            if (*p == '"') p++; // skip leading double quote if string-quoted integer
            while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            if (*p == '\0' || (*p != '-' && (*p < '0' || *p > '9'))) return -1;
            return atoi(p);
        }
        pos = strstr(pos + 1, needle);
    }
    return -1;
}

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    int failed = 0;
    int total = 0;

    auto TEST_STR = [&](const char* testName, const char* json, const char* key, const std::string& expected) {
        total++;
        std::string actual = JsonGetString(json, key);
        if (actual == expected) {
            std::cout << "[PASS] " << testName << std::endl;
        } else {
            std::cout << "[FAIL] " << testName << "\n  Expected: \"" << expected << "\"\n  Got:      \"" << actual << "\"" << std::endl;
            failed++;
        }
    };

    auto TEST_INT = [&](const char* testName, const char* json, const char* key, int expected) {
        total++;
        int actual = JsonGetInt(json, key);
        if (actual == expected) {
            std::cout << "[PASS] " << testName << std::endl;
        } else {
            std::cout << "[FAIL] " << testName << "\n  Expected: " << expected << "\n  Got:      " << actual << std::endl;
            failed++;
        }
    };

    std::cout << "=== Running JsonGetString Unescaping Tests ===" << std::endl;
    TEST_STR("Unescape newline \\n", "{\"prompt\":\"line1\\nline2\"}", "prompt", "line1\nline2");
    TEST_STR("Unescape tab \\t", "{\"prompt\":\"tab1\\ttab2\"}", "prompt", "tab1\ttab2");
    TEST_STR("Unescape carriage return \\r", "{\"prompt\":\"line1\\rline2\"}", "prompt", "line1\rline2");
    TEST_STR("Unescape quotes \\\"", "{\"prompt\":\"hello \\\"world\\\"\"}", "prompt", "hello \"world\"");
    TEST_STR("Unescape backslash \\\\", "{\"path\":\"C:\\\\Users\\\\test\"}", "path", "C:\\Users\\test");
    TEST_STR("Combined control chars", "{\"prompt\":\"\\\"Line1\\\"\\n\\t\\r\\\\End\"}", "prompt", "\"Line1\"\n\t\r\\End");
    TEST_STR("Standard string without escapes", "{\"cmd\":\"ai.summarize\"}", "cmd", "ai.summarize");
    TEST_STR("String with spaces around colon", "{\"cmd\" : \"ai.navigate\"}", "cmd", "ai.navigate");
    TEST_STR("Missing key returns empty", "{\"cmd\":\"ai.summarize\"}", "missingKey", "");
    TEST_STR("Key substring inside value before actual key", "{\"text\":\"mentioning \\\"cmd\\\" in text\",\"cmd\":\"tab.list\"}", "cmd", "tab.list");

    std::cout << "\n=== Running JsonGetInt Parsing Tests ===" << std::endl;
    TEST_INT("String-quoted integer \"42\"", "{\"tabId\":\"42\"}", "tabId", 42);
    TEST_INT("Unquoted integer 42", "{\"tabId\": 42}", "tabId", 42);
    TEST_INT("String-quoted negative integer \"-1\"", "{\"tabId\":\"-1\"}", "tabId", -1);
    TEST_INT("Unquoted negative integer -1", "{\"tabId\": -1}", "tabId", -1);
    TEST_INT("String-quoted zero \"0\"", "{\"tabId\":\"0\"}", "tabId", 0);
    TEST_INT("Unquoted zero 0", "{\"tabId\":0}", "tabId", 0);
    TEST_INT("String-quoted integer with whitespace", "{\"tabId\" : \"100\"}", "tabId", 100);
    TEST_INT("Unquoted integer with whitespace", "{\"tabId\" : 200}", "tabId", 200);
    TEST_INT("Non-numeric string returns -1", "{\"tabId\":\"abc\"}", "tabId", -1);
    TEST_INT("Missing key returns -1", "{\"other\":42}", "tabId", -1);
    TEST_INT("Key substring inside string value before actual int key", "{\"title\":\"tabId 999\",\"tabId\":\"42\"}", "tabId", 42);

    std::cout << "\n=== Summary: " << (total - failed) << "/" << total << " tests passed. ===" << std::endl;
    return failed == 0 ? 0 : 1;
}
