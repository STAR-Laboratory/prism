#!/bin/bash
set -euo pipefail

# Zenodo URL for PrISM CPU traces
# NOTE: eusing the QPRAC (HPCA'25) trace bundle.
URL="https://zenodo.org/record/14607144/files/QPRAC_HPCA25_ramulator2_traces.tar.gz?download=1"
OUTPUT_FILE="cputraces.tar.gz"

if [ -f "$OUTPUT_FILE" ]; then
  echo "[INFO] $OUTPUT_FILE already exists; skipping download."
  exit 0
fi

echo "[INFO] Downloading traces from Zenodo..."
wget -O "$OUTPUT_FILE" "$URL"

echo "[INFO] Download complete: $OUTPUT_FILE"