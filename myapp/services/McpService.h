#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <json/json.h>
#include <functional>
#include <atomic>
#include <sys/types.h>

/**
 * McpService — manages MCP (Model Context Protocol) servers.
 *
 * Supports two transport types:
 *   1. **stdio** — server is spawned as a child process with pipes.
 *   2. **http**  — server is a remote URL (Streamable HTTP / JSON-RPC 2.0
 *                  over HTTP POST), e.g. Zoho MCP.
 *
 * Thread-safety: the internal map is guarded by a mutex; individual process
 * I/O is serialised per-server.
 */
class McpService {
public:
    /// The two supported MCP transport types.
    enum class Transport { Stdio, Http };

    /// Singleton accessor (the service is process-wide).
    static McpService &instance();

    /// Spawn and register a stdio-based MCP server.  Returns true on success.
    bool registerServer(const std::string &name,
                        const std::string &command,
                        const std::vector<std::string> &args);

    /// Register an HTTP-based (Streamable HTTP) MCP server.  Returns true on success.
    bool registerHttpServer(const std::string &name,
                            const std::string &url);

    /// Remove and shut down a registered MCP server.  Returns true if found.
    bool removeServer(const std::string &name);

    /// Restart a registered MCP server.  Returns true on success.
    bool restartServer(const std::string &name);

    /// Send a JSON-RPC 2.0 request and return the result (blocking, with
    /// a 5-second timeout for stdio, 15-second for HTTP).  Returns
    /// Json::nullValue on failure and sets `error` to a human-readable message.
    Json::Value callTool(const std::string &serverName,
                         const std::string &method,
                         const Json::Value &params,
                         std::string &error);

    /// Load server definitions from a JSON config file.
    void loadFromConfig(const std::string &path);

    /// Save current server list back to the config file.
    void saveToConfig(const std::string &path) const;

    /// Set the config path used for auto-saving.
    void setConfigPath(const std::string &path) {
        std::lock_guard<std::mutex> lock(mapMutex_);
        configPath_ = path;
    }

    /// Shut down and reap every managed child process.
    void shutdownAll();

    /// Convenience callback-based wrapper used by the chat controller.
    using ToolCallback = std::function<void(const Json::Value &result,
                                            const std::string &error)>;
    void callToolAsync(const std::string &serverName,
                       const std::string &method,
                       const Json::Value &params,
                       ToolCallback cb);

    /// Return the list of currently registered server names.
    std::vector<std::string> serverNames() const;

    /// Return detailed server info as JSON array.
    Json::Value serverDetails() const;

private:
    McpService() = default;

    /// Internal state for a single MCP server (stdio or HTTP).
    struct ServerProcess {
        Transport transport = Transport::Stdio;

        // ── stdio fields ──
        pid_t  pid      = -1;
        int    stdinFd  = -1;  // write end — we write requests here
        int    stdoutFd = -1;  // read  end — we read responses here

        // ── http fields ──
        std::string url;       // Remote MCP endpoint URL
        bool httpInitialized = false;

        // ── common ──
        std::mutex ioMutex;    // serialise reads/writes per process
        std::atomic<int> nextId{1};
        std::string command;
        std::vector<std::string> args;
    };

    std::string configPath_;
    mutable std::mutex mapMutex_;
    std::unordered_map<std::string, std::shared_ptr<ServerProcess>> servers_;

    /// Write a string to a file descriptor, returning true on success.
    static bool writeFull(int fd, const std::string &data);

    /// Read a single line (terminated by '\n') from a file descriptor
    /// within the given timeout.  Returns "" on failure/timeout.
    static std::string readLine(int fd, int timeoutSec);

    /// Send the MCP `initialize` handshake to a freshly spawned stdio server.
    bool sendInitialize(ServerProcess &proc);

    /// Send the MCP `initialize` handshake to an HTTP server via POST.
    bool sendHttpInitialize(ServerProcess &proc);

    /// Send a JSON-RPC 2.0 request via HTTP POST and parse the response.
    Json::Value httpJsonRpc(ServerProcess &proc,
                            const std::string &method,
                            const Json::Value &params,
                            std::string &error);

    /// Shut down a single server process (assumes caller holds mapMutex_ or not in map).
    void shutdownServer(const std::string &name, std::shared_ptr<ServerProcess> proc);
};
