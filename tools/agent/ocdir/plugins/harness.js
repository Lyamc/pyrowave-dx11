// pyrowave-dx11 agent harness plugin (loaded via OPENCODE_CONFIG_DIR=tools/agent/ocdir).
// Keeps tool output small for the local model (256k context, slow prompt processing) and adds a second enforcement layer for the hard rules.
const READ_MAX_LINES = 200;
const OUT_MAX_CHARS = 12000;    // ~3.5k tokens per tool result
const FORBIDDEN_EDIT = /(^|[\\/])(reference|prototype)[\\/]/i;
const HARNESS_FILES = /(^|[\\/])(tools[\\/]run_agent\.py|tools[\\/]agent[\\/]|logs[\\/]|\.git[\\/])/i;
const BAD_BASH = [
  [/192\.168\.0\.58|\ban515\b/i, "AN515 is off limits"],
  [/\bgit\s+push\b|\bgit\s+remote\s+add\b/i, "never push / add remotes"],
  [/llama|dflash|:8080|\b8080\b/i, "never touch llama-server or port 8080"],
  [/\b(taskkill|stop-process|kill)\b/i, "do not kill processes (the harness handles timeouts)"],
  [/\b(reg(\.exe)?\s+(add|delete|import)|setx|netsh|bcdedit|schtasks|register-scheduledtask|set-service|stop-service|new-service|sc(\.exe)?\s+(config|stop|delete|create)|set-executionpolicy|shutdown|restart-computer|winget|choco)\b/i, "no system settings changes"],
  [/\bgit\s+(reset\s+--hard|rebase|filter-branch|clean|commit\s+--amend|config)\b/i, "no history rewrites / git config"],
  [/(reference|prototype)[\\/][^\s"']*\s*(>|\|\s*(set-content|out-file|add-content))|\b(remove-item|rm|del|move-item|mv|rename-item|ren)\b[^|;]*\s(\.[\\/])?(reference|prototype)([\\/]|\s|$)/i, "reference/ and prototype/ are read-only"],
];
const SSH_OK = /^\s*(ssh|scp)\b/i;

function clip(s) {
  if (typeof s !== "string" || s.length <= OUT_MAX_CHARS) return s;
  const head = s.slice(0, 2000), tail = s.slice(-(OUT_MAX_CHARS - 2200));
  return head + `\n\n...[harness: ${s.length - head.length - tail.length} chars cut; narrow the command or read with offset/limit]...\n\n` + tail;
}

export const PwHarness = async () => ({
  "tool.execute.before": async (input, output) => {
    const a = output.args || {};
    if (input.tool === "read") {
      if (!a.limit || a.limit > READ_MAX_LINES) a.limit = READ_MAX_LINES;
    }
    if (["edit", "write", "patch", "multiedit", "apply_patch"].includes(input.tool)) {
      const paths = [a.filePath, a.path, a.patchText, a.patch].filter(Boolean).join("\n");
      if (FORBIDDEN_EDIT.test(paths)) throw new Error("Blocked by harness: reference/ and prototype/ are read-only. Copy into src/ instead.");
      if (HARNESS_FILES.test(paths)) throw new Error("Blocked by harness: the agent harness, logs/ and .git/ are off limits.");
    }
    if (input.tool === "bash") {
      const c = String(a.command || "");
      for (const [re, why] of BAD_BASH) if (re.test(c)) throw new Error("Blocked by harness: " + why + ". Command: " + c.slice(0, 200));
      if (/\b(ssh|scp)\b/i.test(c) && !/lyam@192\.168\.0\.24/.test(c)) throw new Error("Blocked by harness: ssh/scp only to lyam@192.168.0.24.");
    }
  },
  "tool.execute.after": async (input, output) => {
    if (output && typeof output.output === "string") output.output = clip(output.output);
  },
  "experimental.session.compacting": async (input, output) => {
    output.context.push(
      "Harness note: keep the summary short (<= 25 lines). Include the task id, files created so far, the last build/test " +
      "result, and the next concrete step. After compaction do NOT re-read whole files: read the handoff file " +
      "docs/progress/<task>.md and only the line ranges you need (<= 200 lines per read).");
  },
});
