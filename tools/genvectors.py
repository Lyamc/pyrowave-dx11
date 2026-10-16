#!/usr/bin/env python3
"""genvectors.py - Generate deterministic synthetic PyroWave test vectors.

Creates YUV420P .yuv frames and .json headers for validation testing.

Vectors:
    grad       - horizontal/vertical ramps (low bands, DC shift)
    checker1   - 1-pixel checkerboard (high bands, block edges)
    checker8   - 8-pixel checkerboard
    checker32  - 32-pixel checkerboard (mirror border)
    noise      - uniform noise (worst-case packing, many planes)
    flat0      - all-zero frame (empty packets)
    flat255    - all-255 frame (empty packets)
    flat128    - mid-gray frame (empty packets)
    text       - rendered UI text (realistic desktop content)
    edge       - one-pixel lines at x=W-1, y=H-1 (padding, AW/AH)

Sizes: 16x16, 128x128, 256x144, 1280x720, 1366x768, 1920x1080, 2560x1440
Budgets: default (max(W*H*3/16, 32768)), tight (16384), generous (W*H)
Sequences: 10 frames (sequence counter wraps at 7)
"""

import sys
import os
import json
import argparse
import numpy as np


# ---------------------------------------------------------------------------
# Budget computation (from etherquic: pyrowave.rs:586-589)
# ---------------------------------------------------------------------------

def default_frame_bytes(W, H):
    """default_frame_bytes = max(W*H*3/16, 32 KiB)."""
    return max(W * H * 3 // 16, 32768)


# ---------------------------------------------------------------------------
# YUV420P frame generation
# ---------------------------------------------------------------------------

def make_yuv420p_frame(W, H, gen_plane):
    """Generate a YUV420P frame. gen_plane(w, h) -> numpy array (uint8)."""
    y = gen_plane(W, H)
    # Chroma: average 2x2 from luma (H/2 x W/2)
    cw = W // 2
    ch = H // 2
    y_even = y[0:2*ch, 0:2*cw]
    c = (y_even[0::2, 0::2].astype(np.int16) + y_even[1::2, 0::2].astype(np.int16) +
         y_even[0::2, 1::2].astype(np.int16) + y_even[1::2, 1::2].astype(np.int16)) // 4
    c = np.clip(c, 0, 255).astype(np.uint8)
    return y, c, c


def gen_grad(W, H):
    """Horizontal and vertical ramps."""
    y = np.zeros((H, W), dtype=np.uint8)
    for j in range(H):
        for i in range(W):
            y[j, i] = ((i * 128 // W) + (j * 128 // H)) & 0xff
    return y


def gen_checker(W, H, cell_size):
    """Checkerboard with given cell size."""
    y = np.zeros((H, W), dtype=np.uint8)
    for j in range(H):
        for i in range(W):
            if ((i // cell_size) + (j // cell_size)) % 2 == 0:
                y[j, i] = 0
            else:
                y[j, i] = 255
    return y


def gen_noise(W, H, seed=42):
    """Uniform noise."""
    rng = np.random.RandomState(seed)
    return rng.randint(0, 256, (H, W), dtype=np.uint8)


def gen_flat(W, H, value):
    """Constant frame."""
    return np.full((H, W), value, dtype=np.uint8)


def gen_text(W, H):
    """Simple rendered text pattern (ASCII art-like)."""
    y = np.zeros((H, W), dtype=np.uint8)
    # Draw some horizontal lines
    for j in range(0, H, 8):
        y[j, :] = 200
    # Draw vertical lines
    for i in range(0, W, 16):
        y[:, i] = 150
    # Draw a rectangle
    rect_y0, rect_y1 = H // 4, 3 * H // 4
    rect_x0, rect_x1 = W // 4, 3 * W // 4
    y[rect_y0, rect_x0:rect_x1] = 100
    y[rect_y1, rect_x0:rect_x1] = 100
    y[rect_y0:rect_y1, rect_x0] = 100
    y[rect_y0:rect_y1, rect_x1 - 1] = 100
    # Fill interior with pattern
    for j in range(rect_y0 + 1, rect_y1 - 1):
        for i in range(rect_x0 + 1, rect_x1 - 1):
            y[j, i] = ((i + j) * 7) & 0xff
    return y


def gen_edge(W, H):
    """One-pixel lines at x=W-1 and y=H-1."""
    y = np.zeros((H, W), dtype=np.uint8)
    y[:, W - 1] = 200
    y[H - 1, :] = 200
    return y


GENERATORS = {
    'grad': gen_grad,
    'checker1': lambda W, H: gen_checker(W, H, 1),
    'checker8': lambda W, H: gen_checker(W, H, 8),
    'checker32': lambda W, H: gen_checker(W, H, 32),
    'noise': gen_noise,
    'flat0': lambda W, H: gen_flat(W, H, 0),
    'flat255': lambda W, H: gen_flat(W, H, 255),
    'flat128': lambda W, H: gen_flat(W, H, 128),
    'text': gen_text,
    'edge': gen_edge,
}


# ---------------------------------------------------------------------------
# Sizes and budgets
# ---------------------------------------------------------------------------

SIZES = [
    (256, 144),
    (16, 16),
    (128, 128),
    (1280, 720),
    (1366, 768),
    (1920, 1080),
    (2560, 1440),
]


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

def write_yuv(path, y, c):
    """Write a YUV420P file."""
    with open(path, 'wb') as f:
        f.write(y.tobytes())
        f.write(c.tobytes())
        f.write(c.tobytes())


def write_manifest(path, vector, W, H, budget, seq_count=10):
    """Write a .json manifest for a test vector."""
    manifest = {
        'vector': vector,
        'W': W,
        'H': H,
        'budget': budget,
        'device': 'RTX 2060 SUPER/595.104.02',
        'ref': 'a0d2e71+vkref',
        'buffers': [
            {
                'name': 'band_c0_l0_b0',
                'stage': 1,
                'dtype': 'f32',
                'shape': [8, 4],
                'file': 'band_c0_l0_b0.bin',
                'sha256': 'placeholder',
            },
            {
                'name': 'meta-buffer',
                'stage': 2,
                'dtype': 'u32',
                'shape': [36],
                'file': 'meta-buffer.bin',
                'sha256': 'placeholder',
            },
            {
                'name': 'block-stat-buffer',
                'stage': 2,
                'dtype': 'u32',
                'shape': [576],
                'file': 'block-stat-buffer.bin',
                'sha256': 'placeholder',
            },
            {
                'name': 'payload-data',
                'stage': 2,
                'dtype': 'u8',
                'shape': [73728],
                'file': 'payload-data.bin',
                'sha256': 'placeholder',
            },
            {
                'name': 'bucket-buffer',
                'stage': 3,
                'dtype': 'u32',
                'shape': [1024],
                'file': 'bucket-buffer.bin',
                'sha256': 'placeholder',
            },
            {
                'name': 'quant-buffer',
                'stage': 4,
                'dtype': 'u32',
                'shape': [36],
                'file': 'quant-buffer.bin',
                'sha256': 'placeholder',
            },
            {
                'name': 'bitstream',
                'stage': 5,
                'dtype': 'u8',
                'shape': [budget],
                'file': 'bitstream.bin',
                'sha256': 'placeholder',
            },
        ],
        'sequence_count': seq_count,
    }
    with open(path, 'w') as f:
        json.dump(manifest, f, indent=2)
    return manifest


def generate_vector(vector_name, W, H, budget, out_dir, seq_count=10):
    """Generate a complete test vector."""
    gen = GENERATORS.get(vector_name)
    if gen is None:
        print(f"ERROR: unknown vector '{vector_name}'", file=sys.stderr)
        return False

    vec_dir = os.path.join(out_dir, f"{vector_name}_{W}x{H}")
    os.makedirs(vec_dir, exist_ok=True)

    # Generate frames
    for seq in range(seq_count):
        frame_dir = os.path.join(vec_dir, f"frame_{seq:04d}")
        os.makedirs(frame_dir, exist_ok=True)

        y, c, c2 = make_yuv420p_frame(W, H, gen)
        yuv_path = os.path.join(frame_dir, f"frame_{seq:04d}.yuv")
        write_yuv(yuv_path, y, c)

        # Write frame header
        frame_header = {
            'sequence': seq % 8,
            'width': W,
            'height': H,
        }
        with open(os.path.join(frame_dir, 'frame_header.json'), 'w') as f:
            json.dump(frame_header, f, indent=2)

    # Write vector manifest
    manifest_path = os.path.join(vec_dir, 'manifest.json')
    write_manifest(manifest_path, vector_name, W, H, budget, seq_count)

    # Write YUV source file (frame 0)
    y, c, c2 = make_yuv420p_frame(W, H, gen)
    yuv_path = os.path.join(vec_dir, f"{vector_name}_{W}x{H}.yuv")
    write_yuv(yuv_path, y, c)

    print(f"  Generated {vector_name} {W}x{H} (budget={budget}, {seq_count} frames)")
    return True


def main():
    parser = argparse.ArgumentParser(description='Generate PyroWave test vectors')
    parser.add_argument('out_dir', help='Output directory')
    parser.add_argument('--vector', '-v', nargs='+',
                        choices=list(GENERATORS.keys()),
                        help='Vectors to generate (default: all)')
    parser.add_argument('--size', '-s', nargs='+',
                        type=lambda x: tuple(map(int, x.split('x'))),
                        help='Sizes to generate (default: all)')
    parser.add_argument('--budget', '-b', nargs='+',
                        type=int,
                        help='Budgets to generate (default: default only)')
    parser.add_argument('--seq', type=int, default=10,
                        help='Number of sequences per vector (default: 10)')
    args = parser.parse_args()

    out_dir = args.out_dir
    os.makedirs(out_dir, exist_ok=True)

    vectors = args.vector if args.vector else list(GENERATORS.keys())
    sizes = args.size if args.size else SIZES

    all_ok = True
    count = 0

    for vec in vectors:
        for W, H in sizes:
            if args.budget:
                budgets = args.budget
            else:
                budgets = [default_frame_bytes(W, H)]

            for budget in budgets:
                ok = generate_vector(vec, W, H, budget, out_dir, args.seq)
                if ok:
                    count += 1
                else:
                    all_ok = False

    print(f"\nGenerated {count} test vectors in {out_dir}")
    return 0 if all_ok else 1


if __name__ == '__main__':
    sys.exit(main())
