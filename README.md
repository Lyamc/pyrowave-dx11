# pyrowave-dx11

Direct3D 11 port of [PyroWave](https://github.com/Themaister/pyrowave), the intra-only wavelet video encoder by Hans-Kristian Arntzen (Themaister). Compute shaders are `cs_5_0`. The device must be hardware feature level 11_0, so a GPU with no usable Vulkan driver can still encode. The first target is the Radeon HD 6670 (TeraScale 2).

The bitstream matches the Vulkan encoder. A player decodes it with `libpyrowave-shared`. Upstream PyroWave is MIT; this port is MIT too. Both copyright notices are in [LICENSE](LICENSE). The read-only upstream tree is `reference/pyrowave/` at commit `a0d2e71`, which is the revision etherquic pins.

## Release

Tag **v0.1.3** publishes `pyrowave_d3d11.dll` (x64, MSVC, static CRT, shader bytecode embedded). Tag v0.1.2 is the earlier quant-0 DLL. Put the DLL next to the program, in `C:\Program Files\etherquic`, or point `ETHERQUIC_PYROWAVE_D3D11` at it. `ETHERQUIC_PYROWAVE_D3D11=off` disables the encoder. It loads `d3d11.dll`, `dxgi.dll`, and `D3DCOMPILER_47.dll`, which Windows already provides.

On the Radeon HD 6670, a warm 1280×720 `encode_cpu` at the 4 bit-per-pixel budget (460800 bytes) is the mean of 8 calls after 8 warmup frames. The first frame in a process compiles the shaders.

| Pattern | ms | Record |
| --- | ---: | --- |
| Gradient | 10.4 | 45412 bytes, 1 packet |
| Mild | 12.1 | 150720 bytes, 3 packets |
| Desktop | 15.4 | 289280 bytes, 5 packets |
| Edges | 15.5 | 272244 bytes, 5 packets |
| Noise | 24.0 | does not fit |

After warmup, a desktop frame spends about 0.6 ms uploading, 3.8 ms in the wavelet, 1.5 ms quantising, and 8.9 ms packing. The gradient's pack is about 4.3 ms. Pack is the long call.

`libpyrowave-shared` 1.0.0 (upstream `a0d2e71`) decoded the four records that fit, on the compute path. The gradient and the desktop match the source. The edge grid and the mild pattern come back as pictures, with wavelet error on the hard edges. A live 1280×720 desktop asked for 60 fps and held about 44–48 fps, near 45 Mbit/s. The live path keeps one frame on the GPU, so the picture is one frame behind the capture.

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

## HYTE, 2026-10-26

Lavapipe, OpenGL, the quant-0 prefix encoder on branch `quant0-prefix` (`2c5f8f6`), and Vulkan, on the i9-13900K and RTX 3060. This is a different D3D11 binary from tag v0.1.3 in the Release section. The prefix build keeps 60 blocks when the width is under 1600 and 96 blocks at 1080p. OpenGL is [pyrowave-gl](https://github.com/Lyamc/pyrowave-gl), a WGL 4.3 context, renderer `NVIDIA GeForce RTX 3060/PCIe/SSE2`. Lavapipe is `llvmpipe (LLVM 23.1.2, 512 bits)`. Vulkan asked for vendor `0x10DE`.

The input is the synthetic I420 frame from `prototype/pwbench.cpp`. The rate cap is `max(W*H*3/16, 32 KiB)`. Each cell is min / avg / max / std after 20 warmup frames, then at least 200 frames and 5 seconds. CPU is this process's share of 32 logical processors. Full method, one-core CPU, and private bytes are in [comparison.md](comparison.md).

1280×720, cap 172,800 bytes:

| Encoder | Latency (ms) | CPU (%) | Working set (MiB) | Output bytes |
|---|---|---|---|---|
| lavapipe | 19.76 / 21.69 / 24.64 / 0.86 | 6.3 / 23.5 / 45.5 / 7.8 | 101.1 / 101.1 / 101.1 / 0.0 | 172,740 |
| Vulkan | 0.42 / 0.54 / 2.68 / 0.15 | 0.0 / 1.4 / 3.9 / 0.9 | 198.2 / 198.6 / 198.7 / 0.1 | 172,760 |
| OpenGL | 2.53 / 2.64 / 3.84 / 0.11 | 3.2 / 5.8 / 6.5 / 0.7 | 178.0 / 178.7 / 178.7 / 0.1 | 1,324,828 |
| D3D11 prefix | 0.27 / 0.34 / 2.10 / 0.12 | 1.6 / 4.3 / 6.2 / 1.0 | 45.6 / 45.7 / 45.7 / 0.0 | 41,988 |

1920×1080, cap 388,800 bytes:

| Encoder | Latency (ms) | CPU (%) | Working set (MiB) | Output bytes |
|---|---|---|---|---|
| lavapipe | 30.55 / 33.54 / 36.47 / 1.05 | 6.3 / 27.3 / 49.5 / 8.6 | 117.5 / 117.6 / 118.6 / 0.1 | 388,720 |
| Vulkan | 0.74 / 0.88 / 3.05 / 0.17 | 0.0 / 1.1 / 2.9 / 1.0 | 203.7 / 204.0 / 204.1 / 0.1 | 388,676 |
| OpenGL | 3.99 / 4.19 / 4.93 / 0.14 | 3.3 / 5.7 / 7.8 / 0.7 | 231.8 / 231.9 / 232.8 / 0.3 | 2,831,224 |
| D3D11 prefix | 0.42 / 0.54 / 3.02 / 0.19 | 1.6 / 4.0 / 6.2 / 0.9 | 50.7 / 50.8 / 50.8 / 0.0 | 79,964 |

Lavapipe and Vulkan fill the cap. The prefix D3D11 bitstream is 41,988 and 79,964 bytes. OpenGL packs the whole quant-0 pyramid, 1,324,828 and 2,831,224 bytes. Average frame rate if the next frame waits for packetize: lavapipe 46 and 30, Vulkan 1,870 and 1,140, OpenGL 380 and 240, D3D11 prefix 2,950 and 1,860.

## Performance

The tables below are the 2026-10-20 bench of the previous DLL (tag v0.1.2). That build left rate control at quant 0. The rate cap was `max(W*H*3/16, 32 KiB)`. Each cell is min / avg / max / std after 20 warmup frames, then at least 200 frames and 5 seconds. CPU is this process's share of the machine (32 logical processors on both PCs). Memory is the process working set. The encode-only frame rates in that bench are not a live desktop rate, and they are not the 4 bit-per-pixel encoder in the Release section. Full method, one-core CPU, and private bytes are in [comparison.md](comparison.md).

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

Lavapipe and Vulkan fill that cap (about 173 KB at 720p and 389 KB at 1080p). The quant-0 D3D11 DLL wrote 35,840 bytes at 720p and 73,408 bytes at 1080p, so those D3D11 times are for a smaller bitstream than the 4 bit-per-pixel records above.
