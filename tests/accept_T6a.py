#!/usr/bin/env python3
"""Acceptance checks for the T6a Vulkan dump hooks.

PASS means both patches apply to reference/pyrowave at a0d2e71, a fixture dump
directory with real sha256 values is accepted, and placeholder / invalid /
incomplete / wrong-shape manifests are rejected.

    python tests/accept_T6a.py
    python tests/accept_T6a.py --check-dir path/to/dump
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile


REQUIRED_KEYS = ("vector", "W", "H", "budget", "sequence", "device", "ref", "buffers")
DTYPE_SIZE = {"f32": 4, "f16": 2, "u32": 4, "u16": 2, "u8": 1}


def required_names():
    names = []
    for component in range(3):
        for level in range(5):
            for band in range(4):
                names.append("band_c%d_l%d_b%d" % (component, level, band))
    names.extend([
        "meta-buffer",
        "block-stat-buffer",
        "payload-data",
        "bucket-buffer",
        "quant-buffer",
        "packed-meta",
        "bitstream",
    ])
    return names


def _shape_bytes(dtype, shape):
    if dtype not in DTYPE_SIZE:
        raise ValueError("bad dtype %s" % dtype)
    if not isinstance(shape, list) or not shape:
        raise ValueError("bad shape")
    n = DTYPE_SIZE[dtype]
    for dim in shape:
        if not isinstance(dim, int) or isinstance(dim, bool) or dim <= 0:
            raise ValueError("bad dimension")
        n *= dim
    return n


def check_dump_dir(path):
    """Return a list of error strings. Empty means the directory is a valid §3.2 dump."""
    errors = []
    manifest_path = os.path.join(path, "manifest.json")
    try:
        with open(manifest_path, "r", encoding="utf-8") as handle:
            manifest = json.load(handle)
    except Exception as exc:
        return ["invalid manifest.json: %s" % exc]

    if not isinstance(manifest, dict):
        return ["manifest.json is not an object"]

    for key in REQUIRED_KEYS:
        if key not in manifest:
            errors.append("missing key %s" % key)
    if errors:
        return errors

    buffers = manifest["buffers"]
    if not isinstance(buffers, list):
        return ["buffers is not a list"]

    seen = set()
    for entry in buffers:
        if not isinstance(entry, dict):
            errors.append("buffer entry is not an object")
            continue
        for field in ("name", "stage", "dtype", "shape", "file", "sha256"):
            if field not in entry:
                errors.append("buffer missing %s" % field)
                continue
        name = entry.get("name")
        sha = entry.get("sha256")
        if not isinstance(sha, str) or sha == "placeholder" or len(sha) != 64:
            errors.append("%s sha256 is not a real hash" % name)
            continue
        try:
            int(sha, 16)
        except ValueError:
            errors.append("%s sha256 is not hex" % name)
            continue
        filename = entry.get("file")
        if not isinstance(filename, str) or not filename.endswith(".bin"):
            errors.append("%s file field is not a .bin" % name)
            continue
        bin_path = os.path.join(path, filename)
        if not os.path.isfile(bin_path):
            errors.append("missing file %s" % filename)
            continue
        data = open(bin_path, "rb").read()
        digest = hashlib.sha256(data).hexdigest()
        if digest != sha:
            errors.append("%s sha256 does not match %s" % (name, filename))
        try:
            expect = _shape_bytes(entry.get("dtype"), entry.get("shape"))
        except ValueError as exc:
            errors.append("%s %s" % (name, exc))
            continue
        if expect != len(data):
            errors.append("%s shape %s does not match %d file bytes" % (name, entry.get("shape"), len(data)))
        if isinstance(name, str) and name.startswith("band_") and entry.get("dtype") != "f32":
            errors.append("%s dtype is not f32" % name)
        if isinstance(name, str) and name.startswith("band_"):
            shape = entry.get("shape")
            if not isinstance(shape, list) or len(shape) != 2:
                errors.append("%s shape is not [height, width]" % name)
        seen.add(name)

    for name in required_names():
        if name not in seen:
            errors.append("missing buffer %s" % name)
    return errors


def _write_entry(directory, name, payload, shape, dtype, stage, sha=None):
    filename = name + ".bin"
    with open(os.path.join(directory, filename), "wb") as handle:
        handle.write(payload)
    if sha is None:
        sha = hashlib.sha256(payload).hexdigest()
    return {
        "name": name,
        "stage": stage,
        "dtype": dtype,
        "shape": shape,
        "file": filename,
        "sha256": sha,
    }


def _write_manifest(directory, buffers):
    manifest = {
        "vector": "flat128",
        "W": 16,
        "H": 16,
        "budget": 32768,
        "sequence": 1,
        "device": "test-device/1",
        "ref": "a0d2e71+vkref",
        "buffers": buffers,
    }
    with open(os.path.join(directory, "manifest.json"), "w", encoding="utf-8", newline="\n") as handle:
        json.dump(manifest, handle)


def _good_buffers(directory):
    buffers = []
    # 2x2 f32 band. Shape matches the file. Not the old invented [8, 4].
    band = b"\x00\x00\x80\x3f" * 4
    stage_for = {
        "meta-buffer": 2,
        "block-stat-buffer": 2,
        "payload-data": 2,
        "bucket-buffer": 3,
        "quant-buffer": 4,
        "packed-meta": 5,
        "bitstream": 5,
    }
    for name in required_names():
        if name.startswith("band_"):
            buffers.append(_write_entry(directory, name, band, [2, 2], "f32", 1))
        else:
            payload = b"\x11\x22\x33\x44"
            buffers.append(_write_entry(directory, name, payload, [4], "u8", stage_for[name]))
    return buffers


def _apply_patches(repo_root):
    vkref = os.path.join(repo_root, "tools", "vkref")
    ref = os.path.join(repo_root, "reference", "pyrowave")
    patch1 = os.path.join(vkref, "0001-dump-hooks.patch")
    patch2 = os.path.join(vkref, "0002-inject-bands.patch")
    if not os.path.isfile(patch1) or not os.path.isfile(patch2):
        return False, "patch file missing"
    work = tempfile.mkdtemp(prefix="vkref-apply-")
    try:
        target = os.path.join(work, "pyrowave")
        shutil.copytree(ref, target)
        # Patch 2 depends on patch 1, so check it against the tree patch 1 produces.
        steps = (
            (["git", "apply", "--check", patch1], patch1),
            (["git", "apply", patch1], patch1),
            (["git", "apply", "--check", patch2], patch2),
            (["git", "apply", patch2], patch2),
        )
        for cmd, patch in steps:
            result = subprocess.run(cmd, cwd=target, capture_output=True)
            if result.returncode != 0:
                detail = (result.stderr or result.stdout or b"").decode("utf-8", "replace")
                return False, "%s failed: %s" % (" ".join(cmd[-2:]), detail.strip())
        return True, ""
    finally:
        shutil.rmtree(work, ignore_errors=True)


def _expect_fail(label, errors):
    if errors:
        print("PASS %s" % label)
        return True
    print("FAIL %s (checker accepted a bad manifest)" % label)
    return False


def main(argv):
    if len(argv) == 3 and argv[1] == "--check-dir":
        errors = check_dump_dir(argv[2])
        if errors:
            print("FAIL %s" % argv[2])
            for err in errors:
                print("  %s" % err)
            return 1
        print("PASS %s" % argv[2])
        return 0

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ok = True

    applied, detail = _apply_patches(repo_root)
    if applied:
        print("PASS PATCHES_APPLY")
    else:
        print("FAIL PATCHES_APPLY (%s)" % detail)
        ok = False

    root = tempfile.mkdtemp(prefix="vkref-fixture-")
    try:
        good = os.path.join(root, "good")
        os.makedirs(good)
        _write_manifest(good, _good_buffers(good))
        if check_dump_dir(good):
            print("FAIL FIXTURE")
            for err in check_dump_dir(good):
                print("  %s" % err)
            ok = False
        else:
            print("PASS FIXTURE")

        placeholder = os.path.join(root, "placeholder")
        os.makedirs(placeholder)
        buffers = _good_buffers(placeholder)
        buffers[0]["sha256"] = "placeholder"
        _write_manifest(placeholder, buffers)
        ok = _expect_fail("REJECT_PLACEHOLDER", check_dump_dir(placeholder)) and ok

        invalid = os.path.join(root, "invalid")
        os.makedirs(invalid)
        with open(os.path.join(invalid, "manifest.json"), "w", encoding="utf-8") as handle:
            handle.write("{ this is not json")
        ok = _expect_fail("REJECT_INVALID_JSON", check_dump_dir(invalid)) and ok

        missing = os.path.join(root, "missing")
        os.makedirs(missing)
        buffers = _good_buffers(missing)
        buffers = [b for b in buffers if b["name"] != "packed-meta"]
        _write_manifest(missing, buffers)
        ok = _expect_fail("REJECT_MISSING_NAME", check_dump_dir(missing)) and ok

        # The old generator claimed every level-0 band was shape [8, 4]
        # (width/32, height/32) regardless of the bytes on disk.
        invented = os.path.join(root, "invented")
        os.makedirs(invented)
        buffers = _good_buffers(invented)
        for entry in buffers:
            if entry["name"] == "band_c0_l0_b0":
                entry["shape"] = [8, 4]
        _write_manifest(invented, buffers)
        ok = _expect_fail("REJECT_BAD_SHAPE", check_dump_dir(invented)) and ok
    finally:
        shutil.rmtree(root, ignore_errors=True)

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
