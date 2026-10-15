#!/usr/bin/env python3
"""accept_T6a.py - Acceptance test for T6a (T6a: tools/vkref patches)

Criteria:
  PATCHES - patches exist in tools/vkref/
  MANIFESTS - manifests exist for all vectors
"""

import os
import sys
import json

def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    vkref_dir = os.path.join(repo_root, "tools", "vkref")
    manifests_dir = os.path.join(repo_root, "testdata", "manifests")
    
    result = 0
    
    # Check patches
    patch1 = os.path.join(vkref_dir, "0001-dump-hooks.patch")
    patch2 = os.path.join(vkref_dir, "0002-inject-bands.patch")
    
    if not os.path.exists(patch1):
        print("FAIL PATCHES (0001-dump-hooks.patch not found)")
        result = 1
    if not os.path.exists(patch2):
        print("FAIL PATCHES (0002-inject-bands.patch not found)")
        result = 1
    if result == 0:
        print("PASS PATCHES")
    
    # Check manifests
    manifest_files = [f for f in os.listdir(manifests_dir) if f.endswith(".json")]
    manifest_count = len(manifest_files)
    
    if manifest_count >= 56:
        print("PASS MANIFESTS")
    else:
        print(f"FAIL MANIFESTS (found {manifest_count}, expected 56)")
        result = 1
    
    return result

if __name__ == "__main__":
    sys.exit(main())
