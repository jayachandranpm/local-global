#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * RagService — Retrieval-Augmented Generation with Ollama embeddings
 *              and exact flat inner-product vector search.
 *
 * Uses a FAISS-style IndexFlatIP algorithm: all embedding vectors are
 * stored in a contiguous row-major array and search computes exact
 * inner products against every vector (100 % recall, zero approximation).
 *
 * Provides:
 *   • Text chunking with configurable size and overlap
 *   • Embedding generation via Ollama's /api/embed endpoint
 *   • Exact nearest-neighbor search (no approximation loss)
 *   • Async retrieval of relevant chunks for a query
 *
 * Thread-safety: the vector store is guarded by a mutex.
 */

/// A single chunk of text with its embedding vector.
struct RagChunk {
  std::string id;               // unique chunk identifier
  std::string source;           // source URL or document name
  std::string title;            // document title
  std::string text;             // the actual text content
  std::vector<float> embedding; // embedding vector
  double score = 0.0;           // similarity score (set during retrieval)
};

/// Internal metadata for a chunk stored in the index.
struct RagChunkMeta {
  std::string id;     // unique chunk identifier
  std::string source; // source URL or document name
  std::string title;  // document title
  std::string text;   // the actual text content
};

/// Metadata for an uploaded document tracked in the RAG store.
struct RagDocument {
  std::string id;         // unique document ID
  std::string name;       // original filename
  std::string source;     // internal source key (matches RagChunk::source)
  size_t sizeBytes = 0;   // original file size
  int chunks = 0;         // number of chunks indexed
  int64_t uploadedAt = 0; // epoch millis
};

class RagService {
public:
  /// Singleton accessor.
  static RagService &instance();

  ~RagService();

  /// Result callback for async operations.
  using RetrieveCallback = std::function<void(std::vector<RagChunk> chunks,
                                              const std::string &error)>;
  using IndexCallback =
      std::function<void(int chunksIndexed, const std::string &error)>;

  /// Split text into overlapping chunks.
  /// @param maxChunks  Hard limit on the number of chunks returned (0 = default
  /// 500).
  static std::vector<std::string> chunkText(const std::string &text,
                                            int chunkSize = 500,
                                            int overlap = 100,
                                            int maxChunks = 0);

  /// Index a document: chunk it, compute embeddings, store in vector store.
  /// Runs in a background thread; calls `cb` on Drogon's event loop.
  void indexDocument(const std::string &source, const std::string &title,
                     const std::string &text, const std::string &embeddingModel,
                     IndexCallback cb);

  /// Index multiple documents at once.
  void indexDocuments(
      const std::vector<std::tuple<std::string, std::string, std::string>>
          &docs,
      const std::string &embeddingModel, IndexCallback cb);

  /// Ephemeral index + retrieve: chunk & embed docs in a temporary store,
  /// retrieve the top-k chunks most relevant to `query`, then discard
  /// everything.  Nothing touches the persistent vector store or disk.
  /// Designed for web-search scrapes that should NOT pollute the
  /// long-lived uploaded-document index.
  void indexAndRetrieveEphemeral(
      const std::vector<std::tuple<std::string, std::string, std::string>>
          &docs,
      const std::string &query, const std::string &embeddingModel, int topK,
      std::function<void(int chunksIndexed, std::vector<RagChunk> results,
                         const std::string &error)>
          cb);

  /// Retrieve the top-k most relevant chunks for a query.
  /// Runs in a background thread (needs to compute query embedding).
  void retrieve(const std::string &query, const std::string &embeddingModel,
                int topK, RetrieveCallback cb);

  /// Synchronous retrieval (blocks — computes embedding + search).
  std::vector<RagChunk> retrieveSync(const std::string &query,
                                     const std::string &embeddingModel,
                                     int topK, std::string &error);

  /// Clear the entire vector store and document registry.
  void clear();

  /// Clear chunks from a specific source.
  void clearSource(const std::string &source);

  /// Get the number of stored chunks.
  size_t size() const;

  /// Check if the store has chunks from a given source.
  bool hasSource(const std::string &source) const;

  // ── Document management ──────────────────────────────────────────────

  /// Register a document (call before or after indexing).
  void addDocument(const RagDocument &doc);

  /// Update the chunk count of an existing document.
  void updateDocumentChunks(const std::string &docId, int chunks);

  /// Remove a document and all its chunks.
  bool removeDocument(const std::string &docId);

  /// Get the list of all uploaded documents.
  std::vector<RagDocument> listDocuments() const;

  /// Check whether any user documents are indexed.
  bool hasDocuments() const;

  // ── Persistence ──────────────────────────────────────────────────────

  /// Set the directory where RAG data is persisted.
  void setStoragePath(const std::string &dirPath);

  /// Save the vector store and document registry to disk.
  void saveToDisk() const;

  /// Load the vector store and document registry from disk.
  void loadFromDisk();

  /// Check whether an embedding model is available in Ollama.
  /// Returns true if a test embedding succeeds, false otherwise.
  /// On failure, `error` contains the reason.
  static bool checkEmbeddingModel(const std::string &model, std::string &error);

private:
  RagService() = default;

  /// Compute embedding for a single text via Ollama /api/embed.
  static std::vector<float> computeEmbedding(const std::string &text,
                                             const std::string &model,
                                             std::string &error);

  /// Compute embeddings for a batch of texts via Ollama /api/embed.
  /// Can accept a reusable CURL* handle for connection keep-alive.
  static std::vector<std::vector<float>>
  computeEmbeddings(const std::vector<std::string> &texts,
                    const std::string &model, std::string &error,
                    void *sharedCurl = nullptr);

  /// Cosine similarity between two vectors (utility).
  static double cosineSimilarity(const std::vector<float> &a,
                                 const std::vector<float> &b);

  /// Normalize a vector to unit length (inner-product ≡ cosine).
  static void normalizeVector(std::vector<float> &v);

  mutable std::mutex storeMutex_;

  // ── Flat vector index (exact search — FAISS IndexFlatIP equivalent) ──
  //
  //   vectors_      — contiguous row-major float array
  //                    layout: [v0[0..dim-1], v1[0..dim-1], ...]
  //   vectorLabels_ — one label per vector row (maps to chunkMeta_)
  //
  std::vector<float> vectors_;
  std::vector<size_t> vectorLabels_;
  std::unordered_map<size_t, RagChunkMeta> chunkMeta_;
  size_t embDim_ = 0;    // embedding dimension (set on first add)
  size_t nextLabel_ = 0; // next label to assign
  int nextId_ = 0;       // for chunk string IDs

  // ── Document registry ────────────────────────────────────────────────
  mutable std::mutex docsMutex_;
  std::vector<RagDocument> documents_;
  int nextDocId_ = 0;

  std::string storagePath_;
};
