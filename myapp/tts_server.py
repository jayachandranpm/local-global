#!/usr/bin/env python3
"""
Lightweight TTS HTTP server using Kokoro TTS (kokoro-onnx).
Listens on port 8787 and returns WAV audio for text input.

Endpoints:
  POST /tts   { "text": "...", "voice": "af_sarah" }  →  audio/wav
  GET  /health                                         →  {"status":"ok"}
  GET  /voices                                         →  ["af_sarah",...]
"""

import io
import json
import os
import sys
import urllib.request
import shutil
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn

# ── Resolve user data directory ──────────────────────────────
# The app passes LOCAL_GLOBAL_DATA_DIR via environment; fall back to
# ~/Library/Application Support/Local Global for standalone use.
DATA_DIR = os.environ.get('LOCAL_GLOBAL_DATA_DIR', '')
if not DATA_DIR:
    DATA_DIR = os.path.join(os.path.expanduser('~'),
                            'Library', 'Application Support', 'Local Global')
os.makedirs(DATA_DIR, exist_ok=True)

# ── Resolve model path ───────────────────────────────────────
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

MODEL_FILE = 'kokoro-v1.0.int8.onnx'
VOICES_FILE = 'voices-v1.0.bin'

# Download URLs (GitHub Releases for kokoro-onnx)
KOKORO_RELEASE_BASE = 'https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0'
MODEL_URL = f'{KOKORO_RELEASE_BASE}/{MODEL_FILE}'
VOICES_URL = f'{KOKORO_RELEASE_BASE}/{VOICES_FILE}'

# Check common local paths for model files (user data dir first, then dev paths)
LOCAL_PATHS = [
    os.path.join(DATA_DIR, 'models', 'kokoro'),                            # user data dir (primary)
    os.path.join(SCRIPT_DIR, 'models', 'kokoro'),                          # dev: myapp/models/kokoro
    os.path.join(SCRIPT_DIR, '..', 'Resources', 'models', 'kokoro'),       # .app bundle
]

model_dir = None
for p in LOCAL_PATHS:
    if os.path.isfile(os.path.join(p, MODEL_FILE)):
        model_dir = os.path.abspath(p)
        break


def download_file(url, dest_path, label=''):
    """Download a file with progress reporting."""
    print(f"[TTS] Downloading {label or os.path.basename(dest_path)}...", flush=True)
    print(f"[TTS]   URL: {url}", flush=True)
    tmp_path = dest_path + '.tmp'
    try:
        req = urllib.request.Request(url, headers={'User-Agent': 'LocalGlobal/1.0'})
        with urllib.request.urlopen(req, timeout=300) as resp:
            total = int(resp.headers.get('Content-Length', 0))
            downloaded = 0
            with open(tmp_path, 'wb') as f:
                while True:
                    chunk = resp.read(1024 * 1024)  # 1 MB chunks
                    if not chunk:
                        break
                    f.write(chunk)
                    downloaded += len(chunk)
                    if total > 0:
                        pct = int(downloaded * 100 / total)
                        mb = downloaded / (1024 * 1024)
                        total_mb = total / (1024 * 1024)
                        print(f"[TTS]   {mb:.1f}/{total_mb:.1f} MB ({pct}%)", flush=True)
        shutil.move(tmp_path, dest_path)
        print(f"[TTS]   ✅ Downloaded to {dest_path}", flush=True)
        return True
    except Exception as e:
        print(f"[TTS]   ❌ Download failed: {e}", flush=True)
        if os.path.exists(tmp_path):
            os.remove(tmp_path)
        return False


# If model not found locally, download to user data directory
if not model_dir:
    print(f"[TTS] Kokoro model not found locally. Downloading to {DATA_DIR}/models/kokoro/...", flush=True)
    dl_dir = os.path.join(DATA_DIR, 'models', 'kokoro')
    os.makedirs(dl_dir, exist_ok=True)

    ok1 = download_file(MODEL_URL, os.path.join(dl_dir, MODEL_FILE), MODEL_FILE)
    ok2 = download_file(VOICES_URL, os.path.join(dl_dir, VOICES_FILE), VOICES_FILE)

    if ok1 and ok2:
        model_dir = dl_dir
        print("[TTS] ✅ Kokoro model downloaded successfully!", flush=True)
    else:
        print("[TTS] ❌ Failed to download Kokoro model. TTS will not be available.", flush=True)
        print("[TTS] You can manually download from:", flush=True)
        print(f"[TTS]   {MODEL_URL}", flush=True)
        print(f"[TTS]   {VOICES_URL}", flush=True)
        print(f"[TTS] Place them in: {dl_dir}", flush=True)
        sys.exit(1)

model_path = os.path.join(model_dir, MODEL_FILE)
voices_path = os.path.join(model_dir, VOICES_FILE)

# Now import and load (deferred so download happens first)
import soundfile as sf
from kokoro_onnx import Kokoro

print(f"[TTS] Loading Kokoro model from {model_dir}", flush=True)
kokoro = Kokoro(model_path, voices_path)
print("[TTS] Kokoro model loaded and ready!", flush=True)

# ── Voices & config ──────────────────────────────────────────
VOICES = kokoro.get_voices()
DEFAULT_VOICE = 'af_sarah' if 'af_sarah' in VOICES else VOICES[0]
SAMPLE_RATE = 24000
PORT = 8787
MAX_TEXT_LENGTH = 5000    # hard cap to prevent OOM from huge payloads

SUPPORTED_LANGS = {
    'en-us', 'en-gb', 'ja', 'ko', 'zh', 'fr', 'de', 'es', 'pt', 'it',
    'hi', 'ar', 'ru', 'tr', 'pl', 'nl', 'sv', 'da', 'fi', 'nb',
}

print(f"[TTS] Available voices ({len(VOICES)}): {', '.join(VOICES[:10])}{'...' if len(VOICES) > 10 else ''}", flush=True)
print(f"[TTS] Default voice: {DEFAULT_VOICE}", flush=True)


class TTSHandler(BaseHTTPRequestHandler):
    """Handle TTS HTTP requests."""

    def log_message(self, format, *args):
        """Quieter logging."""
        print(f"[TTS] {args[0]}", flush=True)

    def _send_json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_cors_headers(self):
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'POST, GET, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')

    def do_OPTIONS(self):
        self.send_response(204)
        self._send_cors_headers()
        self.end_headers()

    def do_GET(self):
        if self.path == '/health':
            self._send_json(200, {'status': 'ok', 'model': 'kokoro-v1.0-int8'})
        elif self.path == '/voices':
            self._send_json(200, {'voices': VOICES})
        else:
            self._send_json(404, {'error': 'not found'})

    def do_POST(self):
        if self.path != '/tts':
            self._send_json(404, {'error': 'not found'})
            return

        try:
            length = int(self.headers.get('Content-Length', 0))
            if length < 0 or length > 1_000_000:  # 1 MB JSON body cap
                self._send_json(400, {'error': 'Content-Length out of range'})
                return
            body = json.loads(self.rfile.read(length)) if length else {}
        except Exception:
            self._send_json(400, {'error': 'invalid JSON'})
            return

        text = body.get('text', '').strip()
        if not text:
            self._send_json(400, {'error': 'text is required'})
            return

        if len(text) > MAX_TEXT_LENGTH:
            self._send_json(400, {'error': f'text too long ({len(text)} chars, max {MAX_TEXT_LENGTH})'})
            return

        voice = body.get('voice', DEFAULT_VOICE)
        if voice not in VOICES:
            voice = DEFAULT_VOICE

        # Validate speed parameter
        speed = body.get('speed', 1.0)
        try:
            speed = float(speed)
        except (TypeError, ValueError):
            speed = 1.0
        speed = max(0.1, min(5.0, speed))  # clamp to safe range

        # Validate lang parameter
        lang = body.get('lang', 'en-us')
        if not isinstance(lang, str) or lang not in SUPPORTED_LANGS:
            lang = 'en-us'

        try:
            # Generate audio with Kokoro
            audio, sr = kokoro.create(text, voice=voice, speed=speed, lang=lang)

            # Encode as WAV in memory
            buf = io.BytesIO()
            sf.write(buf, audio, sr, format='WAV', subtype='PCM_16')
            del audio  # free numpy array before copying buffer
            wav_bytes = buf.getvalue()
            buf.close()  # release BytesIO buffer immediately

            self.send_response(200)
            self.send_header('Content-Type', 'audio/wav')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.send_header('Content-Length', str(len(wav_bytes)))
            self.end_headers()
            self.wfile.write(wav_bytes)

        except Exception as e:
            print(f"[TTS] Error: {e}", flush=True)
            self._send_json(500, {'error': 'TTS generation failed'})


class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    """Handle each request in a separate thread so /health isn't blocked."""
    daemon_threads = True


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else PORT
    server = ThreadedHTTPServer(('127.0.0.1', port), TTSHandler)
    print(f"[TTS] Server listening on http://127.0.0.1:{port}", flush=True)

    # A native macOS Quit event can terminate the webview process before its
    # C++ cleanup path runs. Stop this child when it is re-parented so it does
    # not remain orphaned and keep port 8787 occupied.
    try:
        parent_pid = int(os.environ.get('LOCAL_GLOBAL_PARENT_PID', '0'))
    except ValueError:
        parent_pid = 0

    if parent_pid > 1:
        def _watch_parent():
            while os.getppid() == parent_pid:
                threading.Event().wait(0.5)
            print("[TTS] Parent process exited, shutting down...", flush=True)
            server.shutdown()
        threading.Thread(target=_watch_parent, daemon=True).start()

    import signal
    def _shutdown(signum, frame):
        print("[TTS] Received signal, shutting down...", flush=True)
        # shutdown() waits for serve_forever(), so it must not run in the
        # main thread where Python invokes signal handlers.
        threading.Thread(target=server.shutdown, daemon=True).start()
    signal.signal(signal.SIGTERM, _shutdown)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("[TTS] Shutting down...", flush=True)
        server.shutdown()


if __name__ == '__main__':
    main()
