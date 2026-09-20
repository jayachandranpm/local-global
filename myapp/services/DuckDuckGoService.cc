#include "services/DuckDuckGoService.h"

#include <drogon/drogon.h>
#include <curl/curl.h>
#include <json/json.h>
#include <sstream>
#include <algorithm>
#include <thread>

// ─── Lightweight HTML helpers ───────────────────────────────────────────────

static std::string decodeEntities(const std::string &s)
{
    std::string out = s;
    auto replaceAll = [&out](const std::string &from, const std::string &to) {
        size_t pos = 0;
        while ((pos = out.find(from, pos)) != std::string::npos) {
            out.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replaceAll("&amp;",  "&");
    replaceAll("&lt;",   "<");
    replaceAll("&gt;",   ">");
    replaceAll("&quot;", "\"");
    replaceAll("&#x27;", "'");
    replaceAll("&apos;", "'");
    replaceAll("&#39;",  "'");
    replaceAll("&#x2F;", "/");
    replaceAll("&nbsp;", " ");
    return out;
}

static std::string stripTags(const std::string &html)
{
    std::string out;
    out.reserve(html.size());
    bool inTag = false;
    for (char c : html) {
        if (c == '<') { inTag = true; continue; }
        if (c == '>') { inTag = false; continue; }
        if (!inTag) out += c;
    }
    return out;
}

static std::string trim(const std::string &s)
{
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string extractAttr(const std::string &tag, const std::string &attr)
{
    // Try attr="value" and attr='value'
    for (char q : {'"', '\''}) {
        std::string needle = attr + "=" + q;
        auto pos = tag.find(needle);
        if (pos == std::string::npos) continue;
        pos += needle.size();
        auto end = tag.find(q, pos);
        if (end == std::string::npos) continue;
        return tag.substr(pos, end - pos);
    }
    return "";
}

static std::string urlDecode(const std::string &encoded)
{
    std::string out;
    out.reserve(encoded.size());
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {
            int hi = 0, lo = 0;
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                return -1;
            };
            hi = hexVal(encoded[i+1]);
            lo = hexVal(encoded[i+2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        } else if (encoded[i] == '+') {
            out += ' ';
            continue;
        }
        out += encoded[i];
    }
    return out;
}

// ─── curl helpers ───────────────────────────────────────────────────────────

static size_t curlWriteString(char *data, size_t size, size_t nmemb, void *userp)
{
    auto *str = static_cast<std::string *>(userp);
    str->append(data, size * nmemb);
    return size * nmemb;
}

static std::string curlUrlEncode(const std::string &s)
{
    CURL *curl = curl_easy_init();
    if (!curl) return s;
    char *enc = curl_easy_escape(curl, s.c_str(), (int)s.size());
    std::string result = enc ? enc : s;
    if (enc) curl_free(enc);
    curl_easy_cleanup(curl);
    return result;
}

// ─── Robust HTML parser ─────────────────────────────────────────────────────
// DuckDuckGo HTML results contain:
//   <a class="result__a" href="/redirect?uddg=...">Title</a>
//   <a class="result__snippet" href="...">Snippet...</a>
// We also handle <td class="result__snippet"> as a fallback.

static std::vector<SearchResult> parseResults(const std::string &html,
                                               int maxResults = 5)
{
    std::vector<SearchResult> results;
    const std::string anchorMarker  = "class=\"result__a\"";
    // Multiple snippet selectors for robustness.
    const std::vector<std::string> snippetMarkers = {
        "class=\"result__snippet\"",
        "class='result__snippet'",
        "class=\"result__body\"",
    };

    size_t pos = 0;
    while (results.size() < static_cast<size_t>(maxResults)) {
        pos = html.find(anchorMarker, pos);
        if (pos == std::string::npos) break;

        size_t tagStart = html.rfind('<', pos);
        size_t tagEnd   = html.find('>', pos);
        if (tagStart == std::string::npos || tagEnd == std::string::npos) {
            pos++; continue;
        }

        std::string openTag = html.substr(tagStart, tagEnd - tagStart + 1);
        std::string href = extractAttr(openTag, "href");

        // Extract title (inner text of <a>).
        size_t innerStart = tagEnd + 1;
        size_t innerEnd   = html.find("</a>", innerStart);
        std::string title;
        if (innerEnd != std::string::npos)
            title = trim(stripTags(decodeEntities(
                        html.substr(innerStart, innerEnd - innerStart))));

        pos = (innerEnd != std::string::npos) ? innerEnd + 4 : tagEnd + 1;

        // Find snippet using multiple fallback selectors.
        std::string snippet;
        size_t bestSnipPos = std::string::npos;
        for (auto &marker : snippetMarkers) {
            size_t sp = html.find(marker, pos);
            if (sp != std::string::npos &&
                (bestSnipPos == std::string::npos || sp < bestSnipPos))
                bestSnipPos = sp;
        }
        if (bestSnipPos != std::string::npos) {
            // Limit how far ahead we look (don't cross into the next result).
            size_t nextResult = html.find(anchorMarker, pos);
            if (nextResult == std::string::npos || bestSnipPos < nextResult) {
                size_t snipTagEnd = html.find('>', bestSnipPos);
                if (snipTagEnd != std::string::npos) {
                    size_t snipInnerStart = snipTagEnd + 1;
                    // Try multiple closing tags.
                    size_t snipInnerEnd = std::string::npos;
                    for (auto &closer : {"</a>", "</td>", "</span>", "</div>"}) {
                        size_t ce = html.find(closer, snipInnerStart);
                        if (ce != std::string::npos &&
                            (snipInnerEnd == std::string::npos || ce < snipInnerEnd))
                            snipInnerEnd = ce;
                    }
                    if (snipInnerEnd != std::string::npos && snipInnerEnd > snipInnerStart) {
                        snippet = trim(stripTags(decodeEntities(
                                    html.substr(snipInnerStart,
                                                snipInnerEnd - snipInnerStart))));
                    }
                }
            }
        }

        // Resolve DuckDuckGo redirect URLs.
        if (href.find("uddg=") != std::string::npos) {
            auto p = href.find("uddg=");
            std::string encoded = href.substr(p + 5);
            auto ampPos = encoded.find('&');
            if (ampPos != std::string::npos)
                encoded = encoded.substr(0, ampPos);
            href = urlDecode(encoded);
        }
        // Also handle //duckduckgo.com/l/?... redirect wrapper.
        if (href.find("/l/?uddg=") != std::string::npos ||
            href.find("duckduckgo.com/l/?") != std::string::npos)
        {
            auto p = href.find("uddg=");
            if (p != std::string::npos) {
                std::string encoded = href.substr(p + 5);
                auto ampPos = encoded.find('&');
                if (ampPos != std::string::npos)
                    encoded = encoded.substr(0, ampPos);
                href = urlDecode(encoded);
            }
        }

        // Skip empty, ad, or DuckDuckGo internal links.
        if (title.empty() || href.empty()) { continue; }
        if (href.find("duckduckgo.com") != std::string::npos) { continue; }
        if (href[0] == '/') { continue; }

        results.push_back({title, href, snippet});
    }

    LOG_INFO << "DuckDuckGo search: parsed " << results.size() << " results";
    return results;
}

// ─── search (via libcurl — fast, no Drogon SSL overhead) ────────────────────
void DuckDuckGoService::search(const std::string &query, SearchCallback cb)
{
    // Run in a thread to avoid blocking the event loop.
    auto *loop = drogon::app().getLoop();

    std::thread([query, cb, loop]() {
        CURL *curl = curl_easy_init();
        if (!curl) {
            if (cb) loop->queueInLoop([cb]() {
                cb({}, "Failed to initialise curl for search");
            });
            return;
        }

        std::string url = "https://html.duckduckgo.com/html/?q=" + curlUrlEncode(query);
        std::string body;

        struct curl_slist *headers = nullptr;
        headers = curl_slist_append(headers,
            "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
            "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36");
        headers = curl_slist_append(headers, "Accept: text/html");
        headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteString);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 6L);            // 6s max
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);     // 3s connect
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);     // follow redirects
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
        // Accept compressed responses for faster transfer.
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            std::string err = "DuckDuckGo request failed: ";
            err += curl_easy_strerror(res);
            if (cb) loop->queueInLoop([cb, err]() { cb({}, err); });
            return;
        }
        if (httpCode != 200) {
            std::string err = "DuckDuckGo returned HTTP " + std::to_string(httpCode);
            if (cb) loop->queueInLoop([cb, err]() { cb({}, err); });
            return;
        }

        auto results = parseResults(body, 5);
        if (cb) {
            loop->queueInLoop([cb, results = std::move(results)]() mutable {
                cb(std::move(results), "");
            });
        }
    }).detach();
}
