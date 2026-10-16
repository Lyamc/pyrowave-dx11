# pyrowave-dx11

Direct3D 11 port of [PyroWave](https://github.com/Themaister/pyrowave), the intra-only wavelet video encoder by Hans-Kristian Arntzen (Themaister). Compute shaders are `cs_5_0`. The device must be hardware feature level 11_0, so a GPU with no usable Vulkan driver can still encode. The first target is the Radeon HD 6670 (TeraScale 2).

The bitstream matches the Vulkan encoder. A player decodes it with `libpyrowave-shared`. Upstream PyroWave is MIT; this port is MIT too. Both copyright notices are in [LICENSE](LICENSE). The read-only upstream tree is `reference/pyrowave/` at commit `a0d2e71`, which is the revision etherquic pins.

## Release

Tag **v0.1.0** publishes `pyrowave_d3d11.dll` (x64, MSVC, static CRT, shader bytecode embedded). Put that DLL next to the program, in `C:\Program Files\etherquic`, or point `ETHERQUIC_PYROWAVE_D3D11` at it. `ETHERQUIC_PYROWAVE_D3D11=off` disables the encoder.

The 0.1.0 DLL encoded a 64×64 frame on a machine with a hardware Direct3D 11 device, and `libpyrowave-shared` decoded that record. It has not been timed on the HD 6670. It loads `d3d11.dll`, `dxgi.dll`, and `D3DCOMPILER_47.dll`, which Windows already provides.

## Build

From a Visual Studio x64 developer prompt, with CMake and the Windows SDK (`fxc`, `d3d11`, `dxgi`):

```text
cmake -S src -B build
cmake --build build --config Release --target pyrowave_d3d11
```

The shared library is `build/Release/pyrowave_d3d11.dll`. Tests link the sources directly and do not load the DLL. `test_t5b` encodes one 64×64 frame.

## Layout

- `src/`: the encoder. The C ABI is `src/include/pyrowave_d3d11.h`.
- `reference/pyrowave/`: upstream source. Do not edit it.
- `prototype/`: forward-DWT feasibility run, copied from etherquic branch `proto/dx11-dwt` (`83d9398`).
- `docs/`: pipeline spec, stage notes, validation plan, and the etherquic integration notes.
- `AGENTS.md`, `PLAN.md`: how the port was built.

## ABI

`pwd3d11_create_device`, `pwd3d11_encoder_create`, `pwd3d11_encoder_encode_cpu`, then `pwd3d11_encoder_compute_num_packets` and `pwd3d11_encoder_packetize`. Records use the same packet framing as the Vulkan library: a little-endian packet count, then a little-endian size and the bytes of each packet. `PWD3D11_API_VERSION` is 1.
