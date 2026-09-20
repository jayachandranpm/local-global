#pragma once
#include <string>
#include <json/json.h>

/// Represents a parsed tool-call request extracted from the LLM response.
struct ToolCall {
    std::string tool;    // "search" or "mcp"
    std::string query;   // For search tool — the search query
    std::string server;  // For mcp tool — the target MCP server name
    std::string method;  // For mcp tool — the JSON-RPC method (e.g. "tools/call")
    Json::Value params;  // For mcp tool — arbitrary parameters
};
