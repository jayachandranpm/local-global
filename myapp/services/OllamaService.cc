#include "services/OllamaService.h"
#include <atomic>
#include <curl/curl.h>
#include <drogon/HttpClient.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

// ─── Helper: convert our Message vector to the Ollama JSON format ───────────
static Json::Value messagesToJson(const std::vector<Message> &msgs) {
  Json::Value arr(Json::arrayValue);
  for (auto &m : msgs) {
    Json::Value obj;
    obj["role"] = m.role;
    obj["content"] = m.content;
    arr.append(obj);
  }
  return arr;
}

// ─── libcurl streaming context ──────────────────────────────────────────────
struct CurlStreamCtx {
  std::string lineBuffer; // partial line accumulator
  OllamaService::TokenCallback onToken;
  OllamaService::StatsCallback onStats;
  OllamaService::ThinkingCallback onThinking;
  trantor::EventLoop *loop;                     // Drogon's event loop
  std::shared_ptr<std::atomic<bool>> abortFlag; // set to true to cancel
  OllamaService::TokenStats stats;              // accumulated from final chunk
};

// libcurl write callback — called repeatedly as data arrives in real time.
static size_t curlWriteCb(char *data, size_t size, size_t nmemb, void *userp) {
  auto *ctx = static_cast<CurlStreamCtx *>(userp);
  // If abort requested, return 0 to make curl_easy_perform() fail with
  // CURLE_WRITE_ERROR, terminating the transfer immediately.
  if (ctx->abortFlag && ctx->abortFlag->load())
    return 0;

  size_t total = size * nmemb;
  ctx->lineBuffer.append(data, total);

  // Process each complete NDJSON line as it arrives.
  size_t pos;
  while ((pos = ctx->lineBuffer.find('\n')) != std::string::npos) {
    std::string line = ctx->lineBuffer.substr(0, pos);
    ctx->lineBuffer.erase(0, pos + 1);
    if (line.empty())
      continue;

    Json::CharReaderBuilder rb;
    Json::Value val;
    std::string errs;
    std::istringstream ls(line);
    if (!Json::parseFromStream(rb, ls, &val, &errs))
      continue;

    if (val.isMember("message") && val["message"].isMember("content")) {
      std::string tok = val["message"]["content"].asString();
      if (!tok.empty() && ctx->onToken) {
        auto cb = ctx->onToken;
        auto tkn = std::make_shared<std::string>(std::move(tok));
        ctx->loop->queueInLoop([cb, tkn]() { cb(*tkn); });
      }
    }

    // Handle thinking/reasoning tokens (models like qwen3.5)
    if (val.isMember("message") && val["message"].isMember("thinking")) {
      std::string think = val["message"]["thinking"].asString();
      if (!think.empty() && ctx->onThinking) {
        auto cb = ctx->onThinking;
        auto tkn = std::make_shared<std::string>(std::move(think));
        ctx->loop->queueInLoop([cb, tkn]() { cb(*tkn); });
      }
    }

    // Capture token stats from the final "done":true chunk.
    if (val.get("done", false).asBool()) {
      ctx->stats.evalCount = val.get("eval_count", 0).asInt();
      ctx->stats.promptEvalCount = val.get("prompt_eval_count", 0).asInt();
      // Ollama returns durations in nanoseconds
      ctx->stats.evalDuration = val.get("eval_duration", 0).asInt64() / 1e9;
      ctx->stats.totalDuration = val.get("total_duration", 0).asInt64() / 1e9;
    }
  }
  return total;
}

// ─── streamChat (true streaming via libcurl) ────────────────────────────────
OllamaService::AbortFlag OllamaService::streamChat(
    const std::string &model, const std::vector<Message> &messages,
    double temperature, TokenCallback onToken, DoneCallback onDone,
    StatsCallback onStats, AbortFlag existingFlag, ThinkingCallback onThinking,
    bool enableThinking) {
  // Use the caller-provided flag, or create a new one.
  auto abortFlag =
      existingFlag ? existingFlag : std::make_shared<std::atomic<bool>>(false);

  // Build the JSON body.
  Json::Value body;
  body["model"] = model;
  body["messages"] = messagesToJson(messages);
  body["stream"] = true;
  body["keep_alive"] = "10m"; // keep model loaded for fast follow-ups

  if (!enableThinking) {
    body["think"] = false; // disable thinking for thinking models
  }

  Json::Value opts;
  opts["temperature"] = temperature;
  opts["num_predict"] = 2048; // cap output length for speed
  // num_ctx omitted — Ollama uses the model's native context window
  body["options"] = opts;

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  std::string bodyStr = Json::writeString(wb, body);

  // Grab Drogon's main event loop for safe callback dispatch.
  auto *loop = drogon::app().getLoop();

  // Run libcurl in a detached thread so we don't block the event loop.
  std::thread([bodyStr = std::move(bodyStr), onToken, onDone, onStats,
               onThinking, loop, abortFlag]() {
    try {
      CURL *curl = curl_easy_init();
      if (!curl) {
        if (onDone)
          loop->queueInLoop(
              [onDone]() { onDone("Failed to initialise curl"); });
        return;
      }

      CurlStreamCtx ctx;
      ctx.onToken = onToken;
      ctx.onStats = onStats;
      ctx.onThinking = onThinking;
      ctx.loop = loop;
      ctx.abortFlag = abortFlag;

      struct curl_slist *headers = nullptr;
      headers = curl_slist_append(headers, "Content-Type: application/json");

      curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:11434/api/chat");
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)bodyStr.size());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);      // 3 min max
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L); // fast fail
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);       // thread-safe
      curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);    // low latency

      CURLcode res = curl_easy_perform(curl);

      long httpCode = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);

      // Fire stats callback if available (before onDone).
      if (ctx.onStats && !abortFlag->load() && res == CURLE_OK &&
          httpCode == 200) {
        auto statsCopy = std::make_shared<OllamaService::TokenStats>(ctx.stats);
        auto sCb = ctx.onStats;
        loop->queueInLoop([sCb, statsCopy]() { sCb(*statsCopy); });
      }

      if (onDone) {
        bool aborted = abortFlag->load();
        if (aborted) {
          // User-initiated cancellation — not an error.
          loop->queueInLoop([onDone]() { onDone(""); });
        } else if (res != CURLE_OK) {
          std::string err = "Ollama request failed: ";
          err += curl_easy_strerror(res);
          loop->queueInLoop([onDone, err]() { onDone(err); });
        } else if (httpCode != 200) {
          std::string err = "Ollama returned HTTP " + std::to_string(httpCode);
          loop->queueInLoop([onDone, err]() { onDone(err); });
        } else {
          loop->queueInLoop([onDone]() { onDone(""); });
        }
      }
    } catch (const std::exception &ex) {
      LOG_ERROR << "streamChat thread exception: " << ex.what();
      if (onDone)
        loop->queueInLoop([onDone, msg = std::string(ex.what())]() {
          onDone("Internal error: " + msg);
        });
    } catch (...) {
      LOG_ERROR << "streamChat thread: unknown exception";
      if (onDone)
        loop->queueInLoop([onDone]() { onDone("Unknown internal error"); });
    }
  }).detach();

  return abortFlag;
}

// ─── chat (convenience) ─────────────────────────────────────────────────────
void OllamaService::chat(const std::string &model,
                         const std::vector<Message> &messages,
                         double temperature, ChatCallback cb) {
  auto accum = std::make_shared<std::string>();
  streamChat(
      model, messages, temperature,
      [accum](const std::string &tok) { accum->append(tok); },
      [accum, cb](const std::string &err) {
        if (cb)
          cb(*accum, err);
      });
}

// ─── Helper: curl GET/POST returning body string ────────────────────────────
static size_t curlCollectCb(char *data, size_t size, size_t nmemb,
                            void *userp) {
  auto *buf = static_cast<std::string *>(userp);
  buf->append(data, size * nmemb);
  return size * nmemb;
}

static std::string curlGet(const std::string &url) {
  CURL *c = curl_easy_init();
  if (!c)
    return "";
  std::string body;
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlCollectCb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  CURLcode res = curl_easy_perform(c);
  long httpCode = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &httpCode);
  curl_easy_cleanup(c);
  if (res != CURLE_OK || (httpCode != 0 && httpCode != 200))
    return "";
  return body;
}

static std::string curlPost(const std::string &url,
                            const std::string &payload) {
  CURL *c = curl_easy_init();
  if (!c)
    return "";
  std::string body;
  struct curl_slist *hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_POST, 1L);
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)payload.size());
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlCollectCb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  CURLcode res = curl_easy_perform(c);
  long httpCode = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &httpCode);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(c);
  if (res != CURLE_OK || (httpCode != 0 && httpCode != 200))
    return "";
  return body;
}

// ─── listModelsWithInfo ─────────────────────────────────────────────────────
void OllamaService::listModelsWithInfo(ModelsInfoCallback cb) {
  auto *loop = drogon::app().getLoop();

  std::thread([cb, loop]() {
    std::vector<ModelInfo> result;

    // 1. Fetch model list from /api/tags
    std::string tagsBody = curlGet("http://127.0.0.1:11434/api/tags");
    if (tagsBody.empty()) {
      loop->queueInLoop([cb]() { cb({}, "Cannot reach Ollama"); });
      return;
    }

    Json::CharReaderBuilder rb;
    Json::Value tagsRoot;
    std::string errs;
    std::istringstream tss(tagsBody);
    if (!Json::parseFromStream(rb, tss, &tagsRoot, &errs) ||
        !tagsRoot.isMember("models")) {
      loop->queueInLoop([cb]() { cb({}, "Failed to parse Ollama tags"); });
      return;
    }

    // 2. For each model, fetch /api/show to get context_length
    for (auto &m : tagsRoot["models"]) {
      std::string name = m.get("name", "").asString();
      if (name.empty())
        continue;

      ModelInfo info;
      info.name = name;
      info.contextLength = 4096; // safe default

      // Call /api/show
      Json::Value showReq;
      showReq["name"] = name;
      Json::StreamWriterBuilder wb;
      wb["indentation"] = "";
      std::string showPayload = Json::writeString(wb, showReq);

      std::string showBody =
          curlPost("http://127.0.0.1:11434/api/show", showPayload);

      if (!showBody.empty()) {
        Json::Value showRoot;
        std::istringstream sss(showBody);
        if (Json::parseFromStream(rb, sss, &showRoot, &errs) &&
            showRoot.isMember("model_info")) {
          // context_length key is {family}.context_length
          auto &mi = showRoot["model_info"];
          for (auto it = mi.begin(); it != mi.end(); ++it) {
            std::string key = it.name();
            if (key.size() > 15 &&
                key.substr(key.size() - 15) == ".context_length") {
              info.contextLength = it->asInt();
              break;
            }
          }
        }
      }

      result.push_back(info);
    }

    auto res = std::make_shared<std::vector<ModelInfo>>(std::move(result));
    loop->queueInLoop([cb, res]() { cb(*res, ""); });
  }).detach();
}

// ─── listModels ─────────────────────────────────────────────────────────────
void OllamaService::listModels(ModelsCallback cb) {
  auto client = drogon::HttpClient::newHttpClient("http://127.0.0.1:11434");
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setPath("/api/tags");
  req->setMethod(drogon::Get);

  client->sendRequest(
      req,
      [cb](drogon::ReqResult result, const drogon::HttpResponsePtr &resp) {
        std::vector<std::string> models;
        if (result != drogon::ReqResult::Ok || !resp) {
          if (cb)
            cb(models, "Cannot reach Ollama");
          return;
        }
        if (resp->getStatusCode() != drogon::k200OK) {
          if (cb)
            cb(models,
               "Ollama returned HTTP " +
                   std::to_string(static_cast<int>(resp->getStatusCode())));
          return;
        }

        Json::CharReaderBuilder rb;
        Json::Value root;
        std::string errs;
        std::istringstream body(std::string(resp->body()));
        if (!Json::parseFromStream(rb, body, &root, &errs)) {
          if (cb)
            cb(models, "Failed to parse Ollama response");
          return;
        }

        if (root.isMember("models") && root["models"].isArray()) {
          for (auto &m : root["models"]) {
            if (m.isMember("name"))
              models.push_back(m["name"].asString());
          }
        }
        if (cb)
          cb(std::move(models), "");
      },
      /* timeout */ 5.0);
}
