// AeonBrowser — OmniSuggestHUD.cpp
// DelgadoLogic | Omnibox Auto-Suggest Engine (AeonOmni)
//
// Implementation of floating suggestion popup with double-buffered GDI+ rendering,
// DWM glass rounded corners, multi-provider search predictions via WinHTTP,
// local SQLite history/bookmark fuzzy matching, and inline recursive-descent math engine.

#include "OmniSuggestHUD.h"
#include "DwmHelper.h"
#include "../history/HistoryEngine.h"
#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <winhttp.h>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <map>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winhttp.lib")

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace AeonOmni {

// ---------------------------------------------------------------------------
// Design Tokens & Visual Metrics
// ---------------------------------------------------------------------------
static const COLORREF CLR_HUD_BG          = RGB(11, 6, 22);     // #0b0616 Obsidian
static const COLORREF CLR_HUD_BORDER      = RGB(147, 51, 234);  // #9333ea Neon Purple
static const COLORREF CLR_HUD_ROW_HOVER   = RGB(24, 26, 50);    // #181a32 Active Row
static const COLORREF CLR_HUD_CYAN        = RGB(56, 189, 248);  // #38bdf8 Math / URL
static const COLORREF CLR_HUD_PURPLE      = RGB(192, 132, 252); // #c084fc Search
static const COLORREF CLR_HUD_GOLD        = RGB(251, 191, 36);  // #fbbf24 Bookmark
static const COLORREF CLR_HUD_GREEN       = RGB(34, 197, 94);   // #22c55e Direct URL
static const COLORREF CLR_HUD_TEXT_MAIN   = RGB(248, 250, 252); // #f8fafc Primary Text
static const COLORREF CLR_HUD_TEXT_MUTED  = RGB(148, 163, 184); // #94a3b8 Secondary Text
static const COLORREF CLR_HUD_TEXT_FAINT  = RGB(76, 29, 149);   // #4c1d95 Divider

constexpr int HUD_CORNER_RADIUS = 10;
constexpr int HUD_PAD_X         = 10;
constexpr int HUD_PAD_TOP       = 6;
constexpr int HUD_PAD_BOTTOM    = 6;
constexpr int HUD_ROW_H         = 46;
constexpr int HUD_ICON_BOX_W    = 32;
constexpr int HUD_BADGE_W       = 84;
constexpr int HUD_MIN_W         = 560;
constexpr int HUD_MAX_ITEMS     = 8;

// Custom Window Message for background search completion
constexpr UINT WM_AEON_SUGGEST_READY = WM_USER + 0x7E1;

// ---------------------------------------------------------------------------
// String Utility Helpers
// ---------------------------------------------------------------------------
static std::wstring Utf8ToUtf16(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int wLen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wLen <= 0) return L"";
    std::vector<wchar_t> wBuf(wLen);
    if (MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wBuf.data(), wLen) > 0) {
        return std::wstring(wBuf.data());
    }
    return L"";
}

static std::string Utf16ToUtf8(const std::wstring& utf16) {
    if (utf16.empty()) return "";
    int u8Len = WideCharToMultiByte(CP_UTF8, 0, utf16.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (u8Len <= 0) return "";
    std::vector<char> u8Buf(u8Len);
    if (WideCharToMultiByte(CP_UTF8, 0, utf16.c_str(), -1, u8Buf.data(), u8Len, nullptr, nullptr) > 0) {
        return std::string(u8Buf.data());
    }
    return "";
}

static std::string ToLowerStr(const std::string& str) {
    std::string s = str;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

static std::string TrimStr(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

static std::string UrlEncodeQuery(const std::string& input) {
    std::string encoded;
    for (unsigned char c : input) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += (char)c;
        } else if (c == ' ') {
            encoded += '+';
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            encoded += buf;
        }
    }
    return encoded;
}

// ---------------------------------------------------------------------------
// GDI+ Round Rectangle Helpers
// ---------------------------------------------------------------------------
static void FillGdiplusRoundRect(Gdiplus::Graphics& g, const RECT& r, int radius, Gdiplus::Brush& brush) {
    Gdiplus::GraphicsPath path;
    float rx = (float)radius;
    float x = (float)r.left;
    float y = (float)r.top;
    float w = (float)(r.right - r.left);
    float h = (float)(r.bottom - r.top);
    if (w <= 0 || h <= 0) return;

    if (rx * 2.0f > w) rx = w / 2.0f;
    if (rx * 2.0f > h) rx = h / 2.0f;

    path.AddArc(x, y, rx * 2.0f, rx * 2.0f, 180.0f, 90.0f);
    path.AddArc(x + w - rx * 2.0f, y, rx * 2.0f, rx * 2.0f, 270.0f, 90.0f);
    path.AddArc(x + w - rx * 2.0f, y + h - rx * 2.0f, rx * 2.0f, rx * 2.0f, 0.0f, 90.0f);
    path.AddArc(x, y + h - rx * 2.0f, rx * 2.0f, rx * 2.0f, 90.0f, 90.0f);
    path.CloseFigure();
    g.FillPath(&brush, &path);
}

static void DrawGdiplusRoundRect(Gdiplus::Graphics& g, const RECT& r, int radius, Gdiplus::Pen& pen) {
    Gdiplus::GraphicsPath path;
    float rx = (float)radius;
    float x = (float)r.left;
    float y = (float)r.top;
    float w = (float)(r.right - r.left);
    float h = (float)(r.bottom - r.top);
    if (w <= 0 || h <= 0) return;

    if (rx * 2.0f > w) rx = w / 2.0f;
    if (rx * 2.0f > h) rx = h / 2.0f;

    path.AddArc(x, y, rx * 2.0f, rx * 2.0f, 180.0f, 90.0f);
    path.AddArc(x + w - rx * 2.0f, y, rx * 2.0f, rx * 2.0f, 270.0f, 90.0f);
    path.AddArc(x + w - rx * 2.0f, y + h - rx * 2.0f, rx * 2.0f, rx * 2.0f, 0.0f, 90.0f);
    path.AddArc(x, y + h - rx * 2.0f, rx * 2.0f, rx * 2.0f, 90.0f, 90.0f);
    path.CloseFigure();
    g.DrawPath(&pen, &path);
}

// ---------------------------------------------------------------------------
// Inline Recursive-Descent Math Evaluator & Unit Converter
// ---------------------------------------------------------------------------
class MathEvaluator {
public:
    struct EvalResult {
        bool        success;
        double      value;
        std::string formatted;
        std::string expressionEcho;
    };

    static bool Evaluate(const std::string& rawInput, EvalResult& out) {
        out.success = false;
        out.value = 0.0;
        out.formatted = "";
        out.expressionEcho = "";

        std::string s = TrimStr(rawInput);
        if (s.empty()) return false;

        // Skip obvious false positives (URLs, IP addresses, plain text)
        if (IsFalsePositive(s)) return false;

        // 1. Try Unit Conversion: "<value> <unit1> to/in/= <unit2>"
        if (TryUnitConversion(s, out)) {
            return true;
        }

        // 2. Try Percentage phrases: "X% of Y", "X as % of Y"
        if (TryPercentagePhrase(s, out)) {
            return true;
        }

        // 3. Recursive-Descent Expression Parser
        try {
            MathParser parser(s);
            double val = parser.ParseExpression();
            if (parser.HasUnparsedTokens()) return false;
            if (std::isnan(val) || std::isinf(val)) return false;

            out.success = true;
            out.value = val;
            out.formatted = FormatNumber(val);
            out.expressionEcho = s + " = " + out.formatted;
            return true;
        } catch (...) {
            return false;
        }
    }

    static std::string FormatNumber(double val) {
        if (std::isnan(val)) return "NaN";
        if (std::isinf(val)) return (val > 0) ? "Infinity" : "-Infinity";

        // Check if value is cleanly an integer
        if (std::abs(val - std::round(val)) < 1e-9 && std::abs(val) < 1e15) {
            long long intVal = (long long)std::round(val);
            std::string s = std::to_string(std::abs(intVal));
            int n = (int)s.length() - 3;
            while (n > 0) {
                s.insert(n, ",");
                n -= 3;
            }
            if (intVal < 0) s = "-" + s;
            return s;
        }

        // Fixed-precision float formatting
        std::ostringstream oss;
        if (std::abs(val) >= 1e15 || (std::abs(val) > 0 && std::abs(val) < 1e-5)) {
            oss << std::setprecision(8) << val;
            return oss.str();
        }

        oss << std::fixed << std::setprecision(6) << val;
        std::string str = oss.str();
        str.erase(str.find_last_not_of('0') + 1, std::string::npos);
        if (!str.empty() && str.back() == '.') str.pop_back();

        size_t dotPos = str.find('.');
        std::string intPart = (dotPos != std::string::npos) ? str.substr(0, dotPos) : str;
        std::string fracPart = (dotPos != std::string::npos) ? str.substr(dotPos) : "";

        bool neg = (intPart.size() > 0 && intPart[0] == '-');
        if (neg) intPart = intPart.substr(1);

        int n = (int)intPart.length() - 3;
        while (n > 0) {
            intPart.insert(n, ",");
            n -= 3;
        }
        if (neg) intPart = "-" + intPart;
        return intPart + fracPart;
    }

private:
    static bool IsFalsePositive(const std::string& str) {
        if (str.find("://") != std::string::npos) return true;
        if (str.rfind("localhost", 0) == 0 || str.rfind("127.0.0.1", 0) == 0) return true;
        if (str.rfind("@ai ", 0) == 0 || str.rfind("@ask ", 0) == 0) return true;

        // Skip IPv4 address pattern (e.g. 192.168.1.1)
        int dots = 0;
        bool allDigitsOrDots = true;
        for (char c : str) {
            if (c == '.') dots++;
            else if (!isdigit((unsigned char)c) && c != '/') allDigitsOrDots = false;
        }
        if (allDigitsOrDots && dots == 3) return true;

        // Must contain at least one digit or math symbol/keyword
        bool hasMathSignal = false;
        for (char c : str) {
            if (isdigit((unsigned char)c) || c == '+' || c == '-' || c == '*' || c == '/' ||
                c == '^' || c == '%' || c == '(' || c == ')' || c == '$' || c == (char)0xC2 || c == (char)0xC3) {
                hasMathSignal = true;
                break;
            }
        }
        if (!hasMathSignal) {
            std::string lower = ToLowerStr(str);
            if (lower == "e" || lower == "pi" || lower == "phi" || lower == "tau" ||
                lower == "\xcf\x80" || lower == "\xcf\x86" || lower == "\xcf\x84" ||
                lower.find("phi") != std::string::npos || lower.find("tau") != std::string::npos ||
                lower.find("pi") != std::string::npos || lower.find("sqrt") != std::string::npos ||
                lower.find("cbrt") != std::string::npos || lower.find("sin") != std::string::npos ||
                lower.find("cos") != std::string::npos || lower.find("tan") != std::string::npos ||
                lower.find("asin") != std::string::npos || lower.find("acos") != std::string::npos ||
                lower.find("atan") != std::string::npos || lower.find("log") != std::string::npos ||
                lower.find("ln") != std::string::npos || lower.find("exp") != std::string::npos ||
                lower.find("abs") != std::string::npos || lower.find("ceil") != std::string::npos ||
                lower.find("floor") != std::string::npos || lower.find("round") != std::string::npos ||
                lower.find("deg") != std::string::npos || lower.find("rad") != std::string::npos) {
                hasMathSignal = true;
            }
        }
        if (!hasMathSignal) return true;

        // Skip bare single numbers without any operator (e.g. "2026", "404")
        bool isBareNumber = true;
        for (char c : str) {
            if (!isdigit((unsigned char)c) && c != '.' && c != ' ' && c != '\t') {
                isBareNumber = false;
                break;
            }
        }
        if (isBareNumber) return true;

        return false;
    }

    // -----------------------------------------------------------------------
    // Unit Conversion Mapping
    // -----------------------------------------------------------------------
    enum class UnitDim { Length, Mass, Temp, Digital, Time, Speed, Currency };

    struct UnitInfo {
        UnitDim dim;
        double  toBase; // multiplier to base unit
    };

    static const std::map<std::string, UnitInfo>& GetUnitTable() {
        static std::map<std::string, UnitInfo> table = {
            // Length (Base = Meter)
            { "m", { UnitDim::Length, 1.0 } },
            { "meter", { UnitDim::Length, 1.0 } },
            { "meters", { UnitDim::Length, 1.0 } },
            { "km", { UnitDim::Length, 1000.0 } },
            { "kilometer", { UnitDim::Length, 1000.0 } },
            { "kilometers", { UnitDim::Length, 1000.0 } },
            { "cm", { UnitDim::Length, 0.01 } },
            { "centimeter", { UnitDim::Length, 0.01 } },
            { "centimeters", { UnitDim::Length, 0.01 } },
            { "mm", { UnitDim::Length, 0.001 } },
            { "millimeter", { UnitDim::Length, 0.001 } },
            { "millimeters", { UnitDim::Length, 0.001 } },
            { "in", { UnitDim::Length, 0.0254 } },
            { "inch", { UnitDim::Length, 0.0254 } },
            { "inches", { UnitDim::Length, 0.0254 } },
            { "ft", { UnitDim::Length, 0.3048 } },
            { "feet", { UnitDim::Length, 0.3048 } },
            { "foot", { UnitDim::Length, 0.3048 } },
            { "yd", { UnitDim::Length, 0.9144 } },
            { "yard", { UnitDim::Length, 0.9144 } },
            { "yards", { UnitDim::Length, 0.9144 } },
            { "mi", { UnitDim::Length, 1609.344 } },
            { "mile", { UnitDim::Length, 1609.344 } },
            { "miles", { UnitDim::Length, 1609.344 } },
            { "nmi", { UnitDim::Length, 1852.0 } },

            // Mass (Base = Gram)
            { "g", { UnitDim::Mass, 1.0 } },
            { "gram", { UnitDim::Mass, 1.0 } },
            { "grams", { UnitDim::Mass, 1.0 } },
            { "kg", { UnitDim::Mass, 1000.0 } },
            { "kilogram", { UnitDim::Mass, 1000.0 } },
            { "kilograms", { UnitDim::Mass, 1000.0 } },
            { "mg", { UnitDim::Mass, 0.001 } },
            { "milligram", { UnitDim::Mass, 0.001 } },
            { "milligrams", { UnitDim::Mass, 0.001 } },
            { "lb", { UnitDim::Mass, 453.59237 } },
            { "lbs", { UnitDim::Mass, 453.59237 } },
            { "pound", { UnitDim::Mass, 453.59237 } },
            { "pounds", { UnitDim::Mass, 453.59237 } },
            { "oz", { UnitDim::Mass, 28.34952 } },
            { "ounce", { UnitDim::Mass, 28.34952 } },
            { "ounces", { UnitDim::Mass, 28.34952 } },
            { "ton", { UnitDim::Mass, 1000000.0 } },
            { "tonne", { UnitDim::Mass, 1000000.0 } },
            { "tons", { UnitDim::Mass, 1000000.0 } },
            { "stone", { UnitDim::Mass, 6350.293 } },
            { "st", { UnitDim::Mass, 6350.293 } },

            // Temperature (Special affine converters)
            { "c", { UnitDim::Temp, 1.0 } },
            { "celsius", { UnitDim::Temp, 1.0 } },
            { "f", { UnitDim::Temp, 1.0 } },
            { "fahrenheit", { UnitDim::Temp, 1.0 } },
            { "k", { UnitDim::Temp, 1.0 } },
            { "kelvin", { UnitDim::Temp, 1.0 } },

            // Digital Data (Base = Byte)
            { "b", { UnitDim::Digital, 0.125 } },
            { "bit", { UnitDim::Digital, 0.125 } },
            { "bits", { UnitDim::Digital, 0.125 } },
            { "byte", { UnitDim::Digital, 1.0 } },
            { "bytes", { UnitDim::Digital, 1.0 } },
            { "kb", { UnitDim::Digital, 1000.0 } },
            { "mb", { UnitDim::Digital, 1000000.0 } },
            { "gb", { UnitDim::Digital, 1000000000.0 } },
            { "tb", { UnitDim::Digital, 1000000000000.0 } },
            { "kib", { UnitDim::Digital, 1024.0 } },
            { "mib", { UnitDim::Digital, 1048576.0 } },
            { "gib", { UnitDim::Digital, 1073741824.0 } },
            { "tib", { UnitDim::Digital, 1099511627776.0 } },

            // Time (Base = Second)
            { "ms", { UnitDim::Time, 0.001 } },
            { "millisecond", { UnitDim::Time, 0.001 } },
            { "milliseconds", { UnitDim::Time, 0.001 } },
            { "s", { UnitDim::Time, 1.0 } },
            { "sec", { UnitDim::Time, 1.0 } },
            { "second", { UnitDim::Time, 1.0 } },
            { "seconds", { UnitDim::Time, 1.0 } },
            { "min", { UnitDim::Time, 60.0 } },
            { "minute", { UnitDim::Time, 60.0 } },
            { "minutes", { UnitDim::Time, 60.0 } },
            { "hr", { UnitDim::Time, 3600.0 } },
            { "hour", { UnitDim::Time, 3600.0 } },
            { "hours", { UnitDim::Time, 3600.0 } },
            { "day", { UnitDim::Time, 86400.0 } },
            { "days", { UnitDim::Time, 86400.0 } },
            { "wk", { UnitDim::Time, 604800.0 } },
            { "week", { UnitDim::Time, 604800.0 } },
            { "weeks", { UnitDim::Time, 604800.0 } },
            { "mo", { UnitDim::Time, 2629800.0 } },
            { "month", { UnitDim::Time, 2629800.0 } },
            { "months", { UnitDim::Time, 2629800.0 } },
            { "yr", { UnitDim::Time, 31557600.0 } },
            { "year", { UnitDim::Time, 31557600.0 } },
            { "years", { UnitDim::Time, 31557600.0 } },

            // Speed (Base = m/s)
            { "m/s", { UnitDim::Speed, 1.0 } },
            { "km/h", { UnitDim::Speed, 0.277778 } },
            { "kph", { UnitDim::Speed, 0.277778 } },
            { "mph", { UnitDim::Speed, 0.44704 } },
            { "knot", { UnitDim::Speed, 0.514444 } },
            { "knots", { UnitDim::Speed, 0.514444 } },
            { "mach", { UnitDim::Speed, 343.0 } },

            // Currency (Base = USD)
            { "usd", { UnitDim::Currency, 1.0 } },
            { "$", { UnitDim::Currency, 1.0 } },
            { "eur", { UnitDim::Currency, 1.087 } },
            { "€", { UnitDim::Currency, 1.087 } },
            { "gbp", { UnitDim::Currency, 1.282 } },
            { "£", { UnitDim::Currency, 1.282 } },
            { "jpy", { UnitDim::Currency, 0.00656 } },
            { "¥", { UnitDim::Currency, 0.00656 } },
            { "cad", { UnitDim::Currency, 0.730 } },
            { "aud", { UnitDim::Currency, 0.658 } },
            { "chf", { UnitDim::Currency, 1.136 } },
            { "cny", { UnitDim::Currency, 0.138 } },
            { "inr", { UnitDim::Currency, 0.012 } },
            { "brl", { UnitDim::Currency, 0.183 } },
            { "btc", { UnitDim::Currency, 65000.0 } },
            { "eth", { UnitDim::Currency, 3500.0 } }
        };
        return table;
    }

    static bool TryUnitConversion(const std::string& input, EvalResult& out) {
        std::string s = ToLowerStr(input);
        size_t toPos = std::string::npos;
        std::string delim = "";

        if ((toPos = s.find(" to ")) != std::string::npos) {
            delim = " to ";
        } else if ((toPos = s.find(" in ")) != std::string::npos) {
            delim = " in ";
        } else if ((toPos = s.find(" = ")) != std::string::npos) {
            delim = " = ";
        }

        if (toPos == std::string::npos) return false;

        std::string leftPart = TrimStr(s.substr(0, toPos));
        std::string rightPart = TrimStr(s.substr(toPos + delim.length()));

        if (leftPart.empty() || rightPart.empty()) return false;

        const auto& table = GetUnitTable();
        auto itTo = table.find(rightPart);
        if (itTo == table.end()) return false;

        size_t unitStart = leftPart.find_last_not_of("abcdefghijklmnopqrstuvwxyz$/€£¥");
        if (unitStart == std::string::npos) return false;

        std::string valStr = TrimStr(leftPart.substr(0, unitStart + 1));
        std::string unitFromStr = TrimStr(leftPart.substr(unitStart + 1));

        if (unitFromStr.empty() && leftPart[0] == '$') {
            unitFromStr = "$";
            valStr = TrimStr(leftPart.substr(1));
        }

        auto itFrom = table.find(unitFromStr);
        if (itFrom == table.end()) return false;

        if (itFrom->second.dim != itTo->second.dim) return false;

        double inVal = 0.0;
        try {
            MathParser p(valStr);
            inVal = p.ParseExpression();
        } catch (...) {
            return false;
        }

        double converted = 0.0;
        if (itFrom->second.dim == UnitDim::Temp) {
            double cVal = inVal;
            if (unitFromStr == "f" || unitFromStr == "fahrenheit") cVal = (inVal - 32.0) / 1.8;
            else if (unitFromStr == "k" || unitFromStr == "kelvin") cVal = inVal - 273.15;

            if (rightPart == "f" || rightPart == "fahrenheit") converted = (cVal * 1.8) + 32.0;
            else if (rightPart == "k" || rightPart == "kelvin") converted = cVal + 273.15;
            else converted = cVal;
        } else {
            double baseVal = inVal * itFrom->second.toBase;
            converted = baseVal / itTo->second.toBase;
        }

        out.success = true;
        out.value = converted;
        out.formatted = FormatNumber(converted) + " " + rightPart;
        out.expressionEcho = input + " = " + out.formatted;
        return true;
    }

    static bool TryPercentagePhrase(const std::string& input, EvalResult& out) {
        std::string s = ToLowerStr(input);

        size_t pos = s.find(" as % of ");
        if (pos != std::string::npos) {
            std::string partStr = TrimStr(s.substr(0, pos));
            std::string totalStr = TrimStr(s.substr(pos + 9));
            try {
                MathParser p1(partStr);
                double part = p1.ParseExpression();
                if (p1.HasUnparsedTokens()) return false;
                MathParser p2(totalStr);
                double total = p2.ParseExpression();
                if (p2.HasUnparsedTokens()) return false;
                if (std::abs(total) < 1e-12) return false;
                double res = (part / total) * 100.0;

                out.success = true;
                out.value = res;
                out.formatted = FormatNumber(res) + "%";
                out.expressionEcho = input + " = " + out.formatted;
                return true;
            } catch (...) {
                return false;
            }
        }

        pos = s.find("% of ");
        if (pos != std::string::npos) {
            std::string pctStr = TrimStr(s.substr(0, pos));
            std::string totalStr = TrimStr(s.substr(pos + 5));
            try {
                MathParser p1(pctStr);
                double pct = p1.ParseExpression();
                if (p1.HasUnparsedTokens()) return false;
                MathParser p2(totalStr);
                double total = p2.ParseExpression();
                if (p2.HasUnparsedTokens()) return false;
                double res = (pct / 100.0) * total;

                out.success = true;
                out.value = res;
                out.formatted = FormatNumber(res);
                out.expressionEcho = input + " = " + out.formatted;
                return true;
            } catch (...) {
                return false;
            }
        }

        return false;
    }

    // -----------------------------------------------------------------------
    // Internal Recursive-Descent Expression Parser
    // -----------------------------------------------------------------------
    class MathParser {
    public:
        explicit MathParser(const std::string& str) : m_src(str), m_pos(0) {
            Tokenize();
            m_tokIdx = 0;
        }

        double ParseExpression() {
            if (m_tokIdx == 0 && m_tokIdx < (int)m_tokens.size() && Peek().type == Plus) {
                Get(); // allow leading unary '+'
            }
            double val = ParseAdditive();
            if (m_tokIdx < (int)m_tokens.size() && Peek().type == Mod && !CanStartPrimary(PeekNext())) {
                Get(); // consume standalone postfix '%'
                val = val / 100.0;
            }
            return val;
        }

        bool HasUnparsedTokens() const {
            return m_tokIdx < (int)m_tokens.size();
        }

    private:
        enum TokenType { End, Number, Ident, Plus, Minus, Mul, Div, Mod, Pow, LParen, RParen, Comma };
        struct Token {
            TokenType   type;
            double      val;
            std::string text;
        };

        std::string        m_src;
        size_t             m_pos;
        std::vector<Token> m_tokens;
        int                m_tokIdx;

        void Tokenize() {
            while (m_pos < m_src.size()) {
                char c = m_src[m_pos];
                if (isspace((unsigned char)c)) {
                    m_pos++;
                    continue;
                }
                if (isdigit((unsigned char)c) || c == '.') {
                    size_t start = m_pos;
                    while (m_pos < m_src.size() && (isdigit((unsigned char)m_src[m_pos]) || m_src[m_pos] == '.')) {
                        m_pos++;
                    }
                    double val = std::stod(m_src.substr(start, m_pos - start));
                    m_tokens.push_back({ Number, val, "" });
                } else if (isalpha((unsigned char)c) || (unsigned char)c > 127) {
                    size_t start = m_pos;
                    while (m_pos < m_src.size() && (isalnum((unsigned char)m_src[m_pos]) || (unsigned char)m_src[m_pos] > 127)) {
                        m_pos++;
                    }
                    std::string id = ToLowerStr(m_src.substr(start, m_pos - start));
                    m_tokens.push_back({ Ident, 0.0, id });
                } else if (c == '+') {
                    m_tokens.push_back({ Plus, 0.0, "+" }); m_pos++;
                } else if (c == '-') {
                    m_tokens.push_back({ Minus, 0.0, "-" }); m_pos++;
                } else if (c == '*') {
                    if (m_pos + 1 < m_src.size() && m_src[m_pos + 1] == '*') {
                        m_tokens.push_back({ Pow, 0.0, "**" }); m_pos += 2;
                    } else {
                        m_tokens.push_back({ Mul, 0.0, "*" }); m_pos++;
                    }
                } else if (c == '/') {
                    m_tokens.push_back({ Div, 0.0, "/" }); m_pos++;
                } else if (c == '%') {
                    m_tokens.push_back({ Mod, 0.0, "%" }); m_pos++;
                } else if (c == '^') {
                    m_tokens.push_back({ Pow, 0.0, "^" }); m_pos++;
                } else if (c == '(') {
                    m_tokens.push_back({ LParen, 0.0, "(" }); m_pos++;
                } else if (c == ')') {
                    m_tokens.push_back({ RParen, 0.0, ")" }); m_pos++;
                } else if (c == ',') {
                    m_tokens.push_back({ Comma, 0.0, "," }); m_pos++;
                } else {
                    m_pos++; // skip unrecognized symbol
                }
            }
        }

        const Token& Peek() const {
            static Token s_eof = { End, 0.0, "" };
            if (m_tokIdx < (int)m_tokens.size()) return m_tokens[m_tokIdx];
            return s_eof;
        }

        const Token& PeekNext() const {
            static Token s_eof = { End, 0.0, "" };
            if (m_tokIdx + 1 < (int)m_tokens.size()) return m_tokens[m_tokIdx + 1];
            return s_eof;
        }

        Token Get() {
            if (m_tokIdx < (int)m_tokens.size()) return m_tokens[m_tokIdx++];
            return { End, 0.0, "" };
        }

        static bool CanStartPrimary(const Token& tok) {
            return (tok.type == Number || tok.type == Ident || tok.type == LParen || tok.type == Minus || tok.type == Plus);
        }

        double ParseAdditive() {
            double left = ParseMultiplicative();
            while (m_tokIdx < (int)m_tokens.size()) {
                Token t = Peek();
                if (t.type == Plus) {
                    Get();
                    double right = ParseMultiplicative();
                    if (m_tokIdx < (int)m_tokens.size() && Peek().type == Mod && !CanStartPrimary(PeekNext())) {
                        Get();
                        left = left + (left * (right / 100.0));
                    } else {
                        left += right;
                    }
                } else if (t.type == Minus) {
                    Get();
                    double right = ParseMultiplicative();
                    if (m_tokIdx < (int)m_tokens.size() && Peek().type == Mod && !CanStartPrimary(PeekNext())) {
                        Get();
                        left = left - (left * (right / 100.0));
                    } else {
                        left -= right;
                    }
                } else {
                    break;
                }
            }
            return left;
        }

        double ParseMultiplicative() {
            double left = ParsePower();
            while (m_tokIdx < (int)m_tokens.size()) {
                Token t = Peek();
                if (t.type == Mul) {
                    Get();
                    double right = ParsePower();
                    left *= right;
                } else if (t.type == Div) {
                    Get();
                    double right = ParsePower();
                    if (std::abs(right) < 1e-12) throw std::runtime_error("Divide by zero");
                    left /= right;
                } else if (t.type == Mod && CanStartPrimary(PeekNext())) {
                    Get();
                    double right = ParsePower();
                    if (std::abs(right) < 1e-12) throw std::runtime_error("Modulo by zero");
                    left = std::fmod(left, right);
                } else {
                    break;
                }
            }
            return left;
        }

        double ParsePower() {
            double left = ParseFactor();
            if (m_tokIdx < (int)m_tokens.size() && Peek().type == Pow) {
                Get();
                double right = ParsePower();
                return std::pow(left, right);
            }
            return left;
        }

        double ParseFactor() {
            Token t = Peek();
            if (t.type == Minus) {
                Get();
                return -ParseFactor();
            }
            return ParsePrimary();
        }

        double ParsePrimary() {
            Token t = Get();
            if (t.type == Number) {
                if (m_tokIdx < (int)m_tokens.size() && Peek().type == Mod && !CanStartPrimary(PeekNext())) {
                    TokenType nextType = PeekNext().type;
                    if (nextType == Mul || nextType == Div || nextType == Pow) {
                        Get();
                        return t.val / 100.0;
                    }
                }
                return t.val;
            }
            if (t.type == LParen) {
                double val = ParseExpression();
                if (Get().type != RParen) throw std::runtime_error("Missing closing parenthesis");
                return val;
            }
            if (t.type == Ident) {
                if (t.text == "pi" || t.text == "π" || t.text == "\xcf\x80") return 3.14159265358979323846;
                if (t.text == "e")  return 2.71828182845904523536;
                if (t.text == "phi" || t.text == "φ" || t.text == "\xcf\x86") return 1.61803398874989484820;
                if (t.text == "tau" || t.text == "τ" || t.text == "\xcf\x84") return 6.28318530717958647692;

                if (Peek().type == LParen) {
                    Get(); // skip '('
                    double arg1 = ParseExpression();
                    double val = 0.0;
                    if (t.text == "sqrt") {
                        if (arg1 < 0) throw std::runtime_error("Square root of negative number");
                        val = std::sqrt(arg1);
                    }
                    else if (t.text == "cbrt") val = std::cbrt(arg1);
                    else if (t.text == "sin") val = std::sin(arg1);
                    else if (t.text == "cos") val = std::cos(arg1);
                    else if (t.text == "tan") val = std::tan(arg1);
                    else if (t.text == "asin") {
                        if (arg1 < -1.0 || arg1 > 1.0) throw std::runtime_error("asin out of domain");
                        val = std::asin(arg1);
                    }
                    else if (t.text == "acos") {
                        if (arg1 < -1.0 || arg1 > 1.0) throw std::runtime_error("acos out of domain");
                        val = std::acos(arg1);
                    }
                    else if (t.text == "atan") val = std::atan(arg1);
                    else if (t.text == "atan2") {
                        if (Get().type != Comma) throw std::runtime_error("Expected comma in atan2");
                        double arg2 = ParseExpression();
                        val = std::atan2(arg1, arg2);
                    }
                    else if (t.text == "log" || t.text == "ln") {
                        if (arg1 <= 0) throw std::runtime_error("Log of non-positive number");
                        val = std::log(arg1);
                    }
                    else if (t.text == "log10") {
                        if (arg1 <= 0) throw std::runtime_error("Log10 of non-positive number");
                        val = std::log10(arg1);
                    }
                    else if (t.text == "log2") {
                        if (arg1 <= 0) throw std::runtime_error("Log2 of non-positive number");
                        val = std::log2(arg1);
                    }
                    else if (t.text == "exp") val = std::exp(arg1);
                    else if (t.text == "abs") val = std::abs(arg1);
                    else if (t.text == "ceil") val = std::ceil(arg1);
                    else if (t.text == "floor") val = std::floor(arg1);
                    else if (t.text == "round") val = std::round(arg1);
                    else if (t.text == "deg") val = arg1 * (180.0 / 3.14159265358979323846);
                    else if (t.text == "rad") val = arg1 * (3.14159265358979323846 / 180.0);
                    else throw std::runtime_error("Unknown math function: " + t.text);

                    if (Get().type != RParen) throw std::runtime_error("Missing closing paren for function");
                    return val;
                }
            }

            throw std::runtime_error("Invalid syntax token");
        }
    };
};

// ---------------------------------------------------------------------------
// OpenSearch & JSON Streaming Parser (Zero External Dependencies)
// ---------------------------------------------------------------------------
class OpenSearchParser {
public:
    static bool Parse(const char* json, size_t len, std::vector<std::string>& outSuggestions, int maxCount = 5) {
        outSuggestions.clear();
        if (!json || len == 0) return false;

        const char* p = json;
        const char* end = json + len;

        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end || *p != '[') return false;
        p++;

        if (!SkipValue(p, end)) return false;

        while (p < end && *p != ',' && *p != ']') p++;
        if (p >= end || *p != ',') return false;
        p++;

        while (p < end && *p != '[' && *p != ']') p++;
        if (p >= end || *p != '[') return false;
        p++;

        while (p < end && *p != ']' && (int)outSuggestions.size() < maxCount) {
            while (p < end && (isspace((unsigned char)*p) || *p == ',')) p++;
            if (p >= end || *p == ']') break;

            if (*p == '"') {
                std::string s;
                if (ParseString(p, end, s)) {
                    if (!s.empty()) outSuggestions.push_back(s);
                } else {
                    break;
                }
            } else if (*p == '{') {
                std::string s;
                if (ParseObjectPhrase(p, end, s)) {
                    if (!s.empty()) outSuggestions.push_back(s);
                } else {
                    SkipValue(p, end);
                }
            } else {
                SkipValue(p, end);
            }
        }
        return !outSuggestions.empty();
    }

private:
    static bool SkipValue(const char*& p, const char* end) {
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end) return false;

        if (*p == '"') {
            p++;
            while (p < end) {
                if (*p == '\\') { p += 2; continue; }
                if (*p == '"') { p++; return true; }
                p++;
            }
            return false;
        } else if (*p == '[' || *p == '{') {
            char open = *p;
            char close = (open == '[') ? ']' : '}';
            int depth = 1;
            p++;
            while (p < end && depth > 0) {
                if (*p == '"') {
                    p++;
                    while (p < end) {
                        if (*p == '\\') { p += 2; continue; }
                        if (*p == '"') { p++; break; }
                        p++;
                    }
                    continue;
                }
                if (*p == open) depth++;
                else if (*p == close) depth--;
                p++;
            }
            return depth == 0;
        } else {
            while (p < end && *p != ',' && *p != ']' && *p != '}' && !isspace((unsigned char)*p)) p++;
            return true;
        }
    }

    static bool ParseString(const char*& p, const char* end, std::string& out) {
        if (*p != '"') return false;
        p++;
        out.clear();
        while (p < end) {
            if (*p == '"') { p++; return true; }
            if (*p == '\\') {
                p++;
                if (p >= end) return false;
                switch (*p) {
                    case '"':  out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/'; break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        if (p + 4 < end) {
                            char hex[5] = { p[1], p[2], p[3], p[4], 0 };
                            unsigned int cp = (unsigned int)strtoul(hex, nullptr, 16);
                            p += 4;
                            if (cp <= 0x7F) {
                                out += (char)cp;
                            } else if (cp <= 0x7FF) {
                                out += (char)(0xC0 | ((cp >> 6) & 0x1F));
                                out += (char)(0x80 | (cp & 0x3F));
                            } else {
                                out += (char)(0xE0 | ((cp >> 12) & 0x0F));
                                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                                out += (char)(0x80 | (cp & 0x3F));
                            }
                        }
                        break;
                    }
                    default: out += *p; break;
                }
                p++;
            } else {
                out += *p++;
            }
        }
        return false;
    }

    static bool ParseObjectPhrase(const char*& p, const char* end, std::string& out) {
        if (*p != '{') return false;
        p++;
        while (p < end && *p != '}') {
            while (p < end && *p != '"' && *p != '}') p++;
            if (p >= end || *p == '}') break;
            std::string key;
            if (!ParseString(p, end, key)) return false;
            while (p < end && *p != ':') p++;
            if (p >= end) return false;
            p++;
            while (p < end && isspace((unsigned char)*p)) p++;
            if (key == "phrase") {
                if (ParseString(p, end, out)) {
                    while (p < end && *p != '}') p++;
                    if (p < end && *p == '}') p++;
                    return true;
                }
            } else {
                SkipValue(p, end);
            }
        }
        if (p < end && *p == '}') p++;
        return false;
    }
};

// ---------------------------------------------------------------------------
// Asynchronous Search Prediction Service (WinHTTP)
// ---------------------------------------------------------------------------
static std::atomic<uint64_t> g_suggestSeq { 0 };

class SearchPredictor {
public:
    static SearchPredictor& Instance() {
        static SearchPredictor s_inst;
        return s_inst;
    }

    SearchPredictor() : m_running(true) {
        m_hSession = WinHttpOpen(
            L"AeonBrowser-Suggest/1.0",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0
        );
        if (m_hSession) {
            DWORD timeout = 1000;
            WinHttpSetOption(m_hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
            WinHttpSetOption(m_hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
            DWORD disableCookies = WINHTTP_DISABLE_COOKIES;
            WinHttpSetOption(m_hSession, WINHTTP_OPTION_ENABLE_FEATURE, &disableCookies, sizeof(disableCookies));

            // Warm up TLS / Schannel / Crypt32 subsystem
            HINTERNET hConnect = WinHttpConnect(m_hSession, L"duckduckgo.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
            if (hConnect) {
                HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"HEAD", L"/", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
                if (hRequest) {
                    WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
                    WinHttpCloseHandle(hRequest);
                }
                WinHttpCloseHandle(hConnect);
            }
        }
        m_worker = std::thread(&SearchPredictor::WorkerLoop, this);
    }

    ~SearchPredictor() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_running = false;
        }
        m_cv.notify_all();
        if (m_worker.joinable()) {
            m_worker.join();
        }
        if (m_hSession) {
            WinHttpCloseHandle(m_hSession);
            m_hSession = nullptr;
        }
    }

    void Cancel() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_hasPending = false;
    }

    void FetchSuggestions(uint64_t seq, const std::string& query, const std::string& engine, HWND notifyHwnd) {
        if (query.empty() || HistoryEngine::IsPrivate() || !notifyHwnd || !IsWindow(notifyHwnd) || !IsWindowVisible(notifyHwnd)) return;

        std::string encoded = UrlEncodeQuery(query);
        std::wstring host;
        std::wstring path;

        std::string eng = ToLowerStr(engine);
        if (eng == "google") {
            host = L"suggestqueries.google.com";
            path = L"/complete/search?client=chrome&q=" + Utf8ToUtf16(encoded);
        } else if (eng == "brave") {
            host = L"search.brave.com";
            path = L"/api/suggest?q=" + Utf8ToUtf16(encoded);
        } else if (eng == "bing") {
            host = L"api.bing.com";
            path = L"/osjson.aspx?query=" + Utf8ToUtf16(encoded);
        } else if (eng == "searxng") {
            host = L"searx.be";
            path = L"/autocompleter?q=" + Utf8ToUtf16(encoded);
        } else {
            host = L"duckduckgo.com";
            path = L"/ac/?q=" + Utf8ToUtf16(encoded) + L"&type=list";
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingSeq = seq;
            m_pendingHost = host;
            m_pendingPath = path;
            m_pendingNotifyHwnd = notifyHwnd;
            m_hasPending = true;
            m_lastRequestTime = std::chrono::steady_clock::now();
        }
        m_cv.notify_one();
    }

private:
    struct Request {
        uint64_t     seq;
        std::wstring host;
        std::wstring path;
        HWND         notifyHwnd;
    };

    HINTERNET                               m_hSession = nullptr;
    std::thread                             m_worker;
    std::mutex                              m_mutex;
    std::condition_variable                 m_cv;
    bool                                    m_running;
    bool                                    m_hasPending = false;
    uint64_t                                m_pendingSeq = 0;
    std::wstring                            m_pendingHost;
    std::wstring                            m_pendingPath;
    HWND                                    m_pendingNotifyHwnd = nullptr;
    std::chrono::steady_clock::time_point   m_lastRequestTime;

    void WorkerLoop() {
        while (true) {
            Request req = {};
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this]() { return !m_running || m_hasPending; });
                if (!m_running) break;

                while (m_running && m_hasPending) {
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastRequestTime);
                    if (elapsed < std::chrono::milliseconds(150)) {
                        auto waitDur = std::chrono::milliseconds(150) - elapsed;
                        m_cv.wait_for(lock, waitDur);
                    } else {
                        break;
                    }
                }
                if (!m_running) break;

                if (!m_hasPending) continue;
                req.seq = m_pendingSeq;
                req.host = m_pendingHost;
                req.path = m_pendingPath;
                req.notifyHwnd = m_pendingNotifyHwnd;
                m_hasPending = false;
            }

            if (req.seq != g_suggestSeq.load() || !IsWindow(req.notifyHwnd) || !IsWindowVisible(req.notifyHwnd)) continue;

            ExecuteHttpRequest(req);
        }
    }

    void ExecuteHttpRequest(const Request& req) {
        if (!m_hSession) return;

        HINTERNET hConnect = WinHttpConnect(m_hSession, req.host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) return;

        HINTERNET hRequest = WinHttpOpenRequest(
            hConnect, L"GET", req.path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE
        );

        std::string responseBody;
        if (hRequest) {
            if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hRequest, nullptr)) {
                DWORD bytesRead = 0;
                char buf[2048];
                while (WinHttpReadData(hRequest, buf, sizeof(buf) - 1, &bytesRead) && bytesRead > 0) {
                    buf[bytesRead] = 0;
                    responseBody.append(buf, bytesRead);
                    if (req.seq != g_suggestSeq.load()) break;
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);

        if (req.seq == g_suggestSeq.load() && !responseBody.empty() && IsWindow(req.notifyHwnd)) {
            std::vector<std::string> results;
            if (OpenSearchParser::Parse(responseBody.c_str(), responseBody.size(), results, 5)) {
                auto* pData = new std::pair<uint64_t, std::vector<std::string>>(req.seq, results);
                PostMessageW(req.notifyHwnd, WM_AEON_SUGGEST_READY, (WPARAM)pData, 0);
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Singleton HUD State
// ---------------------------------------------------------------------------
static HWND                     g_hwnd          = nullptr;
static HWND                     g_parentHwnd    = nullptr;
static HINSTANCE                g_hInst         = nullptr;
static NavigationCallback       g_navCallback   = nullptr;
static std::string              g_currentQuery  = "";
static std::string              g_searchEngine  = "duckduckgo";
static RECT                     g_anchorRect    = {};
static std::vector<SuggestionItem> g_items;
static int                      g_selectedIdx   = -1;
static int                      g_hoverIdx      = -1;

static HFONT                    g_fontTitle     = nullptr;
static HFONT                    g_fontSubtitle  = nullptr;
static HFONT                    g_fontBadge     = nullptr;
static HFONT                    g_fontIcon      = nullptr;
static ULONG_PTR                g_gdiplusToken  = 0;
static int                      g_dpi           = 96;

static inline int S(int px) {
    return MulDiv(px, g_dpi > 0 ? g_dpi : 96, 96);
}

static void RebuildHudFonts() {
    if (g_fontTitle)    { DeleteObject(g_fontTitle);    g_fontTitle = nullptr; }
    if (g_fontSubtitle) { DeleteObject(g_fontSubtitle); g_fontSubtitle = nullptr; }
    if (g_fontBadge)    { DeleteObject(g_fontBadge);    g_fontBadge = nullptr; }
    if (g_fontIcon)     { DeleteObject(g_fontIcon);     g_fontIcon = nullptr; }

    g_fontTitle = CreateFontW(-S(13), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_fontSubtitle = CreateFontW(-S(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_fontBadge = CreateFontW(-S(10), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_fontIcon = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
}

static void RepaintHUD(HDC hdc, const RECT& rc);
static void UpdateLayoutAndShow();
static std::vector<SuggestionItem> AssembleSuggestions(const std::string& query);
static LRESULT CALLBACK SuggestHUDWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------------
// Public API Implementation
// ---------------------------------------------------------------------------
bool RegisterClass(HINSTANCE hInst) {
    g_hInst = hInst;
    WNDCLASSEXW wcex = {};
    wcex.cbSize        = sizeof(WNDCLASSEXW);
    wcex.style         = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc   = SuggestHUDWndProc;
    wcex.cbClsExtra    = 0;
    wcex.cbWndExtra    = 0;
    wcex.hInstance     = hInst;
    wcex.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = nullptr;
    wcex.lpszMenuName  = nullptr;
    wcex.lpszClassName = L"AeonOmniSuggestHUD";

    return (::RegisterClassExW(&wcex) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
}

void Initialize(HWND parentHwnd, HINSTANCE hInst, NavigationCallback navCb) {
    g_parentHwnd = parentHwnd;
    g_hInst = hInst;
    g_navCallback = navCb;

    if (!g_gdiplusToken) {
        Gdiplus::GdiplusStartupInput gdiInput;
        Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiInput, nullptr);
    }

    RegisterClass(hInst);
    SearchPredictor::Instance();

    g_dpi = (int)AeonDwm::GetWindowDpiSafe(parentHwnd);
    if (!g_fontTitle) {
        RebuildHudFonts();
    }

    if (!g_hwnd) {
        DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        DWORD style   = WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;

        g_hwnd = CreateWindowExW(
            exStyle, L"AeonOmniSuggestHUD", L"", style,
            0, 0, S(HUD_MIN_W), S(100),
            parentHwnd, nullptr, hInst, nullptr
        );

        if (g_hwnd) {
            AeonDwm::ApplyObsidianTransientPopupDwm(g_hwnd, false);
        }
    }

    // Pre-warm GDI+ font caches, glyphs, and rendering pipeline across all item topologies
    HWND targetHwnd = g_hwnd ? g_hwnd : parentHwnd;
    HDC windowDC = targetHwnd ? GetDC(targetHwnd) : GetDC(nullptr);
    if (windowDC) {
        HDC memDC = CreateCompatibleDC(windowDC);
        if (memDC) {
            HBITMAP bmp = CreateCompatibleBitmap(windowDC, HUD_MIN_W, 400);
            if (bmp) {
                HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
                HFONT oldFont = (HFONT)GetCurrentObject(memDC, OBJ_FONT);

                // 1. Explicitly rasterize all 6 MDL2 font glyphs
                SelectObject(memDC, g_fontIcon);
                const wchar_t* glyphs[] = { L"\uE947", L"\uF602", L"\uE735", L"\uE81C", L"\uE774", L"\uE721" };
                for (const wchar_t* glyph : glyphs) {
                    RECT r = { 0, 0, 50, 50 };
                    DrawTextW(memDC, glyph, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }

                // 2. Pre-render full HUD cards for all query types
                const char* sampleQueries[] = {
                    "delgado", "25 * 480", "https://news.ycombinator.com", "weather in tokyo",
                    "100 km to mi", "sqrt(144)", "@ai summarize quantum computing", "github.com",
                    "aeon://settings", "100 + 15%", "5 kg to lbs", "celsius to fahrenheit"
                };
                for (const char* sampleQ : sampleQueries) {
                    g_items = AssembleSuggestions(sampleQ);
                    RECT rc = { 0, 0, HUD_MIN_W, (int)(HUD_PAD_TOP + (g_items.size() * HUD_ROW_H) + HUD_PAD_BOTTOM) };
                    RepaintHUD(memDC, rc);
                }
                g_items.clear();

                SelectObject(memDC, oldFont);
                SelectObject(memDC, oldBmp);
                DeleteObject(bmp);
            }
            DeleteDC(memDC);
        }
        if (targetHwnd) {
            ReleaseDC(targetHwnd, windowDC);
        } else {
            ReleaseDC(nullptr, windowDC);
        }
    }

    if (g_hwnd) {
        // Pre-create DWM redirection surfaces and warm up DWM composition caches for all heights
        for (int rows = 1; rows <= HUD_MAX_ITEMS; rows++) {
            int h = HUD_PAD_TOP + (rows * HUD_ROW_H) + HUD_PAD_BOTTOM;
            SetWindowPos(g_hwnd, HWND_TOPMOST, 102, 73, 1066, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            RedrawWindow(g_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            ShowWindow(g_hwnd, SW_HIDE);
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
}

void ShowSuggestions(const std::string& query, const RECT& anchorRect) {
    g_currentQuery = query;
    g_anchorRect = anchorRect;
    g_selectedIdx = -1;
    g_hoverIdx = -1;

    std::string s = TrimStr(query);
    if (s.empty()) {
        Hide();
        return;
    }

    g_items = AssembleSuggestions(s);
    if (g_items.empty()) {
        Hide();
        return;
    }

    UpdateLayoutAndShow();

    uint64_t seq = ++g_suggestSeq;
    SearchPredictor::Instance().FetchSuggestions(seq, s, g_searchEngine, g_hwnd);
}

void ShowSuggestions(const std::string& query, const RECT& anchorRect, const std::vector<SuggestionItem>& items) {
    g_currentQuery = query;
    g_anchorRect = anchorRect;
    g_items = items;
    g_selectedIdx = -1;
    g_hoverIdx = -1;

    if (g_items.empty()) {
        Hide();
        return;
    }

    UpdateLayoutAndShow();
}

void Hide() {
    g_selectedIdx = -1;
    g_hoverIdx = -1;
    g_items.clear();
    g_suggestSeq++;
    SearchPredictor::Instance().Cancel();
    if (g_hwnd && IsWindow(g_hwnd)) {
        ValidateRect(g_hwnd, nullptr);
        ShowWindow(g_hwnd, SW_HIDE);
    }
}

bool IsVisible() {
    return (g_hwnd && IsWindow(g_hwnd) && IsWindowVisible(g_hwnd));
}

bool HandleKeyDown(WPARAM wParam) {
    if (!IsVisible() || g_items.empty()) return false;

    int count = (int)g_items.size();

    if (wParam == VK_DOWN) {
        if (g_selectedIdx < count - 1) {
            g_selectedIdx++;
        } else {
            g_selectedIdx = -1;
        }
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return true;
    }

    if (wParam == VK_UP) {
        if (g_selectedIdx > -1) {
            g_selectedIdx--;
        } else {
            g_selectedIdx = count - 1;
        }
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return true;
    }

    if (wParam == VK_ESCAPE) {
        Hide();
        return true;
    }

    if (wParam == VK_RETURN) {
        std::string targetUrl = GetSelectedUrl();
        Hide();
        if (g_navCallback && !targetUrl.empty()) {
            g_navCallback(targetUrl);
        }
        return true;
    }

    if (wParam == VK_TAB) {
        if (g_selectedIdx >= 0 && g_selectedIdx < count) {
            return true;
        }
    }

    return false;
}

std::string GetSelectedUrl() {
    if (g_selectedIdx >= 0 && g_selectedIdx < (int)g_items.size()) {
        return g_items[g_selectedIdx].actionUrl;
    }
    return "";
}

int GetSelectedIndex() {
    return g_selectedIdx;
}

void SetSearchEngine(const std::string& providerName) {
    g_searchEngine = providerName;
}

HWND GetHwnd() {
    return g_hwnd;
}

void Destroy() {
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
    if (g_fontTitle)    { DeleteObject(g_fontTitle); g_fontTitle = nullptr; }
    if (g_fontSubtitle) { DeleteObject(g_fontSubtitle); g_fontSubtitle = nullptr; }
    if (g_fontBadge)    { DeleteObject(g_fontBadge); g_fontBadge = nullptr; }
    if (g_fontIcon)     { DeleteObject(g_fontIcon); g_fontIcon = nullptr; }

    if (g_gdiplusToken) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

bool EvaluateMathExpression(const std::string& input, std::string& outResult, std::string& outFormatted) {
    MathEvaluator::EvalResult res;
    if (MathEvaluator::Evaluate(input, res)) {
        outResult = res.formatted;
        outFormatted = res.expressionEcho;
        return true;
    }
    return false;
}

void OnDpiChanged(int newDpi) {
    if (newDpi <= 0 || newDpi == g_dpi) return;
    g_dpi = newDpi;
    RebuildHudFonts();
    if (IsVisible()) {
        UpdateLayoutAndShow();
    }
}

// ---------------------------------------------------------------------------
// Suggestion Assembly Engine (Math -> Local History/Bookmarks -> Direct/Search)
// ---------------------------------------------------------------------------
static std::string BuildSearchUrl(const std::string& query, const std::string& engine) {
    std::string encoded = UrlEncodeQuery(query);
    std::string eng = ToLowerStr(engine);
    if (eng == "google")  return "https://www.google.com/search?q=" + encoded;
    if (eng == "bing")    return "https://www.bing.com/search?q=" + encoded;
    if (eng == "brave")   return "https://search.brave.com/search?q=" + encoded;
    if (eng == "searxng") return "https://searx.be/search?q=" + encoded;
    if (eng == "ecosia")  return "https://www.ecosia.org/search?q=" + encoded;
    return "https://duckduckgo.com/?q=" + encoded;
}

static std::vector<SuggestionItem> AssembleSuggestions(const std::string& query) {
    std::vector<SuggestionItem> items;
    std::string q = TrimStr(query);
    if (q.empty()) return items;

    // 1. Math / Unit Conversion Evaluation
    MathEvaluator::EvalResult mathRes;
    if (MathEvaluator::Evaluate(q, mathRes)) {
        SuggestionItem mItem;
        mItem.type = ItemType::MathResult;
        mItem.title = "= " + mathRes.formatted;
        mItem.subtitle = mathRes.expressionEcho;
        mItem.actionUrl = mathRes.formatted;
        mItem.providerTag = "CALC";
        mItem.accentColor = CLR_HUD_CYAN;
        mItem.iconGlyph = L"\uE947";
        items.push_back(mItem);
    }

    // 2. Direct @ai / @ask query trigger
    if (q.rfind("@ai ", 0) == 0 || q.rfind("@ask ", 0) == 0) {
        std::string prompt = (q.rfind("@ai ", 0) == 0) ? q.substr(4) : q.substr(5);
        SuggestionItem aiItem;
        aiItem.type = ItemType::AiPrompt;
        aiItem.title = "Ask Aeon AI: \"" + prompt + "\"";
        aiItem.subtitle = "aeon://intelligence?q=" + UrlEncodeQuery(prompt);
        aiItem.actionUrl = "aeon://intelligence?q=" + UrlEncodeQuery(prompt);
        aiItem.providerTag = "AEON AI";
        aiItem.accentColor = CLR_HUD_PURPLE;
        aiItem.iconGlyph = L"\uF602";
        items.push_back(aiItem);
    }

    // 3. Local Bookmarks Matching
    auto bookmarks = HistoryEngine::GetBookmarks();
    std::string lowerQ = ToLowerStr(q);
    int bmCount = 0;
    for (const auto& bm : bookmarks) {
        std::string bmUrl = bm.url;
        std::string bmTitle = bm.title;
        std::string lUrl = ToLowerStr(bmUrl);
        std::string lTitle = ToLowerStr(bmTitle);

        if (lUrl.find(lowerQ) != std::string::npos || lTitle.find(lowerQ) != std::string::npos) {
            SuggestionItem bItem;
            bItem.type = ItemType::BookmarkMatch;
            bItem.title = bmTitle.empty() ? bmUrl : bmTitle;
            bItem.subtitle = bmUrl;
            bItem.actionUrl = bmUrl;
            bItem.providerTag = "BOOKMARK";
            bItem.accentColor = CLR_HUD_GOLD;
            bItem.iconGlyph = L"\uE735";
            items.push_back(bItem);
            bmCount++;
            if (bmCount >= 2) break;
        }
    }

    // 4. Local History Visits Matching
    auto history = HistoryEngine::Search(q.c_str(), 5);
    int histCount = 0;
    for (const auto& h : history) {
        std::string hUrl = h.url;
        std::string hTitle = h.title;
        bool duplicate = false;
        for (const auto& it : items) {
            if (it.actionUrl == hUrl) { duplicate = true; break; }
        }
        if (!duplicate) {
            SuggestionItem hItem;
            hItem.type = ItemType::HistoryMatch;
            hItem.title = hTitle.empty() ? hUrl : hTitle;
            hItem.subtitle = hUrl;
            hItem.actionUrl = hUrl;
            hItem.providerTag = "HISTORY";
            hItem.accentColor = CLR_HUD_TEXT_MUTED;
            hItem.iconGlyph = L"\uE81C";
            items.push_back(hItem);
            histCount++;
            if (histCount >= 3) break;
        }
    }

    // 5. Direct URL check
    bool hasScheme = (q.find("://") != std::string::npos || q.rfind("aeon://", 0) == 0 || q.rfind("about:", 0) == 0);
    bool hasDot = (q.find('.') != std::string::npos && q.find(' ') == std::string::npos);
    bool isLocal = (q.rfind("localhost", 0) == 0 || q.rfind("127.0.0.1", 0) == 0);

    if (hasScheme || hasDot || isLocal) {
        std::string target = q;
        if (!hasScheme) target = "https://" + q;

        bool duplicate = false;
        for (const auto& it : items) {
            if (it.actionUrl == target) { duplicate = true; break; }
        }

        if (!duplicate) {
            SuggestionItem uItem;
            uItem.type = ItemType::DirectUrl;
            uItem.title = target;
            uItem.subtitle = "Open URL directly";
            uItem.actionUrl = target;
            uItem.providerTag = "OPEN URL";
            uItem.accentColor = CLR_HUD_GREEN;
            uItem.iconGlyph = L"\uE774";
            items.push_back(uItem);
        }
    }

    // 6. Default Search Query Item
    SuggestionItem sItem;
    sItem.type = ItemType::SearchPrediction;
    sItem.title = q;
    std::string providerTagUpper = g_searchEngine;
    std::transform(providerTagUpper.begin(), providerTagUpper.end(), providerTagUpper.begin(), [](unsigned char c){ return (char)toupper(c); });
    sItem.subtitle = "Search " + providerTagUpper;
    sItem.actionUrl = BuildSearchUrl(q, g_searchEngine);
    sItem.providerTag = providerTagUpper;
    sItem.accentColor = CLR_HUD_PURPLE;
    sItem.iconGlyph = L"\uE721";
    items.push_back(sItem);

    if (items.size() > HUD_MAX_ITEMS) {
        items.resize(HUD_MAX_ITEMS);
    }

    return items;
}

// ---------------------------------------------------------------------------
// Dynamic Layout & Screen Positioning
// ---------------------------------------------------------------------------
static void UpdateLayoutAndShow() {
    if (!g_hwnd || g_items.empty()) return;

    int padTop = S(HUD_PAD_TOP);
    int padBot = S(HUD_PAD_BOTTOM);
    int rowH   = S(HUD_ROW_H);

    int nItems = min(HUD_MAX_ITEMS, (int)g_items.size());
    int totalH = padTop + (nItems * rowH) + padBot;

    int anchorW = g_anchorRect.right - g_anchorRect.left;
    int W = max(S(HUD_MIN_W), anchorW);
    int x = g_anchorRect.left + (anchorW - W) / 2;
    int y = g_anchorRect.bottom + S(4);

    POINT pt = { x, y };
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        if (x < mi.rcWork.left + S(8)) x = mi.rcWork.left + S(8);
        if (x + W > mi.rcWork.right - S(8)) x = mi.rcWork.right - W - S(8);
        if (y + totalH > mi.rcWork.bottom - S(8)) {
            y = g_anchorRect.top - totalH - S(4);
        }
    }

    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, W, totalH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Double-Buffered GDI+ Rendering Pipeline
// ---------------------------------------------------------------------------
static void RepaintHUD(HDC hdc, const RECT& rc) {
    if (rc.right <= 0 || rc.bottom <= 0) return;

    int padX     = S(HUD_PAD_X);
    int padTop   = S(HUD_PAD_TOP);
    int rowH     = S(HUD_ROW_H);
    int iconBoxW = S(HUD_ICON_BOX_W);
    int badgeW   = S(HUD_BADGE_W);
    int cornerR  = S(HUD_CORNER_RADIUS);

    HFONT hOrigFont = (HFONT)GetCurrentObject(hdc, OBJ_FONT);

    // 1. GDI+ Background, Borders, and Highlight Badges
    {
        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, GetRValue(CLR_HUD_BG), GetGValue(CLR_HUD_BG), GetBValue(CLR_HUD_BG)));
        FillGdiplusRoundRect(g, rc, cornerR, bgBrush);

        Gdiplus::Pen borderPen(Gdiplus::Color(255, GetRValue(CLR_HUD_BORDER), GetGValue(CLR_HUD_BORDER), GetBValue(CLR_HUD_BORDER)), 1.5f);
        DrawGdiplusRoundRect(g, rc, cornerR, borderPen);

        int rowY = padTop;
        for (int i = 0; i < (int)g_items.size(); i++) {
            const auto& item = g_items[i];
            bool isSelected = (i == g_selectedIdx);
            bool isHovered  = (i == g_hoverIdx);

            RECT rowR = { padX, rowY, rc.right - padX, rowY + rowH };

            if (isSelected || isHovered) {
                Gdiplus::SolidBrush rowBgBrush(Gdiplus::Color(255, GetRValue(CLR_HUD_ROW_HOVER), GetGValue(CLR_HUD_ROW_HOVER), GetBValue(CLR_HUD_ROW_HOVER)));
                FillGdiplusRoundRect(g, rowR, S(6), rowBgBrush);

                Gdiplus::SolidBrush barBrush(Gdiplus::Color(255,
                    GetRValue(item.accentColor),
                    GetGValue(item.accentColor),
                    GetBValue(item.accentColor)));
                RECT barR = { rowR.left + S(2), rowR.top + S(6), rowR.left + S(5), rowR.bottom - S(6) };
                FillGdiplusRoundRect(g, barR, S(2), barBrush);
            }

            if (!item.providerTag.empty()) {
                RECT badgeR = { rowR.right - badgeW, rowR.top + S(12), rowR.right - S(8), rowR.bottom - S(12) };
                Gdiplus::SolidBrush badgeBg(Gdiplus::Color(40, GetRValue(item.accentColor), GetGValue(item.accentColor), GetBValue(item.accentColor)));
                FillGdiplusRoundRect(g, badgeR, S(4), badgeBg);
                Gdiplus::Pen badgePen(Gdiplus::Color(110, GetRValue(item.accentColor), GetGValue(item.accentColor), GetBValue(item.accentColor)), 1.0f);
                DrawGdiplusRoundRect(g, badgeR, S(4), badgePen);
            }

            rowY += rowH;
        }
        g.Flush();
    }

    // 2. GDI Text & Icon Rendering
    SetBkMode(hdc, TRANSPARENT);
    int rowY = padTop;
    for (int i = 0; i < (int)g_items.size(); i++) {
        const auto& item = g_items[i];
        RECT rowR = { padX, rowY, rc.right - padX, rowY + rowH };

        RECT iconR = { rowR.left + S(8), rowR.top, rowR.left + S(8) + iconBoxW, rowR.bottom };
        SelectObject(hdc, g_fontIcon);
        SetTextColor(hdc, item.accentColor);
        DrawTextW(hdc, item.iconGlyph, -1, &iconR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT titleR = { rowR.left + S(12) + iconBoxW, rowR.top + S(5), rowR.right - badgeW - S(8), rowR.top + S(23) };
        SelectObject(hdc, g_fontTitle);
        SetTextColor(hdc, CLR_HUD_TEXT_MAIN);
        std::wstring wTitle = Utf8ToUtf16(item.title);
        DrawTextW(hdc, wTitle.c_str(), -1, &titleR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT subR = { rowR.left + S(12) + iconBoxW, rowR.top + S(23), rowR.right - badgeW - S(8), rowR.bottom - S(5) };
        SelectObject(hdc, g_fontSubtitle);
        SetTextColor(hdc, CLR_HUD_TEXT_MUTED);
        std::wstring wSub = Utf8ToUtf16(item.subtitle);
        DrawTextW(hdc, wSub.c_str(), -1, &subR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (!item.providerTag.empty()) {
            RECT badgeR = { rowR.right - badgeW, rowR.top + S(12), rowR.right - S(8), rowR.bottom - S(12) };
            SelectObject(hdc, g_fontBadge);
            SetTextColor(hdc, item.accentColor);
            std::wstring wTag = Utf8ToUtf16(item.providerTag);
            DrawTextW(hdc, wTag.c_str(), -1, &badgeR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        rowY += rowH;
    }

    SelectObject(hdc, hOrigFont);
}

// ---------------------------------------------------------------------------
// Window Procedure (Non-Activating Popup)
// ---------------------------------------------------------------------------
static LRESULT CALLBACK SuggestHUDWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);
            int w = max(1L, rc.right);
            int h = max(1L, rc.bottom);

            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
            HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);
            HFONT oldFont = (HFONT)GetCurrentObject(memDC, OBJ_FONT);

            RepaintHUD(memDC, rc);

            BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);

            SelectObject(memDC, oldFont);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);
            EndPaint(hWnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            int y = GET_Y_LPARAM(lParam);
            int padTop = S(HUD_PAD_TOP);
            int rowH   = max(1, S(HUD_ROW_H));
            int newHover = -1;
            if (y >= padTop) {
                int idx = (y - padTop) / rowH;
                if (idx >= 0 && idx < (int)g_items.size()) {
                    newHover = idx;
                }
            }
            if (newHover != g_hoverIdx) {
                g_hoverIdx = newHover;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP: {
            if (msg == WM_LBUTTONUP) {
                int y = GET_Y_LPARAM(lParam);
                int padTop = S(HUD_PAD_TOP);
                int rowH   = max(1, S(HUD_ROW_H));
                if (y >= padTop) {
                    int idx = (y - padTop) / rowH;
                    if (idx >= 0 && idx < (int)g_items.size()) {
                        std::string targetUrl = g_items[idx].actionUrl;
                        Hide();
                        if (g_navCallback && !targetUrl.empty()) {
                            g_navCallback(targetUrl);
                        }
                    }
                }
            }
            return 0;
        }

        case WM_AEON_SUGGEST_READY: {
            auto* pData = reinterpret_cast<std::pair<uint64_t, std::vector<std::string>>*>(wParam);
            if (pData) {
                if (pData->first == g_suggestSeq.load() && !pData->second.empty()) {
                    std::string providerTagUpper = g_searchEngine;
                    std::transform(providerTagUpper.begin(), providerTagUpper.end(), providerTagUpper.begin(), [](unsigned char c){ return (char)toupper(c); });

                    for (const auto& pred : pData->second) {
                        if (g_items.size() >= HUD_MAX_ITEMS) break;

                        bool duplicate = false;
                        for (const auto& existing : g_items) {
                            if (existing.title == pred) { duplicate = true; break; }
                        }
                        if (!duplicate) {
                            SuggestionItem rItem;
                            rItem.type = ItemType::SearchPrediction;
                            rItem.title = pred;
                            rItem.subtitle = "Search " + providerTagUpper;
                            rItem.actionUrl = BuildSearchUrl(pred, g_searchEngine);
                            rItem.providerTag = providerTagUpper;
                            rItem.accentColor = CLR_HUD_PURPLE;
                            rItem.iconGlyph = L"\uE721";
                            g_items.push_back(rItem);
                        }
                    }
                    UpdateLayoutAndShow();
                }
                delete pData;
            }
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

} // namespace AeonOmni
