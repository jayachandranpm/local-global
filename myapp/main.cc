/**
 * main.cc — Application entry point.
 *
 * 1.  Starts the Drogon HTTP / WebSocket server on 127.0.0.1:8080 in a
 *     background thread.
 * 2.  Performs an Ollama health-check — warns if the daemon is unreachable.
 * 3.  Launches a native webview window pointed at the local server.
 * 4.  When the user closes the window, Drogon is shut down cleanly.
 */

#include <drogon/drogon.h>
#include <webview/webview.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <csignal>     // kill()
#include <curl/curl.h> // curl_global_init/cleanup
#include <json/json.h>
#include <sys/stat.h> // mkdir
#include <sys/wait.h> // waitpid
#include <unistd.h>   // fork/exec, getpid

#ifdef __APPLE__
// (libgen.h removed — dirname() replaced by rfind('/'))
#include <mach-o/dyld.h> // _NSGetExecutablePath
#endif

#include "services/McpService.h"
#include "services/OllamaService.h"
#include "services/RagService.h"

// ─── Resolve resource directory ─────────────────────────────────────────────
// When running as a .app bundle the executable is at:
//   MyApp.app/Contents/MacOS/myapp
// and resources are at:
//   MyApp.app/Contents/Resources/
// When running as a plain binary, resources are next to the binary.
static std::string getResourceDir() {
#ifdef __APPLE__
  char pathBuf[4096];
  uint32_t size = sizeof(pathBuf);
  if (_NSGetExecutablePath(pathBuf, &size) == 0) {
    // Resolve symlinks
    char *realPath = realpath(pathBuf, nullptr);
    if (realPath) {
      std::string fullPath(realPath);
      free(realPath);
      auto lastSlash = fullPath.rfind('/');
      std::string execDir = (lastSlash != std::string::npos && lastSlash > 0)
                                ? fullPath.substr(0, lastSlash)
                                : std::string("/");

      // Check if we're inside a .app bundle
      // (executable is in Contents/MacOS/)
      if (execDir.find(".app/Contents/MacOS") != std::string::npos) {
        // Go up to Contents/, then into Resources/
        std::string contentsDir = execDir.substr(0, execDir.rfind("/MacOS"));
        return contentsDir + "/Resources";
      }
      return execDir;
    }
  }
#endif
  return ".";
}

// ─── Resolve user data directory ────────────────────────────────────────────
// Mutable user data (uploads, app_storage, rag_data, logs, models) must NOT
// live inside the .app bundle — macOS code-signing invalidates on write and
// .app bundles are read-only on distributed copies.  We use the standard
// ~/Library/Application Support/<BundleName>/ location.
static std::string getDataDir() {
  std::string base;
  const char *home = getenv("HOME");
  if (home) {
    base = std::string(home) + "/Library/Application Support/Local Global";
  } else {
    base = "/tmp/Local Global";
  }
  // Create the directory tree if it doesn't exist
  mkdir(base.c_str(), 0755);
  return base;
}

// ─── Ollama health-check ────────────────────────────────────────────────────
// Fires a quick HEAD/GET against Ollama's root endpoint.  Non-blocking: we use
// Drogon's own HTTP client (which needs the event loop), so we perform the
// check once the loop is running.

static pid_t g_ttsPid = 0; // PID of the TTS Python server child process

// ─── Signal handler for SIGTERM / SIGINT ────────────────────────────────────
// Ensures TTS and MCP child processes are cleaned up even when the process is
// killed externally (pkill, Activity Monitor, etc.).
static std::atomic<bool> g_shuttingDown{false};

static void signalHandler(int sig) {
  if (g_shuttingDown.exchange(true))
    return; // re-entrant guard

  // Shut down MCP servers.
  McpService::instance().shutdownAll();

  // Kill and reap TTS child.
  if (g_ttsPid > 0) {
    kill(g_ttsPid, SIGTERM);
    int status = 0;
    for (int i = 0; i < 20; ++i) {
      if (waitpid(g_ttsPid, &status, WNOHANG) != 0)
        break;
      usleep(100000);
    }
    if (waitpid(g_ttsPid, &status, WNOHANG) == 0) {
      kill(g_ttsPid, SIGKILL);
      waitpid(g_ttsPid, &status, 0);
    }
    g_ttsPid = 0;
  }

  // Quit Drogon so the background thread exits.
  drogon::app().quit();

  _exit(128 + sig);
}

// ─── Launch TTS server ─────────────────────────────────────────────────────
// Spawns the Python tts_server.py as a child process.
static void launchTTSServer(const std::string &resDir,
                            const std::string &dataDir) {
  // The tts_server.py lives in Resources (resDir).
  // Models are downloaded to dataDir/models/kokoro on first run.
  // The Python venv lives alongside the source dir (not bundled).
  // We search for the venv in several locations.

  std::string ttsScript = resDir + "/tts_server.py";
  {
    char *rp = realpath(ttsScript.c_str(), nullptr);
    if (!rp) {
      LOG_WARN << "tts_server.py not found at " << ttsScript;
      return;
    }
    ttsScript = rp;
    free(rp);
  }

  // Determine the source directory (where tts_venv lives)
  // resDir is e.g. .../myapp/build/Local Global.app/Contents/Resources
  // Source dir is .../myapp/
  std::string sourceDir;
  auto appPos = resDir.find(".app/Contents/Resources");
  if (appPos != std::string::npos) {
    // Inside a bundle: resDir = .../myapp/build/X.app/Contents/Resources
    // Strip from ".app" back to get .../myapp/build/X
    std::string buildDir = resDir.substr(0, appPos);
    // buildDir = .../myapp/build/Local Global
    // Go up to .../myapp/build
    auto slash1 = buildDir.rfind('/');
    if (slash1 != std::string::npos) {
      std::string buildParent = buildDir.substr(0, slash1);
      // buildParent = .../myapp/build  — go up one more to myapp/
      auto slash2 = buildParent.rfind('/');
      if (slash2 != std::string::npos) {
        sourceDir = buildParent.substr(0, slash2);
      }
    }
  }
  if (sourceDir.empty()) {
    sourceDir = resDir; // fallback: plain binary
  }

  // Search for Python interpreter (venv preferred, system fallback)
  std::vector<std::string> pythonCandidates = {
      sourceDir + "/tts_venv/bin/python3",
      resDir + "/tts_venv/bin/python3",
      "/opt/homebrew/bin/python3.12",
      "/usr/local/bin/python3.12",
      "/usr/bin/python3",
  };

  std::string pythonPath;
  for (auto &p : pythonCandidates) {
    // Check file exists (but do NOT resolve symlinks — venv python
    // must be called via its venv path to find site-packages)
    if (access(p.c_str(), X_OK) == 0) {
      pythonPath = p;
      break;
    }
  }

  if (pythonPath.empty()) {
    LOG_WARN << "No suitable Python interpreter found for TTS server";
    return;
  }

  LOG_INFO << "Launching TTS server: " << pythonPath << " " << ttsScript;

  pid_t pid = fork();
  if (pid == 0) {
    // Child process — redirect output to a log file in user data dir
    std::string logPath = dataDir + "/tts_server.log";
    FILE *logFile = fopen(logPath.c_str(), "w");
    if (logFile) {
      dup2(fileno(logFile), STDOUT_FILENO);
      dup2(fileno(logFile), STDERR_FILENO);
      fclose(logFile);
    }
    // Pass data dir to TTS server so it can find/download models there
    setenv("LOCAL_GLOBAL_DATA_DIR", dataDir.c_str(), 1);
    // Let the TTS server detect macOS app termination paths (such as Cmd+Q)
    // that can bypass the normal webview shutdown cleanup.
    std::string parentPid = std::to_string(getppid());
    setenv("LOCAL_GLOBAL_PARENT_PID", parentPid.c_str(), 1);
    // exec the Python TTS server
    execl(pythonPath.c_str(), pythonPath.c_str(), ttsScript.c_str(), nullptr);
    // If execl returns, it failed
    perror("execl failed");
    _exit(1);
  } else if (pid > 0) {
    g_ttsPid = pid;
    LOG_INFO << "TTS server started (PID " << pid << ")";
  } else {
    LOG_WARN << "Failed to fork TTS server process";
  }
}

static void checkOllamaHealth() {
  auto client = drogon::HttpClient::newHttpClient("http://127.0.0.1:11434");
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setPath("/");
  req->setMethod(drogon::Get);

  client->sendRequest(
      req,
      [](drogon::ReqResult result, const drogon::HttpResponsePtr &resp) {
        if (result == drogon::ReqResult::Ok && resp &&
            resp->getStatusCode() == drogon::k200OK) {
          LOG_INFO << "Ollama is running and reachable.";
        } else {
          LOG_WARN << "Ollama does not appear to be running at "
                      "http://localhost:11434 — the assistant will not "
                      "be able to generate responses until it is started.";
        }
      },
      /* timeout (seconds) */ 3.0);
}

// ─── main ───────────────────────────────────────────────────────────────────
int main() {
  // NOTE: Do NOT use signal(SIGCHLD, SIG_IGN) here — it prevents
  // waitpid() from returning the child's exit status, which breaks
  // safeExec() and file-extraction fork/waitpid patterns throughout
  // the codebase.  Short-lived fire-and-forget forks (e.g. "open"
  // commands) produce tiny zombie entries that are cleaned up when the
  // app exits.  The long-lived TTS server child is explicitly killed
  // and waitpid'd at shutdown.

  // Initialise libcurl globally (must be called before any threads use curl).
  curl_global_init(CURL_GLOBAL_ALL);

  // Register signal handlers so external kills clean up child processes.
  std::signal(SIGTERM, signalHandler);
  std::signal(SIGINT, signalHandler);

  // Resolve the resource directory (handles both .app bundle and plain binary)
  std::string resDir = getResourceDir();
  std::string dataDir = getDataDir();
  std::string frontendDir = resDir + "/frontend";
  std::string mcpConfig = resDir + "/mcp_servers.json";

  LOG_INFO << "Resource directory: " << resDir;
  LOG_INFO << "Data directory:     " << dataDir;
  LOG_INFO << "Frontend directory: " << frontendDir;

  // --- Configure Drogon ---------------------------------------------------
  // Create upload directory for file uploads in user data dir (NOT inside .app)
  std::string uploadDir = dataDir + "/uploads";
  {
    mkdir(uploadDir.c_str(), 0755);
    mkdir((uploadDir + "/tmp").c_str(), 0755);
  }

  // --- Create app_storage directory (disk-backed key-value store) ----------
  {
    std::string storageDir = dataDir + "/app_storage";
    mkdir(storageDir.c_str(), 0755);
  }

  // --- Initialise RAG persistence -----------------------------------------
  // Set storage path and load any previously indexed documents / chunks.
  {
    std::string ragDir = dataDir + "/rag_data";
    RagService::instance().setStoragePath(ragDir);
    RagService::instance().loadFromDisk();
  }

  // --- Launch TTS server (Python kitten-tts) --------------------------------
  launchTTSServer(resDir, dataDir);

  drogon::app()
      .setLogLevel(trantor::Logger::kInfo)
      .addListener("127.0.0.1", 8080)
      .setDocumentRoot(frontendDir) // serves static frontend files
      .setUploadPath(uploadDir)     // where uploaded files are stored
      .setStaticFilesCacheTime(0)   // disable cache during development
      .setClientMaxBodySize(50 * 1024 * 1024)       // 50 MB max body
      .setClientMaxMemoryBodySize(50 * 1024 * 1024) // keep body in memory
      .setIdleConnectionTimeout(60)
      .setThreadNum(4);

  // Register a callback that runs once the event loop is up.
  drogon::app().registerBeginningAdvice([&mcpConfig]() {
    LOG_INFO << "Drogon server started on http://127.0.0.1:8080";

    // Fire the Ollama health-check inside the event loop.
    checkOllamaHealth();

    // Load MCP server definitions from mcp_servers.json (best-effort).
    McpService::instance().loadFromConfig(mcpConfig);
  });

  // --- Start Drogon in a background thread --------------------------------
  std::atomic<bool> drogonRunning{true};
  std::thread serverThread([&drogonRunning]() {
    drogon::app().run();
    drogonRunning = false;
  });

  // Poll until Drogon is ready (up to 10 seconds).
  {
    CURL *probe = curl_easy_init();
    bool ready = false;
    for (int attempt = 0; attempt < 100 && drogonRunning; ++attempt) {
      if (probe) {
        curl_easy_setopt(probe, CURLOPT_URL, "http://127.0.0.1:8080/");
        curl_easy_setopt(probe, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(probe, CURLOPT_TIMEOUT_MS, 200L);
        curl_easy_setopt(probe, CURLOPT_CONNECTTIMEOUT_MS, 100L);
        curl_easy_setopt(probe, CURLOPT_NOSIGNAL, 1L);
        if (curl_easy_perform(probe) == CURLE_OK) {
          ready = true;
          break;
        }
        curl_easy_reset(probe);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (probe)
      curl_easy_cleanup(probe);
    if (!ready)
      LOG_WARN
          << "Drogon did not become ready within 10 s — opening webview anyway";
  }

  // --- Launch the native webview window ------------------------------------
  // webview must be created on the main (UI) thread.
  webview::webview wv(/* debug */ true, nullptr);
  wv.set_title("Local Global");
  wv.set_size(1200, 800, WEBVIEW_HINT_NONE);

  // ── Bind native function to open URLs in system browser ─────────────
  wv.bind("openExternal", [](const std::string &args) -> std::string {
    // args is a JSON array like ["https://example.com"]
    Json::CharReaderBuilder rb;
    Json::Value parsed;
    std::string errs;
    std::istringstream ss(args);
    if (Json::parseFromStream(rb, ss, &parsed, &errs) && parsed.isArray() &&
        parsed.size() > 0) {
      std::string url = parsed[0].asString();
      // Only open http/https URLs — reject anything with shell metacharacters
      if ((url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) &&
          url.find('"') == std::string::npos &&
          url.find('\'') == std::string::npos &&
          url.find('`') == std::string::npos &&
          url.find('$') == std::string::npos &&
          url.find(';') == std::string::npos &&
          url.find('|') == std::string::npos &&
          url.find('&') == std::string::npos &&
          url.find('\n') == std::string::npos) {
#ifdef __APPLE__
        // Use double-fork to avoid zombie processes: the
        // intermediate child exits immediately and is reaped by
        // waitpid; the grandchild is reparented to init/launchd.
        pid_t p = fork();
        if (p == 0) {
          if (fork() == 0) {
            execlp("open", "open", url.c_str(), nullptr);
            _exit(127);
          }
          _exit(0);
        } else if (p > 0) {
          int st;
          waitpid(p, &st, 0);
        }
#elif defined(_WIN32)
                // NOTE: If porting to Windows, use ShellExecuteW() instead.
                // ::system() with URL is a shell injection vulnerability.
                (void)url;
#else
                pid_t p = fork();
                if (p == 0) {
                    if (fork() == 0) {
                        execlp("xdg-open", "xdg-open", url.c_str(), nullptr);
                        _exit(127);
                    }
                    _exit(0);
                } else if (p > 0) {
                    int st; waitpid(p, &st, 0);
                }
#endif
      }
    }
    return "{}";
  });

  // ── Bind native function to save files to Downloads ─────────────────
  wv.bind("saveFile", [](const std::string &args) -> std::string {
    Json::CharReaderBuilder rb;
    Json::Value parsed;
    std::string errs;
    std::istringstream ss2(args);
    if (!Json::parseFromStream(rb, ss2, &parsed, &errs) || !parsed.isArray() ||
        parsed.size() < 2) {
      return R"({"error":"Invalid arguments"})";
    }
    std::string filename = parsed[0].asString();
    std::string content = parsed[1].asString();

    // Sanitize filename — strip path separators and null bytes
    for (auto &ch : filename) {
      if (ch == '/' || ch == '\\' || ch == '\0')
        ch = '_';
    }
    // Remove any remaining embedded nulls (belt-and-suspenders)
    filename.erase(std::remove(filename.begin(), filename.end(), '\0'),
                   filename.end());

    // Write to ~/Downloads
    const char *home = getenv("HOME");
    std::string dir = home ? std::string(home) + "/Downloads" : "/tmp";
    std::string path = dir + "/" + filename;

    // If file exists, add a number suffix
    {
      std::ifstream test(path);
      if (test.good()) {
        auto dot = filename.rfind('.');
        std::string base =
            (dot != std::string::npos) ? filename.substr(0, dot) : filename;
        std::string ext =
            (dot != std::string::npos) ? filename.substr(dot) : "";
        bool found = false;
        for (int i = 1; i < 1000; i++) {
          path = dir + "/" + base + "_" + std::to_string(i) + ext;
          std::ifstream t2(path);
          if (!t2.good()) {
            found = true;
            break;
          }
        }
        if (!found) {
          return R"({"error":"Too many copies of this file exist"})";
        }
      }
    }

    std::ofstream ofs(path);
    if (!ofs) {
      return R"({"error":"Failed to write file"})";
    }
    ofs << content;
    ofs.close();
    if (ofs.fail()) {
      return R"({"error":"Failed to write file contents"})";
    }

    // Reveal in Finder (double-fork to avoid zombie processes)
#ifdef __APPLE__
    {
      pid_t p = fork();
      if (p == 0) {
        if (fork() == 0) {
          execlp("open", "open", "-R", path.c_str(), nullptr);
          _exit(127);
        }
        _exit(0);
      } else if (p > 0) {
        int st;
        waitpid(p, &st, 0);
      }
    }
#endif

    Json::Value result;
    result["success"] = true;
    result["path"] = path;
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    return Json::writeString(wb, result);
  });

  // ── Inject JS to intercept external link clicks ─────────────────────
  wv.init(R"JS(
        document.addEventListener('click', function(e) {
            let target = e.target;
            while (target && target.tagName !== 'A') target = target.parentElement;
            if (target && target.href) {
                const url = target.href;
                const isLocal = url.startsWith('http://127.0.0.1:8080') ||
                                url.startsWith('http://localhost:8080');
                const isExternal = url.startsWith('http://') || url.startsWith('https://');
                if (!isLocal && isExternal) {
                    e.preventDefault();
                    e.stopPropagation();
                    window.openExternal(url);
                }
            }
        }, true);
    )JS");

  wv.navigate("http://127.0.0.1:8080");
  wv.run(); // blocks until the window is closed

  // --- Clean shutdown ------------------------------------------------------
  LOG_INFO << "Webview closed — shutting down server…";
  drogon::app().quit();

  if (serverThread.joinable())
    serverThread.join();

  // Tear down any child MCP server processes.
  McpService::instance().shutdownAll();

  // Shut down the TTS server and reap the child to avoid a zombie.
  if (g_ttsPid > 0) {
    LOG_INFO << "Stopping TTS server (PID " << g_ttsPid << ")";
    kill(g_ttsPid, SIGTERM);
    // Give it a moment to exit gracefully, then reap.
    int status = 0;
    pid_t ret = waitpid(g_ttsPid, &status, WNOHANG);
    if (ret == 0) {
      // Still running — wait briefly then force kill.
      usleep(200000); // 200ms
      ret = waitpid(g_ttsPid, &status, WNOHANG);
      if (ret == 0) {
        kill(g_ttsPid, SIGKILL);
        waitpid(g_ttsPid, &status, 0);
      }
    }
    g_ttsPid = 0;
  }

  curl_global_cleanup();

  LOG_INFO << "Goodbye.";
  return 0;
}
