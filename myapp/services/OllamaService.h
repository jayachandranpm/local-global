#pragma once

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <memory>
#include <json/json.h>
#include "models/Message.h"

/**
 * OllamaService — talks to the local Ollama REST API.
 *
 * All methods are asynchronous and schedule work on Drogon's event loop
 * (trantor), so they never block the calling thread.
 */
/// Info about a single Ollama model.
struct ModelInfo {
    std::string name;
    int contextLength = 4096;  // default fallback
};

class OllamaService {
public:
    /// Token-by-token callback: receives each text fragment as it arrives.
    using TokenCallback = std::function<void(const std::string &token)>;
    /// Called when the stream finishes (empty string) or on error (error msg).
    using DoneCallback  = std::function<void(const std::string &error)>;
    /// Called with the full accumulated response text on completion.
    using ChatCallback  = std::function<void(const std::string &fullReply,
                                             const std::string &error)>;
    /// Model-list result callback (names only, for backwards compat).
    using ModelsCallback = std::function<void(std::vector<std::string> models,
                                              const std::string &error)>;
    /// Model-list with context info callback.
    using ModelsInfoCallback = std::function<void(std::vector<ModelInfo> models,
                                                   const std::string &error)>;

    /// Shared flag that callers can set to true to abort a streaming request.
    using AbortFlag = std::shared_ptr<std::atomic<bool>>;

    /// Token usage stats from Ollama's final streaming chunk.
    struct TokenStats {
        int evalCount       = 0;   // tokens generated
        int promptEvalCount = 0;   // prompt tokens evaluated
        double evalDuration = 0;   // generation time in seconds
        double totalDuration = 0;  // total request time in seconds
    };
    using StatsCallback = std::function<void(const TokenStats &stats)>;
    /// Called with thinking/reasoning tokens from models that support it (e.g. qwen3.5).
    using ThinkingCallback = std::function<void(const std::string &token)>;

    /// Stream a chat completion.  `onToken` is called for every chunk; `onDone`
    /// fires once at the end (with an empty error string on success).
    /// `onStats` (optional) is called with token usage stats from the final chunk.
    /// Returns an AbortFlag — set it to true to cancel the request.
    /// If `existingFlag` is provided, it is used instead of creating a new one
    /// (allows pre-registering the flag before streaming begins).
    static AbortFlag streamChat(const std::string &model,
                                const std::vector<Message> &messages,
                                double temperature,
                                TokenCallback onToken,
                                DoneCallback  onDone,
                                StatsCallback onStats = nullptr,
                                AbortFlag existingFlag = nullptr,
                                ThinkingCallback onThinking = nullptr,
                                bool enableThinking = true);

    /// Convenience wrapper: collects the entire response into a single string
    /// and hands it to `cb`.
    static void chat(const std::string &model,
                     const std::vector<Message> &messages,
                     double temperature,
                     ChatCallback cb);

    /// Fetch the list of locally-available model names.
    static void listModels(ModelsCallback cb);

    /// Fetch models with context length info (calls /api/show for each).
    static void listModelsWithInfo(ModelsInfoCallback cb);
};
