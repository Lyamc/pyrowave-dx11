# pyrowave-dx11

Direct3D 11 (cs_5_0, feature level 11_0) port of the PyroWave wavelet video encoder, so etherquic-host can encode on
GPUs without Vulkan, first of all AN515's Radeon HD 6670 (TeraScale 2).

Layout:
- `prototype/` - forward-DWT feasibility prototype, copied from etherquic branch `proto/dx11-dwt` (commit 83d9398,
  not pushed). See `prototype/README.md` for its correctness and timing numbers (0.87 ms at 720p, 1.78 ms at 1080p on the HD 6670).
- `reference/pyrowave/` - read-only copy of upstream PyroWave (Themaister/pyrowave @ a0d2e71, the revision etherquic
  pins in `nix/pyrowave.nix`), taken from `~/scratch/pyrowave` on 192.168.0.24. MIT, see its LICENSE. Do not edit; it is the spec.
- `src/` - the port itself (empty for now).
- `docs/` - stage specs written during the port.
- `tools/strata_agent.py` - driver that sends repo files plus a task to the local Strata model server.
- `PLAN.md` - stages, the bit-exact requirement, build and test.

Build the prototype (VS Developer prompt, Windows SDK provides d3d11/d3dcompiler):
    cd prototype
    cl /O2 /EHsc /arch:AVX2 dx11dwt.cpp
Run `dx11dwt.exe` on AN515 (192.168.0.58, via ssh hop through 192.168.0.24) for the HD 6670 numbers.
