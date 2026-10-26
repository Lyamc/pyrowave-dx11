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

Wall time per frame for `encode_cpu` through `packetize`. The input is the synthetic I420 frame from `prototype/pwbench.cpp`. The rate cap is `max(W*H*3/16, 32 KiB)`. Each cell is min / avg / max / std after 20 warmup frames, then at least 200 frames and 5 seconds. CPU is this process's share of the machine (32 logical processors). Memory is the process working set. Full method, one-core CPU, and private bytes are in [comparison.md](comparison.md).

### HYTE, 2026-10-26

i9-13900K and NVIDIA GeForce RTX 3060. Lavapipe reports `llvmpipe (LLVM 23.1.2, 512 bits)`. Vulkan device creation asked for vendor `0x10DE`. D3D11 used the adapter whose name contains `NVIDIA`. OpenGL is the [pyrowave-gl](https://github.com/Lyamc/pyrowave-gl) encoder in a WGL 4.3 core context. Its renderer string is `NVIDIA GeForce RTX 3060/PCIe/SSE2`.

D3D11 and Vulkan at 720p pass 8,000 frames before 5 seconds, so those runs set `PWCMP_MAX_FRAMES=30000` and stopped at 5 seconds. The OpenGL bench uses the same warmup and the same 5 second window. Its packet buffer is 64 MiB, because the quant-0 pyramid is larger than the cap plus 1 MiB.

1280×720, cap 172,800 bytes:

| Encoder | Latency (ms) | CPU (%) | Working set (MiB) | Output bytes |
|---|---|---|---|---|
| lavapipe | 19.76 / 21.69 / 24.64 / 0.86 | 6.3 / 23.5 / 45.5 / 7.8 | 101.1 / 101.1 / 101.1 / 0.0 | 172,740 |
| Vulkan | 0.42 / 0.54 / 2.68 / 0.15 | 0.0 / 1.4 / 3.9 / 0.9 | 198.2 / 198.6 / 198.7 / 0.1 | 172,760 |
| OpenGL | 2.53 / 2.64 / 3.84 / 0.11 | 3.2 / 5.8 / 6.5 / 0.7 | 178.0 / 178.7 / 178.7 / 0.1 | 1,324,828 |
| D3D11 | 0.27 / 0.34 / 2.10 / 0.12 | 1.6 / 4.3 / 6.2 / 1.0 | 45.6 / 45.7 / 45.7 / 0.0 | 41,988 |

1920×1080, cap 388,800 bytes:

| Encoder | Latency (ms) | CPU (%) | Working set (MiB) | Output bytes |
|---|---|---|---|---|
| lavapipe | 30.55 / 33.54 / 36.47 / 1.05 | 6.3 / 27.3 / 49.5 / 8.6 | 117.5 / 117.6 / 118.6 / 0.1 | 388,720 |
| Vulkan | 0.74 / 0.88 / 3.05 / 0.17 | 0.0 / 1.1 / 2.9 / 1.0 | 203.7 / 204.0 / 204.1 / 0.1 | 388,676 |
| OpenGL | 3.99 / 4.19 / 4.93 / 0.14 | 3.3 / 5.7 / 7.8 / 0.7 | 231.8 / 231.9 / 232.8 / 0.3 | 2,831,224 |
| D3D11 | 0.42 / 0.54 / 3.02 / 0.19 | 1.6 / 4.0 / 6.2 / 0.9 | 50.7 / 50.8 / 50.8 / 0.0 | 79,964 |

Lavapipe and Vulkan fill the cap. This D3D11 tree leaves rate control at quant 0 and keeps a prefix of the pyramid: 60 blocks when the width is under 1600, and 96 blocks at 1080p (41,988 and 79,964 bytes). OpenGL is quant 0 as well and packs the whole pyramid (1,324,828 and 2,831,224 bytes). The times above are for those bitstreams.

If the next frame waits for packetize, the average supports about 46 fps and 30 fps on lavapipe, 1,870 fps and 1,140 fps on Vulkan, 380 fps and 240 fps on OpenGL, and 2,950 fps and 1,860 fps on D3D11.

### 2026-10-20

The tables below are the installed `pyrowave_d3d11.dll` from 2026-10-19, which wrote 35,840 bytes at 720p and 73,408 bytes at 1080p, plus the same lavapipe and Vulkan binaries. The AN515 rows are that day's measurement.

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

Lavapipe and Vulkan filled that cap (about 173 KB at 720p and 389 KB at 1080p). The installed D3D11 DLL left rate control at quant 0 and wrote 35,840 bytes at 720p and 73,408 bytes at 1080p. Those D3D11 times are for that bitstream.
