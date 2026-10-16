#!/usr/bin/env python3
"""Manifests are written by the patched encoder next to the .bin files.

This script used to emit placeholder JSON. That is not a VALIDATION §3.4 dump.
Run pyrowave-vkref-driver with --dump on instead.
"""

import sys

print("refusing to write placeholder manifests; use the vkref driver", file=sys.stderr)
sys.exit(1)
