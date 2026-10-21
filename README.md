# pyrowave-dx11

Direct3D 11 port of [PyroWave](https://github.com/Themaister/pyrowave), the intra-only wavelet video encoder by Hans-Kristian Arntzen (Themaister). Compute shaders are `cs_5_0`. The device must be hardware feature level 11_0, so a GPU with no usable Vulkan driver can still encode. The first target is the Radeon HD 6670 (TeraScale 2).

The bitstream matches the Vulkan encoder. A player decodes it with `libpyrowave-shared`. Upstream PyroWave is MIT; this port is MIT too. Both copyright notices are in [LICENSE](LICENSE). The read-only upstream tree is `reference/pyrowave/` at commit `a0d2e71`, which is the revision etherquic pins.

## Release

Tag **v0.1.1** publishes `pyrowave_d3d11.dll` (x64, MSVC, static CRT, shader bytecode embedded). Put that DLL next to the program, in `C:\Program Files\etherquic`, or point `ETHERQUIC_PYROWAVE_D3D11` at it. `ETHERQUIC_PYROWAVE_D3D11=off` disables the encoder.

v0.1.1 encodes a 1280×720 frame on the Radeon HD 6670 that `libpyrowave-shared` decodes as a picture. It loads `d3d11.dll`, `dxgi.dll`, and `D3DCOMPILER_47.dll`, which Windows already provides.

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

## Performance

Wall time per frame for `encode_cpu` through `packetize`, on 2026-10-20. The input is the synthetic I420 frame from `prototype/pwbench.cpp`. The rate cap is `max(W*H*3/16, 32 KiB)`. Each cell is min / avg / max / std after 20 warmup frames, then at least 200 frames and 5 seconds. CPU is this process's share of the machine (32 logical processors on both PCs). Memory is the process working set. Full method, one-core CPU, and private bytes are in [comparison.md](comparison.md).

1280×720, cap 172,800 bytes:

| Machine | Encoder | Latency (ms) | CPU (%) | Working set (MiB) |
|---|---|---|---|---|
| AN515, Ryzen 9 5950X, Radeon HD 6670 | lavapipe | 21.06 / 22.89 / 28.05 / 1.39 | 11.7 / 34.7 / 65.3 / 10.6 | 89.5 / 94.9 / 99.3 / 4.9 |
| AN515 | D3D11 | 10.06 / 10.20 / 10.64 / 0.09 | 0.0 / 0.1 / 2.4 / 0.4 | 26.3 / 26.3 / 26.3 / 0.0 |
| HYTE, i9-13900K, RTX 3060 | lavapipe | 18.92 / 20.88 / 23.89 / 0.80 | 6.3 / 21.8 / 40.0 / 6.5 | 100.2 / 100.3 / 100.3 / 0.0 |
| HYTE | Vulkan | 0.43 / 0.52 / 3.72 / 0.24 | 0.0 / 1.2 / 2.9 / 0.9 | 197.7 / 198.0 / 198.1 / 0.1 |
| HYTE | D3D11 | 0.56 / 0.66 / 3.46 / 0.22 | 2.3 / 3.9 / 5.6 / 0.8 | 50.4 / 51.0 / 51.0 / 0.1 |

1920×1080, cap 388,800 bytes:

| Machine | Encoder | Latency (ms) | CPU (%) | Working set (MiB) |
|---|---|---|---|---|
| AN515 | lavapipe | 34.19 / 35.96 / 39.27 / 1.10 | 4.8 / 43.7 / 72.3 / 12.1 | 116.0 / 116.0 / 116.1 / 0.0 |
| AN515 | D3D11 | 20.67 / 20.92 / 21.54 / 0.17 | 0.0 / 0.0 / 0.8 / 0.2 | 30.8 / 30.8 / 30.8 / 0.0 |
| HYTE | lavapipe | 30.10 / 32.10 / 34.55 / 0.74 | 4.5 / 25.0 / 50.0 / 9.8 | 117.3 / 117.3 / 117.3 / 0.0 |
| HYTE | Vulkan | 0.73 / 0.86 / 5.00 / 0.34 | 0.0 / 1.0 / 2.9 / 0.8 | 203.2 / 203.6 / 203.7 / 0.1 |
| HYTE | D3D11 | 0.92 / 1.09 / 5.50 / 0.37 | 2.3 / 3.5 / 5.6 / 0.6 | 58.4 / 59.6 / 59.6 / 0.2 |

Lavapipe and Vulkan fill that cap (about 173 KB at 720p and 389 KB at 1080p). This D3D11 build leaves rate control at quant 0, so both GPUs wrote 35,840 bytes at 720p and 73,408 bytes at 1080p. The D3D11 times are for that smaller bitstream.
