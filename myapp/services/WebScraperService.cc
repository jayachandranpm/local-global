#include "services/WebScraperService.h"

#include <curl/curl.h>
#include <drogon/drogon.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

// ─── curl write callback ────────────────────────────────────────────────────
static size_t scraperWriteCb(char *data, size_t size, size_t nmemb,
                             void *userp) {
  auto *buf = static_cast<std::string *>(userp);
  size_t total = size * nmemb;
  // Limit download to ~500 KB to avoid huge pages
  if (buf->size() + total > 500 * 1024) {
    size_t remaining = 500 * 1024 - buf->size();
    if (remaining > 0) {
      // Back up to the last '<' so we don't split mid-tag,
      // which would inject garbage into the HTML parser.
      while (remaining > 0 && data[remaining - 1] != '<')
        --remaining;
      if (remaining > 0)
        buf->append(data, remaining);
    }
    return total; // still return total so curl doesn't error
  }
  buf->append(data, total);
  return total;
}

// ─── HTML tag/element helpers ───────────────────────────────────────────────

/// Tags whose content should be completely removed (not just the tag).
static const std::set<std::string> SKIP_ELEMENTS = {
    "script", "style",  "noscript", "svg",   "path", "iframe",
    "nav",    "footer", "header",   "aside", "form", "button",
    "input",  "select", "textarea", "meta",  "link", "head"};

/// Block-level elements that should produce line breaks.
static const std::set<std::string> BLOCK_ELEMENTS = {
    "div",     "p",       "br",   "h1",         "h2",         "h3",
    "h4",      "h5",      "h6",   "li",         "ul",         "ol",
    "table",   "tr",      "td",   "th",         "blockquote", "pre",
    "article", "section", "main", "figcaption", "dt",         "dd"};

static std::string toLower(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}

// ─── extractTitle ───────────────────────────────────────────────────────────
std::string WebScraperService::extractTitle(const std::string &html) {
  std::string lower = toLower(html);
  auto start = lower.find("<title");
  if (start == std::string::npos)
    return "";

  auto tagEnd = lower.find('>', start);
  if (tagEnd == std::string::npos)
    return "";

  auto closeTag = lower.find("</title>", tagEnd);
  if (closeTag == std::string::npos)
    return "";

  std::string title = html.substr(tagEnd + 1, closeTag - tagEnd - 1);

  // Strip any remaining tags inside <title>
  std::string clean;
  bool inTag = false;
  for (char c : title) {
    if (c == '<') {
      inTag = true;
      continue;
    }
    if (c == '>') {
      inTag = false;
      continue;
    }
    if (!inTag)
      clean += c;
  }
  return normalizeWhitespace(clean);
}

// ─── extractText ────────────────────────────────────────────────────────────
std::string WebScraperService::extractText(const std::string &html) {
  std::string out;
  out.reserve(html.size() / 3);

  size_t i = 0;
  int skipDepth = 0;   // depth inside a SKIP_ELEMENTS element
  std::string skipTag; // which tag we're skipping

  while (i < html.size()) {
    // Check for comment
    if (i + 3 < html.size() && html[i] == '<' && html[i + 1] == '!' &&
        html[i + 2] == '-' && html[i + 3] == '-') {
      auto end = html.find("-->", i + 4);
      i = (end != std::string::npos) ? end + 3 : html.size();
      continue;
    }

    // Check for tag
    if (html[i] == '<') {
      auto tagEnd = html.find('>', i);
      if (tagEnd == std::string::npos) {
        i++;
        continue;
      }

      std::string tagRaw = html.substr(i + 1, tagEnd - i - 1);
      std::string tagLower = toLower(tagRaw);

      // Check if it's a closing tag
      bool isClose = (!tagLower.empty() && tagLower[0] == '/');
      bool isSelfClose = (!tagLower.empty() && tagLower.back() == '/');

      // Extract tag name
      std::string tagName;
      size_t nameStart = isClose ? 1 : 0;
      for (size_t j = nameStart; j < tagLower.size(); ++j) {
        if (std::isalnum(static_cast<unsigned char>(tagLower[j])) ||
            tagLower[j] == '-')
          tagName += tagLower[j];
        else
          break;
      }

      // Handle skip elements
      if (skipDepth > 0) {
        if (isClose && tagName == skipTag) {
          skipDepth--;
        } else if (!isClose && !isSelfClose && tagName == skipTag) {
          skipDepth++;
        }
        i = tagEnd + 1;
        continue;
      }

      if (!isClose && !isSelfClose && SKIP_ELEMENTS.count(tagName)) {
        skipTag = tagName;
        skipDepth = 1;
        i = tagEnd + 1;
        continue;
      }

      // Block elements produce a newline
      if (BLOCK_ELEMENTS.count(tagName)) {
        out += '\n';
      }

      // <br> always produces a newline
      if (tagName == "br") {
        out += '\n';
      }

      i = tagEnd + 1;
      continue;
    }

    // Skip content inside skip elements
    if (skipDepth > 0) {
      i++;
      continue;
    }

    // Decode common HTML entities inline
    if (html[i] == '&') {
      // Look for entity
      auto semi = html.find(';', i);
      if (semi != std::string::npos && semi - i < 10) {
        std::string entity = toLower(html.substr(i, semi - i + 1));
        if (entity == "&amp;") {
          out += '&';
          i = semi + 1;
          continue;
        }
        if (entity == "&lt;") {
          out += '<';
          i = semi + 1;
          continue;
        }
        if (entity == "&gt;") {
          out += '>';
          i = semi + 1;
          continue;
        }
        if (entity == "&quot;") {
          out += '"';
          i = semi + 1;
          continue;
        }
        if (entity == "&apos;") {
          out += '\'';
          i = semi + 1;
          continue;
        }
        if (entity == "&#39;") {
          out += '\'';
          i = semi + 1;
          continue;
        }
        if (entity == "&nbsp;") {
          out += ' ';
          i = semi + 1;
          continue;
        }
        if (entity == "&#x27;") {
          out += '\'';
          i = semi + 1;
          continue;
        }
        if (entity == "&#x2f;") {
          out += '/';
          i = semi + 1;
          continue;
        }
        // Numeric entities
        if (entity.size() > 3 && entity[1] == '#') {
          int codePoint = 0;
          if (entity[2] == 'x' || entity[2] == 'X') {
            // Hex
            for (size_t j = 3; j < entity.size() - 1; ++j) {
              char c = entity[j];
              if (c >= '0' && c <= '9')
                codePoint = codePoint * 16 + (c - '0');
              else if (c >= 'a' && c <= 'f')
                codePoint = codePoint * 16 + 10 + (c - 'a');
            }
          } else {
            for (size_t j = 2; j < entity.size() - 1; ++j) {
              char c = entity[j];
              if (c >= '0' && c <= '9')
                codePoint = codePoint * 10 + (c - '0');
            }
          }
          if (codePoint > 0 && codePoint < 128) {
            out += static_cast<char>(codePoint);
          } else {
            out += ' '; // non-ASCII entity → space
          }
          i = semi + 1;
          continue;
        }
        // Unknown entity — keep as-is
      }
    }

    out += html[i];
    i++;
  }

  return out;
}

// ─── normalizeWhitespace ────────────────────────────────────────────────────
std::string WebScraperService::normalizeWhitespace(const std::string &text) {
  std::string out;
  out.reserve(text.size());

  bool lastWasSpace = false;
  int newlineCount = 0;

  for (char c : text) {
    if (c == '\n' || c == '\r') {
      newlineCount++;
      if (newlineCount <= 2) {
        out += '\n';
      }
      lastWasSpace = true;
      continue;
    }
    if (c == ' ' || c == '\t') {
      if (!lastWasSpace) {
        out += ' ';
        lastWasSpace = true;
      }
      continue;
    }
    newlineCount = 0;
    lastWasSpace = false;
    out += c;
  }

  // Trim leading/trailing whitespace
  auto start = out.find_first_not_of(" \t\r\n");
  if (start == std::string::npos)
    return "";
  auto end = out.find_last_not_of(" \t\r\n");
  return out.substr(start, end - start + 1);
}

// ─── scrapeSingle ───────────────────────────────────────────────────────────
ScrapedPage WebScraperService::scrapeSingle(const std::string &url,
                                            int maxContentChars) {
  ScrapedPage page;
  page.url = url;

  CURL *curl = curl_easy_init();
  if (!curl) {
    page.error = "Failed to initialise curl";
    return page;
  }

  std::string body;
  body.reserve(500 * 1024);

  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(
      headers,
      "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
      "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36");
  headers =
      curl_slist_append(headers, "Accept: text/html,application/xhtml+xml");
  headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, scraperWriteCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");
  // SSL verification enabled for MITM protection
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

  CURLcode res = curl_easy_perform(curl);
  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    page.error = std::string("Fetch failed: ") + curl_easy_strerror(res);
    return page;
  }
  if (httpCode != 200) {
    page.error = "HTTP " + std::to_string(httpCode);
    return page;
  }

  // Extract title and text content
  page.title = extractTitle(body);
  std::string text = extractText(body);
  text = normalizeWhitespace(text);

  // Truncate to maxContentChars
  if (static_cast<int>(text.size()) > maxContentChars) {
    text = text.substr(0, maxContentChars);
    // Try to break at a sentence/word boundary
    auto lastPeriod = text.rfind(". ");
    auto lastNewline = text.rfind('\n');
    size_t breakAt = std::max(lastPeriod, lastNewline);
    if (breakAt != std::string::npos && breakAt > text.size() / 2) {
      text = text.substr(0, breakAt + 1);
    }
    text += "\n[…content truncated]";
  }

  page.content = text;
  page.success = !text.empty();

  if (text.empty()) {
    page.error = "No readable content extracted";
  }

  LOG_INFO << "Scraped " << url << ": " << text.size() << " chars"
           << (page.success ? "" : " (FAILED)");

  return page;
}

// ─── scrapeUrls (parallel) ──────────────────────────────────────────────────
void WebScraperService::scrapeUrls(const std::vector<std::string> &urls,
                                   ScrapeCallback cb, int maxContentChars) {
  if (urls.empty()) {
    if (cb) {
      auto *loop = drogon::app().getLoop();
      loop->queueInLoop([cb]() { cb({}); });
    }
    return;
  }

  auto *loop = drogon::app().getLoop();

  // Scrape all URLs in parallel (one thread per URL) to reduce latency.
  std::thread([urls, cb, loop, maxContentChars]() {
    std::vector<ScrapedPage> pages(urls.size());
    std::vector<std::thread> workers;
    workers.reserve(urls.size());

    for (size_t i = 0; i < urls.size(); ++i) {
      workers.emplace_back([&pages, &urls, i, maxContentChars]() {
        pages[i] = scrapeSingle(urls[i], maxContentChars);
      });
    }
    for (auto &w : workers)
      w.join();

    if (cb) {
      loop->queueInLoop(
          [cb, pages = std::move(pages)]() mutable { cb(std::move(pages)); });
    }
  }).detach();
}
