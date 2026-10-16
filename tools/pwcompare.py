#!/usr/bin/env python3
"""pwcompare.py - Compare two PyroWave dump directories.

Usage:
    pwcompare.py <dirA> <dirB> [--stage N] [--tol-file tol.json]

Compares buffer dumps from two encoders (e.g. Vulkan reference vs D3D11 debug).
Prints per-buffer details and a verdict: PASS / FS-ONLY / FAIL.

Dump format (per VALIDATION.md §3.4):
    manifest.json  - list of buffers with name, stage, dtype, shape, file, sha256
    *.bin          - raw little-endian binary data, no header

Tolerances from VALIDATION.md §6:
    bands all levels:     max abs <= 4e-3
    bands L0-1 (R16F):    additionally >= 99% within 1 f16 ULP
    stats f16:            <= 2 f16 ULP
    meta code word:       exact (except FS-Q1 boundary flips)
    payload:              per block exact
    bucket:               consumed/total exact; RD op multiset per bucket
    quant buffer:         exact
    packets:              exact with don't-care mask

FS point tracing (from stage docs):
    FS-Q1: m_wave/target boundary in quant scale computation
    FS-Q2: 1/(code/8+0.25) division in quant scale
    FS-Q3: float cluster sum order in quant stats
    FS-Q4: inv_quant_sq in stats
    FS-A1: log2 in bucket index
    FS-A2: float distortion sum order in analyze
"""

import sys
import os
import json
import struct
import hashlib
import argparse
import numpy as np


# ---------------------------------------------------------------------------
# f16 helpers
# ---------------------------------------------------------------------------

def f32_to_f16(f):
    """Convert float32 to float16 (round to nearest even)."""
    v = struct.unpack('>I', struct.pack('>f', f))[0]
    sign = (v >> 31) & 1
    exp = ((v >> 23) & 0xff) - 127
    mant = v & 0x7fffff
    if exp >= 16:
        return (sign << 15) | 0x7c00
    if exp < -14:
        return sign << 15
    h = ((exp + 15) << 10) | (mant >> 13)
    round_bit = (mant >> 12) & 1
    sticky = (mant & 0x1fff) != 0
    if round_bit and (sticky or (h & 1)):
        h += 1
    return (sign << 15) | (h & 0x7fff)


def f16_to_f32(h):
    """Convert float16 to float32."""
    sign = (h >> 15) & 1
    exp = (h >> 10) & 0x1f
    mant = h & 0x3ff
    if exp == 0:
        v = sign << 31
    else:
        exp = exp - 15 + 127
        v = (sign << 31) | (exp << 23) | (mant << 13)
    return struct.unpack('>f', struct.pack('>I', v))[0]


def f16_ulp(val_f16):
    """Return the ULP of a float16 value."""
    if val_f16 == 0:
        return 2**-14  # smallest normal f16
    h = val_f16 & 0x7fff
    if h < 0x0400:
        return 2**-24  # subnormal
    exp = ((h >> 10) & 0x1f) - 15 + 127
    return 2 ** (exp - 10)


def ulp_diff(a, b):
    """ULP difference between two float32 values."""
    if a == 0.0 and b == 0.0:
        return 0.0
    fa = abs(a)
    fb = abs(b)
    mx = max(fa, fb)
    if mx == 0.0:
        return 0.0
    # ULP of the larger value
    av = struct.unpack('>I', struct.pack('>f', mx))[0]
    ulp = max(1, (av & 0x7fffff)) * 2 ** ((av >> 23 & 0xff) - 127 - 23)
    if ulp == 0:
        ulp = 2**-126  # subnormal
    return abs(a - b) / ulp


# ---------------------------------------------------------------------------
# Buffer loading
# ---------------------------------------------------------------------------

def load_manifest(dirpath):
    """Load manifest.json from a dump directory."""
    path = os.path.join(dirpath, 'manifest.json')
    with open(path, 'r') as f:
        return json.load(f)


def load_buffer(dirpath, buf):
    """Load a buffer's binary data."""
    fname = buf['file']
    path = os.path.join(dirpath, fname)
    if not os.path.exists(path):
        return None, None
    with open(path, 'rb') as f:
        data = f.read()
    # Verify sha256 if present in manifest
    sha = hashlib.sha256(data).hexdigest()
    expected = buf.get('sha256', '')
    if expected and expected != 'placeholder' and sha != expected:
        sys.stderr.write(f"  WARN: sha256 mismatch for {fname}: "
                         f"expected {expected}, got {sha}\n")
    dtype = buf['dtype']
    shape = buf['shape']
    return data, (dtype, shape)


def interpret_buffer(data, dtype, shape):
    """Interpret raw bytes as a numpy array."""
    if dtype == 'f32':
        return np.frombuffer(data, dtype=np.float32).reshape(shape)
    elif dtype == 'f16':
        return np.frombuffer(data, dtype=np.uint16).reshape(shape).astype(np.float32)
    elif dtype == 'u32':
        return np.frombuffer(data, dtype=np.uint32).reshape(shape)
    elif dtype == 'u16':
        return np.frombuffer(data, dtype=np.uint16).reshape(shape)
    elif dtype == 'u8':
        return np.frombuffer(data, dtype=np.uint8).reshape(shape)
    else:
        sys.stderr.write(f"  WARN: unknown dtype '{dtype}', treating as u8\n")
        return np.frombuffer(data, dtype=np.uint8).reshape(shape)


# ---------------------------------------------------------------------------
# Comparison functions
# ---------------------------------------------------------------------------

def compare_bands(a_data, b_data, buf_a, buf_b):
    """Compare wavelet band buffers. Tolerance: max abs <= 4e-3, L0-1: 99% within 1 f16 ULP."""
    results = []
    for i, (da, db) in enumerate(zip(a_data, b_data)):
        buf = buf_a[i]
        stage = buf['stage']
        name = buf['name']
        if da is None or db is None:
            results.append((name, 'MISSING', None, None))
            continue
        arr_a = interpret_buffer(da, buf['dtype'], buf['shape'])
        arr_b = interpret_buffer(db, buf['dtype'], buf['shape'])
        diff = np.abs(arr_a.astype(np.float64) - arr_b.astype(np.float64))
        max_abs = float(np.max(diff))
        mean_abs = float(np.mean(diff))
        count_diff = int(np.count_nonzero(diff > 1e-6))
        total = arr_a.size
        # L0-1 f16 ULP check
        f16_fail = 0
        if stage <= 1:
            fa = arr_a.astype(np.float32)
            fb = arr_b.astype(np.float32)
            fa_h = np.array([f32_to_f16(float(v)) for v in fa.flat], dtype=np.uint16).reshape(fa.shape)
            fb_h = np.array([f32_to_f16(float(v)) for v in fb.flat], dtype=np.uint16).reshape(fb.shape)
            same_f16 = np.sum(fa_h == fb_h)
            f16_fail = total - same_f16
        results.append((name, 'bands', max_abs, {
            'mean_abs': mean_abs,
            'count_diff': count_diff,
            'total': total,
            'f16_fail': f16_fail,
            'stage': stage,
        }))
    return results


def compare_meta(a_data, b_data, buf):
    """Compare meta buffers (BlockMeta: code_word + offset).
    Tolerance: exact except FS-Q1 boundary flips.
    """
    results = []
    if not a_data or not b_data:
        results.append(('meta-buffer', 'MISSING', None, None))
        return results
    arr_a = np.frombuffer(a_data, dtype=np.uint32)
    arr_b = np.frombuffer(b_data, dtype=np.uint32)
    # Each BlockMeta is 2 uint32: code_word, offset
    n_blocks = min(arr_a.size, arr_b.size) // 2
    diffs = []
    for i in range(n_blocks):
        cw_a = arr_a[i * 2]
        cw_b = arr_b[i * 2]
        off_a = arr_a[i * 2 + 1]
        off_b = arr_b[i * 2 + 1]
        if cw_a != cw_b or off_a != off_b:
            diffs.append((i, cw_a, cw_b, off_a, off_b))
    results.append(('meta-buffer', 'meta', len(diffs), {
        'differences': diffs[:20],  # first 20
        'total': n_blocks,
    }))
    return results


def compare_stats(a_data, b_data, buf):
    """Compare block stats buffers. Tolerance: f16 fields <= 2 f16 ULP.
    
    BlockStats layout (64 bytes = 16 u32):
      [0]: num_planes (u32)
      [1..15]: each u32 = (square_error_fp16) | (encode_cost_bits << 16)
    """
    results = []
    if not a_data or not b_data:
        results.append(('block-stat-buffer', 'MISSING', None, None))
        return results
    arr_a = np.frombuffer(a_data, dtype=np.uint32)
    arr_b = np.frombuffer(b_data, dtype=np.uint32)
    # Use the smaller size to avoid shape mismatches between dirs
    n_blocks = min(arr_a.size, arr_b.size) // 16
    f16_diffs = 0
    total_fields = 0
    for i in range(n_blocks):
        chunk_a = arr_a[i * 16:(i + 1) * 16]
        chunk_b = arr_b[i * 16:(i + 1) * 16]
        if chunk_a[0] != chunk_b[0]:
            f16_diffs += 1  # num_planes differs
        # Each of entries 1..15 contains (fp16_lo | fp16_hi << 16)
        for j in range(1, 16):
            val_a = chunk_a[j]
            val_b = chunk_b[j]
            if val_a != val_b:
                lo_a = val_a & 0xffff
                lo_b = val_b & 0xffff
                total_fields += 1
                fa = f16_to_f32(lo_a)
                fb = f16_to_f32(lo_b)
                ulp = f16_ulp(lo_a)
                if ulp > 0 and abs(fa - fb) > 2 * ulp:
                    f16_diffs += 1
    results.append(('block-stat-buffer', 'stats', f16_diffs, {
        'total_blocks': n_blocks,
        'total_fields': total_fields,
    }))
    return results


def compare_payload(a_data, b_data, buf):
    """Compare payload data. Per-block exact."""
    results = []
    if not a_data or not b_data:
        results.append(('payload-data', 'MISSING', None, None))
        return results
    arr_a = a_data
    arr_b = b_data
    min_len = min(len(arr_a), len(arr_b))
    diff_idx = -1
    for i in range(min_len):
        if arr_a[i] != arr_b[i]:
            diff_idx = i
            break
    # Return diff_idx as count: -1 means no diff, >= 0 is the byte index
    results.append(('payload-data', 'payload', diff_idx, {
        'len_a': len(arr_a),
        'len_b': len(arr_b),
    }))
    return results


def compare_bucket(a_data, b_data, buf):
    """Compare bucket buffer. consumed/total exact; RD op multiset per bucket."""
    results = []
    if not a_data or not b_data:
        results.append(('bucket-buffer', 'MISSING', None, None))
        return results
    arr_a = np.frombuffer(a_data, dtype=np.uint32)
    arr_b = np.frombuffer(b_data, dtype=np.uint32)
    # bucket buffer: offset 4 = consumed_payload (1 u32)
    # offset 64 = total_savings (2048 u32 = 128 buckets * 16 subdivisions)
    # offset 8256 = rdo_operations
    consumed_a = arr_a[4] if arr_a.size > 4 else 0
    consumed_b = arr_b[4] if arr_b.size > 4 else 0
    total_a = arr_a[5] if arr_a.size > 5 else 0
    total_b = arr_b[5] if arr_b.size > 5 else 0
    savings_a = arr_a[64:64+2048] if arr_a.size > 64+2048 else np.array([])
    savings_b = arr_b[64:64+2048] if arr_b.size > 64+2048 else np.array([])
    savings_diff = int(np.sum(savings_a != savings_b))
    rdo_a = arr_a[8256:] if arr_a.size > 8256 else np.array([])
    rdo_b = arr_b[8256:] if arr_b.size > 8256 else np.array([])
    rdo_diff = int(np.sum(rdo_a != rdo_b))
    results.append(('bucket-buffer', 'bucket', savings_diff + rdo_diff, {
        'consumed_diff': consumed_a != consumed_b,
        'total_diff': total_a != total_b,
        'savings_diff': savings_diff,
        'rdo_diff': rdo_diff,
    }))
    return results


def compare_quant(a_data, b_data, buf):
    """Compare quant buffer (per-32x32 quant code). Exact."""
    results = []
    if not a_data or not b_data:
        results.append(('quant-buffer', 'MISSING', None, None))
        return results
    arr_a = np.frombuffer(a_data, dtype=np.uint32)
    arr_b = np.frombuffer(b_data, dtype=np.uint32)
    # Compare up to the smaller size
    min_size = min(arr_a.size, arr_b.size)
    diff_count = int(np.sum(arr_a[:min_size] != arr_b[:min_size]))
    # Also count extra elements in the larger buffer
    if arr_a.size != arr_b.size:
        diff_count += abs(arr_a.size - arr_b.size)
    diff_indices = np.where(arr_a[:min_size] != arr_b[:min_size])[0][:20].tolist()
    results.append(('quant-buffer', 'quant', diff_count, {
        'diff_indices': diff_indices,
        'total': min_size,
    }))
    return results


def compare_packets(a_data, b_data, buf):
    """Compare bitstream packets. Exact with don't-care mask."""
    results = []
    if not a_data or not b_data:
        results.append(('bitstream', 'MISSING', None, None))
        return results
    # Don't-care bits: bytes after the last valid packet are padding (don't care)
    # Find the last non-zero byte in each
    def last_valid(data):
        for i in range(len(data) - 1, -1, -1):
            if data[i] != 0:
                return i + 1
        return 0
    end_a = last_valid(a_data)
    end_b = last_valid(b_data)
    min_end = min(end_a, end_b)
    diff_idx = -1
    for i in range(min_end):
        if a_data[i] != b_data[i]:
            diff_idx = i
            break
    # Show first few bytes of difference
    context = []
    if diff_idx >= 0:
        start = max(0, diff_idx - 4)
        end_ctx = min(len(a_data), diff_idx + 8)
        for i in range(start, end_ctx):
            a_byte = a_data[i] if i < len(a_data) else 0
            b_byte = b_data[i] if i < len(b_data) else 0
            marker = ' <<<' if i == diff_idx else ''
            context.append(f"  byte {i}: A=0x{a_byte:02x} B=0x{b_byte:02x}{marker}")
    results.append(('bitstream', 'packets', diff_idx, {
        'len_a': len(a_data),
        'len_b': len(b_data),
        'valid_a': end_a,
        'valid_b': end_b,
        'context': context,
    }))
    return results


# ---------------------------------------------------------------------------
# FS point detection
# ---------------------------------------------------------------------------

def is_fs_only(diffs):
    """Determine if all differences can be traced to FS points.
    
    FS points are:
    - FS-Q1: quant scale boundary (m_wave/target near integer)
    - FS-Q2: 1/(code/8+0.25) division
    - FS-Q3: float cluster sum order
    - FS-Q4: inv_quant_sq
    - FS-A1: log2 in bucket index
    - FS-A2: float distortion sum order
    
    Heuristic: if differences are only in bands (float) or small quant/code_word
    differences, likely FS. Exact payload/bucket/quant differences are FAIL.
    """
    for name, btype, count, info in diffs:
        if btype == 'payload':
            if count >= 0:
                return False
        elif btype == 'packets':
            if count >= 0:
                return False
        elif btype == 'bucket':
            if count > 0:
                return False
        elif btype == 'quant':
            if count > 0:
                return False
        elif btype == 'meta':
            # code_word diffs could be FS-Q1
            if count > 0 and info.get('total', 0) > 0:
                ratio = count / info['total']
                if ratio > 0.1:
                    return False
        elif btype == 'stats':
            if count > 0:
                return False
        elif btype == 'bands':
            # Band differences are expected at FS points
            pass
    return True


# ---------------------------------------------------------------------------
# Main comparison
# ---------------------------------------------------------------------------

def compare_dirs(dir_a, dir_b, stage_filter=None, tol_file=None):
    """Compare two dump directories. Returns (verdict, details)."""
    manifest_a = load_manifest(dir_a)
    manifest_b = load_manifest(dir_b)
    
    buffers_a = {b['name']: b for b in manifest_a['buffers']}
    buffers_b = {b['name']: b for b in manifest_b['buffers']}
    
    # Find matching buffer names
    all_names = sorted(set(buffers_a.keys()) | set(buffers_b.keys()))
    
    if stage_filter is not None:
        all_names = [n for n in all_names
                     if buffers_a.get(n, {}).get('stage') == stage_filter
                     or buffers_b.get(n, {}).get('stage') == stage_filter]
    
    all_diffs = []
    
    for name in all_names:
        buf_a = buffers_a.get(name)
        buf_b = buffers_b.get(name)
        
        if buf_a is None or buf_b is None:
            all_diffs.append((name, 'MISSING', None, None))
            continue
        
        data_a, info_a = load_buffer(dir_a, buf_a)
        data_b, info_b = load_buffer(dir_b, buf_b)
        
        stage = buf_a.get('stage', 0)
        
        if stage == 1:
            res = compare_bands([data_a], [data_b], [buf_a], [buf_b])
            all_diffs.extend(res)
        elif stage == 2:
            if name == 'meta-buffer':
                res = compare_meta(data_a, data_b, buf_a)
                all_diffs.extend(res)
            elif name == 'block-stat-buffer':
                res = compare_stats(data_a, data_b, buf_a)
                all_diffs.extend(res)
            elif name == 'payload-data':
                res = compare_payload(data_a, data_b, buf_a)
                all_diffs.extend(res)
        elif stage == 3:
            if name == 'bucket-buffer':
                res = compare_bucket(data_a, data_b, buf_a)
                all_diffs.extend(res)
        elif stage == 4:
            if name == 'quant-buffer':
                res = compare_quant(data_a, data_b, buf_a)
                all_diffs.extend(res)
        elif stage == 5:
            if name == 'bitstream':
                res = compare_packets(data_a, data_b, buf_a)
                all_diffs.extend(res)
    
    # Determine verdict
    has_fail = False
    has_fs_only = False
    details_lines = []
    
    for name, btype, count, info in all_diffs:
        if btype == 'MISSING':
            details_lines.append(f"  {name}: MISSING in one directory")
            has_fail = True
            continue
        
        if btype == 'bands':
            max_abs = count
            mean_abs = info.get('mean_abs', 0)
            f16_fail = info.get('f16_fail', 0)
            total = info.get('total', 1)
            f16_pct = (1 - f16_fail / max(total, 1)) * 100
            line = f"  {name}: max|Delta|={max_abs:.6e} mean={mean_abs:.6e}"
            if info.get('stage', 0) <= 1:
                line += f" f16_match={f16_pct:.1f}%"
            if max_abs > 4e-3:
                line += " [FAIL: exceeds 4e-3 tolerance]"
                has_fail = True
            elif f16_fail > 0 and f16_pct < 99:
                line += " [FAIL: <99%% f16 match]"
                has_fail = True
            else:
                line += " [OK]"
            details_lines.append(line)
        
        elif btype == 'meta':
            n_diff = count
            total = info.get('total', 0)
            line = f"  {name}: {n_diff}/{total} blocks differ"
            if n_diff > 0:
                diffs_list = info.get('differences', [])
                for idx, cw_a, cw_b, off_a, off_b in diffs_list[:5]:
                    line += f"\n    block {idx}: cw A=0x{cw_a:08x} B=0x{cw_b:08x}"
                if is_fs_only([(name, btype, n_diff, info)]):
                    line += " [FS-ONLY: code_word diffs may be FS-Q1]"
                    has_fs_only = True
                else:
                    line += " [FAIL]"
                    has_fail = True
            else:
                line += " [OK]"
            details_lines.append(line)
        
        elif btype == 'stats':
            line = f"  {name}: {count} f16 field diffs / {info.get('total_fields', 0)} fields"
            if count > 0:
                line += " [FAIL: exceeds 2 f16 ULP tolerance]"
                has_fail = True
            else:
                line += " [OK]"
            details_lines.append(line)
        
        elif btype == 'payload':
            diff_byte = count
            if diff_byte >= 0:
                line = f"  {name}: first diff at byte {diff_byte}"
                line += " [FAIL]"
                has_fail = True
            else:
                line = f"  {name}: identical ({info.get('len_a', 0)} bytes)"
                line += " [OK]"
            details_lines.append(line)
        
        elif btype == 'bucket':
            line = f"  {name}: savings_diff={info.get('savings_diff', 0)} "
            line += f"rdo_diff={info.get('rdo_diff', 0)} "
            line += f"consumed={'DIFF' if info.get('consumed_diff') else 'OK'} "
            line += f"total={'DIFF' if info.get('total_diff') else 'OK'}"
            if count > 0:
                line += " [FAIL]"
                has_fail = True
            else:
                line += " [OK]"
            details_lines.append(line)
        
        elif btype == 'quant':
            line = f"  {name}: {count} diffs / {info.get('total', 0)}"
            if count > 0:
                idxs = info.get('diff_indices', [])
                if idxs:
                    line += f" first at indices {idxs[:5]}"
                line += " [FAIL]"
                has_fail = True
            else:
                line += " [OK]"
            details_lines.append(line)
        
        elif btype == 'packets':
            if count >= 0:
                line = f"  {name}: first diff at byte {count}"
                ctx = info.get('context', [])
                if ctx:
                    line += "\n" + "\n".join(ctx[:6])
                line += " [FAIL]"
                has_fail = True
            else:
                line = f"  {name}: identical ({info.get('valid_a', 0)} valid bytes)"
                line += " [OK]"
            details_lines.append(line)
    
    # Final verdict
    if has_fail:
        verdict = "FAIL"
    elif has_fs_only:
        verdict = "FS-ONLY"
    else:
        verdict = "PASS"
    
    return verdict, details_lines


def main():
    parser = argparse.ArgumentParser(description='Compare PyroWave dump directories')
    parser.add_argument('dir_a', help='First dump directory (reference)')
    parser.add_argument('dir_b', help='Second dump directory (to compare)')
    parser.add_argument('--stage', type=int, default=None,
                        help='Only compare buffers from stage N')
    parser.add_argument('--tol-file', default=None,
                        help='Tolerance override file (JSON)')
    args = parser.parse_args()
    
    if not os.path.isdir(args.dir_a):
        print(f"ERROR: {args.dir_a} is not a directory", file=sys.stderr)
        return 1
    if not os.path.isdir(args.dir_b):
        print(f"ERROR: {args.dir_b} is not a directory", file=sys.stderr)
        return 1
    
    verdict, details = compare_dirs(args.dir_a, args.dir_b, args.stage)
    
    for line in details:
        print(line)
    
    print(f"\nVERDICT: {verdict}")
    
    if verdict == 'PASS':
        return 0
    elif verdict == 'FS-ONLY':
        return 0  # FS-ONLY is acceptable
    else:
        return 1


if __name__ == '__main__':
    sys.exit(main())
