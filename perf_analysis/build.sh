#!/bin/bash
set -euo pipefail

# Default to Release build if no argument is provided
BUILD_TYPE="${1:-Release}"

# Resolve script location so we build relative to the project root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Create and enter the build directory
mkdir -p build
cd build

# Configure the project
echo "[INFO] Configuring CMake (BUILD_TYPE=$BUILD_TYPE)..."
cmake -DCMAKE_BUILD_TYPE="$BUILD_TYPE" ..

# Build using all available cores
echo "[INFO] Building Ramulator2..."
make -j"$(nproc)"

# Copy the resulting binary to the project root
if [ -f "./ramulator2" ]; then
  cp ./ramulator2 ../ramulator2
  echo "[INFO] Build complete. Binary at: $SCRIPT_DIR/ramulator2"
else
  echo "[ERROR] ramulator2 binary not found after build." >&2
  exit 1
fi