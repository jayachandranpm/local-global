#pragma once

#include <string>
#include <vector>
#include <functional>

/// A single web search result.
struct SearchResult {
    std::string title;
    std::string url;
    std::string snippet;
};

/**
 * DuckDuckGoService — scrapes the DuckDuckGo HTML-only endpoint for
 * web search results.  Returns at most 5 results per query.
 */
class DuckDuckGoService {
public:
    using SearchCallback =
        std::function<void(std::vector<SearchResult> results,
                           const std::string &error)>;

    /// Perform a web search asynchronously (uses Drogon's HTTP client).
    static void search(const std::string &query, SearchCallback cb);
};
