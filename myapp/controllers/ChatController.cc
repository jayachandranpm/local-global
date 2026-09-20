#include "controllers/ChatController.h"
#include "models/Message.h"
#include "models/ToolCall.h"
#include "services/DuckDuckGoService.h"
#include "services/McpService.h"
#include "services/OllamaService.h"
#include "services/RagService.h"
#include "services/WebScraperService.h"

#include <atomic>
#include <drogon/drogon.h>
#include <json/json.h>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>

// ── Per-connection abort flag registry ──────────────────────────────────────
// Tracks the current streaming AbortFlag for each WebSocket connection so
// that a {"type":"stop"} message can cancel the active Ollama request.
static std::mutex abortMapMutex_;
static std::unordered_map<const void *, OllamaService::AbortFlag> activeAborts_;

static void registerAbort(const drogon::WebSocketConnectionPtr &conn,
                          OllamaService::AbortFlag flag) {
  std::lock_guard<std::mutex> lk(abortMapMutex_);
  // Cancel any existing stream before registering the new one
  auto it = activeAborts_.find(conn.get());
  if (it != activeAborts_.end() && it->second) {
    it->second->store(true);
  }
  activeAborts_[conn.get()] = std::move(flag);
}

static void clearAbort(const drogon::WebSocketConnectionPtr &conn) {
  std::lock_guard<std::mutex> lk(abortMapMutex_);
  activeAborts_.erase(conn.get());
}

static void triggerAbort(const drogon::WebSocketConnectionPtr &conn) {
  std::lock_guard<std::mutex> lk(abortMapMutex_);
  auto it = activeAborts_.find(conn.get());
  if (it != activeAborts_.end() && it->second) {
    it->second->store(true);
    activeAborts_.erase(it);
  }
}

// ─── System prompt injected into every conversation ─────────────────────────
static const char *SYSTEM_PROMPT =
    "You are a helpful assistant called Local Global. You have access to two "
    "tools:\n"
    "1. Web search: respond with JSON { \"tool\": \"search\", \"query\": "
    "\"your query\" } "
    "when you need to search the web.\n"
    "2. MCP tools: respond with JSON { \"tool\": \"mcp\", \"server\": "
    "\"name\", "
    "\"method\": \"tools/call\", \"params\": {...} } to use MCP tools.\n"
    "Always use tools when you need current information or need to interact "
    "with "
    "the local filesystem. After receiving tool results, synthesize them into "
    "a "
    "clear, helpful response.";

// System prompt used when RAG context is available — no tool instructions.
static const char *RAG_SYSTEM_PROMPT =
    "You are a helpful AI assistant.\n\n"
    "ABSOLUTE RULES — VIOLATION IS FORBIDDEN:\n"
    "• You have ZERO tools. No search tool. No MCP tool. Nothing.\n"
    "• NEVER output JSON. NEVER write { \"tool\": ... } or any JSON object.\n"
    "• NEVER say you will search. NEVER suggest searching.\n"
    "• If document passages are provided, answer from them and cite with [1], "
    "[2], etc.\n"
    "• If NO document passages are provided, answer from your general "
    "knowledge.\n"
    "• Write your answer in clear, well-formatted markdown.\n"
    "• Start your answer immediately — do NOT preamble with plans or tool "
    "usage.";

// ─── JSON helpers ───────────────────────────────────────────────────────────

static Json::Value makeJson(const std::string &type, const std::string &key,
                            const std::string &value) {
  Json::Value j;
  j["type"] = type;
  j[key] = value;
  return j;
}

static std::string toJsonString(const Json::Value &v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

/// Send a JSON message over a WebSocket connection (thread-safe).
static void wsSend(const drogon::WebSocketConnectionPtr &conn,
                   const Json::Value &msg) {
  conn->send(toJsonString(msg));
}

/// Build a thinking callback that forwards reasoning tokens to the client.
static OllamaService::ThinkingCallback
makeThinkingCallback(const drogon::WebSocketConnectionPtr &conn) {
  return [conn](const std::string &token) {
    wsSend(conn, makeJson("thinking", "content", token));
  };
}

/// Build a stats callback that sends token usage info to the client.
static OllamaService::StatsCallback
makeStatsCallback(const drogon::WebSocketConnectionPtr &conn) {
  return [conn](const OllamaService::TokenStats &stats) {
    if (stats.evalCount <= 0 && stats.promptEvalCount <= 0)
      return;
    Json::Value msg;
    msg["type"] = "stats";
    msg["eval_count"] = stats.evalCount;
    msg["prompt_eval_count"] = stats.promptEvalCount;
    msg["eval_duration"] = stats.evalDuration;
    msg["total_duration"] = stats.totalDuration;
    if (stats.evalDuration > 0) {
      msg["tokens_per_sec"] =
          static_cast<double>(stats.evalCount) / stats.evalDuration;
    }
    wsSend(conn, msg);
  };
}

// ─── Strip <think>…</think> tags ────────────────────────────────────────────
// Thinking models (qwen3, etc.) may embed <think> tags in the content
// stream.  The reasoning inside often mentions tools / JSON, which would
// confuse tryParseToolCall.  Strip those blocks before inspection.

static std::string stripThinkTags(const std::string &text) {
  std::string result;
  result.reserve(text.size());
  size_t pos = 0;
  while (pos < text.size()) {
    // Case-insensitive search for <think>
    size_t start = std::string::npos;
    for (size_t i = pos; i + 7 <= text.size(); ++i) {
      if ((text[i] == '<') && (text[i + 1] == 't' || text[i + 1] == 'T') &&
          (text[i + 2] == 'h' || text[i + 2] == 'H') &&
          (text[i + 3] == 'i' || text[i + 3] == 'I') &&
          (text[i + 4] == 'n' || text[i + 4] == 'N') &&
          (text[i + 5] == 'k' || text[i + 5] == 'K') && (text[i + 6] == '>')) {
        start = i;
        break;
      }
    }
    if (start == std::string::npos) {
      result.append(text, pos, text.size() - pos);
      break;
    }
    // Append text before the tag.
    result.append(text, pos, start - pos);
    // Find matching </think>
    size_t end = std::string::npos;
    for (size_t i = start + 7; i + 8 <= text.size(); ++i) {
      if ((text[i] == '<') && (text[i + 1] == '/') &&
          (text[i + 2] == 't' || text[i + 2] == 'T') &&
          (text[i + 3] == 'h' || text[i + 3] == 'H') &&
          (text[i + 4] == 'i' || text[i + 4] == 'I') &&
          (text[i + 5] == 'n' || text[i + 5] == 'N') &&
          (text[i + 6] == 'k' || text[i + 6] == 'K') && (text[i + 7] == '>')) {
        end = i + 8;
        break;
      }
    }
    if (end == std::string::npos) {
      // Unclosed <think> — skip the tag itself, keep the rest.
      pos = start + 7;
    } else {
      pos = end;
    }
  }
  return result;
}

// ─── Tool-call detection ────────────────────────────────────────────────────
// We look for a JSON object in the assistant's reply that matches the
// expected tool-call schema.  The LLM may embed it within prose, so we
// search for the first '{' … '}' that parses as a valid tool call.

static bool tryParseToolCall(const std::string &text, ToolCall &tc) {
  // Walk through the text looking for JSON objects.
  size_t pos = 0;
  while (pos < text.size()) {
    size_t start = text.find('{', pos);
    if (start == std::string::npos)
      break;

    // Find a matching closing brace (handle nesting).
    int depth = 0;
    size_t end = start;
    for (; end < text.size(); ++end) {
      if (text[end] == '{')
        ++depth;
      else if (text[end] == '}') {
        --depth;
        if (depth == 0)
          break;
      }
    }
    if (depth != 0) {
      pos = start + 1;
      continue;
    }

    std::string candidate = text.substr(start, end - start + 1);
    Json::CharReaderBuilder rb;
    Json::Value val;
    std::string errs;
    std::istringstream ss(candidate);
    if (Json::parseFromStream(rb, ss, &val, &errs) && val.isMember("tool")) {
      tc.tool = val["tool"].asString();
      tc.query = val.get("query", "").asString();
      tc.server = val.get("server", "").asString();
      tc.method = val.get("method", "").asString();
      tc.params = val.get("params", Json::objectValue);
      return true;
    }
    pos = start + 1;
  }
  return false;
}

// ─── Build Ollama messages vector from JSON history ─────────────────────────
static std::vector<Message>
buildMessages(const Json::Value &history, const std::string &userMsg,
              const std::string &memory,
              const std::string &customSystemPrompt = "") {
  std::vector<Message> msgs;
  // Always start with the system prompt + memory.
  std::string sysPrompt;
  if (!customSystemPrompt.empty()) {
    sysPrompt =
        customSystemPrompt + "\n\n" + std::string(SYSTEM_PROMPT) + memory;
  } else {
    sysPrompt = std::string(SYSTEM_PROMPT) + memory;
  }
  msgs.push_back({"system", sysPrompt, ""});

  // Append existing conversation history.
  if (history.isArray()) {
    for (const auto &h : history) {
      Message m;
      m.role = h.get("role", "user").asString();
      m.content = h.get("content", "").asString();
      msgs.push_back(m);
    }
  }
  // Append the latest user message.
  msgs.push_back({"user", userMsg, ""});
  return msgs;
}

// ─── Build and send an augmented response (search + scraping + RAG) ─────────
// Combines search results, scraped page content, and RAG-retrieved chunks
// into a rich context for the LLM.
static void buildAndSendAugmentedResponse(
    const drogon::WebSocketConnectionPtr &conn, const std::string &model,
    const Json::Value &history, const std::string &userMsg, double temperature,
    const std::string &memory, const std::vector<SearchResult> &searchResults,
    const std::vector<ScrapedPage> &scrapedPages,
    const std::vector<RagChunk> &ragChunks,
    const std::string &customSystemPrompt = "") {
  std::vector<Message> msgs;
  // Use RAG-aware prompt when we have RAG chunks, otherwise standard prompt.
  std::string basePrompt = (!ragChunks.empty() ? std::string(RAG_SYSTEM_PROMPT)
                                               : std::string(SYSTEM_PROMPT));
  // Prepend custom system prompt if provided.
  std::string sysPrompt;
  if (!customSystemPrompt.empty()) {
    sysPrompt = customSystemPrompt + "\n\n" + basePrompt + memory;
  } else {
    sysPrompt = basePrompt + memory;
  }
  msgs.push_back({"system", sysPrompt, ""});

  // Append conversation history.
  if (history.isArray()) {
    for (const auto &h : history) {
      msgs.push_back({h.get("role", "user").asString(),
                      h.get("content", "").asString(), ""});
    }
  }

  // Build a rich context message combining all sources.
  std::ostringstream combined;
  combined << userMsg << "\n\n";

  // Section 1: Search results overview
  combined << "═══ Web Search Results ═══\n\n";
  int idx = 1;
  for (auto &r : searchResults) {
    combined << idx++ << ". " << r.title << "\n"
             << "   URL: " << r.url << "\n"
             << "   " << r.snippet << "\n\n";
  }

  // Section 2: Scraped page content (full text from visited websites)
  bool hasScrapedContent = false;
  for (auto &p : scrapedPages) {
    if (p.success && !p.content.empty()) {
      hasScrapedContent = true;
      break;
    }
  }

  if (hasScrapedContent) {
    combined << "═══ Scraped Website Content ═══\n\n";
    for (auto &p : scrapedPages) {
      if (p.success && !p.content.empty()) {
        combined << "── " << (p.title.empty() ? p.url : p.title) << " ──\n"
                 << "Source: " << p.url << "\n\n"
                 << p.content << "\n\n";
      }
    }
  }

  // Section 3: RAG-retrieved relevant passages
  if (!ragChunks.empty()) {
    // Send RAG sources to the client for citation display.
    Json::Value srcMsg;
    srcMsg["type"] = "rag_sources";
    Json::Value sources(Json::arrayValue);
    int si = 1;
    for (auto &chunk : ragChunks) {
      Json::Value src;
      src["index"] = si++;
      src["title"] = chunk.title.empty() ? chunk.source : chunk.title;
      src["source"] = chunk.source;
      src["score"] = static_cast<int>(chunk.score * 100);
      if (chunk.text.size() > 300)
        src["text"] = chunk.text.substr(0, 300) + "…";
      else
        src["text"] = chunk.text;
      src["fullText"] = chunk.text;
      sources.append(src);
    }
    srcMsg["sources"] = sources;
    wsSend(conn, srcMsg);

    combined << "═══ Most Relevant Passages (RAG) ═══\n\n";
    int ci = 1;
    for (auto &chunk : ragChunks) {
      combined << "[Passage " << ci++ << " — from "
               << (chunk.title.empty() ? chunk.source : chunk.title)
               << " (relevance: " << static_cast<int>(chunk.score * 100)
               << "%)]\n"
               << chunk.text << "\n\n";
    }
  }

  combined
      << "═══════════════════════════\n\n"
      << "Please use ALL the above information — search results, "
         "scraped website content, and relevant passages — to provide "
         "a comprehensive, accurate, and well-cited answer. "
         "Cite passage numbers [1], [2] etc. and source URLs when possible.";

  msgs.push_back({"user", combined.str(), ""});

  // Stream the response.  Pre-register the abort flag so disconnect
  // can cancel even before the curl thread starts.
  auto af = std::make_shared<std::atomic<bool>>(false);
  registerAbort(conn, af);
  OllamaService::streamChat(
      model, msgs, temperature,
      [conn](const std::string &token) {
        wsSend(conn, makeJson("token", "content", token));
      },
      [conn](const std::string &e) {
        clearAbort(conn);
        if (!e.empty())
          wsSend(conn, makeJson("error", "message", e));
        else
          wsSend(conn, makeJson("done", "content", ""));
      },
      makeStatsCallback(conn), af, makeThinkingCallback(conn));
}

// ═══════════════════════════════════════════════════════════════════════════
// IndexController — GET /
// ═══════════════════════════════════════════════════════════════════════════

void IndexController::asyncHandleHttpRequest(
    const drogon::HttpRequestPtr & /*req*/,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
  // Serve index.html from Drogon's document root (set in main.cc).
  auto docRoot = drogon::app().getDocumentRoot();
  auto resp = drogon::HttpResponse::newFileResponse(docRoot + "/index.html");
  resp->setContentTypeCode(drogon::CT_TEXT_HTML);
  callback(resp);
}

// ═══════════════════════════════════════════════════════════════════════════
// ChatWebSocket — /ws/chat
// ═══════════════════════════════════════════════════════════════════════════

void ChatWebSocket::handleNewConnection(
    const drogon::HttpRequestPtr & /*req*/,
    const drogon::WebSocketConnectionPtr &conn) {
  LOG_INFO << "WebSocket connected: " << conn->peerAddr().toIpPort();
}

void ChatWebSocket::handleConnectionClosed(
    const drogon::WebSocketConnectionPtr &conn) {
  triggerAbort(conn);
  LOG_INFO << "WebSocket disconnected: " << conn->peerAddr().toIpPort();
}

// ─── Main message handler ───────────────────────────────────────────────────
void ChatWebSocket::handleNewMessage(const drogon::WebSocketConnectionPtr &conn,
                                     std::string &&message,
                                     const drogon::WebSocketMessageType &type) {
  if (type != drogon::WebSocketMessageType::Text)
    return;

  // Parse the incoming JSON payload.
  Json::CharReaderBuilder rb;
  Json::Value payload;
  std::string errs;
  std::istringstream ss(message);
  if (!Json::parseFromStream(rb, ss, &payload, &errs)) {
    wsSend(conn, makeJson("error", "message", "Invalid JSON: " + errs));
    return;
  }

  // ── Handle stop request ───────────────────────────────────────────────
  std::string msgType = payload.get("type", "").asString();
  if (msgType == "stop") {
    // Check if there's an active stream to abort.
    bool hadActive = false;
    {
      std::lock_guard<std::mutex> lk(abortMapMutex_);
      auto it = activeAborts_.find(conn.get());
      if (it != activeAborts_.end() && it->second) {
        it->second->store(true);
        activeAborts_.erase(it);
        hadActive = true;
      }
    }
    if (!hadActive) {
      // No active stream — send "done" directly so the client
      // doesn't hang waiting for a response.
      wsSend(conn, makeJson("done", "content", ""));
    }
    // When hadActive is true, the OllamaService onDone callback will
    // fire and send the "done" message — no need to send it twice.
    return;
  }

  std::string userMsg = payload.get("message", "").asString();
  std::string model = payload.get("model", "").asString();
  double temperature = payload.get("temperature", 0.7).asDouble();
  Json::Value history = payload.get("history", Json::arrayValue);
  bool webSearch = payload.get("webSearch", false).asBool();
  std::string memory = payload.get("memory", "").asString();
  std::string customSystemPrompt = payload.get("systemPrompt", "").asString();

  if (userMsg.empty()) {
    wsSend(conn, makeJson("error", "message", "Empty message."));
    return;
  }
  if (model.empty()) {
    wsSend(conn, makeJson("error", "message",
                          "No model selected. Please choose a model."));
    return;
  }

  // Helper: label for status messages
  auto engineLabel = [&]() -> std::string { return "DuckDuckGo"; };

  // ── Web search flow (with scraping + RAG) ─────────────────────────────
  // When the user has toggled web search on, we:
  //   1. Search using the chosen engine for results
  //   2. Scrape the top URLs to get actual page content
  //   3. Index scraped content into the RAG vector store
  //   4. Retrieve the most relevant chunks for the user's query
  //   5. Pass everything to the LLM for a well-informed answer
  if (webSearch) {
    // Step 1: Tell the client we're searching.
    {
      Json::Value s;
      s["type"] = "tool_status";
      s["step"] = "searching";
      s["message"] = "Searching " + engineLabel() + " for \"" + userMsg + "\"…";
      wsSend(conn, s);
    }

    // Dispatch to the chosen search engine.
    auto searchCb = [conn, model, history, userMsg, temperature, memory,
                     customSystemPrompt](std::vector<SearchResult> results,
                                         const std::string &searchErr) {
      if (!searchErr.empty() || results.empty()) {
        // Search failed — fall back to knowledge
        {
          Json::Value s;
          s["type"] = "tool_status";
          s["step"] = "results";
          s["message"] = searchErr.empty()
                             ? "No results found — answering from knowledge"
                             : "Search failed — answering from knowledge";
          wsSend(conn, s);
        }

        std::vector<Message> msgs;
        std::string sysPrompt =
            (!customSystemPrompt.empty() ? customSystemPrompt + "\n\n" : "") +
            std::string(SYSTEM_PROMPT) + memory;
        msgs.push_back({"system", sysPrompt, ""});
        if (history.isArray()) {
          for (const auto &h : history) {
            msgs.push_back({h.get("role", "user").asString(),
                            h.get("content", "").asString(), ""});
          }
        }
        msgs.push_back(
            {"user",
             userMsg + "\n\n[Web search was attempted but returned no results. "
                       "Please answer based on your knowledge.]",
             ""});

        {
          Json::Value s;
          s["type"] = "tool_status";
          s["step"] = "generating";
          s["message"] = "Generating answer…";
          wsSend(conn, s);
        }

        auto af = std::make_shared<std::atomic<bool>>(false);
        registerAbort(conn, af);
        OllamaService::streamChat(
            model, msgs, temperature,
            [conn](const std::string &token) {
              wsSend(conn, makeJson("token", "content", token));
            },
            [conn](const std::string &e) {
              clearAbort(conn);
              if (!e.empty())
                wsSend(conn, makeJson("error", "message", e));
              else
                wsSend(conn, makeJson("done", "content", ""));
            },
            makeStatsCallback(conn), af, makeThinkingCallback(conn));
        return;
      }

      // Step 2: Report search results found, begin scraping.
      {
        Json::Value s;
        s["type"] = "tool_status";
        s["step"] = "results";
        s["message"] = "Found " + std::to_string(results.size()) +
                       " results — scraping pages…";
        wsSend(conn, s);
      }

      // Collect top 3 URLs to scrape for full content.
      std::vector<std::string> urlsToScrape;
      int scrapeLimit = std::min(static_cast<int>(results.size()), 3);
      for (int i = 0; i < scrapeLimit; ++i) {
        urlsToScrape.push_back(results[i].url);
      }

      // Capture results for later use.
      auto searchResults =
          std::make_shared<std::vector<SearchResult>>(std::move(results));

      WebScraperService::scrapeUrls(
          urlsToScrape,
          [conn, model, history, userMsg, temperature, memory, searchResults,
           customSystemPrompt](std::vector<ScrapedPage> pages) {
            // Step 3: Scraping done — index into RAG.
            int scraped = 0;
            for (auto &p : pages) {
              if (p.success)
                scraped++;
            }

            {
              Json::Value s;
              s["type"] = "tool_status";
              s["step"] = "scraping_done";
              s["message"] = "Scraped " + std::to_string(scraped) + "/" +
                             std::to_string(pages.size()) +
                             " pages — analysing content…";
              wsSend(conn, s);
            }

            // Prepare documents for RAG indexing.
            std::vector<std::tuple<std::string, std::string, std::string>> docs;
            for (auto &p : pages) {
              if (p.success && !p.content.empty()) {
                docs.push_back({p.url, p.title, p.content});
              }
            }

            // Determine the embedding model — use the same model or
            // a dedicated embedding model.
            // Must use the SAME embedding model that was used for indexing.
            std::string embModel = "nomic-embed-text:latest";

            // If we have content to index, do RAG; otherwise skip.
            if (!docs.empty()) {
              RagService::instance().indexAndRetrieveEphemeral(
                  docs, userMsg, embModel, 5,
                  [conn, model, history, userMsg, temperature, memory,
                   searchResults, pages, customSystemPrompt](
                      int chunksIndexed, std::vector<RagChunk> ragChunks,
                      const std::string &ragErr) {
                    // Step 4: Ephemeral RAG done — generate.
                    {
                      Json::Value s;
                      s["type"] = "tool_status";
                      s["step"] = "generating";
                      s["message"] = "Generating answer with " +
                                     std::to_string(ragChunks.size()) +
                                     " relevant passages…";
                      wsSend(conn, s);
                    }

                    buildAndSendAugmentedResponse(
                        conn, model, history, userMsg, temperature, memory,
                        *searchResults, pages, ragChunks, customSystemPrompt);
                  });
            } else {
              // No content scraped — use search snippets only
              {
                Json::Value s;
                s["type"] = "tool_status";
                s["step"] = "generating";
                s["message"] = "Generating answer from search snippets…";
                wsSend(conn, s);
              }

              buildAndSendAugmentedResponse(conn, model, history, userMsg,
                                            temperature, memory, *searchResults,
                                            pages, {}, customSystemPrompt);
            }
          },
          6000); // max 6000 chars per scraped page
    };

    // Dispatch search to DuckDuckGo.
    DuckDuckGoService::search(userMsg, searchCb);
    return; // Don't fall through to the normal chat path.
  }

  // ── Normal (non-search) flow ───────────────────────────────────────────
  //
  // If RAG documents are uploaded, retrieve relevant chunks and inject
  // them into the conversation before calling the LLM.
  bool ragEnabled = payload.get("ragEnabled", true).asBool();

  if (ragEnabled && RagService::instance().hasDocuments()) {
    // Must use the SAME embedding model that was used for indexing.
    std::string embModel = "nomic-embed-text:latest";

    // Retrieve silently — only show the RAG UI if we actually find
    // relevant passages.  This avoids the "Document RAG" progress
    // bar when the query has nothing to do with uploaded docs.
    RagService::instance().retrieve(
        userMsg, embModel, 5,
        [conn, model, history, userMsg, temperature, memory, customSystemPrompt,
         payload](std::vector<RagChunk> ragChunks, const std::string &ragErr) {
          if (!ragErr.empty()) {
            LOG_WARN << "RAG retrieval error: " << ragErr;
          }

          // Filter out very low-relevance chunks (< 30%).
          // If nothing passes, skip RAG entirely and answer normally.
          ragChunks.erase(
              std::remove_if(ragChunks.begin(), ragChunks.end(),
                             [](const RagChunk &c) { return c.score < 0.30; }),
              ragChunks.end());

          if (ragChunks.empty()) {
            // ── No relevant passages — fall through to normal chat ──
            LOG_INFO << "RAG: no relevant passages found, skipping RAG";
            auto messages =
                buildMessages(history, userMsg, memory, customSystemPrompt);
            auto accumulator = std::make_shared<std::string>();
            auto deferredStats = std::make_shared<OllamaService::TokenStats>();
            auto captureStats =
                [deferredStats](const OllamaService::TokenStats &s) {
                  *deferredStats = s;
                };

            auto af = std::make_shared<std::atomic<bool>>(false);
            registerAbort(conn, af);
            OllamaService::streamChat(
                model, messages, temperature,
                [conn, accumulator](const std::string &token) {
                  accumulator->append(token);
                  wsSend(conn, makeJson("token", "content", token));
                },
                [conn, accumulator, model, messages, temperature,
                 deferredStats](const std::string &err) {
                  clearAbort(conn);
                  if (!err.empty()) {
                    wsSend(conn, makeJson("error", "message", err));
                    return;
                  }

                  std::string cleanedAccum = stripThinkTags(*accumulator);
                  ToolCall tc;
                  if (!tryParseToolCall(cleanedAccum, tc)) {
                    if (deferredStats->evalCount > 0 ||
                        deferredStats->promptEvalCount > 0) {
                      makeStatsCallback(conn)(*deferredStats);
                    }
                    wsSend(conn, makeJson("done", "content", ""));
                    return;
                  }

                  // Tool call detected — handle it.
                  LOG_INFO << "Tool call detected (RAG skip path): " << tc.tool;
                  wsSend(conn, makeJson("clear", "content", ""));

                  if (tc.tool == "search") {
                    Json::Value indicator;
                    indicator["type"] = "tool_status";
                    indicator["message"] =
                        "🔍 Searching for: " + tc.query + "…";
                    wsSend(conn, indicator);

                    DuckDuckGoService::search(
                        tc.query, [conn, model, messages, temperature,
                                   tc](std::vector<SearchResult> results,
                                       const std::string &searchErr) {
                          if (!searchErr.empty()) {
                            wsSend(conn,
                                   makeJson("error", "message",
                                            "Search failed: " + searchErr));
                            return;
                          }
                          auto extended = messages;
                          extended.push_back(
                              {"assistant",
                               "I'll search the web for: " + tc.query, ""});
                          std::ostringstream sc;
                          sc << "Here are the web search results for \""
                             << tc.query << "\":\n\n";
                          int idx2 = 1;
                          for (auto &r : results) {
                            sc << idx2++ << ". " << r.title << "\n"
                               << "   URL: " << r.url << "\n"
                               << "   " << r.snippet << "\n\n";
                          }
                          sc << "Please use these results to provide a "
                                "comprehensive answer.";
                          extended.push_back({"user", sc.str(), ""});

                          auto af2 = std::make_shared<std::atomic<bool>>(false);
                          registerAbort(conn, af2);
                          OllamaService::streamChat(
                              model, extended, temperature,
                              [conn](const std::string &token) {
                                wsSend(conn,
                                       makeJson("token", "content", token));
                              },
                              [conn](const std::string &e2) {
                                clearAbort(conn);
                                if (!e2.empty())
                                  wsSend(conn,
                                         makeJson("error", "message", e2));
                                else
                                  wsSend(conn, makeJson("done", "content", ""));
                              },
                              makeStatsCallback(conn), af2,
                              makeThinkingCallback(conn));
                        });
                  } else if (tc.tool == "mcp") {
                    Json::Value indicator;
                    indicator["type"] = "tool_status";
                    indicator["message"] =
                        "⚙️ Calling MCP tool on '" + tc.server + "'…";
                    wsSend(conn, indicator);
                    McpService::instance().callToolAsync(
                        tc.server, tc.method, tc.params,
                        [conn, model, messages, temperature,
                         tc](const Json::Value &result,
                             const std::string &mcpErr) {
                          if (!mcpErr.empty()) {
                            wsSend(conn,
                                   makeJson("error", "message",
                                            "MCP call failed: " + mcpErr));
                            return;
                          }
                          auto extended = messages;
                          extended.push_back(
                              {"assistant",
                               "I'll use the MCP tool on server '" + tc.server +
                                   "'.",
                               ""});
                          Json::StreamWriterBuilder wb;
                          wb["indentation"] = "";
                          extended.push_back(
                              {"user",
                               "[MCP tool result from '" + tc.server + "']\n" +
                                   Json::writeString(wb, result) +
                                   "\n[Please use the above result to answer "
                                   "the user's question.]",
                               ""});
                          auto af3 = std::make_shared<std::atomic<bool>>(false);
                          registerAbort(conn, af3);
                          OllamaService::streamChat(
                              model, extended, temperature,
                              [conn](const std::string &token) {
                                wsSend(conn,
                                       makeJson("token", "content", token));
                              },
                              [conn](const std::string &e2) {
                                clearAbort(conn);
                                if (!e2.empty())
                                  wsSend(conn,
                                         makeJson("error", "message", e2));
                                else
                                  wsSend(conn, makeJson("done", "content", ""));
                              },
                              makeStatsCallback(conn), af3,
                              makeThinkingCallback(conn));
                        });
                  } else {
                    wsSend(conn, makeJson("done", "content", ""));
                  }
                },
                captureStats, af, makeThinkingCallback(conn));
            return;
          }

          // ── Relevant passages found — proceed with RAG flow ──
          LOG_INFO << "RAG: " << ragChunks.size()
                   << " relevant passages found, using RAG context";

          // Show brief RAG status now that we know it's useful.
          {
            Json::Value s;
            s["type"] = "tool_status";
            s["step"] = "generating";
            s["message"] = "Generating answer with " +
                           std::to_string(ragChunks.size()) +
                           " relevant passages…";
            wsSend(conn, s);
          }

          std::vector<Message> msgs;
          std::string sysPrompt =
              (!customSystemPrompt.empty() ? customSystemPrompt + "\n\n" : "") +
              std::string(RAG_SYSTEM_PROMPT) + memory;
          msgs.push_back({"system", sysPrompt, ""});

          if (history.isArray()) {
            for (const auto &h : history) {
              std::string role = h.get("role", "user").asString();
              std::string content = h.get("content", "").asString();
              // In RAG mode, always skip assistant messages that
              // contain tool-call JSON — they confuse the model
              // into thinking it should also emit tool calls.
              if (role == "assistant" &&
                  content.find("\"tool\"") != std::string::npos &&
                  content.find("{") != std::string::npos) {
                continue;
              }
              msgs.push_back({role, content, ""});
            }
          }

          // Send RAG sources to client for citation display.
          if (!ragChunks.empty()) {
            Json::Value srcMsg;
            srcMsg["type"] = "rag_sources";
            Json::Value sources(Json::arrayValue);
            int ci = 1;
            for (auto &chunk : ragChunks) {
              Json::Value src;
              src["index"] = ci++;
              src["title"] = chunk.title;
              src["source"] = chunk.source;
              src["score"] = static_cast<int>(chunk.score * 100);
              // Send a preview of the chunk text (first 300 chars)
              if (chunk.text.size() > 300)
                src["text"] = chunk.text.substr(0, 300) + "…";
              else
                src["text"] = chunk.text;
              src["fullText"] = chunk.text;
              sources.append(src);
            }
            srcMsg["sources"] = sources;
            wsSend(conn, srcMsg);
          }

          // Build user message with RAG context prepended.
          // ragChunks is guaranteed non-empty here (empty case
          // was handled above by falling through to normal chat).
          std::ostringstream augmented;
          augmented << "═══ Relevant Document Context ═══\n\n";
          int ci2 = 1;
          for (auto &chunk : ragChunks) {
            augmented << "[Document Passage " << ci2++ << " — " << chunk.title
                      << " (relevance: " << static_cast<int>(chunk.score * 100)
                      << "%)]\n"
                      << chunk.text << "\n\n";
          }
          augmented << "═══════════════════════════\n\n"
                    << "Use the above document passages to ground your answer "
                       "when relevant. Cite the passage numbers [1], [2] etc. "
                       "and source document names when referencing information "
                       "from the passages.\n\n";
          augmented << userMsg;

          msgs.push_back({"user", augmented.str(), ""});

          // Stream the LLM response — detect tool calls and retry.
          auto msgsPtr =
              std::make_shared<std::vector<Message>>(std::move(msgs));
          auto ragAccum = std::make_shared<std::string>();

          // Capture stats from the first call — forward them only
          // if no tool-call retry is needed (the common path).
          auto deferredStats = std::make_shared<OllamaService::TokenStats>();
          auto captureStats =
              [deferredStats](const OllamaService::TokenStats &s) {
                *deferredStats = s;
              };

          auto af = std::make_shared<std::atomic<bool>>(false);
          registerAbort(conn, af);
          OllamaService::streamChat(
              model, *msgsPtr, temperature,
              [conn, ragAccum](const std::string &token) {
                ragAccum->append(token);
                wsSend(conn, makeJson("token", "content", token));
              },
              [conn, ragAccum, msgsPtr, model, temperature,
               deferredStats](const std::string &err) {
                clearAbort(conn);
                if (!err.empty()) {
                  wsSend(conn, makeJson("error", "message", err));
                  return;
                }

                // Check if the LLM emitted a tool call despite RAG mode.
                // Strip <think>…</think> tags so that reasoning
                // from thinking models isn't mistaken for tool JSON.
                std::string cleanedRag = stripThinkTags(*ragAccum);
                ToolCall tc;
                if (tryParseToolCall(cleanedRag, tc)) {
                  // Clear the streamed tool-call output on the client.
                  wsSend(conn, makeJson("clear", "content", ""));

                  // Re-prompt with the failed response + a firm nudge.
                  auto retryMsgs = *msgsPtr;
                  retryMsgs.push_back({"assistant", *ragAccum, ""});
                  retryMsgs.push_back(
                      {"user",
                       "STOP. Do NOT output JSON or tool calls. You have "
                       "NO tools available. Answer the question DIRECTLY. "
                       "If document passages were provided, use them. "
                       "Otherwise answer from your general knowledge. "
                       "Write your answer in plain markdown now.",
                       ""});

                  auto af2 = std::make_shared<std::atomic<bool>>(false);
                  registerAbort(conn, af2);
                  OllamaService::streamChat(
                      model, retryMsgs, temperature,
                      [conn](const std::string &token) {
                        wsSend(conn, makeJson("token", "content", token));
                      },
                      [conn](const std::string &e2) {
                        clearAbort(conn);
                        if (!e2.empty())
                          wsSend(conn, makeJson("error", "message", e2));
                        else
                          wsSend(conn, makeJson("done", "content", ""));
                      },
                      makeStatsCallback(conn), af2, nullptr, false);
                } else {
                  // No retry — forward the captured stats from this call.
                  if (deferredStats->evalCount > 0 ||
                      deferredStats->promptEvalCount > 0) {
                    makeStatsCallback(conn)(*deferredStats);
                  }
                  wsSend(conn, makeJson("done", "content", ""));
                }
              },
              captureStats, af, nullptr, false);
        });
    return;
  }

  auto messages = buildMessages(history, userMsg, memory, customSystemPrompt);

  // ── First Ollama call — may contain a tool call ────────────────────────
  // We collect the full response to inspect for tool calls, but also stream
  // tokens to the client in real time.
  auto accumulator = std::make_shared<std::string>();

  // Capture stats from first call — forward only if no tool-call retry
  // happens.  If a retry fires, let the retry send its own stats, and
  // discard the first (meaningless) ones from the tool-detection pass.
  auto deferredStats2 = std::make_shared<OllamaService::TokenStats>();
  auto captureStats2 = [deferredStats2](const OllamaService::TokenStats &s) {
    *deferredStats2 = s;
  };

  auto af = std::make_shared<std::atomic<bool>>(false);
  registerAbort(conn, af);
  OllamaService::streamChat(
      model, messages, temperature,
      /* onToken */
      [conn, accumulator](const std::string &token) {
        accumulator->append(token);
        wsSend(conn, makeJson("token", "content", token));
      },
      /* onDone */
      [conn, accumulator, model, messages, temperature,
       deferredStats2](const std::string &err) {
        clearAbort(conn);
        if (!err.empty()) {
          wsSend(conn, makeJson("error", "message", err));
          return;
        }

        // Check if the accumulated response contains a tool call.
        // Strip <think>…</think> tags so that reasoning from thinking
        // models isn't mistaken for tool-call JSON.
        std::string cleanedAccum = stripThinkTags(*accumulator);
        ToolCall tc;
        if (!tryParseToolCall(cleanedAccum, tc)) {
          // No tool call — forward the captured stats and finish.
          if (deferredStats2->evalCount > 0 ||
              deferredStats2->promptEvalCount > 0) {
            makeStatsCallback(conn)(*deferredStats2);
          }
          wsSend(conn, makeJson("done", "content", ""));
          return;
        }

        // ── Tool call detected ─────────────────────────────────────────
        LOG_INFO << "Tool call detected: " << tc.tool;

        // Clear the streamed tool-call output on the client so it
        // doesn't get concatenated with the real answer.
        wsSend(conn, makeJson("clear", "content", ""));

        if (tc.tool == "search") {
          // Notify the client that a search is in progress.
          Json::Value indicator;
          indicator["type"] = "tool_status";
          indicator["message"] = "🔍 Searching for: " + tc.query + "…";
          wsSend(conn, indicator);

          auto toolSearchCb =
              [conn, model, messages, temperature,
               tc](std::vector<SearchResult> results,
                   const std::string &searchErr) {
                if (!searchErr.empty()) {
                  wsSend(conn, makeJson("error", "message",
                                        "Search failed: " + searchErr));
                  return;
                }

                // Build an extended conversation with the tool output.
                auto extended = messages;
                // Insert assistant acknowledgment, then search results as user.
                extended.push_back(
                    {"assistant", "I'll search the web for: " + tc.query, ""});

                std::ostringstream searchContent;
                searchContent << "Here are the web search results for \""
                              << tc.query << "\":\n\n";
                int idx2 = 1;
                for (auto &r : results) {
                  searchContent << idx2++ << ". " << r.title << "\n"
                                << "   URL: " << r.url << "\n"
                                << "   " << r.snippet << "\n\n";
                }
                searchContent
                    << "Please use these results to provide "
                       "a comprehensive answer. Cite sources when relevant.";
                extended.push_back({"user", searchContent.str(), ""});

                // Second Ollama call — synthesize the final answer.
                auto af2 = std::make_shared<std::atomic<bool>>(false);
                registerAbort(conn, af2);
                OllamaService::streamChat(
                    model, extended, temperature,
                    [conn](const std::string &token) {
                      wsSend(conn, makeJson("token", "content", token));
                    },
                    [conn](const std::string &e2) {
                      clearAbort(conn);
                      if (!e2.empty())
                        wsSend(conn, makeJson("error", "message", e2));
                      else
                        wsSend(conn, makeJson("done", "content", ""));
                    },
                    makeStatsCallback(conn), af2, makeThinkingCallback(conn));
              };

          // Dispatch tool search to DuckDuckGo.
          DuckDuckGoService::search(tc.query, toolSearchCb);
        } else if (tc.tool == "mcp") {
          // Notify the client.
          Json::Value indicator;
          indicator["type"] = "tool_status";
          indicator["message"] = "⚙️ Calling MCP tool on '" + tc.server + "'…";
          wsSend(conn, indicator);

          McpService::instance().callToolAsync(
              tc.server, tc.method, tc.params,
              [conn, model, messages, temperature,
               tc](const Json::Value &result, const std::string &mcpErr) {
                if (!mcpErr.empty()) {
                  wsSend(conn, makeJson("error", "message",
                                        "MCP call failed: " + mcpErr));
                  return;
                }

                // Inject tool result and call Ollama again.
                auto extended = messages;
                extended.push_back(
                    {"assistant",
                     "I'll use the MCP tool on server '" + tc.server + "'.",
                     ""});

                Json::StreamWriterBuilder wb;
                wb["indentation"] = "";
                extended.push_back({"user",
                                    "[MCP tool result from '" + tc.server +
                                        "']\n" + Json::writeString(wb, result) +
                                        "\n[Please use the above result to "
                                        "answer the user's question.]",
                                    ""});

                auto af3 = std::make_shared<std::atomic<bool>>(false);
                registerAbort(conn, af3);
                OllamaService::streamChat(
                    model, extended, temperature,
                    [conn](const std::string &token) {
                      wsSend(conn, makeJson("token", "content", token));
                    },
                    [conn](const std::string &e2) {
                      clearAbort(conn);
                      if (!e2.empty())
                        wsSend(conn, makeJson("error", "message", e2));
                      else
                        wsSend(conn, makeJson("done", "content", ""));
                    },
                    makeStatsCallback(conn), af3, makeThinkingCallback(conn));
              });
        } else {
          // Unknown tool — just finalize.
          wsSend(conn, makeJson("done", "content", ""));
        }
      },
      captureStats2, af, makeThinkingCallback(conn));
}
