#!/bin/bash
# =============================================================================
# Aeon Browser — Cloud Build Startup Script
# Runs on GCP n2-highcpu-80 VM (Ubuntu 22.04)
# Compiles Chromium with custom Aeon API patches then uploads to GCS
# =============================================================================
set -e
BUCKET="gs://aeon-chromium-artifacts"
BUILD_LOG="/tmp/aeon_build.log"
exec > >(tee -a $BUILD_LOG) 2>&1
echo "======================================================"
echo " AEON CLOUD BUILD — $(date)"
echo " Host: $(hostname) | CPUs: $(nproc) | RAM: $(free -h | awk '/Mem:/{print $2}')"
echo "======================================================"

# ── 1. System dependencies ────────────────────────────────────────────────────
echo "[1/7] Installing build dependencies..."
apt-get update -qq
apt-get install -y -qq \
  git curl python3 python3-pip lsb-release \
  build-essential pkg-config libglib2.0-dev \
  xvfb libx11-dev gperf bison flex ninja-build \
  libnspr4-dev libnss3-dev libdbus-1-dev \
  libxcb-composite0-dev libatk-bridge2.0-dev \
  libcups2-dev libdrm-dev libxkbcommon-dev

# ── 2. depot_tools ────────────────────────────────────────────────────────────
echo "[2/7] Setting up depot_tools..."
cd /build
if [ ! -d depot_tools ]; then
  git clone --depth=1 https://chromium.googlesource.com/chromium/tools/depot_tools.git
fi
export PATH="/build/depot_tools:$PATH"
export DEPOT_TOOLS_UPDATE=0

# ── 3. Fetch Chromium source ──────────────────────────────────────────────────
echo "[3/7] Fetching Chromium source (this takes 20-30 min)..."
mkdir -p /build/chromium
cd /build/chromium
if [ ! -d src ]; then
  fetch --nohooks chromium
  cd src
  gclient runhooks
else
  cd src
  git pull
  gclient sync --with_branch_heads --with_tags -j$(nproc)
fi

# ── 4. Apply Aeon patches ─────────────────────────────────────────────────────
echo "[4/7] Applying Aeon custom patches..."
cd /build/chromium/src
gsutil cp gs://aeon-chromium-artifacts/patches/aeon_patches.tar.gz /tmp/ 2>/dev/null \
  && tar -xzf /tmp/aeon_patches.tar.gz -C . \
  || echo "  No patches found in GCS, building vanilla ungoogled chromium"

# Apply ungoogled-chromium patches if available
gsutil cp gs://aeon-chromium-artifacts/patches/ungoogled.tar.gz /tmp/ 2>/dev/null \
  && tar -xzf /tmp/ungoogled.tar.gz -C . || true

# ── 5. Configure build ────────────────────────────────────────────────────────
echo "[5/7] Configuring build (gn gen)..."
mkdir -p out/AeonRelease
cat > out/AeonRelease/args.gn << 'ARGSEOF'
# Aeon Browser Build Config — Cloud Edition
is_official_build = true
is_debug = false
symbol_level = 0
enable_nacl = false
blink_symbol_level = 0
v8_symbol_level = 0

# Remove ALL Google services
google_api_key = ""
google_default_client_id = ""
google_default_client_secret = ""
safe_browsing_mode = 0
enable_reporting = false
enable_background_mode = false

# Privacy hardening
build_with_chromium_features = false
enable_mdns = false
enable_service_discovery = false
proprietary_codecs = true
ffmpeg_branding = "Chrome"

# Performance
use_thin_lto = true
use_lto = true
clang_use_chrome_plugins = false
ARGSEOF

gn gen out/AeonRelease --args="$(cat out/AeonRelease/args.gn)"

# ── 6. Build ──────────────────────────────────────────────────────────────────
echo "[6/7] Building with $(nproc) cores — estimated 25-40 min..."
START_TIME=$(date +%s)
autoninja -C out/AeonRelease chrome -j$(nproc)
END_TIME=$(date +%s)
BUILD_MINS=$(( (END_TIME - START_TIME) / 60 ))
echo "  Build complete in ${BUILD_MINS} minutes!"

# ── 7. Package and upload artifacts ──────────────────────────────────────────
echo "[7/7] Uploading artifacts to GCS..."
BUILD_DATE=$(date +%Y%m%d_%H%M%S)
ARTIFACT_DIR="gs://aeon-chromium-artifacts/builds/${BUILD_DATE}"

# Core binaries
gsutil -m cp \
  out/AeonRelease/chrome \
  out/AeonRelease/chrome_sandbox \
  out/AeonRelease/chrome.pak \
  out/AeonRelease/resources.pak \
  out/AeonRelease/icudtl.dat \
  out/AeonRelease/snapshot_blob.bin \
  "${ARTIFACT_DIR}/"

# All required shared libs and resources
gsutil -m cp -r out/AeonRelease/locales "${ARTIFACT_DIR}/locales/"
gsutil -m cp out/AeonRelease/*.so* "${ARTIFACT_DIR}/" 2>/dev/null || true

# Build metadata
echo "{\"build_date\":\"${BUILD_DATE}\",\"build_mins\":${BUILD_MINS},\"commit\":\"$(git rev-parse HEAD)\",\"cores\":$(nproc)}" \
  > /tmp/build_meta.json
gsutil cp /tmp/build_meta.json "${ARTIFACT_DIR}/meta.json"
gsutil cp /tmp/build_meta.json "gs://aeon-chromium-artifacts/latest_build.json"

# Update "latest" symlink pointer
echo "${BUILD_DATE}" | gsutil cp - gs://aeon-chromium-artifacts/latest_build_id.txt

echo "======================================================"
echo " BUILD COMPLETE!"
echo " Artifacts: ${ARTIFACT_DIR}"
echo " Build time: ${BUILD_MINS} minutes"
echo " Shutting down VM in 60 seconds..."
echo "======================================================"

# Upload full log
gsutil cp $BUILD_LOG "${ARTIFACT_DIR}/build.log"

# Auto-shutdown to stop billing
sleep 60
shutdown -h now
