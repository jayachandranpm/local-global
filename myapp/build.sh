#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# build.sh — Build "Local Global" macOS .app bundle
#
# Usage:
#   ./build.sh              Build the .app bundle (Release mode)
#   ./build.sh --install    Build and copy to /Applications
#   ./build.sh --clean      Clean build directory first
#   ./build.sh --run        Build and launch the app
# ──────────────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
APP_NAME="Local Global"

# Parse flags
DO_INSTALL=false
DO_CLEAN=false
DO_RUN=false
for arg in "$@"; do
    case "$arg" in
        --install) DO_INSTALL=true ;;
        --clean)   DO_CLEAN=true ;;
        --run)     DO_RUN=true ;;
        --help|-h)
            echo "Usage: $0 [--clean] [--install] [--run]"
            echo "  --clean    Remove build/ and rebuild from scratch"
            echo "  --install  Copy the .app to /Applications"
            echo "  --run      Launch the app after building"
            exit 0
            ;;
    esac
done

echo "╔══════════════════════════════════════════╗"
echo "║   Building Local Global .app             ║"
echo "╚══════════════════════════════════════════╝"

# ── Clean ────────────────────────────────────────────────────────────────────
if $DO_CLEAN; then
    echo "🧹 Cleaning build directory..."
    rm -rf "${BUILD_DIR}"
fi

# ── Generate icon if needed ──────────────────────────────────────────────────
if [ ! -f "${SCRIPT_DIR}/AppIcon.icns" ]; then
    echo "🎨 Generating app icon..."
    python3 "${SCRIPT_DIR}/generate_icon.py"
fi

# ── Configure ────────────────────────────────────────────────────────────────
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

echo "⚙️  Configuring CMake (Release)..."
cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -5

# ── Build ────────────────────────────────────────────────────────────────────
echo "🔨 Building..."
cmake --build . --config Release 2>&1 | tail -10

# ── Verify ───────────────────────────────────────────────────────────────────
APP_BUNDLE="${BUILD_DIR}/${APP_NAME}.app"
if [ -d "${APP_BUNDLE}" ]; then
    echo ""
    echo "✅ App bundle created:"
    echo "   ${APP_BUNDLE}"
    echo ""
    echo "   Contents:"
    ls -la "${APP_BUNDLE}/Contents/MacOS/"
    echo "   Resources:"
    ls "${APP_BUNDLE}/Contents/Resources/" | head -10
    echo ""

    # Size
    APP_SIZE=$(du -sh "${APP_BUNDLE}" | cut -f1)
    echo "   Size: ${APP_SIZE}"
else
    echo "❌ Build failed — .app bundle not found"
    exit 1
fi

# ── Install to /Applications ────────────────────────────────────────────────
if $DO_INSTALL; then
    echo ""
    echo "📦 Installing to /Applications..."
    rm -rf "/Applications/${APP_NAME}.app"
    cp -R "${APP_BUNDLE}" "/Applications/"
    echo "✅ Installed to /Applications/${APP_NAME}.app"
    echo "   You can now find it in Launchpad or Spotlight!"
fi

# ── Run ──────────────────────────────────────────────────────────────────────
if $DO_RUN; then
    echo ""
    echo "🚀 Launching ${APP_NAME}..."
    open "${APP_BUNDLE}"
fi

echo ""
echo "Done! To launch manually:"
echo "  open \"${APP_BUNDLE}\""
echo ""
echo "To install to Applications:"
echo "  ./build.sh --install"
