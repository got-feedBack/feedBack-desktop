#!/bin/bash
# Docker-based Linux build wrapper
# Runs build-linux-ubuntu.sh inside a reproducible container
#
# --platform linux/amd64 is forced on both build and run: the Linux target is
# x86_64-only end to end (bundle-python.sh's python-build-standalone pin,
# vgmstream, onnxruntime, etc. have no arm64 Linux build, and Steam Deck
# itself is x86_64). On an Apple Silicon host, omitting --platform makes
# `docker build` produce a native arm64 image, so bundle-python.sh's hardcoded
# x86_64 download becomes a foreign-arch binary inside an otherwise-native
# container — Rosetta chokes trying to exec it directly instead of via full
# amd64 emulation. Forcing amd64 for the whole container sidesteps that.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
DEVCONTAINER_DIR="$PROJECT_DIR/.devcontainer"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo "=== Slopsmith Desktop Docker Build ==="
echo ""
echo "This script provides reproducible Linux builds by running"
echo "build-linux-ubuntu.sh inside a Docker container."
echo ""

# Check prerequisites
echo -e "${BLUE}Checking prerequisites...${NC}"

if ! command -v docker &>/dev/null; then
    echo -e "${RED}Error: Docker is not installed${NC}" >&2
    echo "Install: https://docs.docker.com/get-docker/" >&2
    exit 1
fi

if ! docker info &>/dev/null; then
    echo -e "${RED}Error: Docker daemon is not running${NC}" >&2
    exit 1
fi

echo -e "${GREEN}✓${NC} Docker available"
echo ""

# Build container image
echo -e "${BLUE}Building container image...${NC}"
echo " (This will take a few minutes on first run)"
echo ""

docker build \
    --platform linux/amd64 \
    -f "$DEVCONTAINER_DIR/Dockerfile" \
    -t slopsmith-ubuntu-builder \
    "$PROJECT_DIR"
# `set -e` at the top of this script already aborts on a failed
# `docker build` — no manual `$?` check needed (and the check that
# was here would in practice be unreachable).

echo -e "${GREEN}✓${NC} Container image built"
echo ""

# CMakeCache.txt bakes in the configure-time build path, so a cache from a
# DIFFERENT mount path makes cmake abort. This wrapper always mounts the repo
# at the stable /workspace, so that mismatch can't happen across repeated local
# runs — keeping build/ instead gives incremental C++ (the JUCE/NAM compile is
# the biggest cost under amd64 emulation). Wipe only on request; CLEAN_BUILD=1
# forces the old clean-slate behavior. (build-audio.sh still drops build/ on a
# compiler-version mismatch, so a stale cache can't compile with the wrong g++.)
if [[ "${CLEAN_BUILD:-0}" == "1" && -d "$PROJECT_DIR/build" ]]; then
    echo -e "${BLUE}CLEAN_BUILD=1: clearing CMake build dir...${NC}"
    rm -rf "$PROJECT_DIR/build"
fi

# Generate unique container name
CONTAINER_NAME="slopsmith-build-$(date +%s)-$$-$RANDOM"

echo -e "${BLUE}Running build in container...${NC}"
echo -e "${BLUE}Container name:${NC} $CONTAINER_NAME"
echo ""
echo "The container will be preserved after the build to allow debugging."
echo "Clean up when done:"
echo "  docker stop $CONTAINER_NAME && docker rm $CONTAINER_NAME"
echo ""

# Persist caches across runs in named Docker volumes. Each `docker run` starts a
# fresh container, so without these the container-local caches (compiler cache,
# downloaded Electron + electron-builder deps, cmake-js headers) are rebuilt from
# scratch every time. The repo itself is bind-mounted at /workspace, so build/,
# resources/ and node_modules already persist on the host — these volumes cover
# what lives outside the workspace. CCACHE_DIR + the CMakeLists ccache launcher
# make the C++ compile a cache hit when sources are unchanged.
set +e
docker run \
    --platform linux/amd64 \
    --name "$CONTAINER_NAME" \
    -v "$PROJECT_DIR:/workspace" \
    -v slopsmith-ccache:/home/vscode/.ccache \
    -v slopsmith-cache:/home/vscode/.cache \
    -v slopsmith-cmake-js:/home/vscode/.cmake-js \
    -v slopsmith-src:/home/vscode/.slopsmith-src \
    -w /workspace \
    -e ELECTRON_CACHE=/home/vscode/.cache/electron \
    -e ELECTRON_BUILDER_CACHE=/home/vscode/.cache/electron-builder \
    -e CCACHE_DIR=/home/vscode/.ccache \
    -e SLOPSMITH_CLONE_DIR=/home/vscode/.slopsmith-src/core \
    -e GIT_TERMINAL_PROMPT=0 \
    -e "GH_CLONE_TOKEN=${GH_CLONE_TOKEN:-}" \
    -e "SLOPSMITH_REF=${SLOPSMITH_REF:-main}" \
    -e "SLOPSMITH_REPO=${SLOPSMITH_REPO:-got-feedback/feedback}" \
    -e "FAST_BUILD=${FAST_BUILD:-0}" \
    -t \
    slopsmith-ubuntu-builder \
    bash -c './scripts/build-linux-ubuntu.sh'
BUILD_EXIT_CODE=$?
set -e

echo ""
if [[ $BUILD_EXIT_CODE -eq 0 ]]; then
    echo -e "${GREEN}✓${NC} Build completed successfully!"
else
    echo -e "${RED}✗${NC} Build failed (exit code: $BUILD_EXIT_CODE)"
    echo ""
    echo "To debug:"
    echo "  docker exec -it $CONTAINER_NAME /bin/bash"
    echo "  docker logs $CONTAINER_NAME"
    echo ""
fi

exit $BUILD_EXIT_CODE
