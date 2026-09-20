#pragma once

#include <string>
#include <vector>
#include <functional>

/// Scraped page content.
struct ScrapedPage {
    std::string url;
    std::string title;
    std::string content;   // cleaned plain-text body
    bool        success = false;
    std::string error;
};

/**
 * WebScraperService — fetches web pages and extracts clean text content.
 *
 * Uses libcurl to download pages and a lightweight HTML stripper to produce
 * readable plain text suitable for LLM consumption.
 */
class WebScraperService {
public:
    using ScrapeCallback = std::function<void(std::vector<ScrapedPage> pages)>;

    /// Scrape multiple URLs in parallel (one thread per URL, max concurrency).
    /// Calls `cb` on Drogon's event loop once all pages are fetched.
    static void scrapeUrls(const std::vector<std::string> &urls,
                           ScrapeCallback cb,
                           int maxContentChars = 6000);

    /// Scrape a single URL synchronously (blocks the calling thread).
    static ScrapedPage scrapeSingle(const std::string &url,
                                     int maxContentChars = 6000);

private:
    /// Strip HTML tags and extract readable text.
    static std::string extractText(const std::string &html);

    /// Extract the <title> element.
    static std::string extractTitle(const std::string &html);

    /// Collapse excessive whitespace.
    static std::string normalizeWhitespace(const std::string &text);
};
