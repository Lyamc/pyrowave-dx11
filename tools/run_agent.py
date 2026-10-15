#!/usr/bin/env python3
"""Autonomous task driver for pyrowave-dx11.

Runs opencode (local llama-server model) over the AGENTS.md tasks in dependency order.
  * one fresh opencode session per "session" (keeps the context small); handoff via docs/progress/<task>.md
  * after every session: enforce hard rules, then run tests/accept_<task>.cmd independently
  * task done = accept script exit 0 + a "PASS <criterion>" line for every criterion + work committed
  * bounded retries; a failed task is recorded in logs/STATUS.md and the driver moves on to independent tasks
Stop:  create logs/STOP           (kills the running session at the next poll, ~10 s, and exits)
       create logs/STOP_AFTER     (lets the current session + acceptance run finish, then exits)
Resume: just start it again; state lives in logs/state.json.
"""
import json, os, re, subprocess, sys, time, threading, datetime, urllib.request, traceback
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
LOGS = REPO / "logs"
AGENT_DIR = REPO / "tools" / "agent"
TASKS_FILE = AGENT_DIR / "tasks.json"
STATE_FILE = LOGS / "state.json"
STATUS_FILE = LOGS / "STATUS.md"
STOP_FILE = LOGS / "STOP"
STOP_AFTER_FILE = LOGS / "STOP_AFTER"
PID_FILE = LOGS / "driver.pid"
OPENCODE = Path(os.environ.get("APPDATA", r"C:\Users\Lyam\AppData\Roaming")) / "npm" / "node_modules" / "opencode-ai" / "bin" / "opencode.exe"
LLAMA_URL = "http://127.0.0.1:8080"
LLAMA_EXE = r"C:\AI\dflash-llamacpp\llama-server.exe"
FORBIDDEN = ["reference", "prototype"]

MAX_SESSIONS = int(os.environ.get("PW_MAX_SESSIONS", "8"))          # sessions per task in total
MAX_NO_PROGRESS = int(os.environ.get("PW_MAX_NO_PROGRESS", "3"))    # consecutive sessions without any repo change
SESSION_TIMEOUT = int(os.environ.get("PW_SESSION_TIMEOUT", str(150 * 60)))
IDLE_TIMEOUT = int(os.environ.get("PW_IDLE_TIMEOUT", str(30 * 60)))
ACCEPT_TIMEOUT = int(os.environ.get("PW_ACCEPT_TIMEOUT", str(30 * 60)))

RUN_TS = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
LOG_FILE = LOGS / f"agent-{RUN_TS}.log"
RUN_DIR = LOGS / f"run-{RUN_TS}"

_log_lock = threading.Lock()


def now():
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def log(msg):
    line = f"[{now()}] {msg}"
    with _log_lock:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")


def sh(args, timeout=600, check=False, cwd=REPO):
    r = subprocess.run(args, cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace",
                       timeout=timeout, stdin=subprocess.DEVNULL)
    if check and r.returncode != 0:
        raise RuntimeError(f"{args} failed: {r.stderr.strip()[:500]}")
    return r


def git(*a, **kw):
    return sh(["git", *a], **kw)


def head():
    return git("rev-parse", "--short", "HEAD").stdout.strip()


def dirty_paths():
    out = git("status", "--porcelain", "--untracked-files=all").stdout.splitlines()
    return [l[3:].strip().strip('"') for l in out if l.strip()]


# ---------------------------------------------------------------- state / status
def load_state(tasks):
    if STATE_FILE.exists():
        st = json.loads(STATE_FILE.read_text(encoding="utf-8"))
    else:
        st = {"tasks": {}, "history": []}
    for tid in tasks["order"]:
        st["tasks"].setdefault(tid, {"status": "pending", "sessions": 0, "no_progress": 0, "last_commit": "",
                                     "last_result": "", "notes": ""})
    return st


def save_state(st):
    tmp = STATE_FILE.with_suffix(".tmp")
    tmp.write_text(json.dumps(st, indent=1), encoding="utf-8")
    os.replace(tmp, STATE_FILE)


STATS = {"sessions": 0, "steps": 0, "out_tokens": 0, "in_tokens": 0, "gen_seconds": 0.0}


def write_status(st, tasks, current=None, phase="", last_accept_tail=""):
    lines = ["# Agent status", "",
             f"Updated: {now()} (HYTE local time)  ",
             f"Driver PID: {os.getpid()} (`logs/driver.pid`); log: `logs/{LOG_FILE.name}`; raw sessions: `logs/{RUN_DIR.name}/`  ",
             "Stop: create `logs/STOP` (immediate) or `logs/STOP_AFTER` (after the current session). Resume: `tools\\run_agent.ps1`.", "",
             f"**Current:** {current or '-'} {phase}  ",
             f"**HEAD:** {head()} {git('log', '-1', '--format=%s').stdout.strip()}  ",
             f"**Model throughput (this run):** {STATS['sessions']} sessions, {STATS['steps']} LLM steps, "
             f"{STATS['out_tokens']} output tokens"
             + (f", ~{STATS['out_tokens'] / STATS['gen_seconds']:.1f} output tok/s incl. prompt processing" if STATS['gen_seconds'] > 0 else ""),
             "", "| Task | Status | Sessions | No-progress | Last commit | Last result |", "|---|---|---|---|---|---|"]
    for tid in tasks["order"]:
        t = st["tasks"][tid]
        res = (t.get("last_result") or "").replace("|", "/").replace("\n", " ")[:120]
        lines.append(f"| {tid} | {t['status']} | {t['sessions']} | {t['no_progress']} | {t.get('last_commit', '')} | {res} |")
    for tid, why in tasks.get("excluded", {}).items():
        lines.append(f"| {tid} | excluded | 0 | 0 | | {why} |")
    viol = st.get("violations", [])
    if viol:
        lines += ["", "## Rule violations caught", ""] + [f"- {v}" for v in viol[-20:]]
    if last_accept_tail:
        lines += ["", "## Last acceptance output (tail)", "", "```", last_accept_tail.strip()[-3000:], "```"]
    STATUS_FILE.write_text("\n".join(lines) + "\n", encoding="utf-8")


# ---------------------------------------------------------------- llama-server
def llama_pids():
    r = sh(["powershell", "-NoProfile", "-Command",
            "Get-CimInstance Win32_Process -Filter \"name='llama-server.exe'\" | % { \"$($_.ProcessId)|$($_.ExecutablePath)\" }"], timeout=60)
    pids = []
    for l in r.stdout.splitlines():
        if "|" in l:
            pid, path = l.split("|", 1)
            if path.strip().lower() == LLAMA_EXE.lower():
                pids.append(int(pid))
    return sorted(pids)


def llama_healthy():
    try:
        with urllib.request.urlopen(LLAMA_URL + "/health", timeout=10) as r:
            return r.status == 200
    except Exception:
        return False


def wait_for_llama():
    t0 = time.time()
    while not llama_healthy():
        if STOP_FILE.exists():
            return False
        if time.time() - t0 > 3600:
            log("llama-server not healthy for 60 min; giving up this round (driver never restarts it)")
            return False
        log("llama-server not healthy, waiting 60 s (the driver never starts/stops it)")
        time.sleep(60)
    return True


# ---------------------------------------------------------------- enforcement
def ensure_prepush_hook():
    hook = REPO / ".git" / "hooks" / "pre-push"
    body = "#!/bin/sh\necho 'pre-push: pushing is disabled in pyrowave-dx11 (agent harness). Ask Lyam first.' >&2\nexit 1\n"
    if not hook.exists() or hook.read_text(errors="replace") != body:
        hook.write_text(body, newline="\n")


def enforce_rules(st, start_commit, jsonl_path, llama_before):
    msgs = []
    # 1. forbidden paths: revert to the session's start commit
    changed = git("diff", "--name-only", start_commit, "HEAD", "--", *FORBIDDEN).stdout.split()
    changed += [p for p in dirty_paths() if p.split("/")[0] in FORBIDDEN]
    if changed:
        git("checkout", start_commit, "--", *FORBIDDEN)
        git("clean", "-fdq", "--", *FORBIDDEN)
        if git("status", "--porcelain", "--", *FORBIDDEN).stdout.strip():
            git("add", "-A", "--", *FORBIDDEN)
            git("commit", "-q", "-m", "harness: revert forbidden edits to reference/ or prototype/", "--", *FORBIDDEN)
        msgs.append(f"edited forbidden paths {sorted(set(changed))[:10]} -> reverted to {start_commit}")
    # 2. remotes / push
    if git("remote").stdout.strip():
        msgs.append(f"git remote present: {git('remote', '-v').stdout.strip()[:200]} (not removed; check it)")
    ensure_prepush_hook()
    # 3. forbidden hosts / commands seen in the transcript
    try:
        txt = jsonl_path.read_text(encoding="utf-8", errors="replace")
        for pat, what in [(r"192\.168\.0\.58", "AN515 address"), (r"git push", "git push"),
                          (r"pwd3d11-ref", None)]:
            if what and re.search(pat, txt):
                msgs.append(f"transcript mentions {what} (check {jsonl_path.name})")
    except Exception:
        pass
    # 4. llama-server still the same process
    after = llama_pids()
    if llama_before and after != llama_before:
        msgs.append(f"llama-server PIDs changed {llama_before} -> {after}")
    for m in msgs:
        log("RULE: " + m)
        st.setdefault("violations", []).append(f"{now()} {m}")
    return msgs


# ---------------------------------------------------------------- prompt
HARD_RULES = """HARD RULES (the harness enforces them; violations are reverted and logged):
1. Never `git push`, never add remotes, never rewrite history (no amend/rebase/reset --hard). Commit locally only.
2. Never modify anything under reference/ or prototype/ (read-only). Copy code out of prototype/ into src/ instead.
3. Never touch AN515 (192.168.0.58): no ssh, ping, scp or anything else to it, directly or via another host.
4. Never stop, restart or reconfigure llama-server (your own LLM backend on 127.0.0.1:8080). Do not touch port 8080.
5. No system settings changes: no registry, services, scheduled tasks, environment variables (setx), firewall, drivers, installers, admin rights.
6. Stay inside C:\\Build\\codecs\\pyrowave-dx11 plus %TEMP% (C:\\Users\\Lyam\\AppData\\Local\\Temp). Use build/ for build output.
7. Remote: only `ssh -o BatchMode=yes lyam@192.168.0.24 ...` / scp to it, and only when this task needs the Vulkan reference.
   Everything you create or modify there lives under ~/scratch/pwd3d11-ref. Read-only everywhere else (never modify ~/Build/etherquic).
8. Nobody will answer questions. When something is undecided, pick the documented default, write the assumption into
   docs/OPEN_QUESTIONS.md, and continue. User decisions so far: bit-exact reference device = RTX 2060 SUPER on 192.168.0.24
   (confirmed: `nvidia-smi` there reports it, which also answers OQ-19); keep the existing `pyrowave` codec row with the encoder
   reported as `pyrowave-d3d11` (OQ-13 = A); input YUV420P only (OQ-20); all other OQs: documented default."""

WORKFLOW = """HOW TO WORK (your context window is 256k tokens, but prompt processing is slow (~300 tok/s), so stay frugal):
- AGENTS.md is already in your instructions. Do not re-read it. Read ONE file at a time, in pieces: grep first, then read
  <=200 lines (the harness caps reads at 200 lines and any tool output at ~12000 chars). Never read reference/pyrowave/pyrowave.h
  whole: grep for the struct/enum you need, then read just those lines.
- Keep command output short: append `2>&1 | Select-Object -Last 40` to builds and tests. Your shell is Windows PowerShell 5.1.
- Build tools: prefix MSVC/fxc/cmake/ninja commands with `tools\\devenv.cmd`, e.g.
    tools\\devenv.cmd cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    tools\\devenv.cmd cmake --build build
  (devenv.cmd loads vcvars64 and forces cl as the compiler; inside .cmd scripts use `call tools\\devenv.cmd` once, then plain commands.)
  Python: C:\\Users\\Lyam\\AppData\\Local\\Programs\\Python\\Python313\\python.exe (not on PATH).
- Work in small steps: write a file, build, fix errors, repeat. Commit each working step:
    git add <paths> ; git commit -m "area: what changed (TASK)"
- Keep a short handoff file docs/progress/TASK.md (<= 40 lines): what is done, what is next, known problems. Update it
  and commit it before you stop. The next session starts from it with a fresh context.
- Acceptance: create tests\\accept_TASK.cmd (run from the repo root, no arguments). It must build what it needs (via
  `call tools\\devenv.cmd`), run the REAL tests, print exactly one line `PASS <criterion>` or `FAIL <criterion>` per
  criterion listed below, and exit 0 only if all pass. The harness runs it on its own after you stop; the task only counts
  when it passes. Never fake a PASS: a test that cannot be done yet must print FAIL.
- When the acceptance script passes and everything is committed, say DONE and stop. If you get stuck, write the blocker
  into docs/progress/TASK.md, commit, and stop."""


def build_prompt(tid, t, tstate, tasks, last_accept_tail):
    deps = ", ".join(t["deps"]) or "none"
    reads = "\n".join(f"  - {r}" for r in t["read"])
    crit = " ".join(t["criteria"])
    prog = REPO / "docs" / "progress" / f"{tid}.md"
    parts = [f"You are an autonomous coding agent in C:\\Build\\codecs\\pyrowave-dx11 (git repo, branch as checked out).",
             "", f"TASK {tid}: {t['title']}", f"Depends on (already done): {deps}",
             f"Acceptance (AGENTS.md A5): {t['acceptance']}",
             f"Criteria the accept script must print PASS for: {crit}"]
    if t.get("notes"):
        parts.append(f"Notes: {t['notes']}")
    parts += ["Relevant specs (read only what you need):", reads, "", HARD_RULES.replace("TASK", tid), "",
              WORKFLOW.replace("TASK", tid), ""]
    session_no = tstate["sessions"] + 1
    parts.append(f"This is session {session_no} of at most {MAX_SESSIONS} for {tid}.")
    if prog.exists():
        txt = prog.read_text(encoding="utf-8", errors="replace").splitlines()[:50]
        parts += [f"Handoff notes from the previous session (docs/progress/{tid}.md):", *txt, ""]
    log_ = git("log", "--oneline", "-8").stdout.strip()
    parts += ["Recent commits:", log_]
    d = dirty_paths()
    if d:
        parts += ["Uncommitted changes left by the previous session: " + ", ".join(d[:20])]
    if last_accept_tail:
        tail = "\n".join(last_accept_tail.strip().splitlines()[-30:])[-2500:]
        parts += ["", f"Last harness run of tests\\accept_{tid}.cmd (tail):", tail]
    parts += ["", "Start now. Prefer acting over planning."]
    return "\n".join(parts)


# ---------------------------------------------------------------- one opencode session
def summarize_event(e, fsum):
    t = e.get("type")
    p = e.get("part", {}) or {}
    if t == "tool_use":
        stt = p.get("state", {}) or {}
        inp = json.dumps(stt.get("input"), ensure_ascii=False)
        out = stt.get("output") or stt.get("error") or ""
        meta = stt.get("metadata") or {}
        ex = meta.get("exit")
        return f"  TOOL {p.get('tool')} {inp[:400]}" + (f" exit={ex}" if ex is not None else "") + f"\n      -> {str(out)[:600].replace(chr(10), chr(10) + '         ')}"
    if t == "text":
        return "  TEXT " + (p.get("text") or "")[:2000]
    if t == "step_finish":
        tok = p.get("tokens") or {}
        return f"  STEP done reason={p.get('reason')} in={tok.get('input')} cache_read={(tok.get('cache') or {}).get('read')} out={tok.get('output')} reasoning={tok.get('reasoning')}"
    if t == "error":
        return "  ERROR " + json.dumps(e)[:1500]
    return None


def kill_tree(pid):
    sh(["taskkill", "/T", "/F", "/PID", str(pid)], timeout=60)


def run_session(tid, prompt, session_tag):
    RUN_DIR.mkdir(parents=True, exist_ok=True)
    (RUN_DIR / f"{session_tag}.prompt.md").write_text(prompt, encoding="utf-8")
    jsonl = RUN_DIR / f"{session_tag}.jsonl"
    errf = open(RUN_DIR / f"{session_tag}.stderr.txt", "w", encoding="utf-8", errors="replace")
    env = dict(os.environ)
    env["OPENCODE_CONFIG"] = str(AGENT_DIR / "opencode.json")
    env["OPENCODE_CONFIG_DIR"] = str(AGENT_DIR / "ocdir")
    env["OPENCODE_DISABLE_AUTOUPDATE"] = "1"
    cmd = [str(OPENCODE), "run", "--format", "json", "--title", f"{tid} {session_tag}", prompt]
    log(f"=== session {session_tag} start (prompt {len(prompt)} chars)")
    flags = 0x08000000  # CREATE_NO_WINDOW
    proc = subprocess.Popen(cmd, cwd=REPO, env=env, stdout=subprocess.PIPE, stderr=errf, stdin=subprocess.DEVNULL,
                            creationflags=flags)
    last_activity = [time.time()]
    info = {"steps": 0, "out": 0, "inp": 0, "text": "", "session_id": "", "errors": 0, "tools": 0}
    step_t0 = [None]

    def reader():
        with open(jsonl, "a", encoding="utf-8", newline="") as jf:
            for raw in proc.stdout:
                line = raw.decode("utf-8", errors="replace").rstrip("\r\n") + "\n"
                jf.write(line)
                jf.flush()
                last_activity[0] = time.time()
                try:
                    e = json.loads(line)
                except Exception:
                    log("  RAW " + line.strip()[:500])
                    continue
                info["session_id"] = e.get("sessionID", info["session_id"])
                typ = e.get("type")
                if typ == "step_start":
                    step_t0[0] = time.time()
                if typ == "step_finish":
                    tok = (e.get("part") or {}).get("tokens") or {}
                    info["steps"] += 1
                    info["out"] += (tok.get("output") or 0) + (tok.get("reasoning") or 0)
                    info["inp"] += tok.get("input") or 0
                    STATS["steps"] += 1
                    STATS["out_tokens"] += (tok.get("output") or 0) + (tok.get("reasoning") or 0)
                    STATS["in_tokens"] += tok.get("input") or 0
                    if step_t0[0]:
                        STATS["gen_seconds"] += time.time() - step_t0[0]
                if typ == "tool_use":
                    info["tools"] += 1
                if typ == "text":
                    info["text"] = (e.get("part") or {}).get("text") or ""
                if typ == "error":
                    info["errors"] += 1
                s = summarize_event(e, None)
                if s:
                    log(s)

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    t0 = time.time()
    reason = "exited"
    while proc.poll() is None:
        time.sleep(10)
        if STOP_FILE.exists():
            reason = "STOP file"
            kill_tree(proc.pid)
            break
        if time.time() - t0 > SESSION_TIMEOUT:
            reason = f"session timeout {SESSION_TIMEOUT}s"
            kill_tree(proc.pid)
            break
        if time.time() - last_activity[0] > IDLE_TIMEOUT:
            reason = f"idle timeout {IDLE_TIMEOUT}s"
            kill_tree(proc.pid)
            break
    try:
        proc.wait(timeout=60)
    except Exception:
        pass
    th.join(timeout=30)
    errf.close()
    STATS["sessions"] += 1
    dur = time.time() - t0
    log(f"=== session {session_tag} end: {reason}, rc={proc.returncode}, {dur / 60:.1f} min, steps={info['steps']}, "
        f"tools={info['tools']}, out_tokens={info['out']}, errors={info['errors']}, session={info['session_id']}")
    info["reason"] = reason
    info["minutes"] = dur / 60
    return info, jsonl


# ---------------------------------------------------------------- acceptance
def run_accept(tid, t):
    script = REPO / "tests" / f"accept_{tid}.cmd"
    if not script.exists():
        return False, f"tests\\accept_{tid}.cmd does not exist yet"
    log(f"--- acceptance: tests\\accept_{tid}.cmd")
    try:
        r = subprocess.run(["cmd", "/c", str(script)], cwd=REPO, capture_output=True, text=True, encoding="utf-8",
                           errors="replace", timeout=ACCEPT_TIMEOUT, stdin=subprocess.DEVNULL, creationflags=0x08000000)
        out = (r.stdout or "") + ("\n[stderr]\n" + r.stderr if r.stderr.strip() else "")
        rc = r.returncode
    except subprocess.TimeoutExpired as ex:
        out = f"TIMEOUT after {ACCEPT_TIMEOUT}s\n" + str(ex.stdout or "")[-2000:]
        rc = -1
    passed = set(re.findall(r"^\s*PASS\s+(\S+)", out, re.M))
    failed = set(re.findall(r"^\s*FAIL\s+(\S+)", out, re.M))
    missing = [c for c in t["criteria"] if c not in passed]
    ok = rc == 0 and not missing and not failed
    summary = f"rc={rc} pass={sorted(passed)} fail={sorted(failed)} missing={missing}"
    log(f"--- acceptance result: {'OK' if ok else 'NOT OK'} {summary}")
    for l in out.strip().splitlines()[-60:]:
        log("    | " + l)
    return ok, f"{summary}\n{out[-4000:]}"


# ---------------------------------------------------------------- main loop
def pick_next(st, tasks):
    for tid in tasks["order"]:
        ts = st["tasks"][tid]
        if ts["status"] in ("done", "failed"):
            continue
        deps = tasks["tasks"][tid]["deps"]
        if all(st["tasks"].get(d, {}).get("status") == "done" for d in deps):
            return tid
    return None


def main():
    LOGS.mkdir(exist_ok=True)
    PID_FILE.write_text(str(os.getpid()))
    for f in (STOP_FILE, STOP_AFTER_FILE):
        if f.exists():
            log(f"removing stale {f.name} at startup")
            f.unlink()
    tasks = json.loads(TASKS_FILE.read_text(encoding="utf-8"))
    st = load_state(tasks)
    log(f"driver start pid={os.getpid()} repo={REPO} HEAD={head()} opencode={OPENCODE}")
    ensure_prepush_hook()
    llama_ref = llama_pids()
    log(f"llama-server pids: {llama_ref}")
    accept_tail = st.get("last_accept_tail", "")
    while True:
        if STOP_FILE.exists() or STOP_AFTER_FILE.exists():
            log("stop file present, exiting")
            break
        tid = pick_next(st, tasks)
        if not tid:
            log("no runnable task left (all done, failed, or blocked by failed deps)")
            for x in tasks["order"]:
                if st["tasks"][x]["status"] == "pending":
                    st["tasks"][x]["status"] = "blocked"
                    st["tasks"][x]["last_result"] = "blocked: a dependency failed"
            save_state(st)
            write_status(st, tasks, None, "finished", accept_tail)
            break
        t = tasks["tasks"][tid]
        ts = st["tasks"][tid]
        ts["status"] = "running"
        st["current"] = tid
        save_state(st)
        if not wait_for_llama():
            write_status(st, tasks, tid, "(waiting for llama-server)", accept_tail)
            if STOP_FILE.exists():
                continue
            time.sleep(300)
            continue
        llama_before = llama_pids()
        start_commit = git("rev-parse", "HEAD").stdout.strip()
        start_dirty = sorted(dirty_paths())
        tag = f"{tid}-s{ts['sessions'] + 1}"
        prompt = build_prompt(tid, t, ts, tasks, ts.get("accept_tail", ""))
        write_status(st, tasks, tid, f"session {ts['sessions'] + 1}/{MAX_SESSIONS} running", accept_tail)
        try:
            info, jsonl = run_session(tid, prompt, tag)
        except Exception:
            log("session crashed:\n" + traceback.format_exc())
            info, jsonl = {"reason": "driver error", "steps": 0}, RUN_DIR / f"{tag}.jsonl"
        ts["sessions"] += 1
        enforce_rules(st, start_commit, jsonl, llama_before)
        if STOP_FILE.exists():
            ts["status"] = "pending"
            ts["last_result"] = f"interrupted by STOP in session {ts['sessions']}"
            save_state(st)
            write_status(st, tasks, tid, "stopped by logs/STOP", accept_tail)
            log("STOP: exiting after killing the session")
            break
        progressed = git("rev-parse", "HEAD").stdout.strip() != start_commit or sorted(dirty_paths()) != start_dirty
        ok, accept_out = run_accept(tid, t)
        accept_tail = accept_out
        ts["accept_tail"] = accept_out[-3000:]
        st["last_accept_tail"] = accept_out[-3000:]
        if ok:
            if dirty_paths():
                git("add", "-A")
                git("commit", "-q", "-m", f"{tid}: commit remaining work after passing acceptance (harness)")
            ts["status"] = "done"
            ts["last_commit"] = head()
            ts["last_result"] = "PASS " + accept_out.splitlines()[0]
            log(f"### {tid} DONE at {head()}")
        else:
            ts["no_progress"] = 0 if progressed else ts["no_progress"] + 1
            ts["last_commit"] = head()
            ts["last_result"] = f"session {ts['sessions']} ({info.get('reason')}): " + accept_out.splitlines()[0]
            if ts["sessions"] >= MAX_SESSIONS or ts["no_progress"] >= MAX_NO_PROGRESS:
                ts["status"] = "failed"
                ts["last_result"] = (f"FAILED after {ts['sessions']} sessions "
                                     f"({ts['no_progress']} without progress): " + accept_out.splitlines()[0])
                log(f"### {tid} FAILED, moving on to independent tasks")
            else:
                ts["status"] = "pending"
        st["history"].append({"t": now(), "task": tid, "session": tag, "ok": ok, "reason": info.get("reason"),
                              "steps": info.get("steps"), "minutes": round(info.get("minutes", 0), 1), "head": head()})
        save_state(st)
        write_status(st, tasks, tid, "between sessions", accept_tail)
        if STOP_AFTER_FILE.exists():
            log("STOP_AFTER present: exiting after the session")
            break
    st["current"] = None
    save_state(st)
    write_status(st, tasks, None, "driver exited", accept_tail)
    log("driver exit")


if __name__ == "__main__":
    try:
        main()
    except Exception:
        LOGS.mkdir(exist_ok=True)
        log("FATAL:\n" + traceback.format_exc())
        raise
    finally:
        try:
            if PID_FILE.exists() and PID_FILE.read_text().strip() == str(os.getpid()):
                PID_FILE.unlink()
        except Exception:
            pass
