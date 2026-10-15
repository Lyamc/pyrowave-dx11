"""Send a task plus repo files to the local Strata server (OpenAI-compatible, 127.0.0.1:8080).

Usage:
  python tools/strata_agent.py --task "..." [--file path ...] [--out docs/x.md] [--max-tokens N] [--effort low|medium|high|none]
The reply is printed and, with --out, written to that file (review before committing). The model never writes
files or runs commands itself; this is a read-files -> answer loop.
"""
import argparse, json, os, sys, time, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
URL = os.environ.get("STRATA_URL", "http://127.0.0.1:8080/v1/chat/completions")
SYSTEM = ("You are working in the pyrowave-dx11 repo: a Direct3D 11 (cs_5_0, FL11_0, no wave intrinsics) port of the "
          "PyroWave Vulkan wavelet encoder. reference/pyrowave is the read-only spec. The bitstream must be bit-exact "
          "with the Vulkan encoder. Be precise, cite file names and line-level details, do not invent APIs.")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--task", required=True)
    ap.add_argument("--file", action="append", default=[])
    ap.add_argument("--out")
    ap.add_argument("--max-tokens", type=int, default=4096)
    ap.add_argument("--effort", default="medium")
    a = ap.parse_args()
    parts = [PLAN] if (PLAN := open(os.path.join(ROOT, "PLAN.md"), encoding="utf-8").read()) else []
    for f in a.file:
        p = os.path.join(ROOT, f)
        parts.append(f"===== {f} =====\n" + open(p, encoding="utf-8", errors="replace").read())
    user = "\n\n".join(parts) + "\n\n===== TASK =====\n" + a.task
    body = {"model": "strata", "messages": [{"role": "system", "content": SYSTEM}, {"role": "user", "content": user}],
            "max_tokens": a.max_tokens, "reasoning_effort": a.effort}
    req = urllib.request.Request(URL, json.dumps(body).encode(), {"Content-Type": "application/json"})
    t = time.time()
    with urllib.request.urlopen(req, timeout=3600) as r:
        res = json.load(r)
    msg = res["choices"][0]["message"]["content"]
    sys.stderr.write(f"[strata] {time.time()-t:.1f}s usage={res.get('usage')}\n")
    print(msg)
    if a.out:
        with open(os.path.join(ROOT, a.out), "w", encoding="utf-8") as fh:
            fh.write(msg)

if __name__ == "__main__":
    main()
