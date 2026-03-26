// AeonWebMCP.h — WebMCP integration for AeonAgent
// Adopts the WebMCP spec (Model Context Protocol browser integration) but
// RESTRICTS connections to local MCP servers only.
// Remote MCP endpoints are blocked by AeonShield at the NetworkDelegate layer.
//
// Use case: AeonAgent exposes local tools (summarize, translate, spell-check)
// as an MCP server on 127.0.0.1. Web pages can invoke them via the WebMCP API
// without any data leaving the device.

#pragma once
#include <string>
#include <functional>
#include "chrome/browser/aeon/AeonFeatures.h"

namespace aeon {

struct MCPToolResult {
    bool success;
    std::string payload;   // JSON response
    std::string error;
};

class AeonWebMCP {
public:
    AeonWebMCP();
    ~AeonWebMCP();

    // ── Runtime gate: only localhost endpoints allowed ────────────────────────
    static bool IsAllowedEndpoint(const std::string& url);

    // ── Tool dispatch: routes to AeonAgent local tools ────────────────────────
    // Supported built-in tools:
    //   aeon.translate(text, target_lang)  → AeonTranslate (CTranslate2)
    //   aeon.summarize(text)               → AeonAgent (local LLM)
    //   aeon.spellcheck(text, lang)        → AeonSpell (Hunspell)
    //   aeon.voice.start()                 → AeonVoice (Whisper STT)
    //   aeon.voice.stop() → transcript
    MCPToolResult Dispatch(const std::string& tool_name,
                           const std::string& args_json);

    // ── AeonAgent handshake ───────────────────────────────────────────────────
    // Called at browser startup; connects to the local AeonAgent MCP server.
    bool ConnectLocalAgent(int port = 11434);
    bool IsAgentConnected() const { return agent_connected_; }

private:
    bool agent_connected_ = false;
    int  agent_port_      = 11434;

    MCPToolResult CallLocalTool(const std::string& endpoint,
                                const std::string& body);
};

} // namespace aeon
