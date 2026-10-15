#!/usr/bin/env python3
"""accept_T6a.py - Acceptance test for T6a (T6a: tools/vkref patches)

Criteria:
  PATCHES_APPLY - patches apply cleanly to reference/pyrowave (a0d2e71)
  MANIFESTS - manifests exist for all vectors
"""

import os
import sys
import json
import tempfile
import subprocess


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    vkref_dir = os.path.join(repo_root, "tools", "vkref")
    manifests_dir = os.path.join(repo_root, "testdata", "manifests")
    ref_dir = os.path.join(repo_root, "reference", "pyrowave")

    result = 0

    # --- PATCHES_APPLY ---
    patch1 = os.path.join(vkref_dir, "0001-dump-hooks.patch")
    patch2 = os.path.join(vkref_dir, "0002-inject-bands.patch")

    patches_ok = True
    if not os.path.exists(patch1):
        print("FAIL PATCHES_APPLY (0001-dump-hooks.patch not found)")
        patches_ok = False
    if not os.path.exists(patch2):
        print("FAIL PATCHES_APPLY (0002-inject-bands.patch not found)")
        patches_ok = False

    if patches_ok and os.path.isdir(ref_dir):
        # Create a temp copy of reference/pyrowave to test applying patches
        with tempfile.TemporaryDirectory() as tmpdir:
            import shutil
            work_dir = os.path.join(tmpdir, "pyrowave")
            shutil.copytree(ref_dir, work_dir)

            for patch_file in [patch1, patch2]:
                r = subprocess.run(
                    ["git", "apply", "--check", patch_file],
                    cwd=work_dir,
                    capture_output=True,
                )
                if r.returncode != 0:
                    print(f"FAIL PATCHES_APPLY ({os.path.basename(patch_file)} does not apply)")
                    patches_ok = False
                    break

            if patches_ok:
                print("PASS PATCHES_APPLY")

    # --- MANIFESTS ---
    manifest_files = [f for f in os.listdir(manifests_dir) if f.endswith(".json")]
    manifest_count = len(manifest_files)

    if manifest_count >= 56:
        print("PASS MANIFESTS")
    else:
        print(f"FAIL MANIFESTS (found {manifest_count}, expected 56)")
        result = 1

    return 0 if patches_ok and manifest_count >= 56 else 1


if __name__ == "__main__":
    sys.exit(main())
