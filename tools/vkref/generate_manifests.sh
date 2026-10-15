#!/bin/bash
# generate_manifests.sh - Generate manifest.json files for all test vectors
# Usage: ./generate_manifests.sh <output_dir>

set -e

OUTPUT_DIR="${1:-./testdata/manifests}"
mkdir -p "$OUTPUT_DIR"

# Test vectors from VALIDATION.md §7
# Synthetic: grad, checker1/8/32, noise, flat0/flat255/flat128
# Sizes: 256x144, 16x16, 128x128, 1280x720, 1366x768, 1920x1080, 2560x1440

VECTORS=("grad" "checker1" "checker8" "checker32" "noise" "flat0" "flat255" "flat128")
SIZES=("256x144" "16x16" "128x128" "1280x720" "1366x768" "1920x1080" "2560x1440")

DEVICE="RTX 2060 SUPER/595.104.02"
REF="a0d2e71+vkref"

for vec in "${VECTORS[@]}"; do
    for size in "${SIZES[@]}"; do
        W="${size%x*}"
        H="${size#*x}"
        
        # Calculate default budget
        BUDGET=$(( (W * H * 3) / 16 ))
        if [ $BUDGET -lt 32768 ]; then
            BUDGET=32768
        fi
        
        MANIFEST="$OUTPUT_DIR/${vec}_${W}x${H}.json"
        
        cat > "$MANIFEST" << EOF
{
  "vector": "$vec",
  "W": $W,
  "H": $H,
  "budget": $BUDGET,
  "device": "$DEVICE",
  "ref": "$REF",
  "buffers": [
    {"name": "band_c0_l0_b0", "stage": 1, "dtype": "f32", "shape": [$(echo "scale=0; $W / 32" | bc),$(echo "scale=0; $H / 32" | bc)], "file": "band_c0_l0_b0.bin", "sha256": "placeholder"},
    {"name": "meta-buffer", "stage": 2, "dtype": "u32", "shape": [$(echo "scale=0; ($W * $H) / 1024" | bc)], "file": "meta-buffer.bin", "sha256": "placeholder"},
    {"name": "block-stat-buffer", "stage": 2, "dtype": "u32", "shape": [$(echo "scale=0; ($W * $H) / 1024 * 16" | bc)], "file": "block-stat-buffer.bin", "sha256": "placeholder"},
    {"name": "payload-data", "stage": 2, "dtype": "u8", "shape": [$(echo "scale=0; $W * $H * 2" | bc)], "file": "payload-data.bin", "sha256": "placeholder"},
    {"name": "bucket-buffer", "stage": 3, "dtype": "u32", "shape": [1024], "file": "bucket-buffer.bin", "sha256": "placeholder"},
    {"name": "quant-buffer", "stage": 4, "dtype": "u32", "shape": [$(echo "scale=0; ($W * $H) / 1024" | bc)], "file": "quant-buffer.bin", "sha256": "placeholder"},
    {"name": "bitstream", "stage": 5, "dtype": "u8", "shape": [$BUDGET], "file": "bitstream.bin", "sha256": "placeholder"}
  ]
}
EOF
    done
done

echo "Generated $(find "$OUTPUT_DIR" -name "*.json" | wc -l) manifests in $OUTPUT_DIR"
