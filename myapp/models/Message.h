#pragma once
#include <string>

/// Represents a single message in a chat conversation.
struct Message {
    std::string role;      // "user", "assistant", "system", or "tool"
    std::string content;   // The text body of the message
    std::string toolName;  // Optional: name of the tool that produced this message
};
