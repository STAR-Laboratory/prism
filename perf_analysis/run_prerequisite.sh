#!/bin/bash
set -euo pipefail

# Resolve script directory so this works regardless of the caller's CWD
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

## 1. Install Python dependencies
echo "---------------------------"
echo ""
echo "#####################"
echo "[INFO] 1. Installing Python dependencies"
echo "#####################"
pip3 install -r python_dependencies.txt

## 2. Download the required traces
echo "---------------------------"
echo ""
echo "#####################"
echo "[INFO] 2. Downloading Required Traces"
echo "#####################"
mkdir -p cputraces
if [ -n "$(ls -A cputraces/ 2>/dev/null)" ]; then
  echo "cputraces directory already contains the traces. Skipping download."
else
  echo "cputraces directory is empty."
  echo "Downloading the required traces into the cputraces directory..."
  bash ./download_traces.sh
  echo "Decompressing the traces into the cputraces directory..."
  tar -xzf cputraces.tar.gz --no-same-owner -C cputraces/
  rm -f cputraces.tar.gz
fi

## 3. Build Ramulator2
echo "---------------------------"
echo ""
echo "#####################"
echo "[INFO] 3. Building Ramulator2"
echo "#####################"
rm -rf ./build/
bash ./build.sh