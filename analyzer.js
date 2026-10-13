// Rule-based deploy-log analyzer for the grokdesk demo.
// SIMULATED analysis — no LLM and no external API.
// Parses log lines, matches known failure patterns, extracts entities, and composes a short summary.
(function (root) {
  const RULES = [
    { id: "migration_lock_timeout", weight: 10,
      re: /migration\s+(\d{4})\w*\s+failed:.*lock timeout.*table\s+"?(\w+)"?/i,
      extract: m => ({ migration: m[1], table: m[2] }) },
    { id: "lock_wait", weight: 3, re: /waiting for (\w+Lock) on relation "(\w+)"(?:.*\((?:held by )?(.+?)\))?/i,
      extract: m => ({ lockType: m[1], table: m[2], holder: m[3] }) },
    { id: "crashloop", weight: 5, re: /CrashLoopBackOff/i, extract: () => ({}) },
    { id: "readiness", weight: 2, re: /Readiness probe failed.*?statuscode:\s*(\d+)/i, extract: m => ({ status: m[1] }) },
    { id: "oom", weight: 8, re: /OOMKilled/i, extract: () => ({}) },
    { id: "image_pull", weight: 8, re: /ErrImagePull|ImagePullBackOff/i, extract: () => ({}) },
    { id: "progress_deadline", weight: 1, re: /exceeded its progress deadline/i, extract: () => ({}) },
    { id: "version", weight: 0, re: /image updated -> (v[\d.]+)(?: \(previous: (v[\d.]+)\))?/i,
      extract: m => ({ newVersion: m[1], prevVersion: m[2] }) }
  ];

  function analyze(lines) {
    const findings = [], facts = {};
    lines.forEach((line, idx) => {
      for (const r of RULES) {
        const m = line.match(r.re);
        if (m) {
          const data = r.extract(m);
          findings.push({ rule: r.id, weight: r.weight, line: idx, data });
          for (const k in data) if (data[k] && !(k in facts)) facts[k] = data[k];
        }
      }
    });
    const has = id => findings.some(f => f.rule === id);
    const pods = new Set(lines.filter(l => /CrashLoopBackOff/.test(l)).map(l => (l.match(/pod\/([\w-]+)/) || [])[1]).filter(Boolean));
    let cause, effect = "", suggest;
    if (has("migration_lock_timeout")) {
      cause = `DB migration ${facts.migration} timed out — lock on ${facts.table} table` +
              (facts.holder ? ` (held by ${facts.holder.replace(/^pid \d+,\s*/, "")})` : "") + ".";
    } else if (has("oom")) cause = "Container OOMKilled — memory limit too low.";
    else if (has("image_pull")) cause = "Image pull failed — bad tag or registry auth.";
    else if (has("readiness")) cause = `Readiness probe failing (HTTP ${facts.status}).`;
    else cause = "No known failure pattern found.";
    if (has("crashloop")) effect = `Pods crash-looping (${pods.size || "?"}).`;
    suggest = facts.prevVersion ? `Rollback to ${facts.prevVersion}` : "Inspect pod logs";
    const top = findings.slice().sort((a, b) => b.weight - a.weight)[0];
    return { cause, effect, suggest, facts, findings, keyLine: top ? top.line : -1,
             summary: `Grok Bot: ${cause} ${effect} Suggest: ${suggest}.`.replace(/\s+/g, " ") };
  }
  root.DeployAnalyzer = { analyze, RULES };
  if (typeof module !== "undefined") module.exports = root.DeployAnalyzer;
})(typeof window !== "undefined" ? window : globalThis);
