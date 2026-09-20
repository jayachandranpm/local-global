# Local Global — Architecture Documentation

> **Version 1.0.0** · A fully offline, privacy-first AI desktop assistant for macOS.

---

## Table of Contents

1. [Overview](#overview)
2. [High-Level Architecture](#high-level-architecture)
3. [Technology Stack](#technology-stack)
4. [Directory Structure](#directory-structure)
5. [Backend (C++ / Drogon)](#backend-c--drogon)
   - [Entry Point — `main.cc`](#entry-point--maincc)
   - [Controllers](#controllers)
   - [Services](#services)
   - [Models](#models)
6. [Frontend (Alpine.js SPA)](#frontend-alpinejs-spa)
7. [Text-to-Speech (TTS)](#text-to-speech-tts)
8. [RAG Pipeline](#rag-pipeline)
9. [MCP (Model Context Protocol)](#mcp-model-context-protocol)
10. [WebSocket Protocol](#websocket-protocol)
11. [Build System](#build-system)
12. [Data Flow Diagrams](#data-flow-diagrams)
13. [Prerequisites & Setup](#prerequisites--setup)
14. [Security & Privacy](#security--privacy)

---

## Overview

**Local Global** is a native macOS desktop application that runs **entirely on your machine** — no cloud, no API keys, no data leaving your computer. It combines:

- A **C++ backend** (Drogon framework) serving HTTP and WebSocket APIs on `127.0.0.1:8080`
- A **native macOS webview** (WKWebView via `webview/webview`) as the application window
- **Ollama** as the local LLM runtime (port `11434`)
- A **Python TTS server** (kitten-tts-nano) for voice synthesis (port `8787`)
- An **Alpine.js** single-page frontend with real-time streaming

The app is distributed as a `.app` bundle with all assets embedded.

---

## High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        macOS Application                        │
│                                                                 │
│  ┌──────────────────┐    ┌──────────────────────────────────┐  │
│  │   Native Webview  │    │   Drogon HTTP/WS Server          │  │
│  │   (WKWebView)     │───▶│   127.0.0.1:8080                 │  │
│  │                    │    │                                   │  │
│  │  ┌──────────────┐ │    │  ┌─────────────┐ ┌────────────┐ │  │
│  │  │ Alpine.js SPA│ │    │  │ Controllers  │ │  Services  │ │  │
│  │  │ index.html   │ │    │  │             │ │            │ │  │
│  │  │ app.js       │ │    │  │ IndexCtrl   │ │ OllamaSvc  │ │  │
│  │  └──────────────┘ │    │  │ ChatWS      │ │ McpSvc     │ │  │
│  └──────────────────┘    │  │ ModelsCtrl  │ │ RagSvc     │ │  │
│                           │  │ SearchCtrl  │ │ DDGSvc     │ │  │
│                           │  │ RAG Ctrls   │ │ ScraperSvc │ │  │
│                           │  └─────────────┘ └────────────┘ │  │
│                           └──────────┬───────────────────────┘  │
│                                      │                          │
│  ┌──────────────────┐    ┌──────────▼───────────────────────┐  │
│  │  TTS Server       │    │   Ollama LLM Runtime             │  │
│  │  (Python)         │    │   127.0.0.1:11434                │  │
│  │  127.0.0.1:8787   │    │   (llama3, mistral, etc.)        │  │
│  └──────────────────┘    └──────────────────────────────────┘  │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  MCP Servers (child processes via stdio / HTTP)           │  │
│  │  e.g. filesystem, custom tools                            │  │
│  └──────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Technology Stack

| Layer | Technology | Purpose |
|-------|-----------|---------|
| **Desktop Shell** | [webview/webview](https://github.com/webview/webview) 0.12.0 | Native window with WKWebView |
| **HTTP Server** | [Drogon](https://github.com/drogonframework/drogon) | C++17 async HTTP/WebSocket framework |
| **LLM Runtime** | [Ollama](https://ollama.com) | Local model inference (llama3, mistral, etc.) |
| **Frontend** | [Alpine.js](https://alpinejs.dev) 3.x | Reactive SPA with zero build step |
| **TTS** | [kitten-tts-nano](https://github.com/SWivid/kitten-tts) | Offline text-to-speech (ONNX, 8 voices) |
| **Vector Search** | [hnswlib](https://github.com/nmslib/hnswlib) | HNSW approximate nearest neighbors for RAG |
| **Web Scraping** | libcurl + custom HTML parser | Web content extraction for RAG |
| **Markdown** | [marked.js](https://marked.js.org) + [highlight.js](https://highlightjs.org) | Markdown rendering with syntax highlighting |
| **Icons** | [Phosphor Icons](https://phosphoricons.com) | UI icon system |
| **Build** | CMake 3.16+ | Cross-platform build system |
| **Language** | C++17, JavaScript (ES2020), Python 3.12 | Multi-language stack |

---

## Directory Structure

```
myapp/
├── main.cc                      # Application entry point
├── CMakeLists.txt               # Build configuration
├── Info.plist.in                 # macOS app bundle metadata
├── tts_server.py                # Python TTS HTTP server
├── mcp_servers.json             # MCP server definitions
├── build.sh                     # Build helper script
│
├── controllers/
│   ├── ChatController.cc/h      # HTTP routes + WebSocket handler
│   └── SearchController.cc/h    # Model & search API controllers
│
├── services/
│   ├── OllamaService.cc/h       # Ollama API client (streaming chat)
│   ├── McpService.cc/h          # MCP server lifecycle management
│   ├── RagService.cc/h          # RAG: embed, index, retrieve
│   ├── DuckDuckGoService.cc/h   # Web search integration
│   └── WebScraperService.cc/h   # HTML scraping & text extraction
│
├── models/
│   ├── Message.h                # Chat message struct
│   ├── ToolCall.h               # Tool call struct (MCP)
│   └── kitten-tts/              # TTS model files (ONNX + voices)
│
├── frontend/
│   ├── index.html               # Full SPA (HTML + CSS, ~3200 lines)
│   ├── chat.html                # (Unused/legacy)
│   └── static/
│       └── app.js               # Alpine.js component (~1900 lines)
│
└── build/
    └── Local Global.app/  # macOS app bundle output
        └── Contents/
            ├── MacOS/           # Compiled binary
            ├── Resources/       # Frontend, models, config
            └── Info.plist       # Bundle metadata
```

---

## Backend (C++ / Drogon)

### Entry Point — `main.cc`

The application lifecycle:

1. **`curl_global_init()`** — Initialize libcurl for HTTP operations
2. **Resolve resource directory** — Detect `.app` bundle vs. plain binary
3. **Initialize RAG storage** — Load previously indexed documents from disk
4. **Launch TTS server** — Fork a Python child process running `tts_server.py`
5. **Configure & start Drogon** — HTTP server on `127.0.0.1:8080` (4 threads)
6. **Health-check Ollama** — Verify Ollama is reachable at port `11434`
7. **Load MCP servers** — Parse `mcp_servers.json`, spawn child processes
8. **Open webview** — Native window navigated to `http://127.0.0.1:8080`
9. **Shutdown** — Close Drogon, kill MCP servers, kill TTS server, cleanup

Native bindings exposed to JavaScript via the webview:
- **`openExternal(url)`** — Opens URLs in the system browser
- **`saveFile(filename, content)`** — Writes files to `~/Downloads` and reveals in Finder

### Controllers

| Controller | Route | Method | Purpose |
|-----------|-------|--------|---------|
| `IndexController` | `/` | GET | Serves `index.html` |
| `ChatWebSocket` | `/ws/chat` | WebSocket | Real-time streaming chat |
| `ModelsController` | `/api/models` | GET | List available Ollama models |
| `SearchApiController` | `/api/search` | GET | DuckDuckGo web search |
| `McpController` | `/api/mcp/*` | GET/POST/DELETE | MCP server management |
| `RagController` | `/api/rag/*` | GET/POST/DELETE | RAG document management |
| `UploadController` | `/api/upload` | POST | File upload for RAG indexing |

### Services

#### OllamaService
- **Streaming chat** via Ollama's `/api/chat` endpoint with chunked transfer
- **Model listing** with metadata (size, parameter count, quantization)
- **Model management** (pull, delete) via Ollama API
- Supports `AbortFlag` for user-initiated stream cancellation

#### McpService (Singleton)
- Manages **MCP (Model Context Protocol)** server processes
- Two transports: **stdio** (child process with pipes) and **HTTP** (remote endpoint)
- JSON-RPC 2.0 protocol for tool discovery and invocation
- Auto-save/load configuration from `mcp_servers.json`
- Thread-safe with per-server I/O mutex

#### RagService (Singleton)
- **Chunking**: Splits documents into overlapping ~512-token chunks
- **Embedding**: Uses Ollama's embedding model (768-dimensional vectors)
- **Indexing**: HNSW approximate nearest neighbor search via hnswlib
- **Retrieval**: Cosine similarity scoring, returns top-k relevant chunks
- **Persistence**: Saves/loads vectors and metadata to disk

#### DuckDuckGoService
- Searches DuckDuckGo via the instant answer API
- Returns structured results (title, URL, snippet)

#### WebScraperService
- Fetches web pages via libcurl
- Extracts clean text from HTML (strips tags, scripts, styles)
- Normalizes whitespace for RAG indexing

### Models

```cpp
struct Message {
    std::string role;     // "user", "assistant", "system"
    std::string content;  // Message text
};

struct ToolCall {
    std::string id;
    std::string name;     // Tool/function name
    std::string server;   // MCP server name
    Json::Value arguments;
    Json::Value result;
};
```

---

## Frontend (Alpine.js SPA)

The frontend is a **single-page application** built with Alpine.js — no build step, no bundler, no Node.js required.

### Key Features

| Feature | Description |
|---------|-------------|
| **Multi-conversation** | Create, switch, rename, delete, branch conversations |
| **Real-time streaming** | Token-by-token response rendering via WebSocket |
| **Markdown rendering** | Full GFM with syntax-highlighted code blocks |
| **RAG integration** | Upload documents, cite sources in responses |
| **MCP tools** | Discover and invoke external tool servers |
| **Model manager** | Pull, switch, and delete Ollama models |
| **Text-to-Speech** | Read responses aloud with 8 different voices |
| **Chat branching** | Fork conversations at any message |
| **Tags** | Organize conversations with custom tags |
| **Export** | Export chats as Markdown, JSON, or plain text |
| **Token dashboard** | Track token usage per conversation |
| **System prompts** | Built-in + custom prompt templates |
| **Settings** | Font size, send-on-enter, timestamps, TTS voice |
| **Keyboard shortcuts** | Cmd+N, Cmd+K, Cmd+E, etc. |
| **Memory** | Auto-extract and persist facts across sessions |
| **Message editing** | Edit and resend user messages |

### State Management

All state lives in a single Alpine.js `chatApp()` component:
- **Conversations**: `conversations[]` array persisted to `localStorage`
- **Settings**: `settings{}` object persisted to `localStorage`
- **Custom templates**: `customTemplates[]` persisted to `localStorage`
- **WebSocket**: Persistent connection to `/ws/chat` with auto-reconnect

### CSS Architecture

All styles are inline in `index.html` using CSS custom properties (variables):
- Theme colors: `--bg-color`, `--text-primary`, `--accent-color`, etc.
- Responsive: Mobile breakpoint at `768px` with slide-out sidebar
- Animations: Typing indicator, TTS pulse, voice card bounce, loading skeleton

---

## Text-to-Speech (TTS)

### Architecture

```
Frontend (app.js)          TTS Server (Python)         Model Files
┌───────────────┐         ┌──────────────────┐        ┌────────────────┐
│ speakMessage() │──POST──▶│ tts_server.py    │───────▶│ kitten-tts-nano│
│ previewVoice() │  /tts   │ :8787            │        │ v0.8 (int8)    │
│ AudioContext   │◀─WAV────│ KittenTTS_1_Onnx │        │ ~26MB total    │
└───────────────┘         └──────────────────┘        └────────────────┘
```

### Components

- **`tts_server.py`** — Python HTTP server (stdlib `http.server`)
  - `POST /tts` — Generate speech from text + voice name, returns WAV
  - `GET /health` — Health check
  - `GET /voices` — List available voices
- **Model**: `kitten-tts-nano-0.8-int8` (ONNX runtime, 24kHz, PCM_16)
- **8 voices**: Bella, Jasper, Luna, Bruno, Rosie, Hugo, Kiki, Leo
- **Frontend features**:
  - Chunked playback (500-char chunks for fast first response)
  - Pause/resume with exact audio offset tracking
  - Voice preview caching (AudioBuffer cached per voice)
  - 3-state button: play → pause → resume

### Lifecycle

1. `main.cc` forks a child process running the TTS server
2. Server loads the ONNX model and voices into memory
3. Frontend sends text chunks via `fetch()` to `http://127.0.0.1:8787/tts`
4. Server returns WAV audio, frontend decodes via `AudioContext.decodeAudioData()`
5. On app shutdown, `main.cc` sends `SIGTERM` to the TTS process

---

## RAG Pipeline

```
Upload/Scrape → Chunk (512 tokens) → Embed (Ollama) → Index (HNSW)
                                                           │
User Query → Embed → kNN Search → Top-k Chunks → Augment Prompt
```

### Flow

1. **Ingest**: User uploads a file or provides a URL
2. **Scrape** (URLs): `WebScraperService` extracts clean text
3. **Chunk**: `RagService::chunkText()` splits into overlapping segments
4. **Embed**: Each chunk is embedded via Ollama's embedding endpoint (768-dim)
5. **Index**: Vectors are added to an HNSW graph (hnswlib)
6. **Persist**: Vectors + metadata saved to disk as binary files
7. **Retrieve**: User query is embedded → cosine similarity search → top 5 chunks
8. **Augment**: Retrieved chunks are injected into the system prompt

---

## MCP (Model Context Protocol)

The app supports the **Model Context Protocol** for extensible tool use:

- **stdio transport**: Spawns a child process, communicates via stdin/stdout pipes
- **HTTP transport**: Connects to remote MCP endpoints via HTTP POST
- **JSON-RPC 2.0**: Standard request/response protocol
- **Tool discovery**: `tools/list` method returns available tools
- **Tool invocation**: `tools/call` method executes tools with parameters

Example `mcp_servers.json`:
```json
{
  "servers": [
    {
      "name": "filesystem",
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-filesystem", "/path/to/dir"]
    }
  ]
}
```

---

## WebSocket Protocol

The chat uses a persistent WebSocket connection at `/ws/chat`.

### Client → Server Messages

```json
{
  "type": "chat",
  "model": "llama3.2",
  "messages": [
    { "role": "system", "content": "..." },
    { "role": "user", "content": "Hello!" }
  ],
  "temperature": 0.7,
  "tools": ["filesystem"],
  "ragEnabled": true
}
```

### Server → Client Messages

```json
// Streaming token
{ "type": "token", "content": "Hello" }

// Stream complete
{ "type": "done", "totalTokens": 150 }

// Tool call
{ "type": "tool_call", "tool": "filesystem", "method": "readFile", "args": {...} }

// Tool result
{ "type": "tool_result", "result": {...} }

// Error
{ "type": "error", "message": "..." }

// RAG citations
{ "type": "rag_citations", "sources": [...] }
```

---

## Build System

### CMake Configuration

- **C++17** standard required
- **Dependencies**: Drogon, Threads, CURL (system), webview (FetchContent), hnswlib (FetchContent)
- **Output**: macOS `.app` bundle with embedded resources
- **Post-build**: Copies `frontend/`, `tts_server.py`, `models/kitten-tts/`, `mcp_servers.json` into bundle

### Build Commands

```bash
# First-time setup
cd myapp
mkdir -p build && cd build
cmake ..

# Build
cmake --build .

# Run
./Local\ AI\ Assistant.app/Contents/MacOS/Local\ AI\ Assistant
```

---

## Data Flow Diagrams

### Chat Message Flow

```
User types message
       │
       ▼
Frontend sends via WebSocket ──▶ ChatWebSocket controller
                                        │
                        ┌───────────────┤
                        ▼               ▼
                  RAG retrieval    MCP tool calls
                  (if enabled)    (if tools selected)
                        │               │
                        └───────┬───────┘
                                ▼
                    Build augmented prompt
                                │
                                ▼
                    OllamaService::streamChat()
                    (POST to Ollama /api/chat)
                                │
                                ▼
                    Stream tokens back via WebSocket
                                │
                                ▼
                    Frontend renders in real-time
```

---

## Prerequisites & Setup

### System Requirements

| Requirement | Version | Purpose |
|-------------|---------|---------|
| **macOS** | 13+ (Ventura) | Operating system |
| **Ollama** | Latest | LLM inference engine |
| **CMake** | 3.16+ | Build system |
| **Drogon** | Latest | C++ HTTP framework |
| **Python** | 3.10+ | TTS server |
| **libcurl** | System | HTTP client |
| **Xcode CLT** | Latest | C++ compiler (clang++) |

### Installation

```bash
# 1. Install prerequisites
brew install cmake drogon curl jsoncpp

# 2. Install Ollama
# Download from https://ollama.com

# 3. Pull a model
ollama pull llama3.2

# 4. Set up TTS (optional)
cd myapp
python3 -m venv tts_venv
source tts_venv/bin/activate
pip install kittentts soundfile

# 5. Build the app
mkdir -p build && cd build
cmake ..
cmake --build .

# 6. Run
./Local\ AI\ Assistant.app/Contents/MacOS/Local\ AI\ Assistant
```

---

## Security & Privacy

- **100% local**: All processing happens on your machine
- **No telemetry**: Zero data sent to external servers
- **No API keys**: No cloud AI services required
- **Localhost only**: Server binds to `127.0.0.1` — not accessible from the network
- **Sandboxed**: macOS app bundle with proper Info.plist
- **Data storage**: Conversations stored in browser `localStorage` (within the webview)
- **RAG data**: Indexed documents stored locally in the app bundle's Resources directory
