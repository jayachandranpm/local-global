#include "services/McpService.h"

#include <drogon/drogon.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

// POSIX headers for process spawning and pipe I/O.
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

// libcurl for HTTP-based MCP transport.
#include <curl/curl.h>

// ─── Singleton ──────────────────────────────────────────────────────────────
McpService &McpService::instance() {
  static McpService svc;
  return svc;
}

// ─── Low-level I/O helpers ──────────────────────────────────────────────────

bool McpService::writeFull(int fd, const std::string &data) {
  size_t written = 0;
  while (written < data.size()) {
    ssize_t n = ::write(fd, data.data() + written, data.size() - written);
    if (n <= 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    written += static_cast<size_t>(n);
  }
  return true;
}

std::string McpService::readLine(int fd, int timeoutSec) {
  // Buffered reader — reads up to 4 KB chunks instead of one byte at a time,
  // significantly reducing the number of syscalls for large JSON-RPC payloads.
  static thread_local std::string buf;
  static thread_local int bufFd = -1;
  // Reset buffer if the fd changed (different server pipe).
  if (bufFd != fd) {
    buf.clear();
    bufFd = fd;
  }

  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);

  while (true) {
    // Check if a full line is already in the buffer.
    auto pos = buf.find('\n');
    if (pos != std::string::npos) {
      std::string line = buf.substr(0, pos);
      buf.erase(0, pos + 1);
      return line;
    }

    // How much time remains?
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0)
      return ""; // timed out

    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;

    int ret = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
    if (ret <= 0)
      return ""; // timeout or error

    char tmp[4096];
    ssize_t n = ::read(fd, tmp, sizeof(tmp));
    if (n <= 0)
      return ""; // EOF or error
    buf.append(tmp, static_cast<size_t>(n));
  }
}

// ─── registerServer ─────────────────────────────────────────────────────────
bool McpService::registerServer(const std::string &name,
                                const std::string &command,
                                const std::vector<std::string> &args) {
  // Create two pipes: parent→child (stdin) and child→parent (stdout).
  int toChild[2]; // toChild[1]  → write end (parent), toChild[0]  → read end
                  // (child stdin)
  int fromChild[2]; // fromChild[0] → read end (parent), fromChild[1] → write
                    // end (child stdout)

  if (::pipe(toChild) != 0) {
    LOG_ERROR << "McpService: pipe() failed for server '" << name
              << "': " << strerror(errno);
    return false;
  }
  if (::pipe(fromChild) != 0) {
    LOG_ERROR << "McpService: pipe() failed for server '" << name
              << "': " << strerror(errno);
    ::close(toChild[0]);
    ::close(toChild[1]);
    return false;
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    LOG_ERROR << "McpService: fork() failed for server '" << name
              << "': " << strerror(errno);
    ::close(toChild[0]);
    ::close(toChild[1]);
    ::close(fromChild[0]);
    ::close(fromChild[1]);
    return false;
  }

  if (pid == 0) {
    // ── Child process ──────────────────────────────────────────────────
    ::dup2(toChild[0], STDIN_FILENO);
    ::dup2(fromChild[1], STDOUT_FILENO);
    // Close unused ends.
    ::close(toChild[0]);
    ::close(toChild[1]);
    ::close(fromChild[0]);
    ::close(fromChild[1]);

    // Build argv for exec.
    std::vector<const char *> argv;
    argv.push_back(command.c_str());
    for (auto &a : args)
      argv.push_back(a.c_str());
    argv.push_back(nullptr);

    ::execvp(command.c_str(), const_cast<char *const *>(argv.data()));
    // If exec fails, exit the child.
    _exit(127);
  }

  // ── Parent process ─────────────────────────────────────────────────────
  ::close(toChild[0]);   // child's read end
  ::close(fromChild[1]); // child's write end

  // Make the parent's read fd non-blocking for poll-based reads.
  int flags = ::fcntl(fromChild[0], F_GETFL, 0);
  ::fcntl(fromChild[0], F_SETFL, flags | O_NONBLOCK);

  auto proc = std::make_shared<ServerProcess>();
  proc->pid = pid;
  proc->stdinFd = toChild[1];
  proc->stdoutFd = fromChild[0];
  proc->command = command;
  proc->args = args;

  // Send MCP initialize handshake.
  if (!sendInitialize(*proc)) {
    LOG_WARN << "McpService: initialize handshake failed for server '" << name
             << "' — server registered anyway.";
  }

  std::string cfgPath;
  {
    std::lock_guard<std::mutex> lock(mapMutex_);
    servers_[name] = proc;
    cfgPath = configPath_;
  }

  LOG_INFO << "McpService: registered server '" << name << "' (pid " << pid
           << ")";

  // Auto-save config if path is set
  if (!cfgPath.empty()) {
    saveToConfig(cfgPath);
  }

  return true;
}

// ─── sendInitialize ─────────────────────────────────────────────────────────
bool McpService::sendInitialize(ServerProcess &proc) {
  Json::Value req;
  req["jsonrpc"] = "2.0";
  req["id"] = proc.nextId++;
  req["method"] = "initialize";

  Json::Value params;
  params["protocolVersion"] = "2024-11-05";

  Json::Value clientInfo;
  clientInfo["name"] = "myapp";
  clientInfo["version"] = "1.0.0";
  params["clientInfo"] = clientInfo;

  Json::Value capabilities(Json::objectValue);
  params["capabilities"] = capabilities;

  req["params"] = params;

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  std::string payload = Json::writeString(wb, req) + "\n";

  std::lock_guard<std::mutex> lock(proc.ioMutex);
  if (!writeFull(proc.stdinFd, payload))
    return false;

  // Read the initialize response (best-effort).
  std::string line = readLine(proc.stdoutFd, 5);
  if (line.empty())
    return false;

  // After receiving initialize result, send "notifications/initialized".
  Json::Value notif;
  notif["jsonrpc"] = "2.0";
  notif["method"] = "notifications/initialized";
  std::string notifPayload = Json::writeString(wb, notif) + "\n";
  writeFull(proc.stdinFd, notifPayload);

  LOG_INFO << "McpService: initialize handshake completed.";
  return true;
}

// ─── HTTP transport helpers ─────────────────────────────────────────────────

// libcurl write callback — appends received data to a std::string.
static size_t mcpCurlWriteCb(char *ptr, size_t size, size_t nmemb,
                             void *userdata) {
  auto *buf = static_cast<std::string *>(userdata);
  size_t total = size * nmemb;
  buf->append(ptr, total);
  return total;
}

// ─── httpJsonRpc — send a JSON-RPC 2.0 request via HTTP POST ────────────────
Json::Value McpService::httpJsonRpc(ServerProcess &proc,
                                    const std::string &method,
                                    const Json::Value &params,
                                    std::string &error) {
  // Build the JSON-RPC request body.
  Json::Value req;
  req["jsonrpc"] = "2.0";
  req["method"] = method;

  // Notifications (method starts with "notifications/") have no id.
  bool isNotification = (method.rfind("notifications/", 0) == 0);
  if (!isNotification)
    req["id"] = proc.nextId++;

  if (!params.isNull())
    req["params"] = params;

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  std::string body = Json::writeString(wb, req);

  // Perform the HTTP POST with libcurl.
  CURL *curl = curl_easy_init();
  if (!curl) {
    error = "Failed to initialise libcurl handle";
    return Json::nullValue;
  }

  std::string responseBody;

  curl_easy_setopt(curl, CURLOPT_URL, proc.url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mcpCurlWriteCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // thread-safe timeouts

  // Headers
  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  CURLcode res = curl_easy_perform(curl);

  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    error = std::string("HTTP MCP request failed: ") + curl_easy_strerror(res);
    return Json::nullValue;
  }

  // Notifications return empty body — that's fine.
  if (isNotification) {
    return Json::Value(true);
  }

  if (httpCode < 200 || httpCode >= 300) {
    error = "HTTP MCP server returned status " + std::to_string(httpCode);
    return Json::nullValue;
  }

  if (responseBody.empty()) {
    error = "HTTP MCP server returned empty response";
    return Json::nullValue;
  }

  // Parse JSON response.
  Json::CharReaderBuilder rb;
  Json::Value resp;
  std::string parseErrs;
  std::istringstream ss(responseBody);
  if (!Json::parseFromStream(rb, ss, &resp, &parseErrs)) {
    error = "Invalid JSON from HTTP MCP server: " + parseErrs;
    return Json::nullValue;
  }

  if (resp.isMember("error")) {
    error = resp["error"].get("message", "Unknown MCP error").asString();
    return Json::nullValue;
  }

  return resp.isMember("result") ? resp["result"] : resp;
}

// ─── sendHttpInitialize ─────────────────────────────────────────────────────
bool McpService::sendHttpInitialize(ServerProcess &proc) {
  Json::Value params;
  params["protocolVersion"] = "2024-11-05";

  Json::Value clientInfo;
  clientInfo["name"] = "LocalGlobalAI";
  clientInfo["version"] = "1.0.0";
  params["clientInfo"] = clientInfo;

  Json::Value capabilities(Json::objectValue);
  params["capabilities"] = capabilities;

  std::string error;
  Json::Value result = httpJsonRpc(proc, "initialize", params, error);
  if (result.isNull()) {
    LOG_ERROR << "McpService: HTTP initialize failed: " << error;
    return false;
  }

  LOG_INFO << "McpService: HTTP MCP server '"
           << result.get("serverInfo", Json::objectValue)
                  .get("name", "unknown")
                  .asString()
           << "' initialized (protocol "
           << result.get("protocolVersion", "?").asString() << ")";

  // Send the initialized notification (fire-and-forget).
  httpJsonRpc(proc, "notifications/initialized", Json::nullValue, error);

  proc.httpInitialized = true;
  return true;
}

// ─── registerHttpServer ─────────────────────────────────────────────────────
bool McpService::registerHttpServer(const std::string &name,
                                    const std::string &url) {
  auto proc = std::make_shared<ServerProcess>();
  proc->transport = Transport::Http;
  proc->url = url;
  proc->command = url; // store URL in command for save/display

  // Perform MCP initialize handshake over HTTP.
  if (!sendHttpInitialize(*proc)) {
    LOG_WARN << "McpService: HTTP initialize handshake failed for '" << name
             << "' — server registered anyway.";
  }

  std::string cfgPath;
  {
    std::lock_guard<std::mutex> lock(mapMutex_);
    servers_[name] = proc;
    cfgPath = configPath_;
  }

  LOG_INFO << "McpService: registered HTTP MCP server '" << name << "' → "
           << url;

  // Auto-save config if path is set.
  if (!cfgPath.empty()) {
    saveToConfig(cfgPath);
  }

  return true;
}

// ─── callTool ───────────────────────────────────────────────────────────────
Json::Value McpService::callTool(const std::string &serverName,
                                 const std::string &method,
                                 const Json::Value &params,
                                 std::string &error) {
  std::shared_ptr<ServerProcess> proc;
  {
    std::lock_guard<std::mutex> lock(mapMutex_);
    auto it = servers_.find(serverName);
    if (it == servers_.end()) {
      error = "MCP server '" + serverName + "' is not registered.";
      return Json::nullValue;
    }
    proc = it->second;
  }

  // ── HTTP transport path ──
  if (proc->transport == Transport::Http) {
    std::lock_guard<std::mutex> ioLock(proc->ioMutex);
    return httpJsonRpc(*proc, method, params, error);
  }

  // ── Stdio transport path (original) ──
  // Build JSON-RPC 2.0 request.
  Json::Value req;
  req["jsonrpc"] = "2.0";
  req["id"] = proc->nextId++;
  req["method"] = method;
  if (!params.isNull())
    req["params"] = params;

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  std::string payload = Json::writeString(wb, req) + "\n";

  std::lock_guard<std::mutex> ioLock(proc->ioMutex);

  if (!writeFull(proc->stdinFd, payload)) {
    error = "Failed to write to MCP server '" + serverName + "'";
    return Json::nullValue;
  }

  // Read the response line (5 second timeout).
  std::string line = readLine(proc->stdoutFd, 5);
  if (line.empty()) {
    error = "Timeout or EOF reading from MCP server '" + serverName + "'";
    return Json::nullValue;
  }

  // Parse JSON-RPC response.
  Json::CharReaderBuilder rb;
  Json::Value resp;
  std::string parseErrs;
  std::istringstream ls(line);
  if (!Json::parseFromStream(rb, ls, &resp, &parseErrs)) {
    error = "Invalid JSON from MCP server '" + serverName + "': " + parseErrs;
    return Json::nullValue;
  }

  if (resp.isMember("error")) {
    error = resp["error"]["message"].asString();
    return Json::nullValue;
  }

  error.clear();
  return resp.isMember("result") ? resp["result"] : resp;
}

// ─── callToolAsync ──────────────────────────────────────────────────────────
// Runs the blocking callTool in a detached thread so the event loop is
// never blocked.  The callback is dispatched back onto the Drogon loop.
void McpService::callToolAsync(const std::string &serverName,
                               const std::string &method,
                               const Json::Value &params, ToolCallback cb) {
  // Capture copies for the lambda.
  auto sn = serverName;
  auto m = method;
  auto p = params;

  std::thread([sn, m, p, cb, this]() {
    std::string err;
    auto result = callTool(sn, m, p, err);
    // Dispatch the callback back onto Drogon's IO loop for thread safety.
    auto loop = drogon::app().getLoop();
    if (loop) {
      loop->queueInLoop([cb, result, err]() {
        if (cb)
          cb(result, err);
      });
    } else {
      if (cb)
        cb(result, err);
    }
  }).detach();
}

// ─── loadFromConfig ─────────────────────────────────────────────────────────
void McpService::loadFromConfig(const std::string &path) {
  {
    std::lock_guard<std::mutex> lock(mapMutex_);
    configPath_ = path; // Store for auto-saving
  }

  std::ifstream file(path);
  if (!file.is_open()) {
    LOG_INFO << "McpService: no config file '" << path
             << "' found — skipping MCP server auto-registration.";
    return;
  }

  Json::CharReaderBuilder rb;
  Json::Value root;
  std::string errs;
  if (!Json::parseFromStream(rb, file, &root, &errs)) {
    LOG_ERROR << "McpService: failed to parse '" << path << "': " << errs;
    return;
  }

  if (!root.isMember("servers") || !root["servers"].isArray()) {
    LOG_WARN << "McpService: config file missing 'servers' array.";
    return;
  }

  for (auto &srv : root["servers"]) {
    std::string name = srv.get("name", "").asString();
    std::string transport = srv.get("transport", "stdio").asString();

    if (name.empty()) {
      LOG_WARN << "McpService: skipping server entry with missing name.";
      continue;
    }

    if (transport == "http") {
      std::string url = srv.get("url", "").asString();
      if (url.empty()) {
        LOG_WARN << "McpService: skipping HTTP server '" << name
                 << "' with missing URL.";
        continue;
      }
      registerHttpServer(name, url);
    } else {
      std::string cmd = srv.get("command", "").asString();
      std::vector<std::string> args;
      if (srv.isMember("args") && srv["args"].isArray()) {
        for (auto &a : srv["args"])
          args.push_back(a.asString());
      }
      if (cmd.empty()) {
        LOG_WARN << "McpService: skipping stdio server '" << name
                 << "' with missing command.";
        continue;
      }
      registerServer(name, cmd, args);
    }
  }
}

// ─── shutdownAll ────────────────────────────────────────────────────────────
void McpService::shutdownAll() {
  std::lock_guard<std::mutex> lock(mapMutex_);
  for (auto &[name, proc] : servers_) {
    if (proc->transport == Transport::Stdio) {
      if (proc->stdinFd >= 0)
        ::close(proc->stdinFd);
      if (proc->stdoutFd >= 0)
        ::close(proc->stdoutFd);
      if (proc->pid > 0) {
        ::kill(proc->pid, SIGTERM);
        int status = 0;
        // Give the child up to 2 seconds to exit gracefully.
        for (int i = 0; i < 20; ++i) {
          if (::waitpid(proc->pid, &status, WNOHANG) != 0)
            break;
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        // Force-kill if still alive, then reap.
        if (::waitpid(proc->pid, &status, WNOHANG) == 0) {
          ::kill(proc->pid, SIGKILL);
          ::waitpid(proc->pid, &status, 0);
        }
      }
    }
    // HTTP servers just get deregistered — no process to kill.
    LOG_INFO << "McpService: shut down server '" << name << "'";
  }
  servers_.clear();
}

// ─── shutdownServer (internal) ──────────────────────────────────────────────
void McpService::shutdownServer(const std::string &name,
                                std::shared_ptr<ServerProcess> proc) {
  if (proc->transport == Transport::Stdio) {
    if (proc->stdinFd >= 0)
      ::close(proc->stdinFd);
    if (proc->stdoutFd >= 0)
      ::close(proc->stdoutFd);
    if (proc->pid > 0) {
      ::kill(proc->pid, SIGTERM);
      int status = 0;
      // Give the child up to 2 seconds to exit gracefully.
      for (int i = 0; i < 20; ++i) {
        if (::waitpid(proc->pid, &status, WNOHANG) != 0)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      // Force-kill if still alive, then reap.
      if (::waitpid(proc->pid, &status, WNOHANG) == 0) {
        ::kill(proc->pid, SIGKILL);
        ::waitpid(proc->pid, &status, 0);
      }
    }
  }
  // HTTP servers have nothing to shut down.
  LOG_INFO << "McpService: shut down server '" << name << "'";
}

// ─── removeServer ───────────────────────────────────────────────────────────
bool McpService::removeServer(const std::string &name) {
  std::shared_ptr<ServerProcess> proc;
  std::string cfgPath;
  {
    std::lock_guard<std::mutex> lock(mapMutex_);
    auto it = servers_.find(name);
    if (it == servers_.end())
      return false;
    proc = it->second;
    servers_.erase(it);
    cfgPath = configPath_;
  }
  shutdownServer(name, proc);

  // Auto-save config if path is set
  if (!cfgPath.empty()) {
    saveToConfig(cfgPath);
  }
  return true;
}

// ─── restartServer ──────────────────────────────────────────────────────────
bool McpService::restartServer(const std::string &name) {
  Transport transport;
  std::string command;
  std::string url;
  std::vector<std::string> args;
  std::shared_ptr<ServerProcess> proc;
  std::string cfgPath;
  {
    std::lock_guard<std::mutex> lock(mapMutex_);
    auto it = servers_.find(name);
    if (it == servers_.end())
      return false;
    proc = it->second;
    transport = proc->transport;
    command = proc->command;
    args = proc->args;
    url = proc->url;
    servers_.erase(it); // atomically remove under lock
    cfgPath = configPath_;
  }

  // Shutdown the old process (outside lock — may block).
  shutdownServer(name, proc);
  proc.reset();

  // Auto-save config after removal.
  if (!cfgPath.empty()) {
    saveToConfig(cfgPath);
  }

  // Re-register with same settings
  if (transport == Transport::Http) {
    return registerHttpServer(name, url);
  } else {
    return registerServer(name, command, args);
  }
}

// ─── saveToConfig ───────────────────────────────────────────────────────────
void McpService::saveToConfig(const std::string &path) const {
  Json::Value root;
  Json::Value arr(Json::arrayValue);

  {
    std::lock_guard<std::mutex> lock(mapMutex_);
    for (auto &[name, proc] : servers_) {
      Json::Value srv;
      srv["name"] = name;

      if (proc->transport == Transport::Http) {
        srv["transport"] = "http";
        srv["url"] = proc->url;
      } else {
        srv["transport"] = "stdio";
        srv["command"] = proc->command;
        Json::Value argsArr(Json::arrayValue);
        for (auto &a : proc->args)
          argsArr.append(a);
        srv["args"] = argsArr;
      }

      arr.append(srv);
    }
  }

  root["servers"] = arr;

  std::ofstream file(path);
  if (file.is_open()) {
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "  ";
    file << Json::writeString(wb, root);
    LOG_INFO << "McpService: saved config to '" << path << "'";
  } else {
    LOG_ERROR << "McpService: failed to write config to '" << path << "'";
  }
}

// ─── serverDetails ──────────────────────────────────────────────────────────
Json::Value McpService::serverDetails() const {
  Json::Value arr(Json::arrayValue);
  std::lock_guard<std::mutex> lock(mapMutex_);
  for (auto &[name, proc] : servers_) {
    Json::Value srv;
    srv["name"] = name;

    if (proc->transport == Transport::Http) {
      srv["transport"] = "http";
      srv["url"] = proc->url;
      srv["command"] = proc->url; // for backward compat display
      srv["status"] = proc->httpInitialized ? "running" : "error";
    } else {
      srv["transport"] = "stdio";
      srv["command"] = proc->command;
      srv["status"] = (proc->pid > 0) ? "running" : "stopped";
      Json::Value argsArr(Json::arrayValue);
      for (auto &a : proc->args)
        argsArr.append(a);
      srv["args"] = argsArr;
    }

    arr.append(srv);
  }
  return arr;
}

// ─── serverNames ────────────────────────────────────────────────────────────
std::vector<std::string> McpService::serverNames() const {
  std::lock_guard<std::mutex> lock(mapMutex_);
  std::vector<std::string> names;
  names.reserve(servers_.size());
  for (auto &[name, _] : servers_)
    names.push_back(name);
  return names;
}
