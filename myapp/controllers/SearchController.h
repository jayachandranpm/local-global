#pragma once

#include <drogon/HttpSimpleController.h>

/**
 * SearchController provides REST endpoints:
 *   GET  /api/models          → list locally available Ollama models
 *   GET  /api/search?q=...    → perform a DuckDuckGo web search
 *   GET  /api/mcp/servers     → list registered MCP servers (with details)
 *   POST /api/mcp/servers     → add a new MCP server
 *   POST /api/mcp/servers/remove  → remove an MCP server
 *   POST /api/mcp/servers/restart → restart an MCP server
 *   POST /api/rag/upload      → upload a document for RAG indexing
 *   GET  /api/rag/documents   → list uploaded RAG documents
 *   POST /api/rag/documents/remove → remove a RAG document
 *   POST /api/rag/clear       → clear all RAG documents
 */
class ModelsController : public drogon::HttpSimpleController<ModelsController>
{
public:
    void asyncHandleHttpRequest(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) override;
    PATH_LIST_BEGIN
        PATH_ADD("/api/models", drogon::Get);
    PATH_LIST_END
};

class SearchApiController : public drogon::HttpSimpleController<SearchApiController>
{
public:
    void asyncHandleHttpRequest(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) override;
    PATH_LIST_BEGIN
        PATH_ADD("/api/search", drogon::Get);
    PATH_LIST_END
};

class McpServersController : public drogon::HttpSimpleController<McpServersController>
{
public:
    void asyncHandleHttpRequest(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) override;
    PATH_LIST_BEGIN
        PATH_ADD("/api/mcp/servers", drogon::Get);
    PATH_LIST_END
};

class McpAddServerController : public drogon::HttpSimpleController<McpAddServerController>
{
public:
    void asyncHandleHttpRequest(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) override;
    PATH_LIST_BEGIN
        PATH_ADD("/api/mcp/servers", drogon::Post);
    PATH_LIST_END
};

class McpRemoveServerController : public drogon::HttpSimpleController<McpRemoveServerController>
{
public:
    void asyncHandleHttpRequest(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) override;
    PATH_LIST_BEGIN
        PATH_ADD("/api/mcp/servers/remove", drogon::Post);
    PATH_LIST_END
};

class McpRestartServerController : public drogon::HttpSimpleController<McpRestartServerController>
{
public:
    void asyncHandleHttpRequest(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) override;
    PATH_LIST_BEGIN
        PATH_ADD("/api/mcp/servers/restart", drogon::Post);
    PATH_LIST_END
};

// ─── RAG document management ────────────────────────────────────────────────

class RagUploadController : public drogon::HttpSimpleController<RagUploadController>
{
public:
    void asyncHandleHttpRequest(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) override;
    PATH_LIST_BEGIN
        PATH_ADD("/api/rag/upload", drogon::Post);
    PATH_LIST_END
};

class RagDocumentsController : public drogon::HttpSimpleController<RagDocumentsController>
{
public:
    void asyncHandleHttpRequest(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) override;
    PATH_LIST_BEGIN
        PATH_ADD("/api/rag/documents", drogon::Get);
    PATH_LIST_END
};

class RagRemoveDocController : public drogon::HttpSimpleController<RagRemoveDocController>
{
public:
    void asyncHandleHttpRequest(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) override;
    PATH_LIST_BEGIN
        PATH_ADD("/api/rag/documents/remove", drogon::Post);
    PATH_LIST_END
};

class RagClearController : public drogon::HttpSimpleController<RagClearController>
{
public:
    void asyncHandleHttpRequest(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) override;
    PATH_LIST_BEGIN
        PATH_ADD("/api/rag/clear", drogon::Post);
    PATH_LIST_END
};

// ─── Memory extraction ──────────────────────────────────────────────────────

class MemoryExtractController : public drogon::HttpSimpleController<MemoryExtractController>
{
public:
    void asyncHandleHttpRequest(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) override;
    PATH_LIST_BEGIN
        PATH_ADD("/api/memory/extract", drogon::Post);
    PATH_LIST_END
};

// ─── Model management (pull / delete / details) ─────────────────────────────

class ModelPullController : public drogon::HttpSimpleController<ModelPullController>
{
public:
    void asyncHandleHttpRequest(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) override;
    PATH_LIST_BEGIN
        PATH_ADD("/api/models/pull", drogon::Post);
    PATH_LIST_END
};

class ModelDeleteController : public drogon::HttpSimpleController<ModelDeleteController>
{
public:
    void asyncHandleHttpRequest(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) override;
    PATH_LIST_BEGIN
        PATH_ADD("/api/models/delete", drogon::Post);
    PATH_LIST_END
};

class ModelDetailsController : public drogon::HttpSimpleController<ModelDetailsController>
{
public:
    void asyncHandleHttpRequest(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) override;
    PATH_LIST_BEGIN
        PATH_ADD("/api/models/details", drogon::Get);
    PATH_LIST_END
};

// ─── Disk-based key-value storage ───────────────────────────────────────────
// Replaces browser localStorage with file-based persistence so data survives
// webview cache clears and isn't subject to the ~5 MB quota.

class StorageGetController : public drogon::HttpSimpleController<StorageGetController>
{
public:
    void asyncHandleHttpRequest(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) override;
    PATH_LIST_BEGIN
        PATH_ADD("/api/storage", drogon::Get);
    PATH_LIST_END
};

class StorageSetController : public drogon::HttpSimpleController<StorageSetController>
{
public:
    void asyncHandleHttpRequest(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) override;
    PATH_LIST_BEGIN
        PATH_ADD("/api/storage", drogon::Post);
    PATH_LIST_END
};
