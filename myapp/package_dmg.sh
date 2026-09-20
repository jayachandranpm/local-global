#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# package_dmg.sh — Build "Local Global" and package as a distributable .dmg
#
# Usage:
#   ./package_dmg.sh              Build + package DMG
#   ./package_dmg.sh --skip-build Only package (use existing build)
# ──────────────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
APP_NAME="Local Global"
APP_BUNDLE="${BUILD_DIR}/${APP_NAME}.app"
DMG_DIR="${BUILD_DIR}/dmg"
DMG_OUTPUT="${BUILD_DIR}/${APP_NAME}.dmg"
VERSION="1.0.0"

SKIP_BUILD=false
for arg in "$@"; do
    case "$arg" in
        --skip-build) SKIP_BUILD=true ;;
        --help|-h)
            echo "Usage: $0 [--skip-build]"
            echo "  --skip-build   Skip building, use existing .app bundle"
            exit 0
            ;;
    esac
done

echo "╔══════════════════════════════════════════════════╗"
echo "║   Packaging Local Global as DMG                 ║"
echo "╚══════════════════════════════════════════════════╝"
echo ""

# ── Step 1: Build the app ────────────────────────────────────────────────────
if ! $SKIP_BUILD; then
    echo "🔨 Step 1/4: Building app in Release mode..."

    # Generate icon if needed
    if [ ! -f "${SCRIPT_DIR}/AppIcon.icns" ]; then
        echo "  🎨 Generating app icon..."
        python3 "${SCRIPT_DIR}/generate_icon.py" 2>/dev/null || true
    fi

    mkdir -p "${BUILD_DIR}"
    cd "${BUILD_DIR}"
    cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -3
    cmake --build . --config Release -j "$(sysctl -n hw.ncpu)" 2>&1 | tail -5
    cd "${SCRIPT_DIR}"
    echo "  ✅ Build complete"
else
    echo "⏭️  Step 1/4: Skipping build (using existing .app)"
fi

if [ ! -d "${APP_BUNDLE}" ]; then
    echo "❌ App bundle not found at: ${APP_BUNDLE}"
    exit 1
fi

echo ""

# ── Step 2: Clean mutable data from app bundle ───────────────────────────────
echo "🧹 Step 2/5: Removing mutable data from app bundle..."

# Remove any user data that should NOT be distributed inside the .app
# These are created at runtime in ~/Library/Application Support/Local Global/
RESOURCES="${APP_BUNDLE}/Contents/Resources"
rm -rf "${RESOURCES}/app_storage"    2>/dev/null || true
rm -rf "${RESOURCES}/rag_data"       2>/dev/null || true
rm -rf "${RESOURCES}/uploads"        2>/dev/null || true
rm -rf "${RESOURCES}/models"         2>/dev/null || true
rm -f  "${RESOURCES}/tts_server.log" 2>/dev/null || true
echo "  ✅ Mutable data removed from bundle"

echo ""

# ── Step 3: Bundle dynamic libraries ─────────────────────────────────────────
echo "📚 Step 3/5: Bundling dynamic libraries..."

FRAMEWORKS_DIR="${APP_BUNDLE}/Contents/Frameworks"
mkdir -p "${FRAMEWORKS_DIR}"
EXECUTABLE="${APP_BUNDLE}/Contents/MacOS/${APP_NAME}"

# Use dylibbundler to copy and fix all non-system dylibs
dylibbundler \
    -od \
    -b \
    -x "${EXECUTABLE}" \
    -d "${FRAMEWORKS_DIR}" \
    -p @executable_path/../Frameworks/ \
    -s /opt/homebrew/lib \
    -s /opt/homebrew/opt/drogon/lib \
    -s /opt/homebrew/opt/jsoncpp/lib \
    -s /opt/homebrew/opt/openssl@3/lib \
    -s /opt/homebrew/opt/brotli/lib \
    -s /opt/homebrew/opt/c-ares/lib \
    2>&1 | tail -10

echo "  ✅ Libraries bundled"

# Verify no more Homebrew references
echo "  🔍 Verifying library paths..."
REMAINING=$(otool -L "${EXECUTABLE}" 2>/dev/null | grep "/opt/homebrew" || true)
if [ -n "${REMAINING}" ]; then
    echo "  ⚠️  Warning: Some Homebrew references remain:"
    echo "${REMAINING}"
    echo "  Attempting manual fix..."

    # Manual fallback for any remaining libs
    while IFS= read -r line; do
        LIB_PATH=$(echo "$line" | awk '{print $1}')
        LIB_NAME=$(basename "$LIB_PATH")
        if [ -f "$LIB_PATH" ] && [ ! -f "${FRAMEWORKS_DIR}/${LIB_NAME}" ]; then
            cp "$LIB_PATH" "${FRAMEWORKS_DIR}/"
            chmod 644 "${FRAMEWORKS_DIR}/${LIB_NAME}"
        fi
        install_name_tool -change "$LIB_PATH" "@executable_path/../Frameworks/${LIB_NAME}" "${EXECUTABLE}" 2>/dev/null || true
    done <<< "${REMAINING}"
    echo "  ✅ Manual fix applied"
else
    echo "  ✅ All library paths are relative — ready for distribution"
fi

# Also verify bundled libs don't reference Homebrew
echo "  🔍 Verifying bundled libraries..."
for dylib in "${FRAMEWORKS_DIR}"/*.dylib; do
    [ -f "$dylib" ] || continue
    BREW_REFS=$(otool -L "$dylib" 2>/dev/null | grep "/opt/homebrew" | awk '{print $1}' || true)
    if [ -n "$BREW_REFS" ]; then
        while IFS= read -r ref; do
            REF_NAME=$(basename "$ref")
            if [ ! -f "${FRAMEWORKS_DIR}/${REF_NAME}" ] && [ -f "$ref" ]; then
                cp "$ref" "${FRAMEWORKS_DIR}/"
                chmod 644 "${FRAMEWORKS_DIR}/${REF_NAME}"
            fi
            install_name_tool -change "$ref" "@executable_path/../Frameworks/${REF_NAME}" "$dylib" 2>/dev/null || true
        done <<< "$BREW_REFS"
    fi
done
echo "  ✅ All bundled libraries verified"

echo ""

# ── Step 3: Ad-hoc code sign ─────────────────────────────────────────────────
echo "🔏 Step 4/5: Code signing (ad-hoc)..."

# Sign all frameworks first
for dylib in "${FRAMEWORKS_DIR}"/*.dylib; do
    [ -f "$dylib" ] || continue
    codesign --force --sign - "$dylib" 2>/dev/null || true
done

# Sign the main app bundle
codesign --force --deep --sign - "${APP_BUNDLE}" 2>/dev/null || true
echo "  ✅ Ad-hoc signed"

echo ""

# ── Step 4: Create DMG ───────────────────────────────────────────────────────
echo "💿 Step 5/5: Creating DMG..."

# Clean up any previous DMG workspace
rm -rf "${DMG_DIR}"
rm -f "${DMG_OUTPUT}"

mkdir -p "${DMG_DIR}"

# Copy the app into the DMG staging area
cp -R "${APP_BUNDLE}" "${DMG_DIR}/"

# Create a symbolic link to /Applications for drag-and-drop install
ln -s /Applications "${DMG_DIR}/Applications"

# Create DMG using create-dmg with a nice layout
create-dmg \
    --volname "${APP_NAME}" \
    --volicon "${SCRIPT_DIR}/AppIcon.icns" \
    --window-pos 200 120 \
    --window-size 600 400 \
    --icon-size 100 \
    --icon "${APP_NAME}.app" 150 185 \
    --icon "Applications" 450 185 \
    --hide-extension "${APP_NAME}.app" \
    --app-drop-link 450 185 \
    --no-internet-enable \
    "${DMG_OUTPUT}" \
    "${DMG_DIR}" \
    2>&1 || {
        # Fallback: use hdiutil directly if create-dmg fails
        echo "  ⚠️  create-dmg failed, using hdiutil fallback..."
        hdiutil create \
            -volname "${APP_NAME}" \
            -srcfolder "${DMG_DIR}" \
            -ov \
            -format UDZO \
            "${DMG_OUTPUT}"
    }

# Clean up staging directory
rm -rf "${DMG_DIR}"

echo ""

# ── Done ──────────────────────────────────────────────────────────────────────
if [ -f "${DMG_OUTPUT}" ]; then
    DMG_SIZE=$(du -sh "${DMG_OUTPUT}" | cut -f1)
    echo "╔══════════════════════════════════════════════════╗"
    echo "║   ✅ DMG created successfully!                  ║"
    echo "╚══════════════════════════════════════════════════╝"
    echo ""
    echo "   📦 File: ${DMG_OUTPUT}"
    echo "   📏 Size: ${DMG_SIZE}"
    echo ""
    echo "   Share this .dmg file with others."
    echo "   They just need to:"
    echo "     1. Double-click the .dmg to mount it"
    echo "     2. Drag '${APP_NAME}' to the Applications folder"
    echo "     3. Launch from Applications or Spotlight"
    echo ""
    echo "   ⚠️  Note: Recipients may need to right-click → Open"
    echo "   the first time (macOS Gatekeeper for unsigned apps)."
else
    echo "❌ DMG creation failed!"
    exit 1
fi
