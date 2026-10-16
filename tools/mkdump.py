#!/usr/bin/env python3
"""mkdump.py - Create minimal dump directories for pwcompare testing.

Usage:
    mkdump.py <dir> [n_blocks] [budget]

Creates a dump directory with manifest.json and .bin files.
"""

import sys
import os
import struct
import json


def write_bin(path, dtype, data):
    """Write binary data to file."""
    with open(path, 'wb') as f:
        if dtype == 'f32':
            f.write(struct.pack('<' + 'f' * len(data), *data))
        elif dtype == 'u32':
            f.write(struct.pack('<' + 'I' * len(data), *data))
        elif dtype == 'u8':
            f.write(bytes(data))


def make_dump(dirpath, n_blocks=9, budget=32768):
    """Create a minimal dump directory."""
    os.makedirs(dirpath, exist_ok=True)
    
    # band_c0_l0_b0: stage 1, f32, shape [8, 4] = 32 floats
    write_bin(os.path.join(dirpath, 'band_c0_l0_b0.bin'), 'f32', list(range(32)))
    
    # meta-buffer: stage 2, u32, shape [n_blocks * 2]
    meta_data = []
    for i in range(n_blocks):
        meta_data.extend([i * 100, i * 50])
    write_bin(os.path.join(dirpath, 'meta-buffer.bin'), 'u32', meta_data)
    
    # block-stat-buffer: stage 2, u32, shape [n_blocks * 16] (64 bytes per block)
    write_bin(os.path.join(dirpath, 'block-stat-buffer.bin'), 'u32', [i % 256 for _ in range(n_blocks * 16)])
    
    # payload-data: stage 2, u8, shape [73728]
    write_bin(os.path.join(dirpath, 'payload-data.bin'), 'u8', [i % 256 for i in range(73728)])
    
    # bucket-buffer: stage 3, u32, shape [1024]
    write_bin(os.path.join(dirpath, 'bucket-buffer.bin'), 'u32', [i % 1024 for i in range(1024)])
    
    # quant-buffer: stage 4, u32, shape [n_blocks]
    write_bin(os.path.join(dirpath, 'quant-buffer.bin'), 'u32', [i % 8 for i in range(n_blocks)])
    
    # bitstream: stage 5, u8, shape [budget]
    write_bin(os.path.join(dirpath, 'bitstream.bin'), 'u8', [i % 256 for i in range(budget)])
    
    manifest = {
        'vector': 'test',
        'W': 16,
        'H': 16,
        'budget': budget,
        'device': 'test',
        'ref': 'test',
        'buffers': [
            {'name': 'band_c0_l0_b0', 'stage': 1, 'dtype': 'f32', 'shape': [8, 4], 'file': 'band_c0_l0_b0.bin', 'sha256': 'placeholder'},
            {'name': 'meta-buffer', 'stage': 2, 'dtype': 'u32', 'shape': [n_blocks * 2], 'file': 'meta-buffer.bin', 'sha256': 'placeholder'},
            {'name': 'block-stat-buffer', 'stage': 2, 'dtype': 'u32', 'shape': [n_blocks * 16], 'file': 'block-stat-buffer.bin', 'sha256': 'placeholder'},
            {'name': 'payload-data', 'stage': 2, 'dtype': 'u8', 'shape': [73728], 'file': 'payload-data.bin', 'sha256': 'placeholder'},
            {'name': 'bucket-buffer', 'stage': 3, 'dtype': 'u32', 'shape': [1024], 'file': 'bucket-buffer.bin', 'sha256': 'placeholder'},
            {'name': 'quant-buffer', 'stage': 4, 'dtype': 'u32', 'shape': [n_blocks], 'file': 'quant-buffer.bin', 'sha256': 'placeholder'},
            {'name': 'bitstream', 'stage': 5, 'dtype': 'u8', 'shape': [budget], 'file': 'bitstream.bin', 'sha256': 'placeholder'},
        ]
    }
    with open(os.path.join(dirpath, 'manifest.json'), 'w') as f:
        json.dump(manifest, f, indent=2)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <dir> [n_blocks] [budget]", file=sys.stderr)
        sys.exit(1)
    
    dirpath = sys.argv[1]
    n_blocks = int(sys.argv[2]) if len(sys.argv) > 2 else 9
    budget = int(sys.argv[3]) if len(sys.argv) > 3 else 32768
    
    make_dump(dirpath, n_blocks, budget)
    print(f"Created dump directory: {dirpath}")
