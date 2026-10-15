# AGENTS.md: working on pyrowave-dx11

Read this file first. Then read `docs/SPEC.md`, then the stage document for your task in `docs/stages/`.

## 1. Goal

Port the PyroWave encoder (upstream Themaister/pyrowave @ a0d2e71, copied into `reference/pyrowave/`) from Vulkan to
**Direct3D 11, feature level 11_0, `cs_5_0`**, so etherquic's Windows host can encode PyroWave on GPUs without Vulkan.
The first target is AN515's Radeon HD 6670 (TeraScale 2).

Success means:
1. the existing Vulkan PyroWave decoder accepts every frame (SPEC RQ1);
2. given identical DWT bands, the packet bytes equal the Vulkan encoder's, ignoring don't-care bits (SPEC RQ2, §5);
3. where only documented float-sensitive points differ, decoded PSNR is within 0.05 dB (SPEC RQ3);
4. the encoder is callable from etherquic through the C ABI in `docs/INTEGRATION.md` §4.

## 2. Hard constraints

- **`reference/` is read-only.** Never edit, reformat or "fix" it. Patches for dumping go in `tools/vkref/` and are applied
  to a scratch copy on 192.168.0.24.
- **cs_5_0 only**: no SM 6 wave intrinsics, no `min16*` or 16-bit types, no `Store8`/`Store2` (there are none), typed UAV
  loads only for R32 formats, ≤ 32 KB groupshared, ≤ 1024 threads per group, ≤ 8 UAVs, no dependence on 64-wide
  wavefront lockstep. See SPEC §2-4.
- **Barrier rules B1-B6** (SPEC §4) apply to every shader; shaders build with `/WX`.
- **Output must not depend on atomic execution order** (SPEC B3, B6).
- **Machines**: build on HYTE only. Do not run anything on AN515 (192.168.0.58) unless your task is T7 and the user has
  said to go ahead. 192.168.0.24 is for the Vulkan reference and dumps (T2a, T6a).
- **Git**: commit locally with clear messages. **Never push** unless the user asks. Line endings are CRLF (the existing
  files are CRLF); keep them that way.
- **Base everything on the real sources and cite file:line.** If the spec and the reference disagree, the reference
  wins: fix the spec and add an entry to `docs/OPEN_QUESTIONS.md`. Never guess a constant.
- Spec-only phases (like the one that produced these docs) must not add HLSL or C++ port code.

## 3. Repo map

| Path | What |
|---|---|
| `AGENTS.md` | this file |
| `README.md` | short overview, how to build the prototype |
| `PLAN.md` | stage plan, effort, status |
| `docs/SPEC.md` | **normative** whole-pipeline spec: conventions, requirements, cs_5_0 catalogue, barrier rules, FS points |
| `docs/stages/00_input.md` .. `06_frame_and_host.md` | normative per-stage specs (I/O layouts, replacements, barriers, rounding, oracle) |
| `docs/VALIDATION.md` | dumps, comparison tool, test vectors, PSNR, timing |
| `docs/INTEGRATION.md` | how etherquic calls the encoder; C ABI; codec naming |
| `docs/OPEN_QUESTIONS.md` | OQ-1..OQ-21 |
| `prototype/` | forward-DWT feasibility prototype (`dwt.hlsl`, `dx11dwt.cpp` harness, `pwbench.cpp` Vulkan bench, `pyrowave.h`). Historical record: copy from it, do not rework it in place |
| `reference/pyrowave/` | read-only upstream source = the spec of record |
| `src/` | the port (empty until T0) |
| `tools/` | validation tooling (empty until T6) |

Planned layout once implementation starts:
```
src/CMakeLists.txt          (MSVC, static CRT)      src/include/pyrowave_d3d11.h   (C ABI, INTEGRATION §4)
src/shaders/dwt.hlsl  quant.hlsl  analyze.hlsl  analyze_finalize.hlsl  resolve.hlsl  block_packing.hlsl  common.hlsli
src/encoder.cpp  device.cpp  packetize.cpp  tables.cpp (rdo table, quant tables)  c_api.cpp
tools/vkref/  tools/golden/  tools/pwcompare.py  tools/genvectors.py  testdata/
```

## 4. Build environment (HYTE)

- Repo: `C:\Build\codecs\pyrowave-dx11`. Shell: PowerShell. Python: `C:\Users\Lyam\AppData\Local\Programs\Python\Python313\python.exe`.
- Open an **x64 VS Developer prompt** (`vcvars64.bat` or "x64 Native Tools Command Prompt"), which puts `cl`, `link` and
  the Windows SDK `fxc.exe` on PATH. CMake is available (`PLAN.md`).
- C++: `cl /nologo /O2 /EHsc /std:c++17 /W4 /WX /MT /permissive- /DUNICODE /D_UNICODE ... d3d11.lib dxgi.lib`.
  Debug builds use `/Od /Zi /MTd`. `d3dcompiler.lib` only in tools and tests; the DLL embeds precompiled bytecode.
- HLSL: `fxc /nologo /T cs_5_0 /E main /O3 /WX /Ges /Gis /Fh <out>.h /Vn g_<name>_cs <file>.hlsl`.
  - `/Gis` (IEEE strictness) and `/Ges` (strict mode) are required. `precise` goes where the GLSL has `precise`.
  - The runtime equivalents are `D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS | D3DCOMPILE_ENABLE_STRICTNESS
    | D3DCOMPILE_WARNINGS_ARE_ERRORS`. The prototype compiles at runtime with only `OPTIMIZATION_LEVEL3`
    (`prototype/dx11dwt.cpp:346-377`); the port must not.
  - Per-dispatch constants (`DCShift`, `RangeScale`, band indices) go in a cbuffer. No `#define` variants unless the stage
    doc says so.
- Prototype: `cd prototype && cl /O2 /EHsc /arch:AVX2 dx11dwt.cpp` (`prototype/README.md`).
- Linux reference (192.168.0.24, via `ssh -o BatchMode=yes lyam@192.168.0.24` from HYTE): build `reference/pyrowave` with CMake
  at its defaults (`PYROWAVE_PRECISION=1`, `R/CMakeLists.txt:39-47`) in a scratch copy.
- Moving binary files between machines from PowerShell: use `cmd /c "... > file"` or `scp`. PowerShell `>` re-encodes
  output and corrupts binaries.

## 5. Tasks

Each task names its spec, its dependencies, and acceptance criteria (test ids from the stage docs). A task is done
when every criterion passes on HYTE's GPU (or on the RTX for T2a/T6a). AN515 is T7 only.

| Id | Task | Spec | Depends on | Acceptance |
|---|---|---|---|---|
| T0 | Skeleton: `src/` CMake project, fxc build step with the flags above, embedded bytecode, `pyrowave_d3d11.h` with static asserts on every struct shared with `R/pyrowave.h`, device creation (hardware, FL 11_0, WARP rejected), cbuffer structs with `static_assert`ed offsets | INTEGRATION §4, stage 0 | none | builds with `/W4 /WX`; `pwd3d11_create_device` succeeds on HYTE and rejects WARP; T-IN-0 |
| T1 | Move the DWT from `prototype/` into `src/` (upload + 13 dispatches), keeping the band layout | stages 0, 1 | T0 | T-IN-1, T-DWT-1, T-DWT-2 (tolerance VALIDATION §6) |
| T2a | Tables and probes on the reference: dump the 60-entry `rdo_distortion_scale` table from the Linux build; T-Q-5 probe of the FS-Q1/FS-Q2 division results on the RTX and lavapipe; f16 rounding sweep (OQ-3) | stage 2 §2, §7 | none | table committed with its source command; OQ-1 and OQ-3 answered with data |
| T2b | Quant shader, one dispatch per band (42) | stage 2 | T1, T2a, T6b | T-Q-0..T-Q-4 against the golden model and the Vulkan dumps (M3) |
| T2c | Merge the quant dispatches (band table, one dispatch) | stage 2 §8 | T2b | identical buffers to T2b; timing not worse |
| T3 | Analyze (42 dispatches) + finalize, bug-compatible windowed scan | stage 3 | T2b | T-A-1..T-A-4 |
| T4 | Resolve | stage 4 | T3 | T-R-1..T-R-3 |
| T5a | Block packing shader | stage 5 | T4 | T-P-1..T-P-3 |
| T5b | Frame orchestration, readback, CPU packetize, sequence handling, C ABI `encode_cpu`/`compute_num_packets`/`packetize` | stage 6, INTEGRATION §4 | T5a | T-H-1..T-H-3; T-P-4 (end to end, decoded by the Vulkan decoder) |
| T5c | Merge the packing dispatches | stage 6 §2 | T5b | packets identical to T5b |
| T6a | `tools/vkref` patches (dump hooks, band injection), dumps for all vectors on the RTX | VALIDATION §3 | none | patches apply to a0d2e71; manifests for all vectors |
| T6b | CPU golden model of stages 2-6 (`tools/golden`, C++), fixed reduction order, both finalize variants | stages 2-6 | T2a | matches the Vulkan dumps exactly except at proven FS points |
| T6c | D3D11 debug build: dump hooks in the same format, `pwd3d11_debug_run_stage` injection | VALIDATION §3.4, §5 | T0 | round-trips a Vulkan dump through each stage |
| T6d | `tools/pwcompare.py`, `tools/genvectors.py` | VALIDATION §4, §7 | T6a | reports PASS / FS-ONLY / FAIL with locations |
| T7 | **AN515 run** (only on the user's go-ahead): functional M1/M2 on the HD 6670 and timing per VALIDATION §9 | VALIDATION | T5b, T6d | all vectors PASS or FS-ONLY; PSNR within 0.05 dB; timing table; OQ-14/17 answered |
| T8 | etherquic integration (separate repo; only on the user's go-ahead) | INTEGRATION §3 | T7, OQ-13 | probe announces `pyrowave=pyrowave-d3d11` on AN515; a player decodes; fallback to `pyrowave-cpu` works |

Order: T0 → T1 → (T2a ∥ T6a) → T6b → T2b → T3 → T4 → T5a → T5b → T6c/T6d → T2c, T5c → T7 → T8.

## 6. Coding conventions

- One HLSL file per GLSL shader, same names, same order of operations. Every non-obvious line carries a citation
  comment, e.g. `// wq:107 FS-Q1`.
- Use `precise` exactly where the GLSL does, and on every value that feeds an FS point.
- Reductions use the fixed butterfly order from SPEC §3. Never write a reduction whose order depends on atomics or on
  thread scheduling.
- No early `return` in a shader that has a barrier later. Use predicate flags (B1).
- Every groupshared staging array has a debug-build overflow counter written to a debug UAV (stage 2 §5.3, stage 5 §6).
- C++17, `Microsoft::WRL::ComPtr`, no exceptions across the C ABI, no `std::` types in the public header. Nothing printed
  to stdout/stderr from the DLL.
- Struct mirrors of `R/` (push constants, `BlockMeta`, `BlockStats`, `BitstreamPacket`, `RDOperation`) carry
  `static_assert(sizeof/offsetof)` and a citation of the original definition.
- Commit messages: `area: what changed` (for example `quant: groupshared cluster reductions (T2b)`), with the task id.

## 7. What not to touch

- `reference/` (read-only; see §2).
- `prototype/`: keep it as the measured record behind `prototype/README.md`. Copy code from it into `src/`; do not edit
  it in place.
- etherquic: no changes from this repo's tasks before T8.
- AN515: nothing before T7.
- Remote branches: do not push.
