#include "controllers/SearchController.h"
#include "services/DuckDuckGoService.h"
#include "services/McpService.h"
#include "services/OllamaService.h"
#include "services/RagService.h"

#include <chrono>
#include <cstdio>  // for std::remove
#include <cstdlib> // for getenv
#include <curl/curl.h>
#include <fcntl.h> // open, O_WRONLY
#include <fstream>
#include <json/json.h>
#include <random> // for std::mt19937
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h> // fork, execvp, dup2, getpid

// ─── Safe command execution (no shell interpolation) ────────────────────────
// Runs a command with arguments via fork/exec. Returns exit code (0=success).
// This avoids shell injection vulnerabilities that std::system() is prone to.
static int safeExec(const std::vector<std::string> &argv) {
  if (argv.empty())
    return -1;

  pid_t pid = fork();
  if (pid < 0)
    return -1;

  if (pid == 0) {
    // Child: suppress stdout/stderr by redirecting to /dev/null
    int devNull = open("/dev/null", O_WRONLY);
    if (devNull >= 0) {
      dup2(devNull, STDOUT_FILENO);
      dup2(devNull, STDERR_FILENO);
      close(devNull);
    }
    std::vector<const char *> cargv;
    for (auto &a : argv)
      cargv.push_back(a.c_str());
    cargv.push_back(nullptr);
    execvp(cargv[0], const_cast<char *const *>(cargv.data()));
    _exit(127);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// ─── Safe recursive directory removal ───────────────────────────────────────
static void safeRmDir(const std::string &path) {
  safeExec({"rm", "-rf", path});
}

// ─── Safe mkdir -p ──────────────────────────────────────────────────────────
static void safeMkdirP(const std::string &path) {
  safeExec({"mkdir", "-p", path});
}

// ═══════════════════════════════════════════════════════════════════════════
// GET /api/models — list locally available Ollama models
// ═══════════════════════════════════════════════════════════════════════════

void ModelsController::asyncHandleHttpRequest(
    const drogon::HttpRequestPtr & /*req*/,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
  auto cb =
      std::make_shared<std::function<void(const drogon::HttpResponsePtr &)>>(
          std::move(callback));

  OllamaService::listModelsWithInfo(
      [cb](std::vector<ModelInfo> models, const std::string &err) {
        Json::Value root;
        if (!err.empty()) {
          root["error"] = err;
          auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
          resp->setStatusCode(drogon::k502BadGateway);
          (*cb)(resp);
          return;
        }

        // Backwards-compat: "models" is still an array of name strings
        Json::Value arr(Json::arrayValue);
        for (auto &m : models)
          arr.append(m.name);
        root["models"] = arr;

        // New: "model_context" maps model name → context_length
        Json::Value ctx(Json::objectValue);
        for (auto &m : models)
          ctx[m.name] = m.contextLength;
        root["model_context"] = ctx;

        auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
        (*cb)(resp);
      });
}

// ═══════════════════════════════════════════════════════════════════════════
// GET /api/search?q=... — DuckDuckGo search proxy
// ═══════════════════════════════════════════════════════════════════════════

void SearchApiController::asyncHandleHttpRequest(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
  auto cb =
      std::make_shared<std::function<void(const drogon::HttpResponsePtr &)>>(
          std::move(callback));

  std::string query = req->getParameter("q");
  if (query.empty()) {
    Json::Value root;
    root["error"] = "Missing query parameter 'q'";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
    resp->setStatusCode(drogon::k400BadRequest);
    (*cb)(resp);
    return;
  }

  DuckDuckGoService::search(
      query, [cb](std::vector<SearchResult> results, const std::string &err) {
        Json::Value root;
        if (!err.empty()) {
          root["error"] = err;
          auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
          resp->setStatusCode(drogon::k502BadGateway);
          (*cb)(resp);
          return;
        }

        Json::Value arr(Json::arrayValue);
        for (auto &r : results) {
          Json::Value obj;
          obj["title"] = r.title;
          obj["url"] = r.url;
          obj["snippet"] = r.snippet;
          arr.append(obj);
        }
        root["results"] = arr;
        auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
        (*cb)(resp);
      });
}

// ═══════════════════════════════════════════════════════════════════════════
// GET /api/mcp/servers — list registered MCP servers with details
// ═══════════════════════════════════════════════════════════════════════════

void McpServersController::asyncHandleHttpRequest(
    const drogon::HttpRequestPtr & /*req*/,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
  Json::Value root;
  root["servers"] = McpService::instance().serverDetails();

  auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
  callback(resp);
}

// ═══════════════════════════════════════════════════════════════════════════
// POST /api/mcp/servers — add a new MCP server
// ═══════════════════════════════════════════════════════════════════════════

void McpAddServerController::asyncHandleHttpRequest(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
  auto body = req->getJsonObject();
  Json::Value root;

  if (!body) {
    root["success"] = false;
    root["error"] = "Invalid JSON body";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
    resp->setStatusCode(drogon::k400BadRequest);
    callback(resp);
    return;
  }

  std::string name = (*body).get("name", "").asString();
  std::string transport = (*body).get("transport", "stdio").asString();

  if (name.empty()) {
    root["success"] = false;
    root["error"] = "Server name is required";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
    resp->setStatusCode(drogon::k400BadRequest);
    callback(resp);
    return;
  }

  bool ok = false;

  if (transport == "http") {
    // HTTP-based MCP server (remote URL)
    std::string url = (*body).get("url", "").asString();
    if (url.empty()) {
      root["success"] = false;
      root["error"] = "URL is required for HTTP transport";
      auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
      resp->setStatusCode(drogon::k400BadRequest);
      callback(resp);
      return;
    }
    ok = McpService::instance().registerHttpServer(name, url);
  } else {
    // stdio-based MCP server (local command)
    std::string command = (*body).get("command", "").asString();
    std::vector<std::string> args;
    if ((*body).isMember("args") && (*body)["args"].isArray()) {
      for (const auto &a : (*body)["args"]) {
        args.push_back(a.asString());
      }
    }
    if (command.empty()) {
      root["success"] = false;
      root["error"] = "Command is required for stdio transport";
      auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
      resp->setStatusCode(drogon::k400BadRequest);
      callback(resp);
      return;
    }
    ok = McpService::instance().registerServer(name, command, args);
  }

  root["success"] = ok;
  if (!ok) {
    root["error"] = "Failed to register MCP server";
  }
  auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
  callback(resp);
}

// ═══════════════════════════════════════════════════════════════════════════
// POST /api/mcp/servers/remove — remove an MCP server
// ═══════════════════════════════════════════════════════════════════════════

void McpRemoveServerController::asyncHandleHttpRequest(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
  auto body = req->getJsonObject();
  Json::Value root;

  if (!body) {
    root["success"] = false;
    root["error"] = "Invalid JSON body";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
    resp->setStatusCode(drogon::k400BadRequest);
    callback(resp);
    return;
  }

  std::string name = (*body).get("name", "").asString();
  if (name.empty()) {
    root["success"] = false;
    root["error"] = "Server name is required";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
    resp->setStatusCode(drogon::k400BadRequest);
    callback(resp);
    return;
  }

  bool ok = McpService::instance().removeServer(name);
  root["success"] = ok;
  if (!ok) {
    root["error"] = "Server '" + name + "' not found";
  }
  auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
  callback(resp);
}

// ═══════════════════════════════════════════════════════════════════════════
// POST /api/mcp/servers/restart — restart an MCP server
// ═══════════════════════════════════════════════════════════════════════════

void McpRestartServerController::asyncHandleHttpRequest(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
  auto body = req->getJsonObject();
  Json::Value root;

  if (!body) {
    root["success"] = false;
    root["error"] = "Invalid JSON body";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
    resp->setStatusCode(drogon::k400BadRequest);
    callback(resp);
    return;
  }

  std::string name = (*body).get("name", "").asString();
  if (name.empty()) {
    root["success"] = false;
    root["error"] = "Server name is required";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
    resp->setStatusCode(drogon::k400BadRequest);
    callback(resp);
    return;
  }

  bool ok = McpService::instance().restartServer(name);
  root["success"] = ok;
  if (!ok) {
    root["error"] = "Server '" + name + "' not found or failed to restart";
  }
  auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
  callback(resp);
}

// ═══════════════════════════════════════════════════════════════════════════
// POST /api/rag/upload — upload a document for RAG indexing
// ═══════════════════════════════════════════════════════════════════════════
//
// Accepts multipart/form-data with:
//   file:  the document file (.txt, .md, .csv, .json, .html, .xml, .log, etc.)
//   model: (optional) the Ollama model to use for embeddings

void RagUploadController::asyncHandleHttpRequest(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
  auto cb =
      std::make_shared<std::function<void(const drogon::HttpResponsePtr &)>>(
          std::move(callback));

  LOG_INFO << "RAG upload: content-type=" << req->getHeader("content-type")
           << "  body-length=" << req->bodyLength();

  // Parse multipart form data.
  drogon::MultiPartParser parser;
  if (parser.parse(req) != 0) {
    // Fallback: maybe it's not multipart. Try reading parameters directly.
    LOG_WARN << "RAG upload: MultiPartParser failed, checking params";
  }

  // Get file content — try multipart files first, then fall back to
  // a 'content' form parameter.
  std::string fileName;
  std::string fileContent;
  size_t fileSize = 0;
  std::string embModel;

  auto &files = parser.getFiles();
  if (!files.empty()) {
    // Got a real file upload.
    auto &f = files[0];
    fileName = f.getFileName();
    if (f.fileLength() > 0 && f.fileData() != nullptr) {
      fileContent.assign(f.fileData(), f.fileLength());
    }
    fileSize = f.fileLength();
    LOG_INFO << "RAG upload: got file '" << fileName << "' (" << fileSize
             << " bytes)";
  }

  // Check form parameters for metadata / fallback content.
  auto &params = parser.getParameters();
  if (fileName.empty() && params.count("name"))
    fileName = params.at("name");
  if (fileContent.empty() && params.count("content"))
    fileContent = params.at("content");
  if (params.count("model"))
    embModel = params.at("model");
  if (params.count("size")) {
    try {
      fileSize = static_cast<size_t>(std::stoull(params.at("size")));
    } catch (...) {
      // Ignore invalid "size" values — fileSize stays 0 and will
      // be set from fileContent.size() below.
    }
  }

  if (fileName.empty() || fileContent.empty()) {
    Json::Value root;
    root["success"] = false;
    root["error"] = "No file received. Please select a file to upload.";
    root["debug_files"] = static_cast<Json::UInt>(files.size());
    root["debug_params"] = static_cast<Json::UInt>(params.size());
    root["debug_body_len"] = static_cast<Json::UInt64>(req->bodyLength());
    auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
    resp->setStatusCode(drogon::k400BadRequest);
    (*cb)(resp);
    return;
  }

  if (fileSize == 0)
    fileSize = fileContent.size();

  // Use a lightweight embedding model — NOT the full chat model.
  // nomic-embed-text is ~274MB vs gemma3:4b at ~3GB.
  if (embModel.empty())
    embModel = "nomic-embed-text:latest";

  // Limit raw file size to 5MB for binary formats (PDF, DOCX, etc.)
  // Text extraction will further limit to 512K chars.
  if (fileContent.size() > 5 * 1024 * 1024) {
    LOG_WARN << "RAG upload: file too large (" << fileContent.size()
             << " bytes)";
    Json::Value root;
    root["success"] = false;
    root["error"] = "File is too large (max 5 MB).";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
    resp->setStatusCode(drogon::k400BadRequest);
    (*cb)(resp);
    return;
  }

  // Generate timestamp early (needed for temp file names).
  auto now = std::chrono::system_clock::now().time_since_epoch();
  auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  std::string tmpBase = "/tmp/rag_upload_" + std::to_string(millis) + "_" +
                        std::to_string(::getpid()) + "_" + std::to_string([] {
                          static std::mt19937 rng(std::random_device{}());
                          return rng();
                        }());

  // ── Text Extraction ────────────────────────────────────────────────
  // Handles: PDF, DOCX, DOC, RTF, ODT, EPUB, PPTX, XLSX, HTML, XML,
  //          and plain text (.txt, .md, .csv, .json, .log, .yaml, etc.)
  //
  // Strategy:
  //   • PDF  → pdftotext (poppler)
  //   • DOCX/DOC/RTF/ODT/EPUB → textutil (built-in macOS)
  //   • PPTX → unzip + strip XML tags from slides
  //   • XLSX → unzip + strip XML tags from shared strings + sheets
  //   • HTML/HTM/XML → strip tags in-process
  //   • Everything else → treat as plain text

  std::string textContent;
  std::string ext;
  {
    auto dot = fileName.rfind('.');
    if (dot != std::string::npos)
      ext = fileName.substr(dot + 1);
    // Sanitise extension: keep only alphanumeric chars to prevent
    // any shell metacharacter or path-traversal issues.
    std::string safeExt;
    for (auto c : ext) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (std::isalnum(static_cast<unsigned char>(c)))
        safeExt += c;
    }
    ext = safeExt;
  }

  // Also sanitise the display fileName — strip path separators to
  // prevent directory traversal in document source keys.
  {
    std::string safeName;
    for (char c : fileName) {
      if (c == '/' || c == '\\' || c == '\0')
        continue;
      safeName += c;
    }
    if (safeName.empty())
      safeName = "upload";
    fileName = safeName;
  }

  // Helper lambda: write raw content to a temp file with a given extension
  // and return the path.
  auto writeTempFile = [&](const std::string &extension) -> std::string {
    std::string path = tmpBase + "." + extension;
    std::ofstream out(path, std::ios::binary);
    if (!out)
      return "";
    out.write(fileContent.data(), fileContent.size());
    out.close();
    if (!out)
      return ""; // write or close failed
    return path;
  };

  // Helper lambda: read an entire file into a string.
  auto readFile = [](const std::string &path) -> std::string {
    std::ifstream in(path);
    if (!in.good())
      return "";
    std::stringstream buf;
    buf << in.rdbuf();
    return buf.str();
  };

  // Helper lambda: strip XML/HTML tags from a string.
  auto stripTags = [](const std::string &input) -> std::string {
    std::string out;
    out.reserve(input.size());
    bool inTag = false;
    for (char c : input) {
      if (c == '<') {
        inTag = true;
        continue;
      }
      if (c == '>') {
        inTag = false;
        out += ' ';
        continue;
      }
      if (!inTag)
        out += c;
    }
    return out;
  };

  // (runCmd removed — all commands now use safeExec() to avoid shell injection)

  std::string extractionError; // set on failure

  // ── PDF ─────────────────────────────────────────────────────────
  if (ext == "pdf") {
    std::string srcPath = writeTempFile("pdf");
    std::string dstPath = tmpBase + ".txt";
    int ret =
        safeExec({"pdftotext", "-enc", "UTF-8", "-nopgbrk", srcPath, dstPath});
    if (ret == 0)
      textContent = readFile(dstPath);
    if (textContent.empty())
      extractionError = "PDF text extraction failed. "
                        "Install poppler: brew install poppler";
    std::remove(srcPath.c_str());
    std::remove(dstPath.c_str());
  }
  // ── DOCX / DOC / RTF / ODT / EPUB → macOS textutil ─────────────
  else if (ext == "docx" || ext == "doc" || ext == "rtf" || ext == "odt" ||
           ext == "epub") {
    std::string srcPath = writeTempFile(ext);
    std::string dstPath = tmpBase + ".txt";
    int ret = safeExec({"/usr/bin/textutil", "-convert", "txt", "-encoding",
                        "UTF-8", "-output", dstPath, srcPath});
    if (ret == 0)
      textContent = readFile(dstPath);
    if (textContent.empty())
      extractionError = "Could not extract text from ." + ext +
                        " file. textutil conversion failed.";
    std::remove(srcPath.c_str());
    std::remove(dstPath.c_str());
  }
  // ── PPTX (PowerPoint) → unzip slides, strip XML ────────────────
  else if (ext == "pptx") {
    std::string srcPath = writeTempFile("pptx");
    std::string extractDir = tmpBase + "_pptx";
    safeMkdirP(extractDir);
    safeExec(
        {"unzip", "-o", "-q", srcPath, "ppt/slides/*.xml", "-d", extractDir});
    // Read all slide XML files in order
    std::ostringstream combined;
    for (int slideNum = 1; slideNum < 200; ++slideNum) {
      std::string slidePath =
          extractDir + "/ppt/slides/slide" + std::to_string(slideNum) + ".xml";
      std::string xml = readFile(slidePath);
      if (xml.empty())
        break;
      combined << "--- Slide " << slideNum << " ---\n";
      combined << stripTags(xml) << "\n\n";
    }
    textContent = combined.str();
    if (textContent.empty())
      extractionError = "Could not extract text from .pptx file.";
    std::remove(srcPath.c_str());
    safeRmDir(extractDir);
  }
  // ── XLSX (Excel) → unzip shared strings + sheets, strip XML ─────
  else if (ext == "xlsx") {
    std::string srcPath = writeTempFile("xlsx");
    std::string extractDir = tmpBase + "_xlsx";
    safeMkdirP(extractDir);
    safeExec({"unzip", "-o", "-q", srcPath, "xl/sharedStrings.xml",
              "xl/worksheets/*.xml", "-d", extractDir});
    std::ostringstream combined;
    // Shared strings first (contains most cell text in XLSX)
    std::string shared = readFile(extractDir + "/xl/sharedStrings.xml");
    if (!shared.empty()) {
      combined << stripTags(shared) << "\n\n";
    }
    // Then each sheet
    for (int sheetNum = 1; sheetNum < 50; ++sheetNum) {
      std::string sheetPath = extractDir + "/xl/worksheets/sheet" +
                              std::to_string(sheetNum) + ".xml";
      std::string xml = readFile(sheetPath);
      if (xml.empty())
        break;
      combined << "--- Sheet " << sheetNum << " ---\n";
      combined << stripTags(xml) << "\n\n";
    }
    textContent = combined.str();
    if (textContent.empty())
      extractionError = "Could not extract text from .xlsx file.";
    std::remove(srcPath.c_str());
    safeRmDir(extractDir);
  }
  // ── XLS (legacy Excel) → try textutil, fallback to strings ──────
  else if (ext == "xls") {
    std::string srcPath = writeTempFile("xls");
    std::string dstPath = tmpBase + ".txt";
    // textutil doesn't handle xls — use strings with safe redirect:
    // fork, redirect child stdout to dstPath, exec strings.
    {
      pid_t p = fork();
      if (p == 0) {
        FILE *out = fopen(dstPath.c_str(), "w");
        if (out) {
          dup2(fileno(out), STDOUT_FILENO);
          fclose(out);
        } else {
          // fopen failed — silence stdout so it doesn't leak to parent
          int devNull2 = open("/dev/null", O_WRONLY);
          if (devNull2 >= 0) {
            dup2(devNull2, STDOUT_FILENO);
            close(devNull2);
          }
        }
        int devNull = open("/dev/null", O_WRONLY);
        if (devNull >= 0) {
          dup2(devNull, STDERR_FILENO);
          close(devNull);
        }
        execlp("strings", "strings", srcPath.c_str(), nullptr);
        _exit(127);
      } else if (p > 0) {
        int st = 0;
        waitpid(p, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
          textContent = readFile(dstPath);
      }
    }
    if (textContent.empty())
      extractionError = "Could not extract text from .xls file. "
                        "Try saving as .xlsx or .csv.";
    std::remove(srcPath.c_str());
    std::remove(dstPath.c_str());
  }
  // ── HTML / HTM / XML / SVG → strip tags in-process ──────────────
  else if (ext == "html" || ext == "htm" || ext == "xml" || ext == "svg") {
    textContent = stripTags(fileContent);
  }
  // ── Plain text formats → use as-is ──────────────────────────────
  else if (ext == "txt" || ext == "md" || ext == "csv" || ext == "tsv" ||
           ext == "json" || ext == "jsonl" || ext == "yaml" || ext == "yml" ||
           ext == "log" || ext == "ini" || ext == "cfg" || ext == "conf" ||
           ext == "toml" || ext == "tex" || ext == "bib" || ext == "sql" ||
           ext == "sh" || ext == "py" || ext == "js" || ext == "ts" ||
           ext == "c" || ext == "cc" || ext == "cpp" || ext == "h" ||
           ext == "hpp" || ext == "java" || ext == "rs" || ext == "go" ||
           ext == "rb" || ext == "swift" || ext == "kt" || ext == "r" ||
           ext == "lua" || ext == "pl" || ext == "php" || ext == "cs" ||
           ext == "css" || ext == "scss" || ext == "less" || ext == "jsx" ||
           ext == "tsx" || ext == "vue" || ext == "makefile" ||
           ext == "dockerfile" || ext.empty()) {
    textContent = fileContent;
  }
  // ── Unknown format → try textutil, then fall back to raw text ───
  else {
    std::string srcPath = writeTempFile(ext);
    std::string dstPath = tmpBase + ".txt";
    int ret = safeExec({"/usr/bin/textutil", "-convert", "txt", "-encoding",
                        "UTF-8", "-output", dstPath, srcPath});
    if (ret == 0) {
      std::string extracted = readFile(dstPath);
      if (!extracted.empty())
        textContent = extracted;
      else
        textContent = fileContent; // fallback to raw
    } else {
      textContent = fileContent; // fallback to raw
      LOG_WARN << "RAG upload: unrecognised format '." << ext
               << "' — treating as plain text";
    }
    std::remove(srcPath.c_str());
    std::remove(dstPath.c_str());
  }

  // If extraction explicitly failed, report it.
  if (!extractionError.empty()) {
    LOG_ERROR << "RAG upload: " << extractionError;
    Json::Value root;
    root["success"] = false;
    root["error"] = extractionError;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
    resp->setStatusCode(drogon::k500InternalServerError);
    (*cb)(resp);
    return;
  }

  // Collapse excessive whitespace.
  {
    std::string cleaned;
    cleaned.reserve(textContent.size());
    bool lastWasSpace = false;
    for (char c : textContent) {
      if (c == '\r')
        continue;
      bool isSpace = (c == ' ' || c == '\t');
      if (isSpace && lastWasSpace)
        continue;
      cleaned += c;
      lastWasSpace = isSpace;
    }
    textContent = std::move(cleaned);
  }

  // Final check — if text is too short, it's probably garbled.
  if (textContent.size() < 50) {
    Json::Value root;
    root["success"] = false;
    root["error"] = "Extracted text is too short (" +
                    std::to_string(textContent.size()) +
                    " chars). The file may be empty or in an "
                    "unsupported format.";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
    resp->setStatusCode(drogon::k400BadRequest);
    (*cb)(resp);
    return;
  }

  LOG_INFO << "RAG upload: extracted " << textContent.size()
           << " chars of text from ." << ext << " file";

  // Limit extracted text to 500KB.
  if (textContent.size() > 512000) {
    LOG_WARN << "RAG upload: truncating extracted text from "
             << textContent.size() << " to 512000 chars";
    textContent = textContent.substr(0, 512000);
  }

  // Generate a unique document source key (millis already computed above).
  std::string docSource = "upload://" + fileName + ":" + std::to_string(millis);

  LOG_INFO << "RAG upload: '" << fileName << "' (" << fileSize
           << " bytes) — indexing with model " << embModel;

  // ── Pre-flight check: verify the embedding model is available ────
  // Try embedding a tiny string — if the model isn't pulled this fails
  // fast and we can return a clear error before registering the doc.
  {
    std::string testErr;
    if (!RagService::checkEmbeddingModel(embModel, testErr)) {
      LOG_ERROR << "RAG upload: embedding model '" << embModel
                << "' is not available: " << testErr;
      Json::Value root;
      root["success"] = false;
      root["error"] = "Embedding model '" + embModel +
                      "' is not available. Please run: ollama pull " + embModel;
      auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
      resp->setStatusCode(drogon::k400BadRequest);
      (*cb)(resp);
      return;
    }
  }

  // Register the document immediately with chunks=-1 (means "indexing").
  std::string docId = "doc_" + std::to_string(millis);
  RagDocument doc;
  doc.id = docId;
  doc.name = fileName;
  doc.source = docSource;
  doc.sizeBytes = fileSize;
  doc.chunks = -1; // -1 = indexing in progress
  doc.uploadedAt = millis;
  RagService::instance().addDocument(doc);

  // Respond immediately — don't wait for embeddings.
  Json::Value root;
  root["success"] = true;
  root["document"] = Json::Value();
  root["document"]["id"] = docId;
  root["document"]["name"] = fileName;
  root["document"]["size"] = static_cast<Json::UInt64>(fileSize);
  root["document"]["chunks"] = -1; // indexing in progress
  root["document"]["uploadedAt"] = static_cast<Json::Int64>(millis);
  auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
  (*cb)(resp);

  // Index in the background — update the doc when done.
  RagService::instance().indexDocument(
      docSource, fileName, textContent, embModel,
      [docId, fileName](int chunksIndexed, const std::string &ragErr) {
        if (!ragErr.empty()) {
          LOG_ERROR << "RAG indexing failed for '" << fileName
                    << "': " << ragErr;
        }
        // Update the document's chunk count (from -1 to actual count).
        RagService::instance().updateDocumentChunks(docId, chunksIndexed);
        LOG_INFO << "RAG indexing done: '" << fileName << "' → "
                 << chunksIndexed << " chunks";
      });
}

// ═══════════════════════════════════════════════════════════════════════════
// GET /api/rag/documents — list uploaded RAG documents
// ═══════════════════════════════════════════════════════════════════════════

void RagDocumentsController::asyncHandleHttpRequest(
    const drogon::HttpRequestPtr & /*req*/,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
  auto docs = RagService::instance().listDocuments();
  Json::Value root;
  Json::Value arr(Json::arrayValue);
  for (auto &d : docs) {
    Json::Value obj;
    obj["id"] = d.id;
    obj["name"] = d.name;
    obj["size"] = static_cast<Json::UInt64>(d.sizeBytes);
    obj["chunks"] = d.chunks;
    obj["uploadedAt"] = static_cast<Json::Int64>(d.uploadedAt);
    arr.append(obj);
  }
  root["documents"] = arr;
  root["totalChunks"] =
      static_cast<Json::UInt64>(RagService::instance().size());

  auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
  callback(resp);
}

// ═══════════════════════════════════════════════════════════════════════════
// POST /api/rag/documents/remove — remove a RAG document
// ═══════════════════════════════════════════════════════════════════════════

void RagRemoveDocController::asyncHandleHttpRequest(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
  auto body = req->getJsonObject();
  Json::Value root;

  if (!body) {
    root["success"] = false;
    root["error"] = "Invalid JSON body";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
    resp->setStatusCode(drogon::k400BadRequest);
    callback(resp);
    return;
  }

  std::string docId = (*body).get("id", "").asString();
  if (docId.empty()) {
    root["success"] = false;
    root["error"] = "Document ID is required";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
    resp->setStatusCode(drogon::k400BadRequest);
    callback(resp);
    return;
  }

  bool ok = RagService::instance().removeDocument(docId);
  root["success"] = ok;
  if (!ok) {
    root["error"] = "Document '" + docId + "' not found";
  }
  auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
  callback(resp);
}

// ═══════════════════════════════════════════════════════════════════════════
// POST /api/rag/clear — clear all RAG documents and chunks
// ═══════════════════════════════════════════════════════════════════════════

void RagClearController::asyncHandleHttpRequest(
    const drogon::HttpRequestPtr & /*req*/,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
  RagService::instance().clear();
  Json::Value root;
  root["success"] = true;
  root["message"] = "All RAG documents and chunks cleared";
  auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
  callback(resp);
}

// ═══════════════════════════════════════════════════════════════════════════
// POST /api/memory/extract — extract personal facts from a conversation turn
// ═══════════════════════════════════════════════════════════════════════════

void MemoryExtractController::asyncHandleHttpRequest(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
  auto body = req->getJsonObject();
  Json::Value root;

  if (!body) {
    root["facts"] = Json::arrayValue;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
    callback(resp);
    return;
  }

  std::string userMsg = (*body).get("userMessage", "").asString();
  std::string assistMsg = (*body).get("assistantMessage", "").asString();
  std::string model = (*body).get("model", "").asString();

  if (userMsg.empty() || model.empty()) {
    root["facts"] = Json::arrayValue;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
    callback(resp);
    return;
  }

  // Build a lightweight extraction prompt.
  std::vector<Message> msgs;
  msgs.push_back(
      {"system",
       "You are a concise fact extractor. Your ONLY job is to identify "
       "personal facts the user revealed about themselves in the conversation "
       "below. Output ONLY a JSON array of short fact strings. If no personal "
       "facts were revealed, output an empty array [].\n\n"
       "Examples of personal facts:\n"
       "- \"User's name is Alex\"\n"
       "- \"User is a Python developer\"\n"
       "- \"User lives in San Francisco\"\n"
       "- \"User prefers dark mode\"\n"
       "- \"User is learning Rust\"\n\n"
       "Do NOT include:\n"
       "- Questions the user asked\n"
       "- General topics discussed\n"
       "- Facts about the world (only facts about the USER)\n\n"
       "Output ONLY a valid JSON array. No explanation, no markdown.",
       ""});

  std::string prompt = "User said: \"" + userMsg + "\"";
  if (!assistMsg.empty()) {
    // Truncate assistant message to keep prompt small
    std::string truncated =
        assistMsg.size() > 500 ? assistMsg.substr(0, 500) + "…" : assistMsg;
    prompt += "\n\nAssistant replied: \"" + truncated + "\"";
  }
  prompt += "\n\nExtract personal facts about the user as a JSON array:";
  msgs.push_back({"user", prompt, ""});

  auto cb =
      std::make_shared<std::function<void(const drogon::HttpResponsePtr &)>>(
          std::move(callback));

  OllamaService::chat(
      model, msgs, 0.1, [cb](const std::string &reply, const std::string &err) {
        Json::Value root;
        root["facts"] = Json::arrayValue;

        if (!err.empty() || reply.empty()) {
          auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
          (*cb)(resp);
          return;
        }

        // Parse the JSON array from the reply.
        // The LLM may wrap it in markdown code fences — strip them.
        std::string cleaned = reply;
        // Remove ```json ... ``` or ``` ... ```
        auto stripFences = [](std::string &s) {
          size_t start = s.find("```");
          if (start != std::string::npos) {
            size_t lineEnd = s.find('\n', start);
            if (lineEnd != std::string::npos)
              s = s.substr(lineEnd + 1);
          }
          size_t end = s.rfind("```");
          if (end != std::string::npos)
            s = s.substr(0, end);
        };
        stripFences(cleaned);

        // Trim whitespace
        auto trim = [](std::string &s) {
          size_t a = s.find_first_not_of(" \t\n\r");
          size_t b = s.find_last_not_of(" \t\n\r");
          if (a == std::string::npos) {
            s.clear();
            return;
          }
          s = s.substr(a, b - a + 1);
        };
        trim(cleaned);

        Json::CharReaderBuilder rb;
        Json::Value parsed;
        std::string errs;
        std::istringstream ss(cleaned);
        if (Json::parseFromStream(rb, ss, &parsed, &errs) && parsed.isArray()) {
          for (const auto &item : parsed) {
            if (item.isString() && !item.asString().empty()) {
              root["facts"].append(item.asString());
            }
          }
        }

        auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
        (*cb)(resp);
      });
}

// ═══════════════════════════════════════════════════════════════════════════
// POST /api/models/pull — pull (download) a model from Ollama registry
// ═══════════════════════════════════════════════════════════════════════════

void ModelPullController::asyncHandleHttpRequest(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
  auto cb =
      std::make_shared<std::function<void(const drogon::HttpResponsePtr &)>>(
          std::move(callback));

  Json::CharReaderBuilder rb;
  Json::Value body;
  std::string errs;
  std::istringstream ss(std::string(req->body()));
  if (!Json::parseFromStream(rb, ss, &body, &errs) || !body.isMember("name") ||
      body["name"].asString().empty()) {
    Json::Value err;
    err["error"] = "Missing 'name' field";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
    resp->setStatusCode(drogon::k400BadRequest);
    (*cb)(resp);
    return;
  }

  std::string modelName = body["name"].asString();

  Json::Value pullBody;
  pullBody["name"] = modelName;
  pullBody["stream"] = true;
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  std::string payload = Json::writeString(wb, pullBody);

  auto resp = drogon::HttpResponse::newAsyncStreamResponse(
      [payload = std::move(payload)](drogon::ResponseStreamPtr stream) {
        // Move the unique_ptr into a shared_ptr so it can be shared
        // with the PullCtx struct inside the thread.
        auto sharedStream =
            std::shared_ptr<drogon::ResponseStream>(std::move(stream));
        std::thread([payload, sharedStream]() {
          CURL *curl = curl_easy_init();
          if (!curl) {
            sharedStream->send("{\"error\":\"curl init failed\"}\n");
            sharedStream->close();
            return;
          }

          struct PullCtx {
            std::string lineBuffer;
            std::shared_ptr<drogon::ResponseStream> stream;
          };
          PullCtx ctx;
          ctx.stream = sharedStream;

          auto writeCb = [](char *data, size_t size, size_t nmemb,
                            void *userp) -> size_t {
            auto *c = static_cast<PullCtx *>(userp);
            size_t total = size * nmemb;
            c->lineBuffer.append(data, total);
            size_t pos;
            while ((pos = c->lineBuffer.find('\n')) != std::string::npos) {
              std::string line = c->lineBuffer.substr(0, pos + 1);
              c->lineBuffer.erase(0, pos + 1);
              c->stream->send(line);
            }
            return total;
          };

          struct curl_slist *hdrs = nullptr;
          hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

          curl_easy_setopt(curl, CURLOPT_URL,
                           "http://127.0.0.1:11434/api/pull");
          curl_easy_setopt(curl, CURLOPT_POST, 1L);
          curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
          curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)payload.size());
          curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
          curl_easy_setopt(
              curl, CURLOPT_WRITEFUNCTION,
              static_cast<size_t (*)(char *, size_t, size_t, void *)>(writeCb));
          curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
          curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
          curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
          curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);

          CURLcode res = curl_easy_perform(curl);
          if (res != CURLE_OK) {
            Json::Value errJson;
            errJson["error"] = std::string(curl_easy_strerror(res));
            Json::StreamWriterBuilder wb;
            wb["indentation"] = "";
            ctx.stream->send(Json::writeString(wb, errJson) + "\n");
          }

          curl_slist_free_all(hdrs);
          curl_easy_cleanup(curl);
          ctx.stream->close();
        }).detach();
      });
  resp->setContentTypeString("application/x-ndjson");
  resp->addHeader("Cache-Control", "no-cache");
  (*cb)(resp);
}

// ═══════════════════════════════════════════════════════════════════════════
// POST /api/models/delete — delete an Ollama model
// ═══════════════════════════════════════════════════════════════════════════

void ModelDeleteController::asyncHandleHttpRequest(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
  auto cb =
      std::make_shared<std::function<void(const drogon::HttpResponsePtr &)>>(
          std::move(callback));

  Json::CharReaderBuilder rb;
  Json::Value body;
  std::string errs;
  std::istringstream ss(std::string(req->body()));
  if (!Json::parseFromStream(rb, ss, &body, &errs) || !body.isMember("name") ||
      body["name"].asString().empty()) {
    Json::Value err;
    err["error"] = "Missing 'name' field";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
    resp->setStatusCode(drogon::k400BadRequest);
    (*cb)(resp);
    return;
  }

  std::string modelName = body["name"].asString();

  std::thread([cb, modelName]() {
    try {
      CURL *curl = curl_easy_init();
      if (!curl) {
        Json::Value err;
        err["error"] = "curl init failed";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
        resp->setStatusCode(drogon::k500InternalServerError);
        drogon::app().getLoop()->queueInLoop([cb, resp]() { (*cb)(resp); });
        return;
      }

      Json::Value delBody;
      delBody["name"] = modelName;
      Json::StreamWriterBuilder wb;
      wb["indentation"] = "";
      std::string payload = Json::writeString(wb, delBody);

      std::string respBody;
      auto collectCb = [](char *data, size_t size, size_t nmemb,
                          void *userp) -> size_t {
        auto *buf = static_cast<std::string *>(userp);
        buf->append(data, size * nmemb);
        return size * nmemb;
      };

      struct curl_slist *hdrs = nullptr;
      hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

      curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:11434/api/delete");
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)payload.size());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
      curl_easy_setopt(
          curl, CURLOPT_WRITEFUNCTION,
          static_cast<size_t (*)(char *, size_t, size_t, void *)>(collectCb));
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &respBody);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

      CURLcode res = curl_easy_perform(curl);
      long httpCode = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
      curl_slist_free_all(hdrs);
      curl_easy_cleanup(curl);

      Json::Value root;
      if (res != CURLE_OK) {
        root["error"] = std::string("Curl error: ") + curl_easy_strerror(res);
      } else if (httpCode == 200) {
        root["success"] = true;
      } else {
        root["error"] = "Ollama returned HTTP " + std::to_string(httpCode);
        if (!respBody.empty())
          root["detail"] = respBody;
      }

      auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
      if (!root.isMember("success"))
        resp->setStatusCode(drogon::k500InternalServerError);
      drogon::app().getLoop()->queueInLoop([cb, resp]() { (*cb)(resp); });
    } catch (const std::exception &ex) {
      LOG_ERROR << "ModelDelete thread exception: " << ex.what();
      Json::Value err;
      err["error"] = std::string("Internal error: ") + ex.what();
      auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
      resp->setStatusCode(drogon::k500InternalServerError);
      drogon::app().getLoop()->queueInLoop([cb, resp]() { (*cb)(resp); });
    } catch (...) {
      LOG_ERROR << "ModelDelete thread: unknown exception";
      Json::Value err;
      err["error"] = "Unknown internal error";
      auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
      resp->setStatusCode(drogon::k500InternalServerError);
      drogon::app().getLoop()->queueInLoop([cb, resp]() { (*cb)(resp); });
    }
  }).detach();
}

// ═══════════════════════════════════════════════════════════════════════════
// GET /api/models/details — get detailed info for all models
// ═══════════════════════════════════════════════════════════════════════════

void ModelDetailsController::asyncHandleHttpRequest(
    const drogon::HttpRequestPtr & /*req*/,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
  auto cb =
      std::make_shared<std::function<void(const drogon::HttpResponsePtr &)>>(
          std::move(callback));

  std::thread([cb]() {
    try {
      CURL *c = curl_easy_init();
      if (!c) {
        Json::Value err;
        err["error"] = "curl init failed";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
        drogon::app().getLoop()->queueInLoop([cb, resp]() { (*cb)(resp); });
        return;
      }

      std::string body;
      auto collectCb = [](char *data, size_t size, size_t nmemb,
                          void *userp) -> size_t {
        auto *buf = static_cast<std::string *>(userp);
        buf->append(data, size * nmemb);
        return size * nmemb;
      };
      curl_easy_setopt(c, CURLOPT_URL, "http://127.0.0.1:11434/api/tags");
      curl_easy_setopt(
          c, CURLOPT_WRITEFUNCTION,
          static_cast<size_t (*)(char *, size_t, size_t, void *)>(collectCb));
      curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
      curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);
      curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
      CURLcode res = curl_easy_perform(c);
      curl_easy_cleanup(c);

      if (res != CURLE_OK) {
        Json::Value err;
        err["error"] =
            std::string("curl request failed: ") + curl_easy_strerror(res);
        auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
        drogon::app().getLoop()->queueInLoop([cb, resp]() { (*cb)(resp); });
        return;
      }

      Json::CharReaderBuilder rb;
      Json::Value tagsRoot;
      std::string errs;
      std::istringstream tss(body);
      if (!Json::parseFromStream(rb, tss, &tagsRoot, &errs) ||
          !tagsRoot.isMember("models")) {
        Json::Value err;
        err["error"] = "Failed to parse Ollama response";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
        drogon::app().getLoop()->queueInLoop([cb, resp]() { (*cb)(resp); });
        return;
      }

      Json::Value result(Json::arrayValue);
      for (auto &m : tagsRoot["models"]) {
        Json::Value info;
        info["name"] = m.get("name", "").asString();
        info["model"] = m.get("model", "").asString();
        info["size"] = m.get("size", 0).asInt64();
        info["digest"] = m.get("digest", "").asString();
        info["modified_at"] = m.get("modified_at", "").asString();
        if (m.isMember("details")) {
          auto &d = m["details"];
          info["parameter_size"] = d.get("parameter_size", "").asString();
          info["quantization_level"] =
              d.get("quantization_level", "").asString();
          info["family"] = d.get("family", "").asString();
          info["format"] = d.get("format", "").asString();
        }
        result.append(info);
      }

      Json::Value root;
      root["models"] = result;
      auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
      drogon::app().getLoop()->queueInLoop([cb, resp]() { (*cb)(resp); });
    } catch (const std::exception &ex) {
      LOG_ERROR << "ModelDetails thread exception: " << ex.what();
      Json::Value err;
      err["error"] = std::string("Internal error: ") + ex.what();
      auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
      resp->setStatusCode(drogon::k500InternalServerError);
      drogon::app().getLoop()->queueInLoop([cb, resp]() { (*cb)(resp); });
    } catch (...) {
      LOG_ERROR << "ModelDetails thread: unknown exception";
      Json::Value err;
      err["error"] = "Unknown internal error";
      auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
      resp->setStatusCode(drogon::k500InternalServerError);
      drogon::app().getLoop()->queueInLoop([cb, resp]() { (*cb)(resp); });
    }
  }).detach();
}

// ═══════════════════════════════════════════════════════════════════════════
// Disk-based key-value storage  (replaces browser localStorage)
// ═══════════════════════════════════════════════════════════════════════════
//
// GET  /api/storage?key=<name>        → { "value": "..." } or { "value": null }
// POST /api/storage  { "key": "...", "value": "..." }  → { "success": true }
//
// Files are stored under <resourceDir>/app_storage/<key>.json.
// Keys are restricted to [a-zA-Z0-9_-] to prevent path traversal.

static std::string getStorageDir() {
  // Store data in ~/Library/Application Support/Local Global/app_storage
  // (NOT inside the .app bundle, which must remain read-only)
  std::string base;
  const char *home = getenv("HOME");
  if (home) {
    base = std::string(home) + "/Library/Application Support/Local Global";
  } else {
    base = "/tmp/Local Global";
  }
  mkdir(base.c_str(), 0755);
  std::string storageDir = base + "/app_storage";
  mkdir(storageDir.c_str(), 0755);
  return storageDir;
}

static bool isValidStorageKey(const std::string &key) {
  if (key.empty() || key.size() > 128)
    return false;
  for (char c : key) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
      return false;
  }
  return true;
}

// GET /api/storage?key=<name>
void StorageGetController::asyncHandleHttpRequest(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
  auto key = req->getParameter("key");
  if (!isValidStorageKey(key)) {
    Json::Value err;
    err["error"] = "Invalid key (alphanumeric, _, - only; max 128 chars)";
    callback(drogon::HttpResponse::newHttpJsonResponse(err));
    return;
  }

  std::string path = getStorageDir() + "/" + key + ".json";
  std::ifstream in(path);
  Json::Value root;
  if (in.good()) {
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    root["value"] = content;
  } else {
    root["value"] = Json::nullValue;
  }
  callback(drogon::HttpResponse::newHttpJsonResponse(root));
}

// POST /api/storage  { "key": "...", "value": "..." }
void StorageSetController::asyncHandleHttpRequest(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
  auto jsonPtr = req->getJsonObject();
  if (!jsonPtr) {
    Json::Value err;
    err["error"] = "Invalid JSON body";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
    resp->setStatusCode(drogon::k400BadRequest);
    callback(resp);
    return;
  }

  auto &body = *jsonPtr;
  std::string key = body.get("key", "").asString();
  if (!isValidStorageKey(key)) {
    Json::Value err;
    err["error"] = "Invalid key (alphanumeric, _, - only; max 128 chars)";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
    resp->setStatusCode(drogon::k400BadRequest);
    callback(resp);
    return;
  }

  std::string value = body.get("value", "").asString();
  std::string dir = getStorageDir();
  std::string path = dir + "/" + key + ".json";

  // If value is empty or null, delete the file (equivalent of removeItem)
  if (value.empty() || body["value"].isNull()) {
    std::remove(path.c_str());
    Json::Value ok;
    ok["success"] = true;
    callback(drogon::HttpResponse::newHttpJsonResponse(ok));
    return;
  }

  std::ofstream out(path);
  if (!out) {
    Json::Value err;
    err["error"] = "Failed to write storage file";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
    resp->setStatusCode(drogon::k500InternalServerError);
    callback(resp);
    return;
  }
  out << value;
  out.close();

  Json::Value ok;
  ok["success"] = true;
  callback(drogon::HttpResponse::newHttpJsonResponse(ok));
}