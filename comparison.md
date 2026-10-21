# PyroWave encode comparison

Measured on 2026-10-20. Same installed binaries on both machines:

| Binary | Path | Size | SHA-256 prefix | Written |
|---|---|---|---|---|
| D3D11 encoder | `C:\Program Files\etherquic\pyrowave_d3d11.dll` | 353,792 | `D04101D13D90B0BE` | 2026-10-19 12:44 |
| Vulkan / lavapipe encoder | `C:\Program Files\etherquic\libpyrowave-shared-1.dll` | 4,292,076 | `23F78ED66B0B399B` | 2026-10-17 10:15 |
| lavapipe driver | `C:\Program Files\etherquic\vulkan-cpu\vulkan_lvp.dll` | 56,386,048 |  |  |

The bench is `tools/pwcmpbench.cpp`. It loads those DLLs and times `encode_cpu`, then `compute_num_packets` and `packetize`.

## Machines

| | AN515 (`NITRO-AN515-53`, 192.168.0.58) | HYTE (192.168.0.30) |
|---|---|---|
| OS | Windows 10.0.19045 | Windows 10.0.26200 |
| CPU | AMD Ryzen 9 5950X, 16 cores / 32 threads | Intel Core i9-13900K, 24 cores / 32 threads |
| RAM | 16 GB | 96 GB |
| GPU used | AMD Radeon HD 6670, driver 15.201.1151.1008, 1,057,685,504 bytes dedicated | NVIDIA GeForce RTX 3060 (12 GB, device 2504) for Vulkan and D3D11 |

HYTE also has an Intel UHD Graphics 770. Vulkan device creation asked for vendor `0x10DE`, so the timed Vulkan device is the RTX 3060. D3D11 used the first DXGI adapter whose name contains `NVIDIA` (adapter 0). DXGI lists that RTX twice; the run printed `NVIDIA GeForce RTX 3060`.

AN515 has no hardware Vulkan device. Its CPU path is lavapipe.

## Method

- Input is the deterministic synthetic I420 pattern from `prototype/pwbench.cpp` (gradients, checker, and noise). One buffer is reused every frame.
- 4:2:0. Sizes 1280×720 and 1920×1080.
- Rate cap is etherquic's `default_frame_bytes`: `max(W*H*3/16, 32 KiB)`. That is 172,800 bytes at 720p and 388,800 bytes at 1080p.
- Packetize uses one packet boundary the size of the output buffer, matching `prototype/pwbench.cpp`.
- 20 warmup frames, then frames until both 200 frames and 5 seconds are done. Cap 20,000 frames or 90 seconds. Every run stopped on the 5 second mark except where a frame simply lasts longer than 5/200 seconds, in which case it ran 200 frames.
- Latency is `QueryPerformanceCounter` from the start of `encode_cpu` through `packetize`, per frame. Min, average, max, and sample standard deviation (divide by n−1).
- Every 50 ms during the measured region the bench samples the process. CPU is `(kernel + user time from GetProcessTimes) / wall time`, divided by 32 logical processors, so 100 means the whole machine is busy in this process. The one-core column is that same delta before the divide, so 100 means one logical processor is full. Memory is `WorkingSetSize`. Private bytes are below.
- lavapipe: the process preloads `vulkan-cpu\vulkan-1.dll` (it loads `vulkan_lvp.dll` beside itself) and sets `LP_NATIVE_VECTOR_WIDTH=512`. The device string is `llvmpipe (LLVM 23.1.2, 512 bits)`. `encoder_create` fails at vector width 128 and 256. The SSH session is elevated, so the Vulkan loader ignores `VK_DRIVER_FILES`; preloading this loader is what selects lavapipe.
- A throwaway instance enumerate runs before the timed create so the log names the device. Warmup covers pipeline compilation. The first lavapipe frame, before warmup, was about 2.9 s.

## 1280×720

Budget 172,800 bytes. CPU is percent of the machine. Memory is process working set, MiB.

| Machine | Encoder | Frames | Latency ms min / avg / max / std | CPU % min / avg / max / std | Working set MiB min / avg / max / std | Output bytes |
|---|---|---|---|---|---|---|
| AN515 | lavapipe | 219 | 21.06 / 22.89 / 28.05 / 1.39 | 11.7 / 34.7 / 65.3 / 10.6 | 89.5 / 94.9 / 99.3 / 4.9 | 172,740 |
| AN515 | D3D11 HD 6670 | 491 | 10.06 / 10.20 / 10.64 / 0.09 | 0.0 / 0.1 / 2.4 / 0.4 | 26.3 / 26.3 / 26.3 / 0.0 | 35,840 |
| HYTE | lavapipe | 240 | 18.92 / 20.88 / 23.89 / 0.80 | 6.3 / 21.8 / 40.0 / 6.5 | 100.2 / 100.3 / 100.3 / 0.0 | 172,740 |
| HYTE | Vulkan RTX 3060 | 9,633 | 0.43 / 0.52 / 3.72 / 0.24 | 0.0 / 1.2 / 2.9 / 0.9 | 197.7 / 198.0 / 198.1 / 0.1 | 172,760 |
| HYTE | D3D11 RTX 3060 | 7,616 | 0.56 / 0.66 / 3.46 / 0.22 | 2.3 / 3.9 / 5.6 / 0.8 | 50.4 / 51.0 / 51.0 / 0.1 | 35,840 |

One logical processor = 3.125% of either machine. Average one-core CPU (100 = one full thread): AN515 lavapipe 1,111, AN515 D3D11 4, HYTE lavapipe 698, HYTE Vulkan 40, HYTE D3D11 123.

## 1920×1080

Budget 388,800 bytes.

| Machine | Encoder | Frames | Latency ms min / avg / max / std | CPU % min / avg / max / std | Working set MiB min / avg / max / std | Output bytes |
|---|---|---|---|---|---|---|
| AN515 | lavapipe | 200 | 34.19 / 35.96 / 39.27 / 1.10 | 4.8 / 43.7 / 72.3 / 12.1 | 116.0 / 116.0 / 116.1 / 0.0 | 388,720 |
| AN515 | D3D11 HD 6670 | 239 | 20.67 / 20.92 / 21.54 / 0.17 | 0.0 / 0.0 / 0.8 / 0.2 | 30.8 / 30.8 / 30.8 / 0.0 | 73,408 |
| HYTE | lavapipe | 200 | 30.10 / 32.10 / 34.55 / 0.74 | 4.5 / 25.0 / 50.0 / 9.8 | 117.3 / 117.3 / 117.3 / 0.0 | 388,720 |
| HYTE | Vulkan RTX 3060 | 5,819 | 0.73 / 0.86 / 5.00 / 0.34 | 0.0 / 1.0 / 2.9 / 0.8 | 203.2 / 203.6 / 203.7 / 0.1 | 388,676 |
| HYTE | D3D11 RTX 3060 | 4,585 | 0.92 / 1.09 / 5.50 / 0.37 | 2.3 / 3.5 / 5.6 / 0.6 | 58.4 / 59.6 / 59.6 / 0.2 | 73,408 |

Average one-core CPU: AN515 lavapipe 1,399, AN515 D3D11 1, HYTE lavapipe 800, HYTE Vulkan 33, HYTE D3D11 113.

## Private bytes

Process private bytes stayed flat. Averages, MiB:

| Machine | Encoder | 720p | 1080p |
|---|---|---|---|
| AN515 | lavapipe | 112.0 | 135.6 |
| AN515 | D3D11 | 48.7 | 88.6 |
| HYTE | lavapipe | 109.6 | 132.5 |
| HYTE | Vulkan | 263.7 | 294.8 |
| HYTE | D3D11 | 96.8 | 149.9 |

Working set and private bytes are host memory. They do not include GPU dedicated memory. The HD 6670 run reported 1,057,685,504 bytes of dedicated video memory on the adapter.

## What the numbers say

Average frame time, and the frame rate that time supports if the next frame waits for packetize:

| | 720p avg | 720p fps | 1080p avg | 1080p fps |
|---|---|---|---|---|
| AN515 lavapipe | 22.89 ms | 44 | 35.96 ms | 28 |
| AN515 D3D11 | 10.20 ms | 98 | 20.92 ms | 48 |
| HYTE lavapipe | 20.88 ms | 48 | 32.10 ms | 31 |
| HYTE Vulkan | 0.52 ms | 1,920 | 0.86 ms | 1,160 |
| HYTE D3D11 | 0.66 ms | 1,520 | 1.09 ms | 920 |

On AN515 the HD 6670 D3D11 encode is a little under half the lavapipe frame time, and the process stays near idle (under 0.2% of the machine on average). Lavapipe holds about 11 to 14 threads busy.

On HYTE the RTX 3060 Vulkan encode is the fastest and the lightest on the CPU. D3D11 on the same GPU is about 1.3× that frame time and about one logical processor of CPU, which is the submit and readback path.

Lavapipe output size matches across the two CPUs (172,740 and 388,720 bytes) and sits on the rate cap. Vulkan on the RTX is within a few dozen bytes of that (172,760 and 388,676). Both D3D11 runs, on the HD 6670 and on the RTX 3060, wrote 35,840 bytes at 720p and 73,408 bytes at 1080p. The current encoder leaves rate control at quant 0 (`src/frame.cpp`, the clear of the quant UAV before packing), so this D3D11 DLL does less bitstream work than lavapipe and Vulkan at the same cap. The D3D11 times are for that smaller output.

CPU samples on the GPU paths often read 0. `GetProcessTimes` does not advance while the thread is waiting on the GPU, and a 50 ms slice can fall between the short bursts of submit and map. The average over the run is the figure to use. Lavapipe's CPU standard deviation is large because the 50 ms slices swing from a few threads to most of the chip.

D3D11 working set on the HD 6670 was 26 MiB at 720p and 31 MiB at 1080p. Lavapipe was about 95 to 116 MiB. Vulkan on the RTX was about 198 to 204 MiB.
