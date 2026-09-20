#include "services/RagService.h"

#include <curl/curl.h>
#include <drogon/drogon.h>
#include <json/json.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numeric>
#include <queue>
#include <sstream>
#include <sys/stat.h> // mkdir
#include <thread>
#include <unordered_set>

// ─── Tuning constants ───────────────────────────────────────────────────────
// Lower threshold — let the LLM judge which passages are truly relevant.
// With exact search we get reliable similarity scores, so 0.10 is safe.
static constexpr double MIN_SIMILARITY = 0.10;

// ─── Singleton ──────────────────────────────────────────────────────────────
RagService &RagService::instance() {
  static RagService svc;
  return svc;
}

RagService::~RagService() = default;

// ─── curl write callback ────────────────────────────────────────────────────
static size_t ragCurlWriteCb(char *data, size_t size, size_t nmemb,
                             void *userp) {
  auto *buf = static_cast<std::string *>(userp);
  buf->append(data, size * nmemb);
  return size * nmemb;
}

// ═══════════════════════════════════════════════════════════════════════════
// chunkText — sentence-aware text splitting
// ═══════════════════════════════════════════════════════════════════════════

std::vector<std::string> RagService::chunkText(const std::string &text,
                                               int chunkSize, int overlap,
                                               int maxChunks) {
  std::vector<std::string> chunks;
  if (text.empty())
    return chunks;

  if (maxChunks <= 0)
    maxChunks = 500;

  const int maxSentenceLen = chunkSize * 2;

  // ── 1. Split into sentences ──────────────────────────────────────
  std::vector<std::string> sentences;
  {
    std::string current;
    current.reserve(
        std::min(static_cast<int>(text.size()), maxSentenceLen + 64));

    auto flushCurrent = [&]() {
      if (current.empty())
        return;
      auto start = current.find_first_not_of(" \t\r\n");
      if (start != std::string::npos) {
        auto end = current.find_last_not_of(" \t\r\n");
        sentences.push_back(current.substr(start, end - start + 1));
      }
      current.clear();
    };

    for (size_t i = 0; i < text.size(); ++i) {
      current += text[i];

      if (static_cast<int>(current.size()) >= maxSentenceLen) {
        flushCurrent();
        continue;
      }

      bool isSentenceEnd = false;
      if (text[i] == '.' || text[i] == '!' || text[i] == '?') {
        if (i + 1 >= text.size() || text[i + 1] == ' ' || text[i + 1] == '\n') {
          isSentenceEnd = true;
        }
      }
      if (text[i] == '\n' && current.size() > 20) {
        isSentenceEnd = true;
      }
      if (isSentenceEnd) {
        flushCurrent();
      }
    }
    flushCurrent();
  }

  // ── 2. Combine sentences into chunks ─────────────────────────────
  std::string currentChunk;
  currentChunk.reserve(chunkSize + overlap);
  size_t sentIdx = 0;

  while (sentIdx < sentences.size()) {
    if (static_cast<int>(chunks.size()) >= maxChunks)
      break;

    if (currentChunk.empty()) {
      currentChunk = sentences[sentIdx];
      sentIdx++;
    } else if (static_cast<int>(currentChunk.size() +
                                sentences[sentIdx].size() + 1) <= chunkSize) {
      currentChunk += " " + sentences[sentIdx];
      sentIdx++;
    } else {
      chunks.push_back(currentChunk);
      if (static_cast<int>(chunks.size()) >= maxChunks)
        break;

      currentChunk.clear();
      int overlapChars = 0;
      size_t overlapStart = sentIdx;
      for (size_t j = sentIdx; j > 0; --j) {
        int sentLen = static_cast<int>(sentences[j - 1].size());
        if (overlapChars + sentLen > overlap)
          break;
        overlapChars += sentLen + 1;
        overlapStart = j - 1;
      }
      for (size_t j = overlapStart; j < sentIdx; ++j) {
        if (currentChunk.empty())
          currentChunk = sentences[j];
        else
          currentChunk += " " + sentences[j];
      }

      if (sentIdx < sentences.size() && !currentChunk.empty() &&
          static_cast<int>(currentChunk.size() + sentences[sentIdx].size() +
                           1) > chunkSize) {
        chunks.push_back(currentChunk);
        if (static_cast<int>(chunks.size()) >= maxChunks)
          break;
        currentChunk = sentences[sentIdx];
        sentIdx++;
      }
    }
  }

  if (!currentChunk.empty() && static_cast<int>(chunks.size()) < maxChunks) {
    chunks.push_back(currentChunk);
  }

  // ── 3. Fallback: character-based chunking ────────────────────────
  if (chunks.empty() && !text.empty()) {
    int step = std::max(1, chunkSize - overlap);
    for (int i = 0; i < static_cast<int>(text.size()); i += step) {
      int len = std::min(chunkSize, static_cast<int>(text.size()) - i);
      chunks.push_back(text.substr(i, len));
      if (i + len >= static_cast<int>(text.size()))
        break;
      if (static_cast<int>(chunks.size()) >= maxChunks)
        break;
    }
  }

  return chunks;
}

// ═══════════════════════════════════════════════════════════════════════════
// Embedding helpers
// ═══════════════════════════════════════════════════════════════════════════

bool RagService::checkEmbeddingModel(const std::string &model,
                                     std::string &error) {
  auto emb = computeEmbedding("test", model, error);
  return error.empty() && !emb.empty();
}

std::vector<float> RagService::computeEmbedding(const std::string &text,
                                                const std::string &model,
                                                std::string &error) {
  auto embs = computeEmbeddings({text}, model, error);
  if (!embs.empty())
    return std::move(embs[0]);
  return {};
}

std::vector<std::vector<float>>
RagService::computeEmbeddings(const std::vector<std::string> &texts,
                              const std::string &model, std::string &error,
                              void *sharedCurl) {
  std::vector<std::vector<float>> embs;
  if (texts.empty())
    return embs;

  CURL *curl = sharedCurl ? static_cast<CURL *>(sharedCurl) : curl_easy_init();
  if (!curl) {
    error = "Failed to initialise curl for embedding";
    return embs;
  }

  Json::Value body;
  body["model"] = model;
  Json::Value inputArr(Json::arrayValue);
  for (const auto &t : texts)
    inputArr.append(t);
  body["input"] = inputArr;

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  std::string bodyStr = Json::writeString(wb, body);

  std::string response;
  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:11434/api/embed");
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(bodyStr.size()));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ragCurlWriteCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (sharedCurl) {
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 120L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 60L);
  }

  CURLcode res = curl_easy_perform(curl);
  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

  curl_slist_free_all(headers);
  if (!sharedCurl)
    curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    error = std::string("Embedding request failed: ") + curl_easy_strerror(res);
    return embs;
  }
  if (httpCode != 200) {
    error = "Embedding API returned HTTP " + std::to_string(httpCode) +
            " — body: " + response.substr(0, 200);
    return embs;
  }

  Json::CharReaderBuilder rb;
  Json::Value val;
  std::string parseErrs;
  std::istringstream ss(response);
  if (!Json::parseFromStream(rb, ss, &val, &parseErrs)) {
    error = "Failed to parse embedding response";
    return embs;
  }

  // Ollama /api/embed returns {"embeddings": [[...]]}
  if (val.isMember("embeddings") && val["embeddings"].isArray()) {
    for (const auto &vec : val["embeddings"]) {
      if (vec.isArray()) {
        std::vector<float> e;
        e.reserve(vec.size());
        for (const auto &v : vec) {
          e.push_back(v.asFloat());
        }
        embs.push_back(std::move(e));
      }
    }
  }
  // Fallback: older Ollama may return {"embedding": [...]}
  else if (val.isMember("embedding") && val["embedding"].isArray()) {
    std::vector<float> e;
    const auto &vec = val["embedding"];
    e.reserve(vec.size());
    for (const auto &v : vec) {
      e.push_back(v.asFloat());
    }
    embs.push_back(std::move(e));
  } else {
    error = "No embedding found in response";
  }

  return embs;
}

double RagService::cosineSimilarity(const std::vector<float> &a,
                                    const std::vector<float> &b) {
  if (a.size() != b.size() || a.empty())
    return 0.0;

  double dot = 0.0, normA = 0.0, normB = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
    normA += static_cast<double>(a[i]) * static_cast<double>(a[i]);
    normB += static_cast<double>(b[i]) * static_cast<double>(b[i]);
  }

  double denom = std::sqrt(normA) * std::sqrt(normB);
  return (denom > 0.0) ? (dot / denom) : 0.0;
}

void RagService::normalizeVector(std::vector<float> &v) {
  float norm = 0.0f;
  for (float x : v)
    norm += x * x;
  norm = std::sqrt(norm);
  if (norm > 0.0f) {
    for (float &x : v)
      x /= norm;
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// Indexing
// ═══════════════════════════════════════════════════════════════════════════

void RagService::indexDocument(const std::string &source,
                               const std::string &title,
                               const std::string &text,
                               const std::string &embeddingModel,
                               IndexCallback cb) {
  auto *loop = drogon::app().getLoop();

  std::thread([this, source, title, text, embeddingModel, cb, loop]() {
    try {
      // Smaller chunks (1 000 chars) give more focused embeddings →
      // better retrieval accuracy.  100 chunks per doc is generous.
      constexpr int MAX_CHUNKS_PER_DOC = 100;
      auto chunks = chunkText(text, 1000, 200, MAX_CHUNKS_PER_DOC);

      int indexed = 0;
      std::string lastError;

      CURL *sharedCurl = curl_easy_init();
      constexpr size_t BATCH_SIZE = 20;

      for (size_t i = 0; i < chunks.size(); i += BATCH_SIZE) {
        std::vector<std::string> batch;
        for (size_t j = i; j < std::min(chunks.size(), i + BATCH_SIZE); ++j) {
          batch.push_back(chunks[j]);
        }

        std::string err;
        auto batchEmbs =
            computeEmbeddings(batch, embeddingModel, err, sharedCurl);
        if (!err.empty()) {
          lastError = err;
          LOG_WARN << "RAG: embedding failed for batch from " << source << ": "
                   << err;
          continue;
        }

        for (size_t j = 0; j < batchEmbs.size(); ++j) {
          auto &embedding = batchEmbs[j];
          if (embedding.empty())
            continue;

          normalizeVector(embedding);

          {
            std::lock_guard<std::mutex> lock(storeMutex_);

            if (embDim_ == 0) {
              embDim_ = embedding.size();
              vectors_.reserve(vectors_.size() + chunks.size() * embDim_);
              LOG_INFO << "RAG: embedding dimension = " << embDim_;
            }

            if (embedding.size() != embDim_) {
              LOG_WARN << "RAG: embedding dimension mismatch (got "
                       << embedding.size() << ", expected " << embDim_
                       << ") — skipping chunk from " << source;
              continue;
            }

            size_t label = nextLabel_++;
            RagChunkMeta meta;
            meta.id = "chunk_" + std::to_string(nextId_++);
            meta.source = source;
            meta.title = title;
            meta.text = batch[j];

            vectors_.insert(vectors_.end(), embedding.begin(), embedding.end());
            vectorLabels_.push_back(label);
            chunkMeta_[label] = std::move(meta);
          }
          indexed++;
        }
      }

      if (sharedCurl)
        curl_easy_cleanup(sharedCurl);

      LOG_INFO << "RAG: indexed " << indexed << "/" << chunks.size()
               << " chunks from " << source << " (store now has "
               << vectorLabels_.size() << " vectors)";

      saveToDisk();

      if (cb) {
        loop->queueInLoop(
            [cb, indexed, lastError]() { cb(indexed, lastError); });
      }
    } catch (const std::exception &ex) {
      LOG_ERROR << "RAG indexDocument thread exception: " << ex.what();
      if (cb)
        loop->queueInLoop([cb, msg = std::string(ex.what())]() { cb(0, msg); });
    } catch (...) {
      LOG_ERROR << "RAG indexDocument thread: unknown exception";
      if (cb)
        loop->queueInLoop([cb]() { cb(0, "Unknown internal error"); });
    }
  }).detach();
}

void RagService::indexDocuments(
    const std::vector<std::tuple<std::string, std::string, std::string>> &docs,
    const std::string &embeddingModel, IndexCallback cb) {
  auto *loop = drogon::app().getLoop();

  std::thread([this, docs, embeddingModel, cb, loop]() {
    try {
      int totalIndexed = 0;
      std::string lastError;

      CURL *sharedCurl = curl_easy_init();
      constexpr size_t BATCH_SIZE = 20;

      for (const auto &[source, title, text] : docs) {
        constexpr int MAX_CHUNKS_PER_DOC = 100;
        auto chunks = chunkText(text, 1000, 200, MAX_CHUNKS_PER_DOC);

        for (size_t i = 0; i < chunks.size(); i += BATCH_SIZE) {
          std::vector<std::string> batch;
          for (size_t j = i; j < std::min(chunks.size(), i + BATCH_SIZE); ++j) {
            batch.push_back(chunks[j]);
          }

          std::string err;
          auto batchEmbs =
              computeEmbeddings(batch, embeddingModel, err, sharedCurl);
          if (!err.empty()) {
            lastError = err;
            continue;
          }

          for (size_t j = 0; j < batchEmbs.size(); ++j) {
            auto &embedding = batchEmbs[j];
            if (embedding.empty())
              continue;

            normalizeVector(embedding);

            {
              std::lock_guard<std::mutex> lock(storeMutex_);
              if (embDim_ == 0) {
                embDim_ = embedding.size();
                LOG_INFO << "RAG: embedding dimension = " << embDim_;
              }

              if (embedding.size() != embDim_)
                continue;

              size_t label = nextLabel_++;
              RagChunkMeta meta;
              meta.id = "chunk_" + std::to_string(nextId_++);
              meta.source = source;
              meta.title = title;
              meta.text = batch[j];

              vectors_.insert(vectors_.end(), embedding.begin(),
                              embedding.end());
              vectorLabels_.push_back(label);
              chunkMeta_[label] = std::move(meta);
            }
            totalIndexed++;
          }
        }
      }

      if (sharedCurl)
        curl_easy_cleanup(sharedCurl);

      LOG_INFO << "RAG: indexed " << totalIndexed << " chunks from "
               << docs.size() << " documents"
               << " (store now has " << vectorLabels_.size() << " vectors)";

      saveToDisk();

      if (cb) {
        loop->queueInLoop(
            [cb, totalIndexed, lastError]() { cb(totalIndexed, lastError); });
      }
    } catch (const std::exception &ex) {
      LOG_ERROR << "RAG indexDocuments thread exception: " << ex.what();
      if (cb)
        loop->queueInLoop([cb, msg = std::string(ex.what())]() { cb(0, msg); });
    } catch (...) {
      LOG_ERROR << "RAG indexDocuments thread: unknown exception";
      if (cb)
        loop->queueInLoop([cb]() { cb(0, "Unknown internal error"); });
    }
  }).detach();
}

// ═══════════════════════════════════════════════════════════════════════════
// Ephemeral index + retrieve — temporary store, nothing persisted
// ═══════════════════════════════════════════════════════════════════════════

void RagService::indexAndRetrieveEphemeral(
    const std::vector<std::tuple<std::string, std::string, std::string>> &docs,
    const std::string &query, const std::string &embeddingModel, int topK,
    std::function<void(int, std::vector<RagChunk>, const std::string &)> cb) {
  auto *loop = drogon::app().getLoop();

  std::thread([this, docs, query, embeddingModel, topK, cb, loop]() {
    try {
      // ── 1. Chunk & embed into a local (ephemeral) store ─────────────
      struct EphChunk {
        std::string source;
        std::string title;
        std::string text;
        std::vector<float> embedding;
      };
      std::vector<EphChunk> ephChunks;
      int totalIndexed = 0;
      std::string lastError;
      size_t dim = 0;

      CURL *sharedCurl = curl_easy_init();
      constexpr size_t BATCH_SIZE = 20;

      for (const auto &[source, title, text] : docs) {
        constexpr int MAX_CHUNKS_PER_DOC = 100;
        auto textChunks = chunkText(text, 1000, 200, MAX_CHUNKS_PER_DOC);

        for (size_t i = 0; i < textChunks.size(); i += BATCH_SIZE) {
          std::vector<std::string> batch;
          for (size_t j = i; j < std::min(textChunks.size(), i + BATCH_SIZE);
               ++j) {
            batch.push_back(textChunks[j]);
          }

          std::string err;
          auto batchEmbs =
              computeEmbeddings(batch, embeddingModel, err, sharedCurl);
          if (!err.empty()) {
            lastError = err;
            continue;
          }

          for (size_t j = 0; j < batchEmbs.size(); ++j) {
            auto &embedding = batchEmbs[j];
            if (embedding.empty())
              continue;

            normalizeVector(embedding);
            if (dim == 0)
              dim = embedding.size();
            if (embedding.size() != dim)
              continue;

            EphChunk ec;
            ec.source = source;
            ec.title = title;
            ec.text = batch[j];
            ec.embedding = std::move(embedding);
            ephChunks.push_back(std::move(ec));
            totalIndexed++;
          }
        }
      }
      if (sharedCurl)
        curl_easy_cleanup(sharedCurl);

      LOG_INFO << "RAG (ephemeral): indexed " << totalIndexed << " chunks from "
               << docs.size() << " documents";

      if (ephChunks.empty() || dim == 0) {
        if (cb) {
          loop->queueInLoop([cb, totalIndexed, lastError]() {
            cb(totalIndexed, {}, lastError);
          });
        }
        return;
      }

      // ── 2. Compute query embedding ──────────────────────────────────
      std::string qerr;
      auto queryEmb = computeEmbedding(query, embeddingModel, qerr);
      if (!qerr.empty() || queryEmb.empty()) {
        std::string e = qerr.empty() ? "empty query embedding" : qerr;
        if (cb) {
          loop->queueInLoop(
              [cb, totalIndexed, e]() { cb(totalIndexed, {}, e); });
        }
        return;
      }
      normalizeVector(queryEmb);

      // ── 3. Exact search over the ephemeral vectors ──────────────────
      size_t k = std::min(static_cast<size_t>(topK), ephChunks.size());
      using Entry = std::pair<float, size_t>;
      std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap;

      for (size_t i = 0; i < ephChunks.size(); ++i) {
        float score = 0.0f;
        for (size_t d = 0; d < dim; ++d)
          score += queryEmb[d] * ephChunks[i].embedding[d];

        if (score < static_cast<float>(MIN_SIMILARITY))
          continue;

        if (heap.size() < k) {
          heap.push({score, i});
        } else if (score > heap.top().first) {
          heap.pop();
          heap.push({score, i});
        }
      }

      std::vector<RagChunk> results;
      while (!heap.empty()) {
        auto [score, idx] = heap.top();
        heap.pop();
        RagChunk rc;
        rc.id = "eph_" + std::to_string(idx);
        rc.source = ephChunks[idx].source;
        rc.title = ephChunks[idx].title;
        rc.text = ephChunks[idx].text;
        rc.score = static_cast<double>(score);
        results.push_back(std::move(rc));
      }

      std::sort(results.begin(), results.end(),
                [](const RagChunk &a, const RagChunk &b) {
                  return a.score > b.score;
                });

      LOG_INFO << "RAG (ephemeral): search returned " << results.size()
               << " chunks for query";

      if (cb) {
        loop->queueInLoop([cb, totalIndexed, results = std::move(results),
                           lastError]() mutable {
          cb(totalIndexed, std::move(results), lastError);
        });
      }
    } catch (const std::exception &ex) {
      LOG_ERROR << "RAG ephemeral thread exception: " << ex.what();
      if (cb)
        loop->queueInLoop(
            [cb, msg = std::string(ex.what())]() { cb(0, {}, msg); });
    } catch (...) {
      LOG_ERROR << "RAG ephemeral thread: unknown exception";
      if (cb)
        loop->queueInLoop([cb]() { cb(0, {}, "Unknown internal error"); });
    }
  }).detach();
}

// ═══════════════════════════════════════════════════════════════════════════
// Retrieval — exact inner-product search (FAISS IndexFlatIP algorithm)
// ═══════════════════════════════════════════════════════════════════════════

void RagService::retrieve(const std::string &query,
                          const std::string &embeddingModel, int topK,
                          RetrieveCallback cb) {
  auto *loop = drogon::app().getLoop();

  std::thread([this, query, embeddingModel, topK, cb, loop]() {
    try {
      std::string error;
      auto results = retrieveSync(query, embeddingModel, topK, error);

      if (cb) {
        loop->queueInLoop([cb, results = std::move(results), error]() mutable {
          cb(std::move(results), error);
        });
      }
    } catch (const std::exception &ex) {
      LOG_ERROR << "RAG retrieve thread exception: " << ex.what();
      if (cb)
        loop->queueInLoop(
            [cb, msg = std::string(ex.what())]() { cb({}, msg); });
    } catch (...) {
      LOG_ERROR << "RAG retrieve thread: unknown exception";
      if (cb)
        loop->queueInLoop([cb]() { cb({}, "Unknown internal error"); });
    }
  }).detach();
}

std::vector<RagChunk>
RagService::retrieveSync(const std::string &query,
                         const std::string &embeddingModel, int topK,
                         std::string &error) {
  std::vector<RagChunk> results;

  {
    std::lock_guard<std::mutex> lock(storeMutex_);
    if (vectorLabels_.empty()) {
      LOG_INFO << "RAG: vector store is empty, nothing to retrieve";
      return results;
    }
  }

  // Compute query embedding (outside the lock — network call).
  auto queryEmb = computeEmbedding(query, embeddingModel, error);
  if (!error.empty() || queryEmb.empty()) {
    LOG_WARN << "RAG: query embedding failed: " << error;
    return results;
  }

  normalizeVector(queryEmb);

  {
    std::lock_guard<std::mutex> lock(storeMutex_);
    if (vectorLabels_.empty())
      return results;

    // Validate query embedding dimension matches the stored vectors.
    if (queryEmb.size() != embDim_) {
      error = "Query embedding dimension (" + std::to_string(queryEmb.size()) +
              ") does not match store dimension (" + std::to_string(embDim_) +
              ")";
      LOG_ERROR << "RAG: " << error;
      return results;
    }

    size_t numVecs = vectorLabels_.size();
    size_t k = std::min(static_cast<size_t>(topK), numVecs);
    if (k == 0)
      return results;

    // ── Exact inner-product search (FAISS IndexFlatIP equivalent) ──
    //
    // For unit-normalised vectors:  inner_product == cosine_similarity
    //
    // We use a min-heap of size k so that:
    //   • heap.top() is always the WORST of the current top-k
    //   • a new score beats the heap only if it's higher than that worst
    //
    using Entry = std::pair<float, size_t>; // (score, vecIndex)
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap;

    for (size_t i = 0; i < numVecs; ++i) {
      const float *vec = vectors_.data() + i * embDim_;

      // Dot product (= cosine for unit vectors)
      float score = 0.0f;
      for (size_t d = 0; d < embDim_; ++d) {
        score += queryEmb[d] * vec[d];
      }

      if (score < static_cast<float>(MIN_SIMILARITY))
        continue;

      if (heap.size() < k) {
        heap.push({score, i});
      } else if (score > heap.top().first) {
        heap.pop();
        heap.push({score, i});
      }
    }

    // Drain the heap into results.
    while (!heap.empty()) {
      auto [score, idx] = heap.top();
      heap.pop();

      size_t label = vectorLabels_[idx];
      auto it = chunkMeta_.find(label);
      if (it == chunkMeta_.end())
        continue;

      RagChunk chunk;
      chunk.id = it->second.id;
      chunk.source = it->second.source;
      chunk.title = it->second.title;
      chunk.text = it->second.text;
      chunk.score = static_cast<double>(score);
      results.push_back(std::move(chunk));
    }
  }

  // Heap gave worst-first; sort best-first.
  std::sort(
      results.begin(), results.end(),
      [](const RagChunk &a, const RagChunk &b) { return a.score > b.score; });

  // Log scores for all returned chunks — invaluable for debugging
  // retrieval quality.
  {
    std::ostringstream oss;
    oss << "RAG: exact search returned " << results.size()
        << " chunks — scores:";
    for (size_t i = 0; i < results.size(); ++i) {
      oss << " [" << (i + 1) << "]=" << static_cast<int>(results[i].score * 100)
          << "%";
    }
    LOG_INFO << oss.str();
  }

  return results;
}

// ═══════════════════════════════════════════════════════════════════════════
// Store management
// ═══════════════════════════════════════════════════════════════════════════

void RagService::clear() {
  {
    std::lock_guard<std::mutex> lock(storeMutex_);
    vectors_.clear();
    vectorLabels_.clear();
    chunkMeta_.clear();
    embDim_ = 0;
    nextLabel_ = 0;
    nextId_ = 0;
  }
  {
    std::lock_guard<std::mutex> lock(docsMutex_);
    documents_.clear();
  }
  LOG_INFO << "RAG: vector store and document registry cleared";
  saveToDisk();
}

void RagService::clearSource(const std::string &source) {
  {
    std::lock_guard<std::mutex> lock(storeMutex_);
    if (embDim_ == 0 || vectorLabels_.empty())
      return;

    // Build compacted arrays without the deleted source's vectors.
    std::vector<float> newVectors;
    std::vector<size_t> newLabels;
    newVectors.reserve(vectors_.size());
    newLabels.reserve(vectorLabels_.size());

    size_t numVecs = vectorLabels_.size();
    for (size_t i = 0; i < numVecs; ++i) {
      size_t label = vectorLabels_[i];
      auto it = chunkMeta_.find(label);
      if (it != chunkMeta_.end() && it->second.source == source) {
        chunkMeta_.erase(it);
        continue; // skip this vector
      }
      // Keep this vector.
      newLabels.push_back(label);
      const float *base = vectors_.data() + i * embDim_;
      newVectors.insert(newVectors.end(), base, base + embDim_);
    }

    vectors_ = std::move(newVectors);
    vectorLabels_ = std::move(newLabels);

    LOG_INFO << "RAG: cleared source '" << source << "' — "
             << vectorLabels_.size() << " vectors remaining";
  }
  // Persist outside the lock — saveToDisk() acquires storeMutex_ internally.
  saveToDisk();
}

size_t RagService::size() const {
  std::lock_guard<std::mutex> lock(storeMutex_);
  return chunkMeta_.size();
}

bool RagService::hasSource(const std::string &source) const {
  std::lock_guard<std::mutex> lock(storeMutex_);
  return std::any_of(
      chunkMeta_.begin(), chunkMeta_.end(),
      [&source](const auto &p) { return p.second.source == source; });
}

// ═══════════════════════════════════════════════════════════════════════════
// Document management
// ═══════════════════════════════════════════════════════════════════════════

void RagService::addDocument(const RagDocument &doc) {
  std::lock_guard<std::mutex> lock(docsMutex_);
  documents_.push_back(doc);
  LOG_INFO << "RAG: registered document '" << doc.name << "' (id=" << doc.id
           << ", " << doc.chunks << " chunks)";
}

void RagService::updateDocumentChunks(const std::string &docId, int chunks) {
  {
    std::lock_guard<std::mutex> lock(docsMutex_);
    for (auto &d : documents_) {
      if (d.id == docId) {
        d.chunks = chunks;
        LOG_INFO << "RAG: updated document '" << d.name
                 << "' chunks=" << chunks;
        break;
      }
    }
  }
  saveToDisk();
}

bool RagService::removeDocument(const std::string &docId) {
  std::string source;
  {
    std::lock_guard<std::mutex> lock(docsMutex_);
    auto it =
        std::find_if(documents_.begin(), documents_.end(),
                     [&docId](const RagDocument &d) { return d.id == docId; });
    if (it == documents_.end())
      return false;
    source = it->source;
    documents_.erase(it);
  }
  clearSource(source); // clearSource() already saves to disk
  LOG_INFO << "RAG: removed document id=" << docId << " and its chunks";
  return true;
}

std::vector<RagDocument> RagService::listDocuments() const {
  std::lock_guard<std::mutex> lock(docsMutex_);
  return documents_;
}

bool RagService::hasDocuments() const {
  std::lock_guard<std::mutex> lock(docsMutex_);
  return !documents_.empty();
}

// ═══════════════════════════════════════════════════════════════════════════
// Persistence
//
//   rag_documents.json  — lightweight document metadata (same format as before)
//   rag_meta.json       — chunk metadata (id, source, title, text) + index
//   params rag_vectors.bin     — binary: header + flat float array + labels
//
// The binary format for rag_vectors.bin:
//   uint64_t  magic        (0x52414756 = "RAGV")
//   uint64_t  version      (1)
//   uint64_t  numVectors
//   uint64_t  dim
//   --- for each vector ---
//   uint64_t  label
//   float[dim] data
// ═══════════════════════════════════════════════════════════════════════════

static constexpr uint64_t RAG_MAGIC = 0x52414756; // "RAGV"
static constexpr uint64_t RAG_VERSION = 1;

void RagService::setStoragePath(const std::string &dirPath) {
  storagePath_ = dirPath;
  // Use POSIX mkdir instead of ::system() to avoid shell injection.
  mkdir(dirPath.c_str(), 0755);
  LOG_INFO << "RAG: storage path set to " << dirPath;
}

void RagService::saveToDisk() const {
  if (storagePath_.empty())
    return;

  // ── Save documents ──────────────────────────────────────────────────
  {
    Json::Value root(Json::arrayValue);
    std::lock_guard<std::mutex> lock(docsMutex_);
    for (const auto &d : documents_) {
      if (d.chunks < 0)
        continue;
      Json::Value jd;
      jd["id"] = d.id;
      jd["name"] = d.name;
      jd["source"] = d.source;
      jd["sizeBytes"] = static_cast<Json::UInt64>(d.sizeBytes);
      jd["chunks"] = d.chunks;
      jd["uploadedAt"] = static_cast<Json::Int64>(d.uploadedAt);
      root.append(jd);
    }
    std::string path = storagePath_ + "/rag_documents.json";
    std::ofstream out(path);
    if (out.good()) {
      Json::StreamWriterBuilder wb;
      wb["indentation"] = "  ";
      out << Json::writeString(wb, root);
    } else {
      LOG_ERROR << "RAG: failed to write " << path;
    }
  }

  // ── Save chunk metadata (JSON) + vectors (binary) ───────────────────
  {
    std::lock_guard<std::mutex> lock(storeMutex_);

    // 1. Metadata JSON.
    {
      std::string path = storagePath_ + "/rag_meta.json";
      std::ofstream out(path);
      if (out.good()) {
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        out << "{";
        out << "\"embDim\":" << embDim_ << ",";
        out << "\"nextLabel\":" << nextLabel_ << ",";
        out << "\"nextId\":" << nextId_ << ",";
        out << "\"chunks\":[";
        bool first = true;
        for (auto &[label, meta] : chunkMeta_) {
          Json::Value jm;
          jm["label"] = static_cast<Json::UInt64>(label);
          jm["id"] = meta.id;
          jm["source"] = meta.source;
          jm["title"] = meta.title;
          jm["text"] = meta.text;
          if (!first)
            out << ",";
          out << Json::writeString(wb, jm);
          first = false;
        }
        out << "]}";
      } else {
        LOG_ERROR << "RAG: failed to write " << path;
      }
    }

    // 2. Vectors binary.
    {
      std::string path = storagePath_ + "/rag_vectors.bin";
      if (vectorLabels_.empty()) {
        // No vectors — remove stale files so loadFromDisk()
        // doesn't get confused on next startup.
        std::remove(path.c_str());
        std::remove((storagePath_ + "/rag_meta.json").c_str());
      } else {
        std::ofstream out(path, std::ios::binary);
        if (out.good()) {
          uint64_t nv = vectorLabels_.size();
          uint64_t dim = embDim_;
          uint64_t magic = RAG_MAGIC;
          uint64_t version = RAG_VERSION;

          out.write(reinterpret_cast<const char *>(&magic), sizeof(magic));
          out.write(reinterpret_cast<const char *>(&version), sizeof(version));
          out.write(reinterpret_cast<const char *>(&nv), sizeof(nv));
          out.write(reinterpret_cast<const char *>(&dim), sizeof(dim));

          for (size_t i = 0; i < nv; ++i) {
            uint64_t label = vectorLabels_[i];
            out.write(reinterpret_cast<const char *>(&label), sizeof(label));
            const float *vec = vectors_.data() + i * embDim_;
            out.write(reinterpret_cast<const char *>(vec),
                      embDim_ * sizeof(float));
          }
        } else {
          LOG_ERROR << "RAG: failed to write " << path;
        }
      }
    }

    // Clean up legacy persistence files if present.
    {
      std::string oldPath = storagePath_ + "/rag_hnsw.index";
      std::remove(oldPath.c_str());
    }
    {
      std::string oldPath = storagePath_ + "/rag_chunks.json";
      std::remove(oldPath.c_str());
    }

    LOG_INFO << "RAG: saved " << vectorLabels_.size()
             << " vectors + documents to disk";
  }
}

void RagService::loadFromDisk() {
  if (storagePath_.empty())
    return;

  // ── Load documents ──────────────────────────────────────────────────
  {
    std::string path = storagePath_ + "/rag_documents.json";
    std::ifstream in(path);
    if (!in.good()) {
      LOG_INFO << "RAG: no persisted documents found at " << path;
      return;
    }
    Json::CharReaderBuilder rb;
    Json::Value root;
    std::string errs;
    if (!Json::parseFromStream(rb, in, &root, &errs) || !root.isArray()) {
      LOG_ERROR << "RAG: failed to parse " << path << ": " << errs;
      return;
    }
    std::lock_guard<std::mutex> lock(docsMutex_);
    documents_.clear();
    for (const auto &jd : root) {
      RagDocument d;
      d.id = jd.get("id", "").asString();
      d.name = jd.get("name", "").asString();
      d.source = jd.get("source", "").asString();
      d.sizeBytes = static_cast<size_t>(jd.get("sizeBytes", 0).asUInt64());
      d.chunks = jd.get("chunks", 0).asInt();
      d.uploadedAt = jd.get("uploadedAt", 0).asInt64();
      documents_.push_back(std::move(d));
    }
    LOG_INFO << "RAG: loaded " << documents_.size() << " documents from disk";
  }

  // ── Try new format first (rag_meta.json + rag_vectors.bin) ──────────
  bool loadedNewFormat = false;
  {
    std::string metaPath = storagePath_ + "/rag_meta.json";
    std::ifstream in(metaPath);
    if (in.good()) {
      Json::CharReaderBuilder rb;
      Json::Value root;
      std::string errs;
      if (Json::parseFromStream(rb, in, &root, &errs) && root.isObject()) {
        std::lock_guard<std::mutex> lock(storeMutex_);
        embDim_ = static_cast<size_t>(root.get("embDim", 0).asUInt64());
        nextLabel_ = static_cast<size_t>(root.get("nextLabel", 0).asUInt64());
        nextId_ = root.get("nextId", 0).asInt();

        chunkMeta_.clear();
        const auto &arr = root["chunks"];
        if (arr.isArray()) {
          for (const auto &jm : arr) {
            size_t label = static_cast<size_t>(jm.get("label", 0).asUInt64());
            RagChunkMeta meta;
            meta.id = jm.get("id", "").asString();
            meta.source = jm.get("source", "").asString();
            meta.title = jm.get("title", "").asString();
            meta.text = jm.get("text", "").asString();
            chunkMeta_[label] = std::move(meta);
          }
        }

        LOG_INFO << "RAG: loaded " << chunkMeta_.size()
                 << " chunk metadata entries (new format)";

        // Load vectors binary.
        std::string vecPath = storagePath_ + "/rag_vectors.bin";
        std::ifstream vin(vecPath, std::ios::binary);
        if (vin.good()) {
          uint64_t magic = 0, version = 0, nv = 0, dim = 0;
          vin.read(reinterpret_cast<char *>(&magic), sizeof(magic));
          vin.read(reinterpret_cast<char *>(&version), sizeof(version));
          vin.read(reinterpret_cast<char *>(&nv), sizeof(nv));
          vin.read(reinterpret_cast<char *>(&dim), sizeof(dim));

          if (magic == RAG_MAGIC && version == RAG_VERSION && dim > 0) {
            embDim_ = dim;
            vectors_.clear();
            vectorLabels_.clear();
            vectors_.reserve(nv * dim);
            vectorLabels_.reserve(nv);

            std::vector<float> buf(dim);
            for (uint64_t i = 0; i < nv; ++i) {
              uint64_t label = 0;
              vin.read(reinterpret_cast<char *>(&label), sizeof(label));
              vin.read(reinterpret_cast<char *>(buf.data()),
                       dim * sizeof(float));
              if (!vin.good()) {
                LOG_ERROR << "RAG: truncated vector file at vector " << i;
                break;
              }
              if (chunkMeta_.count(static_cast<size_t>(label))) {
                vectorLabels_.push_back(static_cast<size_t>(label));
                vectors_.insert(vectors_.end(), buf.begin(), buf.end());
              }
            }

            LOG_INFO << "RAG: loaded " << vectorLabels_.size()
                     << " vectors from disk (dim=" << embDim_ << ")";
            loadedNewFormat = true;
          }
        }
      }
    }
  }

  // ── Prune orphan chunks whose source doesn't match any registered
  //    document.  This cleans up web-search scrapes that were
  //    accidentally persisted in earlier versions. ──────────────────────
  if (loadedNewFormat) {
    std::unordered_set<std::string> docSources;
    {
      std::lock_guard<std::mutex> dlock(docsMutex_);
      for (const auto &d : documents_) {
        docSources.insert(d.source);
      }
    }
    {
      std::lock_guard<std::mutex> slock(storeMutex_);
      size_t beforeCount = vectorLabels_.size();

      // Build compacted arrays keeping only document-owned chunks.
      std::vector<float> newVectors;
      std::vector<size_t> newLabels;
      newVectors.reserve(vectors_.size());
      newLabels.reserve(vectorLabels_.size());

      size_t numVecs = vectorLabels_.size();
      for (size_t i = 0; i < numVecs; ++i) {
        size_t label = vectorLabels_[i];
        auto it = chunkMeta_.find(label);
        if (it != chunkMeta_.end() &&
            docSources.find(it->second.source) == docSources.end()) {
          // This chunk belongs to an unknown source (web scrape) — drop it.
          chunkMeta_.erase(it);
          continue;
        }
        newLabels.push_back(label);
        const float *base = vectors_.data() + i * embDim_;
        newVectors.insert(newVectors.end(), base, base + embDim_);
      }

      vectors_ = std::move(newVectors);
      vectorLabels_ = std::move(newLabels);

      size_t pruned = beforeCount - vectorLabels_.size();
      if (pruned > 0) {
        LOG_INFO << "RAG: pruned " << pruned
                 << " orphan chunks (web scrapes) — " << vectorLabels_.size()
                 << " vectors remaining";
      }
    }
    // Re-save so pruned data doesn't come back next startup.
    if (!docSources.empty())
      saveToDisk();
    return;
  }

  // ── Try legacy format: rag_chunks.json with base64 embeddings ───────
  //
  // Old format:  [ { "id": "...", "source": "...", "title": "...",
  //                  "text": "...", "embedding": "<base64>",
  //                  "embeddingSize": 768 }, ... ]
  //
  // The "embedding" field is base64-encoded little-endian float[embeddingSize].
  {
    std::string legacyPath = storagePath_ + "/rag_chunks.json";
    std::ifstream in(legacyPath);
    if (!in.good()) {
      LOG_WARN << "RAG: no legacy rag_chunks.json found — "
                  "documents exist but need re-upload";
      // Clear documents so hasDocuments() returns false and
      // the LLM doesn't enter RAG mode with zero vectors.
      {
        std::lock_guard<std::mutex> lock(docsMutex_);
        documents_.clear();
      }
      // Persist the cleared state so the stale rag_documents.json
      // doesn't confuse the next startup either.
      saveToDisk();
      return;
    }

    Json::CharReaderBuilder rb;
    Json::Value root;
    std::string errs;
    if (!Json::parseFromStream(rb, in, &root, &errs) || !root.isArray()) {
      LOG_ERROR << "RAG: failed to parse legacy " << legacyPath;
      std::lock_guard<std::mutex> lock(docsMutex_);
      documents_.clear();
      return;
    }

    LOG_INFO << "RAG: migrating " << root.size()
             << " chunks from legacy rag_chunks.json";

    std::lock_guard<std::mutex> lock(storeMutex_);
    vectors_.clear();
    vectorLabels_.clear();
    chunkMeta_.clear();
    embDim_ = 0;
    nextLabel_ = 0;
    nextId_ = 0;

    for (const auto &jc : root) {
      std::string b64 = jc.get("embedding", "").asString();
      size_t dim = static_cast<size_t>(jc.get("embeddingSize", 0).asUInt64());
      if (b64.empty() || dim == 0)
        continue;

      // ── Base64 decode ──────────────────────────────────────
      // Standard base64 decode table.
      static const int b64Table[256] = {
          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
          52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
          -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14,
          15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
          -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
          41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      };

      std::vector<unsigned char> raw;
      raw.reserve(b64.size() * 3 / 4);
      uint32_t accum = 0;
      int bits = 0;
      for (unsigned char ch : b64) {
        int val = b64Table[ch];
        if (val < 0)
          continue; // skip '=', whitespace, etc.
        accum = (accum << 6) | static_cast<uint32_t>(val);
        bits += 6;
        if (bits >= 8) {
          bits -= 8;
          raw.push_back(static_cast<unsigned char>((accum >> bits) & 0xFF));
        }
      }

      size_t expectedBytes = dim * sizeof(float);
      if (raw.size() < expectedBytes) {
        LOG_WARN << "RAG: skipping legacy chunk — decoded " << raw.size()
                 << " bytes, expected " << expectedBytes;
        continue;
      }

      // Interpret as float array (little-endian, same as platform).
      std::vector<float> embedding(dim);
      std::memcpy(embedding.data(), raw.data(), expectedBytes);

      // Normalise for inner-product search.
      normalizeVector(embedding);

      if (embDim_ == 0) {
        embDim_ = dim;
        LOG_INFO << "RAG: legacy embedding dimension = " << embDim_;
      }

      size_t label = nextLabel_++;
      RagChunkMeta meta;
      meta.id = jc.get("id", "chunk_" + std::to_string(nextId_)).asString();
      meta.source = jc.get("source", "").asString();
      meta.title = jc.get("title", "").asString();
      meta.text = jc.get("text", "").asString();
      nextId_++;

      vectors_.insert(vectors_.end(), embedding.begin(), embedding.end());
      vectorLabels_.push_back(label);
      chunkMeta_[label] = std::move(meta);
    }

    LOG_INFO << "RAG: migrated " << vectorLabels_.size()
             << " vectors from legacy format (dim=" << embDim_ << ")";
  }

  // If migration produced nothing, clear documents so hasDocuments()
  // returns false and the LLM doesn't enter RAG mode with zero vectors.
  {
    bool empty = false;
    {
      std::lock_guard<std::mutex> sLock(storeMutex_);
      empty = vectorLabels_.empty();
    }
    if (empty) {
      LOG_WARN << "RAG: no vectors loaded — clearing document registry";
      std::lock_guard<std::mutex> dLock(docsMutex_);
      documents_.clear();
      return;
    }
  }

  // Save in the new format so next startup is fast.
  saveToDisk();
}
