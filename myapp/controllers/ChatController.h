#pragma once

#include <drogon/WebSocketController.h>
#include <drogon/HttpSimpleController.h>

/**
 * ChatController handles:
 *   GET  /            → serves index.html
 *   WebSocket /ws/chat → bidirectional chat streaming
 */

// ─── HTTP: serve the root page ──────────────────────────────────────────────
class IndexController : public drogon::HttpSimpleController<IndexController>
{
public:
    void asyncHandleHttpRequest(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) override;

    PATH_LIST_BEGIN
        PATH_ADD("/", drogon::Get);
    PATH_LIST_END
};

// ─── WebSocket: /ws/chat ────────────────────────────────────────────────────
class ChatWebSocket : public drogon::WebSocketController<ChatWebSocket>
{
public:
    void handleNewMessage(const drogon::WebSocketConnectionPtr &conn,
                          std::string &&message,
                          const drogon::WebSocketMessageType &type) override;

    void handleNewConnection(const drogon::HttpRequestPtr &req,
                             const drogon::WebSocketConnectionPtr &conn) override;

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr &conn) override;

    WS_PATH_LIST_BEGIN
        WS_PATH_ADD("/ws/chat");
    WS_PATH_LIST_END
};
