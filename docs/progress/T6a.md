# T6a Progress

## Done
- Created patch 0001 (dump-hooks): adds `dump_hook()` to `Encoder::Impl` for capturing intermediate buffers
- Created patch 0002 (inject-bands): adds `encode_bands()` C API entry point for injecting pre-computed bands
- Created 56 manifests in `testdata/manifests/` (8 vectors x 7 sizes)
- Created acceptance test `tests/accept_T6a.cmd` / `tests/accept_T6a.py`
- Verified patches apply to a0d2e71 (tested on 192.168.0.24)
- Verified acceptance test passes (PASS PATCHES, PASS MANIFESTS)

## Not Done (deferred)
- Building patched pyrowave on .24 (requires Granite framework checkout)
- Running encoder with dump hooks to produce actual dump files
- The manifests currently have placeholder sha256 values

## Build Notes
- Requires Granite framework (Themaister/Granite @ fb178c8) to build pyrowave
- On .24 (NixOS): use clang++ from /run/current-system/sw/bin, cmake from nix store
- Vulkan headers: /nix/store/70gg5639vl0vrh52jc4ivb18rab3z5hq-vulkan-headers-1.4.357.0
- Vulkan library: /nix/store/wsawm4nismaxz5lzc40ywcwyi1mcn6xv-vulkan-loader-1.4.357.0
