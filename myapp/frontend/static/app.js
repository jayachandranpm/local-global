/**
 * app.js — Alpine.js chatApp() component for Local Global.
 *
 * Manages:
 *   • WebSocket connection to /ws/chat
 *   • Multi-conversation persistence (disk-backed storage)
 *   • Context window management & token tracking
 *   • Streaming token assembly
 *   • Sidebar toggle with disk-backed persistence
 *   • Model & MCP server list fetching + add/remove
 *   • Toast notifications
 */

const STORAGE_KEY = 'localai_conversations';
const MEMORY_KEY = 'localai_memory';
const DEFAULT_CONTEXT_TOKENS = 4096;
const CHARS_PER_TOKEN = 4;

function generateId() {
  return Date.now().toString(36) + Math.random().toString(36).slice(2, 8);
}

function estimateTokens(text) {
  return Math.ceil((text || '').length / CHARS_PER_TOKEN);
}

// ─── Disk-backed storage (replaces browser localStorage) ─────────────────
// Stores data on the local filesystem via /api/storage REST endpoints.
// Falls back to browser localStorage if the server is unreachable.
const diskStorage = (() => {
  const ALL_KEYS = [
    'localai_conversations', 'localai_memory', 'localai_settings',
    'localai_auto_memory', 'localai_custom_templates', 'sidebarCollapsed',
    'localai_maxContextTokens', 'localai_session_stats',
    'localai_sidebar_sections', 'localai_tool_params'
  ];
  const cache = {};

  const ready = (async () => {
    try {
      const results = await Promise.all(
        ALL_KEYS.map(k =>
          fetch(`/api/storage?key=${k}`)
            .then(r => r.json())
            .then(j => [k, j.value])
        )
      );
      let diskEmpty = true;
      for (const [k, v] of results) {
        if (v !== null && v !== undefined) { cache[k] = v; diskEmpty = false; }
      }
      // One-time migration: if disk is empty but browser localStorage has data
      if (diskEmpty) {
        for (const k of ALL_KEYS) {
          const lv = localStorage.getItem(k);
          if (lv !== null) {
            cache[k] = lv;
            fetch('/api/storage', {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify({ key: k, value: lv })
            }).catch(() => { });
          }
        }
      }
    } catch (e) {
      // Server unreachable — fall back to browser localStorage
      console.warn('[diskStorage] Server unreachable, falling back to localStorage:', e);
      for (const k of ALL_KEYS) {
        const lv = localStorage.getItem(k);
        if (lv !== null) cache[k] = lv;
      }
    }
  })();

  return {
    ready,
    getItem(key) {
      return cache[key] ?? null;
    },
    setItem(key, value) {
      const str = String(value);
      cache[key] = str;
      fetch('/api/storage', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ key, value: str })
      }).catch(e => console.warn('[diskStorage] write failed:', e));
    },
    removeItem(key) {
      delete cache[key];
      fetch('/api/storage', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ key, value: null })
      }).catch(e => console.warn('[diskStorage] remove failed:', e));
    }
  };
})();

function chatApp() {
  return {
    // ─── Reactive state ──────────────────────────────────────────
    messages: [],
    inputText: '',
    currentModel: '',
    models: [],
    mcpServers: [],         // [{name, command, args, status}]
    temperature: 0.7,
    isStreaming: false,
    streamingContent: '',
    _rawStreamBuffer: '', // Non-reactive buffer for throttled incoming tokens
    _renderRafTimer: null,
    toolStatus: '',
    searchSteps: [],
    searchElapsed: '',
    _searchTimer: null,
    _searchStart: 0,
    sidebarCollapsed: false,
    mobileOverlayActive: false,
    webSearchEnabled: false,
    ws: null,
    _wsReconnectTimer: null,

    // ─── Conversations ───────────────────────────────────────────
    conversations: [],      // [{id, title, messages, createdAt, updatedAt}]
    currentConversationId: null,
    contextTokens: 0,       // estimated tokens in current context
    maxContextTokens: DEFAULT_CONTEXT_TOKENS,
    modelContextMap: {},    // { 'model_name': context_length }

    // ─── Memory ──────────────────────────────────────────────────
    memories: [],            // [{id, text, createdAt}]
    showMemoryPanel: false,
    newMemoryText: '',
    autoMemoryEnabled: true, // auto-extract facts from conversations

    // ─── MCP management ──────────────────────────────────────────
    showMcpModal: false,
    mcpForm: { name: '', transport: 'stdio', command: '', args: '', url: '' },
    mcpTools: [],
    mcpToolsLoading: false,
    showMcpToolsPanel: false,
    savedToolParams: {},
    editingToolParam: null,
    editingToolParamJson: '',

    // ─── RAG (Document Upload) ───────────────────────────────────
    ragDocuments: [],        // [{id, name, size, chunks, uploadedAt}]
    ragEnabled: true,        // auto-use RAG context when docs exist
    showRagPanel: false,
    isUploading: false,
    uploadProgress: '',
    pendingRagSources: [],   // RAG sources for the currently streaming message
    isThinking: false,           // whether the model is in thinking/reasoning phase
    thinkingContent: '',         // accumulated thinking tokens
    _rawThinkingBuffer: '',      // Non-reactive buffer for incoming thinking tokens
    pendingThinkingContent: '',  // thinking content for the currently streaming message

    // ─── Drag & Drop / Search / Stop ─────────────────────────────
    isDragging: false,
    conversationSearch: '',
    userScrolledUp: false,
    wsConnected: false,

    // ─── System Prompt & Templates ────────────────────────────────
    showSystemPromptModal: false,
    editingSystemPrompt: '',
    showTemplatePanel: false,

    // ─── Model Manager (#13) ─────────────────────────────────────
    showModelManager: false,
    modelDetails: [],          // [{name, size, parameter_size, quantization_level, family, ...}]
    modelPullName: '',
    modelPullProgress: null,   // {status, completed, total, percent} or null
    isModelPulling: false,

    // ─── Conversation Tags (#16) ─────────────────────────────────
    tagFilter: '',             // active tag filter in sidebar
    editingTagConvId: null,    // conv id currently editing tags
    newTagText: '',

    // ─── UI enhancements ─────────────────────────────────────────
    showKeyboardShortcuts: false,
    sidebarSections: {
      model: true,
      temperature: false,
      memory: false,
      mcpServers: false,
      mcpTools: false,
      ragDocs: false,
    },
    batchSelectMode: false,
    selectedConvIds: new Set(),
    focusedMessageIdx: -1,
    mcpToolsError: false,

    // ─── Custom confirm modal (replaces window.confirm) ──────────
    confirmVisible: false,
    confirmMessage: '',
    _confirmResolve: null,     // Promise resolve callback

    // ─── Token Usage Dashboard (#17) ─────────────────────────────
    showTokenDashboard: false,
    lastStats: null,           // {eval_count, prompt_eval_count, eval_duration, total_duration, tokens_per_sec}
    sessionStats: { totalGenerated: 0, totalPrompt: 0, totalDuration: 0, requestCount: 0 },

    // ─── Conversation Rename (#19) ───────────────────────────────
    renamingConvId: null,
    renameText: '',

    // ─── Message Edit & Resend (#20) ─────────────────────────────
    editingMessageIdx: null,
    editMessageText: '',

    // ─── Settings Panel (#38) ────────────────────────────────────
    showSettingsPanel: false,
    settings: {
      fontSize: 15,
      sendOnEnter: true,
      showTimestamps: false,
      defaultModel: '',
      ttsVoice: 'af_sarah',
      theme: 'system',
    },

    // ─── Text-to-Speech (TTS) ────────────────────────────────────
    ttsPlayingIdx: null,     // index of message currently being spoken
    _ttsAborted: false,
    _audioCtx: null,
    _ttsSource: null,
    previewingVoice: null,   // voice name currently being previewed
    _previewSource: null,    // AudioBufferSourceNode for preview
    _voicePreviewCache: {},  // cached AudioBuffers for voice previews
    _previewGeneration: 0,   // generation counter to discard stale preview callbacks
    ttsPaused: false,        // is TTS playback paused?
    _ttsChunks: [],          // text chunks for current TTS
    _ttsChunkIndex: 0,       // current chunk being played/fetched
    _ttsPauseOffset: 0,      // seconds into current chunk (for resume)
    _ttsCurrentBuffer: null, // AudioBuffer of current chunk
    _ttsChunkStartTime: 0,   // audioCtx.currentTime when chunk started
    _ttsPausingNow: false,   // distinguishes pause-stop from natural end
    _ttsVoice: '',           // voice for current TTS session
    promptTemplates: [
      { name: 'Default', icon: 'ph-chat-circle-dots', prompt: '', description: 'No custom system prompt' },
      { name: 'Python Tutor', icon: 'ph-code', prompt: 'You are an expert Python tutor. Explain concepts clearly with examples. When the user shares code, provide constructive feedback. Use beginner-friendly language unless asked otherwise.', description: 'Patient Python teacher' },
      { name: 'Code Reviewer', icon: 'ph-magnifying-glass-plus', prompt: 'You are a senior code reviewer. Analyze code for bugs, security issues, performance problems, and style violations. Be thorough but constructive. Suggest specific improvements with code examples.', description: 'Thorough code analysis' },
      { name: 'ELI5', icon: 'ph-lightbulb', prompt: 'You are an expert at explaining complex topics in simple terms. Explain everything as if talking to a curious 5-year-old. Use analogies, everyday examples, and short sentences. Avoid jargon.', description: 'Simple explanations' },
      { name: 'Creative Writer', icon: 'ph-pencil-line', prompt: 'You are a talented creative writer. Help with storytelling, world-building, character development, and prose. Be imaginative and evocative. Offer multiple creative options when asked.', description: 'Storytelling & prose' },
      { name: 'Summarizer', icon: 'ph-article', prompt: 'You are a concise summarizer. When given text, extract the key points and present them clearly. Use bullet points for multiple items. Aim for 20% of the original length while keeping all essential information.', description: 'Concise key points' },
      { name: 'Translator', icon: 'ph-translate', prompt: 'You are a professional translator. Translate text accurately while preserving tone, nuance, and cultural context. When ambiguous, provide alternatives. Always state the source and target languages.', description: 'Accurate translations' },
      { name: 'Technical Writer', icon: 'ph-file-text', prompt: 'You are a technical documentation expert. Write clear, well-structured documentation with proper headings, code examples, and step-by-step instructions. Follow best practices for technical writing.', description: 'Docs & guides' },
      { name: 'Debate Partner', icon: 'ph-scales', prompt: 'You are a thoughtful debate partner. Present well-reasoned arguments from multiple perspectives. Challenge assumptions respectfully. Cite evidence when possible. Help the user think critically about topics.', description: 'Critical thinking' },
      { name: 'Math Tutor', icon: 'ph-math-operations', prompt: 'You are a patient math tutor. Solve problems step-by-step, explaining your reasoning at each stage. Use clear notation. When the user makes mistakes, guide them to the correct approach rather than just giving the answer.', description: 'Step-by-step math' },
    ],
    customTemplates: [],
    showAddTemplateForm: false,
    newTemplate: { name: '', icon: 'ph-star', prompt: '', description: '' },
    templateIcons: [
      'ph-star', 'ph-code', 'ph-lightbulb', 'ph-pencil-line', 'ph-book-open-text',
      'ph-brain', 'ph-rocket', 'ph-chat-circle-dots', 'ph-globe', 'ph-flask',
      'ph-heart', 'ph-graduation-cap', 'ph-paint-brush', 'ph-wrench', 'ph-megaphone',
      'ph-shield-check', 'ph-chart-line', 'ph-music-note', 'ph-camera', 'ph-atom',
    ],

    // ─── Lifecycle ───────────────────────────────────────────────
    async init() {
      await diskStorage.ready;
      this.sidebarCollapsed = diskStorage.getItem('sidebarCollapsed') === 'true';
      // Restore sidebar section collapse state
      const savedSections = diskStorage.getItem('localai_sidebar_sections');
      if (savedSections) {
        try { Object.assign(this.sidebarSections, JSON.parse(savedSections)); } catch(e) {}
      }
      this.loadSettings();
      this.applyTheme();
      this.loadCustomTemplates();
      this.loadConversations();
      this.loadMemories();
      this.fetchModels();
      this.fetchMcpServers();
      this.fetchMcpTools();
      this.fetchSavedToolParams();
      this.fetchRagDocuments();
      this.connectWebSocket();

      // Cleanup on page unload
      window.addEventListener('beforeunload', () => {
        this.cleanupRagPollers();
        if (this._wsReconnectTimer) clearTimeout(this._wsReconnectTimer);
        if (this._saveTimer) {
          clearTimeout(this._saveTimer);
          this._saveTimer = null;
          this._executeSaveConversations();
        }
      });

      // Markdown is sanitized before insertion, so code-copy behavior is
      // attached here instead of using inline event handlers.
      if (!window._codeCopyRegistered) {
        window._codeCopyRegistered = true;
        document.addEventListener('click', (event) => {
          const button = event.target.closest?.('.code-copy-btn[data-copy-id]');
          if (!button) return;
          const value = window.__codeCopyStore?.[button.dataset.copyId];
          if (typeof value !== 'string') return;
          navigator.clipboard.writeText(value).then(() => {
            button.textContent = 'Copied!';
            setTimeout(() => { button.textContent = 'Copy'; }, 1500);
          }).catch(() => { });
        });
      }

      // Listen for system theme changes
      window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
        if (this.settings.theme === 'system') this.applyTheme();
      });

      // ─── Keyboard shortcuts ──────────────────────────────────
      // Guard against double-init
      if (!window._kbShortcutsRegistered) {
        window._kbShortcutsRegistered = true;
        const self = this;
        let lastToggleTime = 0;
        window.addEventListener('keydown', (e) => {
          const isMeta = e.metaKey || e.ctrlKey;
          if (!isMeta) {
            // Escape → Close modals or stop generation
            if (e.key === 'Escape') {
              e.preventDefault();
              if (self.confirmVisible) { self.resolveConfirm(false); return; }
              if (self.showKeyboardShortcuts) { self.showKeyboardShortcuts = false; return; }
              if (self.showSettingsPanel) { self.closeSettingsPanel(); return; }
              if (self.showModelManager) { self.showModelManager = false; return; }
              if (self.showTemplatePanel) { self.showTemplatePanel = false; return; }
              if (self.showTokenDashboard) { self.showTokenDashboard = false; return; }
              if (self.showMcpModal) { self.closeMcpModal(); return; }
              if (self.showSystemPromptModal) { self.showSystemPromptModal = false; return; }
              if (self.isStreaming) { self.stopGeneration(); return; }
            }
            // ? key (when not in input) → Show keyboard shortcuts
            if (e.key === '?' && !['INPUT', 'TEXTAREA', 'SELECT'].includes(document.activeElement?.tagName)) {
              e.preventDefault();
              self.showKeyboardShortcuts = !self.showKeyboardShortcuts;
            }
            return;
          }

          // Cmd/Ctrl + N  → New chat
          if (!e.shiftKey && (e.key === 'n' || e.key === 'N')) {
            e.preventDefault();
            e.stopImmediatePropagation();
            self.createNewConversation();
            return;
          }
          // Cmd/Ctrl + F  → Focus conversation search
          if (e.key === 'f' || e.key === 'F') {
            e.preventDefault();
            e.stopImmediatePropagation();
            if (self.sidebarCollapsed) self.toggleSidebar();
            setTimeout(() => {
              const searchInput = document.querySelector('.search-conversations-input');
              if (searchInput) searchInput.focus();
            }, 100);
            return;
          }
          // Cmd/Ctrl + Shift + / (⌘?) → Keyboard shortcuts help
          if (e.shiftKey && (e.code === 'Slash' || e.key === '?')) {
            e.preventDefault();
            e.stopImmediatePropagation();
            self.showKeyboardShortcuts = !self.showKeyboardShortcuts;
            return;
          }
          // Cmd/Ctrl + \ or / or B → Toggle sidebar (debounced to prevent double-fire)
          const isToggle = e.code === 'Backslash' || e.code === 'Slash' ||
            e.key === '\\' || e.key === '/' ||
            ((e.key === 'b' || e.key === 'B') && !e.shiftKey);
          if (isToggle) {
            e.preventDefault();
            e.stopImmediatePropagation();
            const now = Date.now();
            if (now - lastToggleTime < 300) return; // debounce
            lastToggleTime = now;
            console.log('[KB] Toggle sidebar via', e.key, e.code);
            self.toggleSidebar();
            return;
          }
        }, true);
      }

      // ─── Watch model changes to update context window ────────
      this.$watch('currentModel', () => this.updateMaxContext());
    },

    // ─── Conversations persistence ───────────────────────────────
    loadConversations() {
      try {
        const raw = diskStorage.getItem(STORAGE_KEY);
        if (raw) {
          this.conversations = JSON.parse(raw);
          // Load the most recent conversation
          if (this.conversations.length > 0) {
            const latest = this.conversations[0];
            this.currentConversationId = latest.id;
            this.messages = latest.messages || [];
            this.lastStats = latest.lastStats || null;
            this.updateContextTokens();
          }
        }
      } catch (e) {
        console.warn('Failed to load conversations:', e);
        this.conversations = [];
      }
    },

    _saveTimer: null,

    saveConversations(immediate = false) {
      if (immediate) {
        if (this._saveTimer) {
          clearTimeout(this._saveTimer);
          this._saveTimer = null;
        }
        this._executeSaveConversations();
        return;
      }
      // Debounce saving to at most once every 1000ms
      if (this._saveTimer) return;
      this._saveTimer = setTimeout(() => {
        this._executeSaveConversations();
        this._saveTimer = null;
      }, 1000);
    },

    _executeSaveConversations() {
      try {
        // Update current conversation in list
        if (this.currentConversationId) {
          const idx = this.conversations.findIndex(c => c.id === this.currentConversationId);
          if (idx >= 0) {
            this.conversations[idx].messages = this.messages;
            this.conversations[idx].updatedAt = Date.now();
            this.conversations[idx].lastStats = this.lastStats;
          }
        }
        diskStorage.setItem(STORAGE_KEY, JSON.stringify(this.conversations));
      } catch (e) {
        console.warn('Failed to save conversations:', e);
      }
    },

    createNewConversation() {
      // Stop any active TTS before switching away
      this.stopSpeaking();
      // Save current first
      this.saveConversations();

      const conv = {
        id: generateId(),
        title: 'New Chat',
        messages: [],
        createdAt: Date.now(),
        updatedAt: Date.now()
      };
      this.conversations.unshift(conv);
      this.currentConversationId = conv.id;
      this.messages = [];
      this.streamingContent = '';
      this.isStreaming = false;
      this.toolStatus = '';
      this.isThinking = false;
      this.thinkingContent = '';
      this.pendingThinkingContent = '';
      this.lastStats = null;
      this.contextTokens = 0;
      this.saveConversations();
      this.$nextTick(() => { if (this.$refs.inputBox) this.$refs.inputBox.focus(); });
    },

    switchConversation(convId) {
      if (convId === this.currentConversationId) return;
      if (this.isStreaming) return;

      // Stop any active TTS before switching
      this.stopSpeaking();

      // Save current
      this.saveConversations();

      const conv = this.conversations.find(c => c.id === convId);
      if (conv) {
        this.currentConversationId = convId;
        this.messages = conv.messages || [];
        this.streamingContent = '';
        this.isStreaming = false;
        this.toolStatus = '';
        this.isThinking = false;
        this.thinkingContent = '';
        this.pendingThinkingContent = '';
        this.lastStats = conv.lastStats || null;
        this.updateContextTokens();
        this.$nextTick(() => { if (this.$refs.inputBox) this.$refs.inputBox.focus(); });
      }
    },

    async deleteConversation(convId) {
      const conv = this.conversations.find(c => c.id === convId);
      const title = conv ? (conv.title || 'this chat') : 'this chat';
      if (!(await this.showConfirm(`Delete "${title}"?`))) return;
      this.conversations = this.conversations.filter(c => c.id !== convId);
      if (this.currentConversationId === convId) {
        if (this.conversations.length > 0) {
          this.switchConversation(this.conversations[0].id);
        } else {
          this.createNewConversation();
        }
      }
      this.saveConversations();
    },

    togglePinConversation(convId) {
      const conv = this.conversations.find(c => c.id === convId);
      if (conv) {
        conv.pinned = !conv.pinned;
        this.saveConversations();
      }
    },

    getMessageStats(content) {
      if (!content) return null;
      const words = content.trim().split(/\s+/).filter(Boolean).length;
      if (words < 10) return null;
      const mins = Math.round(words / 200);
      const readTime = mins < 1 ? `${Math.max(1, Math.ceil(words / 3.3))} sec read` : `${mins} min read`;
      return { words, readTime };
    },

    autoTitleConversation(userMsg) {
      if (!this.currentConversationId) return;
      const conv = this.conversations.find(c => c.id === this.currentConversationId);
      if (conv && conv.title === 'New Chat') {
        // Use first 50 chars of first user message as title
        conv.title = userMsg.length > 50 ? userMsg.slice(0, 50) + '…' : userMsg;
        this.saveConversations();
      }
    },

    getConversationTitle(conv) {
      return conv.title || 'New Chat';
    },

    // ─── Conversation Rename (#19) ───────────────────────────────
    startRename(conv) {
      this.renamingConvId = conv.id;
      this.renameText = conv.title || '';
    },

    confirmRename(convId) {
      const text = this.renameText.trim();
      if (!text) { this.renamingConvId = null; return; }
      const conv = this.conversations.find(c => c.id === convId);
      if (conv) {
        conv.title = text;
        this.saveConversations();
      }
      this.renamingConvId = null;
    },

    cancelRename() {
      this.renamingConvId = null;
      this.renameText = '';
    },

    formatDate(ts) {
      const d = new Date(ts);
      const now = new Date();
      const diff = now - d;
      if (diff < 60000) return 'Just now';
      if (diff < 3600000) return Math.floor(diff / 60000) + 'm ago';
      if (diff < 86400000) return Math.floor(diff / 3600000) + 'h ago';
      if (diff < 604800000) return Math.floor(diff / 86400000) + 'd ago';
      return d.toLocaleDateString();
    },

    // ─── Context window management ───────────────────────────────
    updateContextTokens() {
      let total = 0;
      for (const m of this.messages) {
        total += estimateTokens(m.content);
        total += estimateTokens(m.thinkingContent || '');
      }
      this.contextTokens = total;
    },

    getContextMessages() {
      // Return messages trimmed to fit within context window
      // Keep system prompt budget (~200 tokens) + memory budget
      const memoryTokens = this.memories.reduce((acc, m) => acc + estimateTokens(m.text), 0);
      const budget = this.maxContextTokens - 200 - memoryTokens - 500; // 500 for response

      if (budget <= 0) return this.messages.slice(-4); // minimum last 4 messages

      const msgs = [];
      let used = 0;
      // Walk backwards to keep most recent messages
      for (let i = this.messages.length - 1; i >= 0; i--) {
        const tokens = estimateTokens(this.messages[i].content) +
          estimateTokens(this.messages[i].thinkingContent || '');
        if (used + tokens > budget && msgs.length >= 2) break;
        msgs.unshift(this.messages[i]);
        used += tokens;
      }
      return msgs;
    },

    get contextPercentage() {
      return Math.min(100, Math.round((this.contextTokens / this.maxContextTokens) * 100));
    },

    get contextColor() {
      if (this.contextPercentage > 85) return '#dc2626';
      if (this.contextPercentage > 60) return '#f59e0b';
      return '#22c55e';
    },

    get filteredConversations() {
      const q = (this.conversationSearch || '').trim().toLowerCase();
      if (!q) return this.conversations;
      return this.conversations.filter(c => {
        if ((c.title || '').toLowerCase().includes(q)) return true;
        return (c.messages || []).some(m =>
          (m.content || '').toLowerCase().includes(q)
        );
      });
    },

    getSearchMatchInfo(conv) {
      const q = (this.conversationSearch || '').trim().toLowerCase();
      if (!q) return null;
      let matchCount = 0;
      let snippet = '';
      for (const m of (conv.messages || [])) {
        const content = (m.content || '').toLowerCase();
        const idx = content.indexOf(q);
        if (idx >= 0) {
          matchCount++;
          if (!snippet) {
            const raw = m.content || '';
            const start = Math.max(0, idx - 30);
            const end = Math.min(raw.length, idx + q.length + 40);
            snippet = (start > 0 ? '…' : '') +
              raw.slice(start, end).trim() +
              (end < raw.length ? '…' : '');
          }
        }
      }
      if (matchCount === 0 && (conv.title || '').toLowerCase().includes(q)) {
        return { matchCount: 0, snippet: 'Title match' };
      }
      return matchCount > 0 ? { matchCount, snippet } : null;
    },

    updateMaxContext() {
      // Always prefer the model's actual context length when known.
      // Fall back to user-saved value, then the default.
      if (this.currentModel && this.modelContextMap[this.currentModel]) {
        this.maxContextTokens = this.modelContextMap[this.currentModel];
      } else {
        const savedCtx = diskStorage.getItem('localai_maxContextTokens');
        if (savedCtx) {
          this.maxContextTokens = parseInt(savedCtx, 10) || DEFAULT_CONTEXT_TOKENS;
        } else {
          this.maxContextTokens = DEFAULT_CONTEXT_TOKENS;
        }
      }
    },

    // ─── Memory system ───────────────────────────────────────────
    loadMemories() {
      try {
        const raw = diskStorage.getItem(MEMORY_KEY);
        if (raw) this.memories = JSON.parse(raw);
        const autoMem = diskStorage.getItem('localai_auto_memory');
        if (autoMem !== null) this.autoMemoryEnabled = autoMem === 'true';
      } catch (e) {
        this.memories = [];
      }
    },

    saveMemories() {
      diskStorage.setItem(MEMORY_KEY, JSON.stringify(this.memories));
    },

    addMemory() {
      const text = this.newMemoryText.trim();
      if (!text) return;
      this.memories.push({
        id: generateId(),
        text: text,
        createdAt: Date.now()
      });
      this.newMemoryText = '';
      this.saveMemories();
      this.showToast('Memory saved');
    },

    removeMemory(memId) {
      this.memories = this.memories.filter(m => m.id !== memId);
      this.saveMemories();
    },

    async extractMemories(userMessage, assistantMessage) {
      try {
        const resp = await fetch('/api/memory/extract', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({
            userMessage,
            assistantMessage,
            model: this.currentModel || 'gemma3:4b'
          })
        });
        const data = await resp.json();
        if (data.facts && data.facts.length > 0) {
          let added = 0;
          for (const fact of data.facts) {
            // Avoid duplicates (case-insensitive)
            const exists = this.memories.some(m =>
              m.text.toLowerCase() === fact.toLowerCase()
            );
            if (!exists) {
              this.memories.push({
                id: generateId(),
                text: fact,
                createdAt: Date.now(),
                auto: true  // mark as auto-extracted
              });
              added++;
            }
          }
          if (added > 0) {
            this.saveMemories();
            this.showToast(`${added} memory${added > 1 ? 's' : ''} auto-saved`);
          }
        }
      } catch (err) {
        console.warn('Memory extraction failed:', err);
      }
    },

    getMemoryPrompt() {
      if (this.memories.length === 0) return '';
      let prompt = '\n\nUser memories (important facts to remember):\n';
      for (const m of this.memories) {
        prompt += `- ${m.text}\n`;
      }
      return prompt;
    },

    // ─── System prompt & templates ───────────────────────────────
    getCurrentSystemPrompt() {
      if (!this.currentConversationId) return '';
      const conv = this.conversations.find(c => c.id === this.currentConversationId);
      return conv?.systemPrompt || '';
    },

    openSystemPromptModal() {
      this.editingSystemPrompt = this.getCurrentSystemPrompt();
      this.showSystemPromptModal = true;
    },

    saveSystemPrompt() {
      if (!this.currentConversationId) return;
      const conv = this.conversations.find(c => c.id === this.currentConversationId);
      if (conv) {
        conv.systemPrompt = this.editingSystemPrompt.trim();
        this.saveConversations();
        this.showToast(conv.systemPrompt ? 'System prompt saved' : 'System prompt cleared');
      }
      this.showSystemPromptModal = false;
    },

    get allPromptTemplates() {
      return [...this.promptTemplates, ...this.customTemplates];
    },

    loadCustomTemplates() {
      try {
        const raw = diskStorage.getItem('localai_custom_templates');
        if (raw) this.customTemplates = JSON.parse(raw);
      } catch (e) { console.warn('Failed to load custom templates:', e); }
    },

    saveCustomTemplates() {
      try {
        diskStorage.setItem('localai_custom_templates', JSON.stringify(this.customTemplates));
      } catch (e) { console.warn('Failed to save custom templates:', e); }
    },

    addCustomTemplate() {
      // Strip HTML tags for defense-in-depth (names are rendered with x-text
      // which is safe, but they also end up in conv.title and disk storage).
      const stripHtml = (s) => s.replace(/<[^>]*>/g, '').trim();
      const n = stripHtml(this.newTemplate.name.trim());
      const p = this.newTemplate.prompt.trim();
      if (!n || !p) { this.showToast('Name and prompt are required', 'error'); return; }
      if (this.allPromptTemplates.some(t => t.name === n)) {
        this.showToast('A template with that name already exists', 'error'); return;
      }
      this.customTemplates.push({
        name: n,
        icon: this.newTemplate.icon || 'ph-star',
        prompt: p,
        description: stripHtml(this.newTemplate.description.trim()) || n,
        custom: true,
      });
      this.saveCustomTemplates();
      this.newTemplate = { name: '', icon: 'ph-star', prompt: '', description: '' };
      this.showAddTemplateForm = false;
      this.showToast('Template added!');
    },

    async deleteCustomTemplate(name) {
      if (!(await this.showConfirm(`Delete template "${name}"?`))) return;
      this.customTemplates = this.customTemplates.filter(t => t.name !== name);
      this.saveCustomTemplates();
      this.showToast('Template deleted');
    },

    applyTemplate(template) {
      this.createNewConversation();
      const conv = this.conversations.find(c => c.id === this.currentConversationId);
      if (conv) {
        conv.systemPrompt = template.prompt;
        if (template.name !== 'Default') {
          conv.title = template.icon + ' ' + template.name;
        }
        this.saveConversations();
      }
      this.showTemplatePanel = false;
      this.showToast(`Started chat with "${template.name}" persona`);
    },

    getActiveTemplateName() {
      const sp = this.getCurrentSystemPrompt();
      if (!sp) return '';
      const match = this.allPromptTemplates.find(t => t.prompt && t.prompt === sp);
      return match ? match.name : 'Custom';
    },

    getActiveTemplateIcon() {
      const sp = this.getCurrentSystemPrompt();
      if (!sp) return '';
      const match = this.allPromptTemplates.find(t => t.prompt && t.prompt === sp);
      return match ? match.icon : 'ph-sliders-horizontal';
    },

    // ─── Sidebar ─────────────────────────────────────────────────
    toggleSidebar() {
      if (window.innerWidth <= 768) {
        const sidebar = document.getElementById('sidebar');
        if (sidebar) {
          sidebar.classList.toggle('mobile-open');
          this.mobileOverlayActive = sidebar.classList.contains('mobile-open');
        }
      } else {
        this.sidebarCollapsed = !this.sidebarCollapsed;
        diskStorage.setItem('sidebarCollapsed', this.sidebarCollapsed);
      }
    },

    toggleSidebarSection(key) {
      this.sidebarSections[key] = !this.sidebarSections[key];
      diskStorage.setItem('localai_sidebar_sections', JSON.stringify(this.sidebarSections));
    },

    toggleBatchSelect() {
      this.batchSelectMode = !this.batchSelectMode;
      if (!this.batchSelectMode) this.selectedConvIds = new Set();
    },

    rateMessage(idx, rating) {
      const msg = this.messages[idx];
      if (!msg) return;
      msg.rating = msg.rating === rating ? null : rating;
      this.saveConversations();
    },

    retryMcpTools() {
      this.mcpToolsError = false;
      this.fetchMcpTools();
    },

    async fetchMcpTools() {
      this.mcpToolsLoading = true;
      this.mcpToolsError = false;
      try {
        const resp = await fetch('/api/mcp/tools');
        const data = await resp.json();
        this.mcpTools = data.tools || [];
      } catch (err) {
        console.warn('Failed to fetch MCP tools:', err);
        this.mcpToolsError = true;
        this.mcpTools = [];
      } finally {
        this.mcpToolsLoading = false;
      }
    },

    fetchSavedToolParams() {
      try {
        const raw = diskStorage.getItem('localai_tool_params');
        this.savedToolParams = raw ? JSON.parse(raw) : {};
      } catch (e) {
        this.savedToolParams = {};
      }
    },

    saveToolParam(toolName, params) {
      this.savedToolParams[toolName] = params;
      diskStorage.setItem('localai_tool_params', JSON.stringify(this.savedToolParams));
      this.showToast('Defaults saved for ' + toolName, 'success');
    },

    removeToolParam(toolName) {
      delete this.savedToolParams[toolName];
      diskStorage.setItem('localai_tool_params', JSON.stringify(this.savedToolParams));
      this.showToast('Defaults removed for ' + toolName, 'info');
    },

    selectAllConversations() {
      this.selectedConvIds = new Set(this.conversations.map(c => c.id));
    },

    async batchDeleteConversations() {
      if (this.selectedConvIds.size === 0) return;
      const ok = await this.showConfirm('Delete ' + this.selectedConvIds.size + ' conversation(s)?');
      if (!ok) return;
      const deletedCurrent = this.selectedConvIds.has(this.currentConversationId);
      for (const id of this.selectedConvIds) {
        const idx = this.conversations.findIndex(c => c.id === id);
        if (idx !== -1) this.conversations.splice(idx, 1);
      }
      this.selectedConvIds = new Set();
      this.batchSelectMode = false;
      if (deletedCurrent) {
        if (this.conversations.length > 0) {
          const next = this.conversations[0];
          this.currentConversationId = next.id;
          this.messages = next.messages || [];
          this.lastStats = next.lastStats || null;
          this.updateContextTokens();
        } else {
          this.currentConversationId = null;
          this.createNewConversation();
        }
      }
      this.saveConversations();
      this.showToast('Conversations deleted', 'success');
    },

    batchTagConversations() {
      if (this.selectedConvIds.size === 0) return;
      const tag = prompt('Enter tag for selected conversations:');
      if (!tag || !tag.trim()) return;
      for (const id of this.selectedConvIds) {
        const conv = this.conversations.find(c => c.id === id);
        if (conv) {
          if (!conv.tags) conv.tags = [];
          if (!conv.tags.includes(tag.trim())) conv.tags.push(tag.trim());
        }
      }
      this.saveConversations();
      this.showToast('Tag added to ' + this.selectedConvIds.size + ' conversation(s)', 'success');
    },

    batchExportConversations() {
      if (this.selectedConvIds.size === 0) return;
      const exported = [];
      for (const id of this.selectedConvIds) {
        const conv = this.conversations.find(c => c.id === id);
        if (conv) exported.push(conv);
      }
      const blob = new Blob([JSON.stringify(exported, null, 2)], { type: 'application/json' });
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = 'conversations-export.json';
      a.click();
      URL.revokeObjectURL(url);
      this.showToast('Exported ' + exported.length + ' conversation(s)', 'success');
    },

    // ─── WebSocket ───────────────────────────────────────────────
    connectWebSocket() {
      // Cancel any pending reconnect timer
      if (this._wsReconnectTimer) {
        clearTimeout(this._wsReconnectTimer);
        this._wsReconnectTimer = null;
      }
      // Close any existing socket to prevent stacked connections
      if (this.ws) {
        try {
          this.ws.onclose = null; // prevent triggering reconnect
          this.ws.onerror = null;
          this.ws.onmessage = null;
          this.ws.close();
        } catch (e) { }
        this.ws = null;
      }
      const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
      const url = `${protocol}//${location.host}/ws/chat`;
      this.ws = new WebSocket(url);

      this.ws.onopen = () => {
        console.log('[ws] connected');
        this.wsConnected = true;
      };

      this.ws.onmessage = (event) => {
        this.handleWsMessage(event.data);
      };

      this.ws.onclose = () => {
        console.log('[ws] disconnected — reconnecting in 2s');
        this.wsConnected = false;
        // Clear any pending reconnect to prevent stacking
        if (this._wsReconnectTimer) clearTimeout(this._wsReconnectTimer);
        this._wsReconnectTimer = setTimeout(() => {
          this._wsReconnectTimer = null;
          this.connectWebSocket();
        }, 2000);
      };

      this.ws.onerror = (err) => {
        console.error('[ws] error', err);
      };
    },

    _scheduleRender() {
      if (this._renderRafTimer) return;
      this._renderRafTimer = setTimeout(() => {
        requestAnimationFrame(() => {
          this.streamingContent = this._rawStreamBuffer;
          this.thinkingContent = this._rawThinkingBuffer;
          this.scrollToBottom();
          this._renderRafTimer = null;
        });
      }, 50); // Flush DOM max 20 times per second
    },

    handleWsMessage(raw) {
      let msg;
      try {
        msg = JSON.parse(raw);
      } catch {
        return;
      }

      switch (msg.type) {
        case 'thinking':
          if (!this.isStreaming) {
            this.isStreaming = true;
            this.streamingContent = '';
            this._rawStreamBuffer = '';
            this.thinkingContent = '';
            this._rawThinkingBuffer = '';
            this.pendingThinkingContent = '';
          }
          this.isThinking = true;
          this._rawThinkingBuffer += msg.content;
          this._scheduleRender();
          break;

        case 'token':
          if (!this.isStreaming) {
            this.isStreaming = true;
            this.streamingContent = '';
            this._rawStreamBuffer = '';
            this.thinkingContent = '';
            this._rawThinkingBuffer = '';
            this.pendingThinkingContent = '';
          }
          // Transition from thinking to content
          if (this.isThinking) {
            this.isThinking = false;
            // Force flush thinking
            this.thinkingContent = this._rawThinkingBuffer;
            this.pendingThinkingContent = this.thinkingContent;
          }
          this.toolStatus = '';
          if (this.searchSteps.length > 0) {
            clearInterval(this._searchTimer);
            this._searchTimer = null;
            this.searchSteps = [];
            this.searchElapsed = '';
          }
          this._rawStreamBuffer += msg.content;
          this._scheduleRender();
          break;

        case 'tool_status': {
          this.streamingContent = '';
          const step = msg.step || 'searching';
          const label = msg.message || '⚙️ Working…';
          if (step === 'rag_context') {
            // RAG retrieval during normal chat (non-search)
            this._searchStart = Date.now();
            this.searchElapsed = '0.0s';
            this.searchSteps = [
              { label: label, state: 'active' },
              { label: 'Generating answer…', state: 'pending' }
            ];
            clearInterval(this._searchTimer);
            this._searchTimer = setInterval(() => {
              const s = ((Date.now() - this._searchStart) / 1000).toFixed(1);
              this.searchElapsed = s + 's';
            }, 100);
          } else if (step === 'searching') {
            this._searchStart = Date.now();
            this.searchElapsed = '0.0s';
            this.searchSteps = [
              { label: label, state: 'active' },
              { label: 'Scraping web pages…', state: 'pending' },
              { label: 'Indexing for RAG…', state: 'pending' },
              { label: 'Retrieving relevant context…', state: 'pending' },
              { label: 'Generating answer…', state: 'pending' }
            ];
            clearInterval(this._searchTimer);
            this._searchTimer = setInterval(() => {
              const s = ((Date.now() - this._searchStart) / 1000).toFixed(1);
              this.searchElapsed = s + 's';
            }, 100);
          } else if (step === 'results') {
            if (this.searchSteps.length >= 2) {
              this.searchSteps[0].state = 'done';
              this.searchSteps[1] = { label: label, state: 'active' };
            }
          } else if (step === 'scraping_done') {
            if (this.searchSteps.length >= 3) {
              this.searchSteps[0].state = 'done';
              this.searchSteps[1].state = 'done';
              this.searchSteps[2] = { label: label, state: 'active' };
            }
          } else if (step === 'rag_retrieval') {
            if (this.searchSteps.length >= 4) {
              this.searchSteps[0].state = 'done';
              this.searchSteps[1].state = 'done';
              this.searchSteps[2].state = 'done';
              this.searchSteps[3] = { label: label, state: 'active' };
            }
          } else if (step === 'generating') {
            // Mark all previous steps as done
            for (let i = 0; i < this.searchSteps.length - 1; i++) {
              this.searchSteps[i].state = 'done';
            }
            if (this.searchSteps.length > 0) {
              this.searchSteps[this.searchSteps.length - 1] = { label: label, state: 'active' };
            }
          }
          this.scrollToBottom();
          break;
        }

        case 'rag_sources':
          // Store RAG sources for the currently streaming response
          this.pendingRagSources = (msg.sources || []).map(s => ({
            ...s,
            expanded: false  // UI state for collapsible text
          }));
          break;

        case 'clear':
          // Server detected a tool-call in RAG mode — reset streamed content
          // so the retried response replaces it cleanly.
          this.streamingContent = '';
          this._rawStreamBuffer = '';
          this.isThinking = false;
          this.thinkingContent = '';
          this._rawThinkingBuffer = '';
          this.pendingThinkingContent = '';
          break;

        case 'done':
          // Force flush any remaining buffer
          if (this._renderRafTimer) {
            clearTimeout(this._renderRafTimer);
            this._renderRafTimer = null;
          }
          this.streamingContent = this._rawStreamBuffer;
          this.thinkingContent = this._rawThinkingBuffer;

          if (this.streamingContent.trim()) {
            const savedMsg = {
              role: 'assistant',
              content: this.streamingContent,
              timestamp: Date.now()
            };
            // Attach thinking content if model used reasoning
            if (this.pendingThinkingContent) {
              savedMsg.thinkingContent = this.pendingThinkingContent;
            }
            // Attach RAG sources if any were received during this response
            if (this.pendingRagSources.length > 0) {
              savedMsg.ragSources = this.pendingRagSources;
            }

            // ONE FINAL RENDER WITH FULL HIGHLIGHTING:
            // The template uses `renderMarkdown(msg.content)` which will hit HLJS
            // because `isStreaming` is about to become false below.
            this.messages.push(savedMsg);
            this.updateContextTokens();
            this.saveConversations(true);
            // Auto-extract memories from this exchange (skip RAG-augmented responses)
            if (this.autoMemoryEnabled && this.pendingRagSources.length === 0) {
              const lastUserMsg = [...this.messages].reverse().find(m => m.role === 'user');
              if (lastUserMsg) {
                this.extractMemories(lastUserMsg.content, this.streamingContent);
              }
            }
          }
          this.isStreaming = false;
          this.streamingContent = '';
          this._rawStreamBuffer = '';
          this.toolStatus = '';
          this.pendingRagSources = [];
          this.isThinking = false;
          this.thinkingContent = '';
          this._rawThinkingBuffer = '';
          this.pendingThinkingContent = '';
          clearInterval(this._searchTimer);
          this._searchTimer = null;
          this.searchSteps = [];
          this.searchElapsed = '';
          this.scrollToBottom();
          break;

        case 'error':
          if (this._renderRafTimer) clearTimeout(this._renderRafTimer);
          this._renderRafTimer = null;
          this.showToast(msg.message || 'Unknown error', 'error');
          this.isStreaming = false;
          this.toolStatus = '';
          this.streamingContent = '';
          this._rawStreamBuffer = '';
          this.pendingRagSources = [];
          this.isThinking = false;
          this.thinkingContent = '';
          this._rawThinkingBuffer = '';
          this.pendingThinkingContent = '';
          clearInterval(this._searchTimer);
          this._searchTimer = null;
          this.searchSteps = [];
          this.searchElapsed = '';
          break;

        case 'stats':
          // Token usage stats from backend
          this.lastStats = {
            eval_count: msg.eval_count || 0,
            prompt_eval_count: msg.prompt_eval_count || 0,
            eval_duration: msg.eval_duration || 0,
            total_duration: msg.total_duration || 0,
            tokens_per_sec: msg.tokens_per_sec || 0
          };
          this.sessionStats.totalGenerated += this.lastStats.eval_count;
          this.sessionStats.totalPrompt += this.lastStats.prompt_eval_count;
          this.sessionStats.totalDuration += this.lastStats.total_duration;
          this.sessionStats.requestCount++;
          diskStorage.setItem('localai_session_stats', JSON.stringify(this.sessionStats));
          this.saveConversations();
          break;
      }
    },

    // ─── Stop generation ────────────────────────────────────────
    stopGeneration() {
      if (!this.isStreaming) return;
      // Tell the backend to abort the current stream
      if (this.ws && this.ws.readyState === WebSocket.OPEN) {
        this.ws.send(JSON.stringify({ type: 'stop' }));
      }
      // Save any partial content as an assistant message
      if (this.streamingContent.trim()) {
        const stoppedMsg = {
          role: 'assistant',
          content: this.streamingContent,
          stopped: true,
          timestamp: Date.now()
        };
        // Preserve thinking content from reasoning models (qwen3, etc.)
        if (this.pendingThinkingContent) {
          stoppedMsg.thinkingContent = this.pendingThinkingContent;
        }
        // Preserve RAG citation sources if any were received
        if (this.pendingRagSources.length > 0) {
          stoppedMsg.ragSources = this.pendingRagSources;
        }
        this.messages.push(stoppedMsg);
        this.updateContextTokens();
        this.saveConversations(true);
      }
      this.streamingContent = '';
      this.isStreaming = false;
      this.toolStatus = '';
      this.pendingRagSources = [];
      this.isThinking = false;
      this.thinkingContent = '';
      this.pendingThinkingContent = '';
      clearInterval(this._searchTimer);
      this._searchTimer = null;
      this.searchSteps = [];
      this.searchElapsed = '';
      this.scrollToBottom();
    },

    // ─── Drag & drop file upload ─────────────────────────────────
    _blockedFileExtensions: new Set([
      'jpg', 'jpeg', 'png', 'gif', 'bmp', 'webp', 'ico', 'tiff', 'tif', 'svg',
      'mp3', 'wav', 'ogg', 'flac', 'aac', 'm4a', 'wma',
      'mp4', 'avi', 'mov', 'mkv', 'wmv', 'flv', 'webm',
      'zip', 'tar', 'gz', 'rar', '7z', 'bz2', 'xz',
      'exe', 'dll', 'so', 'dylib', 'bin', 'dmg', 'iso', 'img',
      'ttf', 'otf', 'woff', 'woff2', 'eot',
    ]),

    handleDrop(event) {
      this.isDragging = false;
      const files = event.dataTransfer?.files;
      if (!files || files.length === 0) return;

      // Filter out unsupported binary file types
      const fileList = Array.from(files);
      const accepted = [];
      const rejected = [];
      for (const f of fileList) {
        const ext = (f.name.split('.').pop() || '').toLowerCase();
        if (this._blockedFileExtensions.has(ext)) {
          rejected.push(f.name);
        } else {
          accepted.push(f);
        }
      }
      if (rejected.length > 0) {
        const names = rejected.length <= 3 ? rejected.join(', ') : rejected.slice(0, 3).join(', ') + ` +${rejected.length - 3} more`;
        this.showToast(`Unsupported file type: ${names}`, 'error');
      }
      if (accepted.length === 0) return;

      this.isUploading = true;
      const total = accepted.length;
      let succeeded = 0;
      let i = 0;

      const next = async () => {
        if (i >= accepted.length) {
          this.isUploading = false;
          this.uploadProgress = '';
          if (total > 1) {
            this.showToast(`${succeeded} of ${total} files accepted for indexing`);
          }
          return;
        }
        const file = accepted[i];
        const label = total > 1 ? `(${i + 1}/${total}) ` : '';
        this.uploadProgress = `${label}Uploading "${file.name}" (${this.formatFileSize(file.size)})…`;
        const ok = await this.uploadRagDocument(file);
        if (ok) succeeded++;
        i++;
        await next();
      };
      next();
    },

    // ─── Send a message ──────────────────────────────────────────
    sendMessage() {
      const text = this.inputText.trim();
      if (!text || this.isStreaming) return;

      // Create a new conversation if none exists
      if (!this.currentConversationId) {
        this.createNewConversation();
      }

      this.messages.push({ role: 'user', content: text, timestamp: Date.now() });
      this.autoTitleConversation(text);
      this.updateContextTokens();
      this.inputText = '';
      this.isStreaming = true;
      this.streamingContent = '';

      this.$nextTick(() => {
        if (this.$refs.inputBox) {
          this.$refs.inputBox.style.height = 'auto';
        }
      });

      // Use context-managed messages for history
      const contextMsgs = this.getContextMessages();

      // Exclude the last message (the one we just pushed) from history
      // because the backend's buildMessages() will append it from the
      // 'message' field — sending it in both places duplicates it.
      const historyMsgs = contextMsgs.slice(0, -1);

      const payload = {
        message: text,
        model: this.currentModel || (this.models.length > 0 ? this.models[0] : 'gemma3:4b'),
        temperature: this.temperature,
        webSearch: this.webSearchEnabled,
        ragEnabled: this.ragEnabled && this.ragDocuments.length > 0,
        memory: this.getMemoryPrompt(),
        systemPrompt: this.getCurrentSystemPrompt(),
        history: historyMsgs.filter(m => m.role !== 'tool').map(m => ({
          role: m.role,
          content: m.content
        }))
      };

      if (this.ws && this.ws.readyState === WebSocket.OPEN) {
        this.ws.send(JSON.stringify(payload));
      } else {
        this.showToast('WebSocket is not connected.', 'error');
        this.isStreaming = false;
      }

      this.saveConversations();
      this.scrollToBottom(true);
    },

    // ─── Regenerate a bot response ───────────────────────────────
    regenerateMessage(idx) {
      const msg = this.messages[idx];
      if (!msg || msg.role !== 'assistant') return;
      if (this.isStreaming) return;

      let userContent = '';
      let userMsgRef = null;
      for (let i = idx - 1; i >= 0; i--) {
        if (this.messages[i].role === 'user') {
          userContent = this.messages[i].content;
          userMsgRef = this.messages[i];
          break;
        }
      }
      if (!userContent) return;

      this.messages.splice(idx, 1);
      this.updateContextTokens();
      this.saveConversations();

      this.isStreaming = true;
      this.streamingContent = '';

      const contextMsgs = this.getContextMessages();

      // Exclude the user message we're re-sending from history by
      // object reference — avoids false matches when the same text
      // appears in multiple user messages.
      const historyMsgs = [];
      let excluded = false;
      for (let i = contextMsgs.length - 1; i >= 0; i--) {
        if (!excluded && contextMsgs[i] === userMsgRef) {
          excluded = true;
          continue;
        }
        historyMsgs.unshift(contextMsgs[i]);
      }

      const payload = {
        message: userContent,
        model: this.currentModel || (this.models.length > 0 ? this.models[0] : 'gemma3:4b'),
        temperature: this.temperature,
        webSearch: this.webSearchEnabled,
        ragEnabled: this.ragEnabled && this.ragDocuments.length > 0,
        memory: this.getMemoryPrompt(),
        systemPrompt: this.getCurrentSystemPrompt(),
        history: historyMsgs.filter(m => m.role !== 'tool').map(m => ({
          role: m.role,
          content: m.content
        }))
      };

      if (this.ws && this.ws.readyState === WebSocket.OPEN) {
        this.ws.send(JSON.stringify(payload));
      } else {
        this.showToast('WebSocket is not connected.', 'error');
        this.isStreaming = false;
      }
    },

    // ─── Utilities ───────────────────────────────────────────────
    clearHistory() {
      this.createNewConversation();
    },

    copyMessage(content) {
      navigator.clipboard.writeText(content).then(() => {
        this.showToast('Copied to clipboard');
      }).catch(() => {
        this.showToast('Failed to copy', 'error');
      });
    },

    renderMarkdown(text) {
      if (!text) return '';
      try {
        if (!this._cachedRenderer) this._cachedRenderer = this._buildMarkdownRenderer();
        const html = marked.parse(text, { breaks: true, gfm: true, renderer: this._cachedRenderer });
        return this.sanitizeHtml(html);
      } catch (err) {
        console.warn('Markdown rendering failed, falling back to plain text:', err);
        return this.escapeHtml(text);
      }
    },

    sanitizeHtml(html) {
      const template = document.createElement('template');
      template.innerHTML = html;
      const allowedTags = new Set([
        'A', 'P', 'BR', 'STRONG', 'EM', 'DEL', 'BLOCKQUOTE', 'CODE', 'PRE',
        'UL', 'OL', 'LI', 'H1', 'H2', 'H3', 'H4', 'H5', 'H6', 'HR',
        'TABLE', 'THEAD', 'TBODY', 'TR', 'TH', 'TD', 'IMG', 'SPAN', 'DIV',
        'BUTTON'
      ]);
      const allowedAttrs = {
        A: new Set(['href', 'title', 'target', 'rel']),
        IMG: new Set(['src', 'alt', 'title']),
        CODE: new Set(['class']),
        SPAN: new Set(['class']),
        DIV: new Set(['class']),
        BUTTON: new Set(['class', 'type', 'data-copy-id']),
        TH: new Set(['align']),
        TD: new Set(['align'])
      };

      for (const element of [...template.content.querySelectorAll('*')]) {
        if (!allowedTags.has(element.tagName)) {
          element.replaceWith(document.createTextNode(element.textContent || ''));
          continue;
        }

        const permitted = allowedAttrs[element.tagName] || new Set();
        for (const attr of [...element.attributes]) {
          if (!permitted.has(attr.name.toLowerCase())) {
            element.removeAttribute(attr.name);
          }
        }

        if (element.tagName === 'A') {
          const href = (element.getAttribute('href') || '').trim();
          try {
            const protocol = new URL(href, window.location.href).protocol;
            if (!['http:', 'https:', 'mailto:', 'tel:'].includes(protocol)) {
              element.removeAttribute('href');
            }
          } catch (_) {
            element.removeAttribute('href');
          }
          element.setAttribute('target', '_blank');
          element.setAttribute('rel', 'noopener noreferrer');
        } else if (element.tagName === 'IMG') {
          const src = (element.getAttribute('src') || '').trim();
          let safe = false;
          try {
            const url = new URL(src, window.location.href);
            safe = url.protocol === 'http:' || url.protocol === 'https:';
          } catch (_) { }
          if (/^data:image\/(?:png|jpe?g|gif|webp);base64,/i.test(src)) safe = true;
          if (!safe) element.remove();
        } else if (element.tagName === 'BUTTON') {
          const isCopyButton = element.classList.contains('code-copy-btn') &&
            element.hasAttribute('data-copy-id');
          if (!isCopyButton) {
            element.replaceWith(document.createTextNode(element.textContent || ''));
          }
        }
      }
      return template.innerHTML;
    },

    _buildMarkdownRenderer() {
      const self = this;
      const renderer = new marked.Renderer();
      // Make links open externally
      renderer.link = function (href, title, text) {
        const h = (href?.href || href || '').replace(/"/g, '&quot;');
        const t = href?.text || text || h;
        const protoMatch = h.match(/^([a-zA-Z][a-zA-Z0-9+\-.]*?):/);
        const allowedProtocols = ['http', 'https', 'mailto', 'tel'];
        if (protoMatch && !allowedProtocols.includes(protoMatch[1].toLowerCase())) {
          return `<span>${t}</span>`;
        }
        const rawTitle = href?.title || title || '';
        const titleAttr = rawTitle ? ` title="${rawTitle.replace(/"/g, '&quot;')}"` : '';
        return `<a href="${h}" target="_blank" rel="noopener noreferrer"${titleAttr}>${t}</a>`;
      };
      // Syntax-highlighted code blocks with language label + copy button
      renderer.code = function (codeObj) {
        const code = codeObj?.text ?? codeObj ?? '';
        const lang = codeObj?.lang || '';
        let highlighted;

        // LAZY HIGHLIGHTING: Skip heavy hljs parsing while streaming.
        if (self.isStreaming) {
          highlighted = code.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
        } else {
          if (lang && typeof hljs !== 'undefined' && hljs.getLanguage(lang)) {
            highlighted = hljs.highlight(code, { language: lang }).value;
          } else if (typeof hljs !== 'undefined') {
            highlighted = hljs.highlightAuto(code).value;
          } else {
            highlighted = code.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
          }
        }

        const langLabel = lang ? `<span class="code-lang-label">${lang.replace(/[<>"&]/g, '')}</span>` : '';
        if (!window.__codeCopyStore) window.__codeCopyStore = {};
        const now = Date.now();
        if (!window.__codeCopyLastPurge || now - window.__codeCopyLastPurge > 60000) {
          const cutoff = now - 300000;
          for (const k of Object.keys(window.__codeCopyStore)) {
            const ts = parseInt(k.split('-')[1] || '0', 36);
            if (ts < cutoff) delete window.__codeCopyStore[k];
          }
          window.__codeCopyLastPurge = now;
        }
        const copyId = 'cc-' + now.toString(36) + '-' + Math.random().toString(36).slice(2, 7);
        window.__codeCopyStore[copyId] = code;
        const copyBtn = `<button type="button" class="code-copy-btn" data-copy-id="${copyId}">Copy</button>`;
        return `<div class="code-block-wrapper">${langLabel}${copyBtn}<pre><code class="hljs${lang ? ' language-' + lang.replace(/[<>"&]/g, '') : ''}">${highlighted}</code></pre></div>`;
      };
      // Security: block dangerous protocols in image sources
      renderer.image = function (href, title, text) {
        const src = (href?.href || href || '').replace(/"/g, '&quot;');
        const alt = (href?.text || text || '').replace(/"/g, '&quot;');
        const proto = src.match(/^([a-zA-Z][a-zA-Z0-9+\-.]*?):/);
        if (proto && !['http', 'https', 'data'].includes(proto[1].toLowerCase())) {
          return `<span>[image blocked]</span>`;
        }
        const rawTitle = href?.title || title || '';
        const titleAttr = rawTitle ? ` title="${rawTitle.replace(/"/g, '&quot;')}"` : '';
        return `<img src="${src}" alt="${alt}"${titleAttr} style="max-width:100%">`;
      };
      return renderer;
    },

    escapeHtml(text) {
      const div = document.createElement('div');
      div.textContent = text;
      return div.innerHTML;
    },

    autoResize(event) {
      const el = event.target;
      el.style.height = 'auto';
      el.style.height = Math.min(el.scrollHeight, 200) + 'px';
    },

    _scrollDebounceTimer: null,

    scrollToBottom(force = false) {
      if (this.userScrolledUp && !force) return;
      this.userScrolledUp = false;

      if (force) {
        this._executeScroll();
      } else {
        if (this._scrollDebounceTimer) return;
        this._scrollDebounceTimer = setTimeout(() => {
          this._executeScroll();
          this._scrollDebounceTimer = null;
        }, 30);
      }
    },

    _executeScroll() {
      this.$nextTick(() => {
        const container = this.$refs.chatContainer;
        if (container) {
          container.scrollTop = container.scrollHeight;
        }
      });
    },

    handleChatScroll() {
      const container = this.$refs.chatContainer;
      if (!container) return;
      // Consider "at bottom" if within 120px of the end
      const atBottom = container.scrollHeight - container.scrollTop - container.clientHeight < 120;
      this.userScrolledUp = !atBottom;
    },

    // ─── Empty state suggestion chips ────────────────────────────
    handleSuggestion(text) {
      this.inputText = text;
      this.$nextTick(() => {
        this.sendMessage();
      });
    },

    // ─── Theme ───────────────────────────────────────────────────
    applyTheme() {
      const theme = this.settings.theme || 'system';
      let dark = false;
      if (theme === 'dark') dark = true;
      else if (theme === 'system') dark = window.matchMedia('(prefers-color-scheme: dark)').matches;

      document.documentElement.setAttribute('data-theme', dark ? 'dark' : 'light');

      // Swap highlight.js theme for code blocks
      const hlLink = document.querySelector('link[href*="highlightjs"]');
      if (hlLink) {
        hlLink.href = dark
          ? 'https://cdn.jsdelivr.net/gh/highlightjs/cdn-release@11.9.0/build/styles/github-dark.min.css'
          : 'https://cdn.jsdelivr.net/gh/highlightjs/cdn-release@11.9.0/build/styles/github.min.css';
      }
    },

    // ─── Toast notifications ─────────────────────────────────────
    showToast(message, type = 'success') {
      const toast = document.createElement('div');
      toast.className = 'toast-notification' + (type === 'error' ? ' error' : '');
      toast.textContent = message;
      document.body.appendChild(toast);

      requestAnimationFrame(() => {
        toast.classList.add('visible');
      });

      setTimeout(() => {
        toast.classList.remove('visible');
        setTimeout(() => toast.remove(), 300);
      }, 3000);
    },

    // ─── Custom confirm dialog (replaces window.confirm) ─────────
    showConfirm(message) {
      return new Promise((resolve) => {
        this.confirmMessage = message;
        this._confirmResolve = resolve;
        this.confirmVisible = true;
      });
    },

    resolveConfirm(result) {
      this.confirmVisible = false;
      if (this._confirmResolve) {
        this._confirmResolve(result);
        this._confirmResolve = null;
      }
    },

    // ─── API fetchers ────────────────────────────────────────────
    async fetchModels() {
      try {
        const resp = await fetch('/api/models');
        const data = await resp.json();
        if (data.models && data.models.length > 0) {
          this.models = data.models;
          // Use settings default model if available, otherwise first model
          if (this.settings.defaultModel && this.models.includes(this.settings.defaultModel)) {
            if (!this.currentModel) this.currentModel = this.settings.defaultModel;
          } else if (!this.currentModel) {
            this.currentModel = data.models[0];
          }
          // Store per-model context lengths from Ollama
          if (data.model_context) {
            this.modelContextMap = data.model_context;
            this.updateMaxContext();
          }
        }
      } catch (err) {
        console.warn('Failed to fetch models:', err);
      }
    },

    async fetchMcpServers() {
      try {
        const resp = await fetch('/api/mcp/servers');
        const data = await resp.json();
        if (data.servers) {
          this.mcpServers = data.servers.map(s => {
            if (typeof s === 'string') return { name: s, status: 'running' };
            return { ...s, status: s.status || 'running' };
          });
        }
      } catch (err) {
        console.warn('Failed to fetch MCP servers:', err);
      }
    },

    // ─── MCP management ──────────────────────────────────────────
    openMcpModal() {
      this.mcpForm = { name: '', transport: 'stdio', command: '', args: '', url: '' };
      this.showMcpModal = true;
    },

    closeMcpModal() {
      this.showMcpModal = false;
    },

    async addMcpServer() {
      const name = this.mcpForm.name.trim();
      const transport = this.mcpForm.transport;

      if (!name) {
        this.showToast('Server name is required', 'error');
        return;
      }

      let payload = { name, transport };

      if (transport === 'http') {
        const url = this.mcpForm.url.trim();
        if (!url) {
          this.showToast('URL is required for remote servers', 'error');
          return;
        }
        payload.url = url;
      } else {
        const command = this.mcpForm.command.trim();
        if (!command) {
          this.showToast('Command is required for local servers', 'error');
          return;
        }
        const argsStr = this.mcpForm.args.trim();
        const args = argsStr ? argsStr.split(/\s+/) : [];
        payload.command = command;
        payload.args = args;
      }

      try {
        const resp = await fetch('/api/mcp/servers', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(payload)
        });
        const data = await resp.json();
        if (data.success) {
          this.showToast(`MCP server "${name}" added`);
          this.closeMcpModal();
          await this.fetchMcpServers();
        } else {
          this.showToast(data.error || 'Failed to add server', 'error');
        }
      } catch (err) {
        this.showToast('Failed to add MCP server: ' + err.message, 'error');
      }
    },

    async removeMcpServer(name) {
      if (!(await this.showConfirm(`Remove MCP server "${name}"?`))) return;

      try {
        const resp = await fetch('/api/mcp/servers/remove', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ name })
        });
        const data = await resp.json();
        if (data.success) {
          this.showToast(`MCP server "${name}" removed`);
          await this.fetchMcpServers();
        } else {
          this.showToast(data.error || 'Failed to remove server', 'error');
        }
      } catch (err) {
        this.showToast('Failed to remove MCP server: ' + err.message, 'error');
      }
    },

    async restartMcpServer(name) {
      try {
        const resp = await fetch('/api/mcp/servers/restart', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ name })
        });
        const data = await resp.json();
        if (data.success) {
          this.showToast(`MCP server "${name}" restarted`);
          await this.fetchMcpServers();
        } else {
          this.showToast(data.error || 'Failed to restart server', 'error');
        }
      } catch (err) {
        this.showToast('Failed to restart MCP server: ' + err.message, 'error');
      }
    },

    // ─── RAG document management ─────────────────────────────────
    async fetchRagDocuments() {
      try {
        const resp = await fetch('/api/rag/documents');
        const data = await resp.json();
        if (data.documents) {
          this.ragDocuments = data.documents;
        }
      } catch (err) {
        console.warn('Failed to fetch RAG documents:', err);
      }
    },

    triggerFileUpload() {
      const input = this.$refs.ragFileInput;
      if (input) input.click();
    },

    async handleFileUpload(event) {
      // Guard against re-entrant calls while an upload is in progress
      if (this.isUploading) return;

      // Snapshot the selected files immediately — the FileList can become
      // invalid if the input element re-renders during async work.
      const fileList = Array.from(event.target.files || []);
      // Reset the file input right away so it can be used again and so
      // Alpine re-renders don't interfere with the FileList reference.
      event.target.value = '';

      if (fileList.length === 0) return;

      // Filter out unsupported binary file types (same check as drag-drop)
      const accepted = [];
      const rejected = [];
      for (const f of fileList) {
        const ext = (f.name.split('.').pop() || '').toLowerCase();
        if (this._blockedFileExtensions.has(ext)) {
          rejected.push(f.name);
        } else {
          accepted.push(f);
        }
      }
      if (rejected.length > 0) {
        const names = rejected.length <= 3 ? rejected.join(', ') : rejected.slice(0, 3).join(', ') + ` +${rejected.length - 3} more`;
        this.showToast(`Unsupported file type: ${names}`, 'error');
      }
      if (accepted.length === 0) return;

      this.isUploading = true;
      const total = accepted.length;
      let succeeded = 0;

      for (let i = 0; i < accepted.length; i++) {
        const file = accepted[i];
        const label = total > 1 ? `(${i + 1}/${total}) ` : '';
        this.uploadProgress = `${label}Uploading "${file.name}" (${this.formatFileSize(file.size)})…`;
        const ok = await this.uploadRagDocument(file);
        if (ok) succeeded++;
      }

      this.isUploading = false;
      this.uploadProgress = '';

      if (total > 1) {
        this.showToast(`${succeeded} of ${total} files accepted for indexing`);
      }
    },

    async uploadRagDocument(file) {
      try {
        // Use standard multipart/form-data upload.
        const formData = new FormData();
        formData.append('file', file, file.name);
        formData.append('model', 'nomic-embed-text:latest');

        const resp = await fetch('/api/rag/upload', {
          method: 'POST',
          body: formData  // browser sets Content-Type: multipart/form-data automatically
        });

        const data = await resp.json();
        if (data.success) {
          this.showToast(`"${file.name}" accepted — indexing in background…`);
          await this.fetchRagDocuments();
          // Poll until indexing finishes (chunks changes from -1 to actual count).
          this.pollRagIndexing(data.document.id);
          return true;
        } else {
          this.showToast(data.error || `Upload failed for "${file.name}"`, 'error');
          return false;
        }
      } catch (err) {
        this.showToast(`Failed to upload "${file.name}": ${err.message}`, 'error');
        return false;
      }
    },

    pollRagIndexing(docId) {
      if (!this._ragPollers) this._ragPollers = new Map();
      // Clear any existing poller for this doc
      if (this._ragPollers.has(docId)) clearInterval(this._ragPollers.get(docId));
      let elapsed = 0;
      const poll = setInterval(async () => {
        elapsed += 2000;
        await this.fetchRagDocuments();
        const doc = this.ragDocuments.find(d => d.id === docId);
        if (!doc || doc.chunks >= 0 || elapsed >= 120000) {
          clearInterval(poll);
          this._ragPollers.delete(docId);
          if (elapsed >= 120000 && doc && doc.chunks < 0) {
            this.showToast(`"${doc.name}" indexing timed out`, 'error');
          } else if (doc && doc.chunks > 0) {
            this.showToast(`"${doc.name}" indexed — ${doc.chunks} chunks ready`);
          } else if (doc && doc.chunks === 0) {
            this.showToast(`"${doc.name}" indexing failed — 0 chunks`, 'error');
          }
        }
      }, 2000);
      this._ragPollers.set(docId, poll);
    },

    cleanupRagPollers() {
      if (!this._ragPollers) return;
      for (const id of this._ragPollers.values()) clearInterval(id);
      this._ragPollers.clear();
    },

    async removeRagDocument(docId, docName) {
      if (!(await this.showConfirm(`Remove "${docName}" from RAG?`))) return;

      try {
        const resp = await fetch('/api/rag/documents/remove', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ id: docId })
        });
        const data = await resp.json();
        if (data.success) {
          this.showToast(`"${docName}" removed`);
          await this.fetchRagDocuments();
        } else {
          this.showToast(data.error || 'Failed to remove', 'error');
        }
      } catch (err) {
        this.showToast('Failed to remove document: ' + err.message, 'error');
      }
    },

    async clearRagDocuments() {
      if (!(await this.showConfirm('Remove all uploaded documents from RAG?'))) return;

      try {
        const resp = await fetch('/api/rag/clear', {
          method: 'POST'
        });
        const data = await resp.json();
        if (data.success) {
          this.showToast('All RAG documents cleared');
          this.ragDocuments = [];
        } else {
          this.showToast(data.error || 'Failed to clear', 'error');
        }
      } catch (err) {
        this.showToast('Failed to clear RAG: ' + err.message, 'error');
      }
    },

    formatFileSize(bytes) {
      if (bytes < 1024) return bytes + ' B';
      if (bytes < 1048576) return (bytes / 1024).toFixed(1) + ' KB';
      return (bytes / 1048576).toFixed(1) + ' MB';
    },

    toggleSource(msg, sourceIdx) {
      if (Array.isArray(msg.ragSources) && msg.ragSources[sourceIdx]) {
        msg.ragSources[sourceIdx].expanded = !msg.ragSources[sourceIdx].expanded;
      }
    },

    toggleAllSources(msg) {
      if (!Array.isArray(msg.ragSources)) return;
      msg._sourcesCollapsed = !msg._sourcesCollapsed;
    },

    get hasRagDocuments() {
      return this.ragDocuments.length > 0;
    },

    get ragTotalChunks() {
      return this.ragDocuments.reduce((sum, d) => sum + Math.max(0, d.chunks), 0);
    },

    // ─── Model Manager (#13) ─────────────────────────────────────
    async openModelManager() {
      this.showModelManager = true;
      if (!this.isModelPulling) {
        this.modelPullProgress = null;
        this.modelPullName = '';
      }
      await this.fetchModelDetails();
    },

    async fetchModelDetails() {
      try {
        const resp = await fetch('/api/models/details');
        const data = await resp.json();
        if (data.models) this.modelDetails = data.models;
      } catch (err) {
        console.warn('Failed to fetch model details:', err);
      }
    },

    formatModelSize(bytes) {
      if (!bytes) return '—';
      const gb = bytes / (1024 * 1024 * 1024);
      if (gb >= 1) return gb.toFixed(1) + ' GB';
      return (bytes / (1024 * 1024)).toFixed(0) + ' MB';
    },

    async pullModel() {
      const name = this.modelPullName.trim();
      if (!name || this.isModelPulling) return;
      this.isModelPulling = true;
      this.modelPullProgress = { status: 'Starting…', percent: 0 };
      try {
        const resp = await fetch('/api/models/pull', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ name })
        });
        const reader = resp.body.getReader();
        const decoder = new TextDecoder();
        let buffer = '';
        while (true) {
          const { value, done } = await reader.read();
          if (done) break;
          buffer += decoder.decode(value, { stream: true });
          const lines = buffer.split('\n');
          buffer = lines.pop();
          for (const line of lines) {
            if (!line.trim()) continue;
            try {
              const j = JSON.parse(line);
              if (j.error) {
                this.modelPullProgress = { status: 'Error: ' + j.error, percent: 0 };
                this.isModelPulling = false;
                return;
              }
              const pct = (j.completed && j.total) ? Math.round((j.completed / j.total) * 100) : 0;
              this.modelPullProgress = {
                status: j.status || 'Downloading…',
                percent: pct,
                completed: j.completed,
                total: j.total
              };
            } catch { }
          }
        }
        this.modelPullProgress = { status: 'Done!', percent: 100 };
        this.showToast(`Model "${name}" pulled successfully`);
        this.modelPullName = '';
        await this.fetchModelDetails();
        await this.fetchModels();
      } catch (err) {
        this.modelPullProgress = { status: 'Error: ' + err.message, percent: 0 };
      }
      this.isModelPulling = false;
    },

    async deleteModel(modelName) {
      if (!(await this.showConfirm(`Delete model "${modelName}"? This cannot be undone.`))) return;
      try {
        const resp = await fetch('/api/models/delete', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ name: modelName })
        });
        const data = await resp.json();
        if (data.success) {
          this.showToast(`Model "${modelName}" deleted`);
          await this.fetchModelDetails();
          await this.fetchModels();
        } else {
          this.showToast(data.error || 'Failed to delete model', 'error');
        }
      } catch (err) {
        this.showToast('Failed to delete: ' + err.message, 'error');
      }
    },

    // ─── Conversation Export (#14) ────────────────────────────────
    async exportConversation(format) {
      const conv = this.conversations.find(c => c.id === this.currentConversationId);
      if (!conv || !conv.messages || conv.messages.length === 0) {
        this.showToast('No messages to export', 'error');
        return;
      }
      let content, filename, mimeType;
      const title = conv.title || 'conversation';
      const safeTitle = title.replace(/[^a-zA-Z0-9_-]/g, '_');

      if (format === 'json') {
        content = JSON.stringify({
          title: conv.title,
          createdAt: conv.createdAt,
          updatedAt: conv.updatedAt,
          tags: conv.tags || [],
          systemPrompt: conv.systemPrompt || '',
          messages: conv.messages.map(m => {
            const entry = { role: m.role, content: m.content, timestamp: m.timestamp };
            if (m.thinkingContent) entry.thinkingContent = m.thinkingContent;
            if (m.ragSources) entry.ragSources = m.ragSources;
            if (m.stopped) entry.stopped = true;
            return entry;
          })
        }, null, 2);
        filename = safeTitle + '.json';
        mimeType = 'application/json';
      } else {
        // Markdown
        let md = '# ' + title + '\n\n';
        md += '_Exported on ' + new Date().toLocaleString() + '_\n\n---\n\n';
        for (const m of conv.messages) {
          const label = m.role === 'user' ? '**You**' : '**Assistant**';
          md += label + ':\n\n';
          if (m.thinkingContent) {
            md += '<details><summary>Thinking</summary>\n\n' + m.thinkingContent + '\n\n</details>\n\n';
          }
          md += m.content;
          if (m.stopped) md += '\n\n*generation stopped*';
          md += '\n\n---\n\n';
        }
        content = md;
        filename = safeTitle + '.md';
        mimeType = 'text/markdown';
      }
      // Use native saveFile binding if available (webview), else fallback to blob download
      if (typeof window.saveFile === 'function') {
        try {
          const result = await window.saveFile(filename, content);
          const parsed = typeof result === 'string' ? JSON.parse(result) : result;
          if (parsed.success) {
            this.showToast(`Saved to Downloads: ${filename}`);
          } else {
            this.showToast(parsed.error || 'Failed to save', 'error');
          }
        } catch (err) {
          this.showToast('Export failed: ' + err.message, 'error');
        }
      } else {
        const blob = new Blob([content], { type: mimeType });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = filename;
        document.body.appendChild(a);
        a.click();
        a.remove();
        URL.revokeObjectURL(url);
        this.showToast(`Exported as ${format.toUpperCase()}`);
      }
    },

    // ─── Chat Branching (#15) ─────────────────────────────────────
    branchFromMessage(idx) {
      if (this.isStreaming) return;
      this.stopSpeaking();
      const conv = this.conversations.find(c => c.id === this.currentConversationId);
      if (!conv) return;
      // Fork messages up to and including the selected index
      const forkedMessages = JSON.parse(JSON.stringify(conv.messages.slice(0, idx + 1)));
      this.saveConversations();
      const newConv = {
        id: generateId(),
        title: '🔀 Branch: ' + (conv.title || 'Chat'),
        messages: forkedMessages,
        tags: [...(conv.tags || [])],
        createdAt: Date.now(),
        updatedAt: Date.now(),
        branchedFrom: conv.id
      };
      this.conversations.unshift(newConv);
      this.currentConversationId = newConv.id;
      this.messages = forkedMessages;
      this.updateContextTokens();
      this.saveConversations();
      this.showToast('Branched conversation created');
    },

    // ─── Conversation Tags (#16) ─────────────────────────────────
    getConvTags(conv) {
      return conv.tags || [];
    },

    tagSuggestionsFor(convId) {
      const conv = this.conversations.find(c => c.id === convId);
      const existing = conv?.tags || [];
      const q = this.newTagText.trim().toLowerCase();
      return this.allTags
        .filter(t => !existing.includes(t))
        .filter(t => !q || t.toLowerCase().includes(q));
    },

    selectTagSuggestion(convId, tag) {
      this.newTagText = tag;
      this.addTagToConversation(convId);
    },

    addTagToConversation(convId) {
      const tag = this.newTagText.trim();
      if (!tag) return;
      const conv = this.conversations.find(c => c.id === convId);
      if (!conv) return;
      if (!conv.tags) conv.tags = [];
      if (!conv.tags.includes(tag)) {
        conv.tags.push(tag);
        this.saveConversations();
      }
      this.newTagText = '';
    },

    removeTagFromConversation(convId, tag) {
      const conv = this.conversations.find(c => c.id === convId);
      if (!conv || !conv.tags) return;
      conv.tags = conv.tags.filter(t => t !== tag);
      this.saveConversations();
    },

    get allTags() {
      const tags = new Set();
      for (const c of this.conversations) {
        for (const t of (c.tags || [])) tags.add(t);
      }
      return [...tags].sort();
    },

    get filteredConversationsWithTags() {
      let list = this.filteredConversations;
      if (this.tagFilter) {
        list = list.filter(c => (c.tags || []).includes(this.tagFilter));
      }
      return list;
    },

    get conversationGroups() {
      const list = this.filteredConversationsWithTags;
      if (list.length === 0) return [];
      const pinned = list.filter(c => c.pinned);
      const unpinned = list.filter(c => !c.pinned);
      // Sort unpinned by timestamp descending
      const sorted = [...unpinned].sort((a, b) => {
        const ta = a.updatedAt || a.createdAt || 0;
        const tb = b.updatedAt || b.createdAt || 0;
        return tb - ta;
      });
      const groups = [];
      // Pinned group always first
      if (pinned.length > 0) {
        groups.push({ label: '📌 Pinned', conversations: pinned });
      }
      const now = new Date();
      const todayStart = new Date(now.getFullYear(), now.getMonth(), now.getDate()).getTime();
      const yesterdayStart = todayStart - 86400000;
      const weekStart = todayStart - 6 * 86400000;
      let currentLabel = null;
      for (const conv of sorted) {
        const ts = conv.updatedAt || conv.createdAt || 0;
        let label;
        if (ts >= todayStart) label = 'Today';
        else if (ts >= yesterdayStart) label = 'Yesterday';
        else if (ts >= weekStart) label = 'This Week';
        else label = 'Older';
        if (label !== currentLabel) {
          groups.push({ label, conversations: [] });
          currentLabel = label;
        }
        groups[groups.length - 1].conversations.push(conv);
      }
      return groups;
    },

    // ─── Token Usage helpers (#17) ────────────────────────────────
    formatTokenSpeed(tps) {
      if (!tps || tps <= 0) return '—';
      return tps.toFixed(1) + ' tok/s';
    },

    formatDuration(sec) {
      if (!sec || sec <= 0) return '—';
      if (sec < 1) return (sec * 1000).toFixed(0) + 'ms';
      return sec.toFixed(1) + 's';
    },

    // ─── Message Edit & Resend (#20) ─────────────────────────────
    startEditMessage(idx) {
      if (this.isStreaming) return;
      this.editingMessageIdx = idx;
      this.editMessageText = this.messages[idx].content;
    },

    cancelEditMessage() {
      this.editingMessageIdx = null;
      this.editMessageText = '';
    },

    confirmEditMessage(idx) {
      const text = this.editMessageText.trim();
      if (!text) return;
      if (this.isStreaming) return;

      // Stop TTS before truncating — avoids playing audio for removed messages
      this.stopSpeaking();

      // Truncate conversation from this message onwards and resend
      this.messages = this.messages.slice(0, idx);
      this.editingMessageIdx = null;
      this.editMessageText = '';

      // Push the edited message as a new user message
      this.messages.push({ role: 'user', content: text, timestamp: Date.now() });
      this.updateContextTokens();

      // Send it
      this.isStreaming = true;
      this.streamingContent = '';

      const contextMsgs = this.getContextMessages();
      // Exclude the last message (the one we just pushed) from history
      // because the backend's buildMessages() will append it from the
      // 'message' field — sending it in both places duplicates it.
      const historyMsgs = contextMsgs.slice(0, -1);
      const payload = {
        message: text,
        model: this.currentModel || (this.models.length > 0 ? this.models[0] : 'gemma3:4b'),
        temperature: this.temperature,
        webSearch: this.webSearchEnabled,
        ragEnabled: this.ragEnabled && this.ragDocuments.length > 0,
        memory: this.getMemoryPrompt(),
        systemPrompt: this.getCurrentSystemPrompt(),
        history: historyMsgs.filter(m => m.role !== 'tool').map(m => ({
          role: m.role,
          content: m.content
        }))
      };

      if (this.ws && this.ws.readyState === WebSocket.OPEN) {
        this.ws.send(JSON.stringify(payload));
      } else {
        this.showToast('WebSocket is not connected.', 'error');
        this.isStreaming = false;
      }

      this.saveConversations();
      this.scrollToBottom(true);
    },

    // ─── TTS Voice Preview ─────────────────────────────────────────
    async previewVoice(voiceName) {
      // If already previewing this voice, stop
      if (this.previewingVoice === voiceName) {
        this.stopPreview();
        return;
      }
      // Stop any ongoing TTS message playback to prevent audio collision
      this.stopSpeaking();
      this.stopPreview();
      this.previewingVoice = voiceName;

      const voiceDisplayName = voiceName.split('_').slice(1).join(' ');
      const sampleText = `Hello! I'm ${voiceDisplayName}. This is a preview of my voice.`;

      // Increment generation so any in-flight async work for a previous
      // preview is silently discarded when it resumes.
      const gen = ++this._previewGeneration;

      try {
        if (!this._audioCtx) {
          this._audioCtx = new (window.AudioContext || window.webkitAudioContext)();
        }
        if (this._audioCtx.state === 'suspended') await this._audioCtx.resume();

        let audioBuf = this._voicePreviewCache[voiceName];
        if (!audioBuf) {
          const resp = await fetch('http://127.0.0.1:8787/tts', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ text: sampleText, voice: voiceName })
          });
          if (!resp.ok) throw new Error('TTS error');
          if (gen !== this._previewGeneration) return; // stale

          const arrayBuf = await resp.arrayBuffer();
          if (gen !== this._previewGeneration) return; // stale

          audioBuf = await this._audioCtx.decodeAudioData(arrayBuf);
          if (gen !== this._previewGeneration) return; // stale

          this._voicePreviewCache[voiceName] = audioBuf;
        }

        // Double-check we're still the active preview after all awaits
        if (gen !== this._previewGeneration) return;

        const source = this._audioCtx.createBufferSource();
        source.buffer = audioBuf;
        source.connect(this._audioCtx.destination);
        this._previewSource = source;
        source.onended = () => {
          // Only clear state if this source is still the active one
          if (gen !== this._previewGeneration) return;
          if (this._previewSource === source) {
            try { source.disconnect(); } catch (e) { }
            this._previewSource = null;
          }
          this.previewingVoice = null;
        };
        source.start(0);
      } catch (err) {
        if (gen !== this._previewGeneration) return; // stale — silently ignore
        console.error('Preview error:', err);
        this.previewingVoice = null;
        this.showToast('TTS server unavailable', 'error');
      }
    },

    stopPreview() {
      // Bump generation to invalidate any in-flight async fetches/decodes
      this._previewGeneration++;
      if (this._previewSource) {
        try { this._previewSource.stop(); } catch (e) { }
        try { this._previewSource.disconnect(); } catch (e) { }
        this._previewSource = null;
      }
      this.previewingVoice = null;
    },

    // ─── Text-to-Speech (TTS via Kokoro) ─────────────────────────
    speakMessage(idx) {
      const msg = this.messages[idx];
      if (!msg || !msg.content) return;

      // If this message is currently active (playing or paused), toggle
      if (this.ttsPlayingIdx === idx) {
        if (this.ttsPaused) {
          this._resumeTTS();
        } else {
          this._pauseTTS();
        }
        return;
      }

      // Stop any current speech
      this.stopSpeaking();

      // Strip markdown for cleaner speech
      const plainText = msg.content
        .replace(/```[\s\S]*?```/g, ' code block omitted ')
        .replace(/`([^`]+)`/g, '$1')
        .replace(/\*\*([^*]+)\*\*/g, '$1')
        .replace(/\*([^*]+)\*/g, '$1')
        .replace(/#{1,6}\s*/g, '')
        .replace(/\[([^\]]+)\]\([^)]+\)/g, '$1')
        .replace(/[\-\*]\s+/g, ', ')
        .replace(/\n{2,}/g, '. ')
        .replace(/\n/g, ' ')
        .trim();

      if (!plainText) return;

      // Split into chunks (~500 chars for faster first-chunk response)
      const MAX_CHUNK = 500;
      const chunks = [];
      let remaining = plainText;
      while (remaining.length > 0) {
        if (remaining.length <= MAX_CHUNK) {
          chunks.push(remaining);
          break;
        }
        let splitAt = remaining.lastIndexOf('. ', MAX_CHUNK);
        if (splitAt < MAX_CHUNK / 3) splitAt = remaining.lastIndexOf(', ', MAX_CHUNK);
        if (splitAt < MAX_CHUNK / 3) splitAt = remaining.lastIndexOf(' ', MAX_CHUNK);
        if (splitAt < 50) splitAt = MAX_CHUNK;
        chunks.push(remaining.slice(0, splitAt + 1));
        remaining = remaining.slice(splitAt + 1).trim();
      }

      this.ttsPlayingIdx = idx;
      this._ttsAborted = false;
      this.ttsPaused = false;
      this._ttsChunks = chunks;
      this._ttsChunkIndex = 0;
      this._ttsPauseOffset = 0;
      this._ttsCurrentBuffer = null;
      this._ttsVoice = this.settings.ttsVoice || 'af_sarah';

      this._playTTSChunk();
    },

    /* ── TTS pause / resume / helpers ───────────────────────────── */

    _pauseTTS() {
      this.ttsPaused = true;
      if (this._ttsSource) {
        // Use performance.now() for drift-free offset tracking
        const elapsed = (performance.now() - this._ttsWallStart) / 1000;
        this._ttsPauseOffset = elapsed;
        this._ttsPausingNow = true;
        try { this._ttsSource.stop(); } catch (e) { }
        this._ttsSource = null;
      }
    },

    _resumeTTS() {
      this.ttsPaused = false;
      if (this._ttsCurrentBuffer && !this._ttsSource) {
        this._playTTSBuffer(this._ttsCurrentBuffer, this._ttsPauseOffset);
      }
      // If buffer is null, fetch is in-flight — will auto-play on completion
    },

    async _playTTSChunk() {
      const i = this._ttsChunkIndex;
      if (i >= this._ttsChunks.length || this._ttsAborted) {
        this.ttsPlayingIdx = null;
        this.ttsPaused = false;
        return;
      }

      try {
        const resp = await fetch('http://127.0.0.1:8787/tts', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ text: this._ttsChunks[i], voice: this._ttsVoice })
        });
        if (!resp.ok) throw new Error('TTS server error ' + resp.status);
        if (this._ttsAborted) return;

        const arrayBuf = await resp.arrayBuffer();
        if (this._ttsAborted) return;

        if (!this._audioCtx) {
          this._audioCtx = new (window.AudioContext || window.webkitAudioContext)();
        }
        const audioBuf = await this._audioCtx.decodeAudioData(arrayBuf);
        if (this._ttsAborted) return;

        this._ttsCurrentBuffer = audioBuf;
        this._ttsPauseOffset = 0;
        if (this.ttsPaused) return; // paused while fetching — buffer saved for resume

        this._playTTSBuffer(audioBuf, 0);
      } catch (err) {
        console.error('TTS error:', err);
        this.ttsPlayingIdx = null;
        this.ttsPaused = false;
        if (err.message.includes('TTS server error') || err.message.includes('Failed to fetch')) {
          this.showToast('TTS server unavailable — is it running on port 8787?', 'error');
        }
      }
    },

    _playTTSBuffer(audioBuf, offset) {
      if (!this._audioCtx) {
        this._audioCtx = new (window.AudioContext || window.webkitAudioContext)();
      }
      const source = this._audioCtx.createBufferSource();
      source.buffer = audioBuf;
      source.connect(this._audioCtx.destination);
      this._ttsSource = source;
      this._ttsChunkStartTime = this._audioCtx.currentTime;
      this._ttsWallStart = performance.now() - (offset * 1000);
      this._ttsPausingNow = false;

      source.onended = () => {
        if (this._ttsPausingNow) { this._ttsPausingNow = false; return; }
        this._ttsSource = null;
        this._ttsCurrentBuffer = null;
        this._ttsChunkIndex++;
        this._ttsPauseOffset = 0;
        this._playTTSChunk();
      };
      source.start(0, offset);
    },

    stopSpeaking() {
      this._ttsAborted = true;
      this._ttsPausingNow = false;
      this.ttsPaused = false;
      if (this._ttsSource) {
        try { this._ttsSource.stop(); } catch (e) { }
        this._ttsSource = null;
      }
      this._ttsCurrentBuffer = null;
      this._ttsChunks = [];
      this._ttsChunkIndex = 0;
      this._ttsPauseOffset = 0;
      this.ttsPlayingIdx = null;
    },

    // ─── Settings Panel (#38) ────────────────────────────────────
    loadSettings() {
      try {
        const raw = diskStorage.getItem('localai_settings');
        if (raw) {
          const saved = JSON.parse(raw);
          this.settings = { ...this.settings, ...saved };
        }
        // Apply loaded settings
        const am = diskStorage.getItem('localai_auto_memory');
        if (am !== null) this.autoMemoryEnabled = am === 'true';
        const savedCtx = diskStorage.getItem('localai_maxContextTokens');
        if (savedCtx) this.maxContextTokens = parseInt(savedCtx, 10) || DEFAULT_CONTEXT_TOKENS;
        const savedSession = diskStorage.getItem('localai_session_stats');
        if (savedSession) {
          try { this.sessionStats = { ...this.sessionStats, ...JSON.parse(savedSession) }; } catch (e) { }
        }
        document.documentElement.style.setProperty('--chat-font-size', this.settings.fontSize + 'px');
      } catch (e) {
        console.warn('Failed to load settings:', e);
      }
    },

    saveSettings() {
      try {
        diskStorage.setItem('localai_settings', JSON.stringify(this.settings));
        diskStorage.setItem('localai_auto_memory', this.autoMemoryEnabled);
        diskStorage.setItem('localai_maxContextTokens', this.maxContextTokens);
        document.documentElement.style.setProperty('--chat-font-size', this.settings.fontSize + 'px');
        this.showToast('Settings saved');
      } catch (e) {
        console.warn('Failed to save settings:', e);
      }
    },

    openSettingsPanel() {
      this.showSettingsPanel = true;
    },

    closeSettingsPanel() {
      this.saveSettings();
      this.showSettingsPanel = false;
    },

    exportAllData() {
      const data = {
        conversations: this.conversations,
        memories: this.memories,
        settings: this.settings,
        customTemplates: this.customTemplates,
        autoMemoryEnabled: this.autoMemoryEnabled,
        temperature: this.temperature,
        maxContextTokens: this.maxContextTokens,
        sessionStats: this.sessionStats,
        exportedAt: new Date().toISOString()
      };
      const json = JSON.stringify(data, null, 2);
      const filename = 'local-ai-backup-' + new Date().toISOString().slice(0, 10) + '.json';
      if (typeof window.saveFile === 'function') {
        window.saveFile(filename, json).then(result => {
          try {
            const parsed = typeof result === 'string' ? JSON.parse(result) : result;
            if (parsed.success) this.showToast('Backup saved to Downloads');
            else this.showToast(parsed.error || 'Failed to save', 'error');
          } catch { this.showToast('Backup saved'); }
        }).catch(() => {
          this.showToast('Failed to save backup', 'error');
        });
      } else {
        const blob = new Blob([json], { type: 'application/json' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url; a.download = filename;
        document.body.appendChild(a); a.click(); a.remove();
        URL.revokeObjectURL(url);
        this.showToast('Backup exported');
      }
    },

    importAllData(event) {
      const file = event.target.files[0];
      if (!file) return;
      const reader = new FileReader();
      reader.onload = (e) => {
        try {
          const data = JSON.parse(e.target.result);
          if (data.conversations) {
            this.conversations = data.conversations;
            if (this.conversations.length > 0) {
              this.currentConversationId = this.conversations[0].id;
              this.messages = this.conversations[0].messages || [];
            }
            this.saveConversations();
          }
          if (data.memories) {
            this.memories = data.memories;
            diskStorage.setItem(MEMORY_KEY, JSON.stringify(this.memories));
          }
          if (data.settings) {
            this.settings = { ...this.settings, ...data.settings };
          }
          if (data.customTemplates && Array.isArray(data.customTemplates)) {
            this.customTemplates = data.customTemplates;
            this.saveCustomTemplates();
          }
          if (data.autoMemoryEnabled !== undefined) {
            this.autoMemoryEnabled = data.autoMemoryEnabled;
          }
          if (data.temperature !== undefined) {
            this.temperature = data.temperature;
          }
          if (data.maxContextTokens !== undefined) {
            this.maxContextTokens = data.maxContextTokens;
          }
          if (data.sessionStats !== undefined) {
            this.sessionStats = { ...this.sessionStats, ...data.sessionStats };
            diskStorage.setItem('localai_session_stats', JSON.stringify(this.sessionStats));
          }
          this.saveSettings();
          this.applyTheme();
          this.updateContextTokens();
          this.showToast('Backup imported successfully');
        } catch (err) {
          this.showToast('Invalid backup file', 'error');
        }
      };
      reader.readAsText(file);
      event.target.value = '';
    },

    async clearAllData() {
      if (!(await this.showConfirm('This will permanently delete ALL conversations, memories, and settings. Are you sure?'))) return;
      // Stop any active TTS or streaming before wiping data
      this.stopSpeaking();
      if (this.isStreaming) this.stopGeneration();
      diskStorage.removeItem(STORAGE_KEY);
      diskStorage.removeItem(MEMORY_KEY);
      diskStorage.removeItem('localai_settings');
      diskStorage.removeItem('localai_auto_memory');
      diskStorage.removeItem('localai_custom_templates');
      diskStorage.removeItem('localai_session_stats');
      diskStorage.removeItem('localai_maxContextTokens');
      diskStorage.removeItem('localai_sidebar_sections');
      diskStorage.removeItem('localai_tool_params');
      diskStorage.removeItem('sidebarCollapsed');
      this.conversations = [];
      this.memories = [];
      this.messages = [];
      this.customTemplates = [];
      this.currentConversationId = null;
      this.sessionStats = { totalGenerated: 0, totalPrompt: 0, totalDuration: 0, requestCount: 0 };
      this.lastStats = null;
      this.savedToolParams = {};
      this.sidebarCollapsed = false;
      this.maxContextTokens = DEFAULT_CONTEXT_TOKENS;
      this.createNewConversation();
      this.showToast('All data cleared');
    }
  };
}
