#!/usr/bin/env python3
"""generate_manifests.py - Generate manifest.json files for all test vectors (T6a).

Usage: python generate_manifests.py <output_dir>

Creates manifests for all synthetic test vectors at all sizes defined in VALIDATION.md §7.
"""

import json
import os
import sys

def main():
    if len(sys.argv) < 2:
        print("Usage: generate_manifests.py <output_dir>")
        sys.exit(1)
    
    output_dir = sys.argv[1]
    os.makedirs(output_dir, exist_ok=True)
    
    vectors = ["grad", "checker1", "checker8", "checker32", "noise", "flat0", "flat255", "flat128"]
    sizes = [(256, 144), (16, 16), (128, 128), (1280, 720), (1366, 768), (1920, 1080), (2560, 1440)]
    
    device = "RTX 2060 SUPER/595.104.02"
    ref = "a0d2e71+vkref"
    
    count = 0
    for vec in vectors:
        for w, h in sizes:
            budget = max((w * h * 3) // 16, 32768)
            block_count = (w * h) // 1024
            
            manifest = {
                "vector": vec,
                "W": w,
                "H": h,
                "budget": budget,
                "device": device,
                "ref": ref,
                "buffers": [
                    {"name": "band_c0_l0_b0", "stage": 1, "dtype": "f32", "shape": [w // 32, h // 32], "file": "band_c0_l0_b0.bin", "sha256": "placeholder"},
                    {"name": "meta-buffer", "stage": 2, "dtype": "u32", "shape": [block_count], "file": "meta-buffer.bin", "sha256": "placeholder"},
                    {"name": "block-stat-buffer", "stage": 2, "dtype": "u32", "shape": [block_count * 16], "file": "block-stat-buffer.bin", "sha256": "placeholder"},
                    {"name": "payload-data", "stage": 2, "dtype": "u8", "shape": [w * h * 2], "file": "payload-data.bin", "sha256": "placeholder"},
                    {"name": "bucket-buffer", "stage": 3, "dtype": "u32", "shape": [1024], "file": "bucket-buffer.bin", "sha256": "placeholder"},
                    {"name": "quant-buffer", "stage": 4, "dtype": "u32", "shape": [block_count], "file": "quant-buffer.bin", "sha256": "placeholder"},
                    {"name": "bitstream", "stage": 5, "dtype": "u8", "shape": [budget], "file": "bitstream.bin", "sha256": "placeholder"}
                ]
            }
            
            filename = f"{vec}_{w}x{h}.json"
            path = os.path.join(output_dir, filename)
            with open(path, "w") as f:
                json.dump(manifest, f, indent=2)
            count += 1
    
    print(f"Generated {count} manifests in {output_dir}")

if __name__ == "__main__":
    main()
