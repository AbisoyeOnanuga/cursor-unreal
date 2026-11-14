// Talks to a running editor's MCP server over a real socket: checks that the security rules hold
// on the wire, that tools can be listed and called, and measures call latency.
//
//   CURSOR_UNREAL_MCP_TOKEN=<token> node Scripts/mcp-probe.mjs [--url http://127.0.0.1:18732/mcp] [--calls 30] [--json]
//                                                               [--label text] [--check-approval]
//
// Everything it calls by default is read-only. --check-approval additionally asks to run a dangerous
// tool to confirm it is refused; only use that against a headless editor (see the comment at the call).
//
// The token is read from the environment so it never appears on a command line. It is never printed.
// Scripts/Run-McpProbe.ps1 fetches it from Windows Credential Manager and runs this for you.

import http from "node:http";
import { performance } from "node:perf_hooks";

const args = process.argv.slice(2);
const option = (name, fallback) => {
  const index = args.indexOf(`--${name}`);
  return index >= 0 && index + 1 < args.length ? args[index + 1] : fallback;
};
const url = new URL(option("url", "http://127.0.0.1:18732/mcp"));
const callCount = Math.max(1, Number.parseInt(option("calls", "30"), 10) || 30);
const asJson = args.includes("--json");
const checkApproval = args.includes("--check-approval");
const label = option("label", "");
const token = process.env.CURSOR_UNREAL_MCP_TOKEN ?? "";

if (!token) {
  console.error("Set CURSOR_UNREAL_MCP_TOKEN, or run Scripts/Run-McpProbe.ps1 which reads it from Credential Manager.");
  process.exit(2);
}
if (!["127.0.0.1", "localhost", "[::1]"].includes(url.hostname)) {
  console.error("This probe only talks to the local machine.");
  process.exit(2);
}

// One connection, reused, as Cursor does.
const agent = new http.Agent({ keepAlive: true, maxSockets: 1 });

function send({ method = "POST", headers = {}, body } = {}) {
  return new Promise((resolve, reject) => {
    const payload = body === undefined ? undefined : Buffer.from(JSON.stringify(body));
    const request = http.request(
      {
        agent,
        host: url.hostname.replace(/^\[|\]$/g, ""),
        port: url.port,
        path: url.pathname,
        method,
        headers: {
          ...(payload ? { "Content-Type": "application/json", "Content-Length": payload.length } : {}),
          Accept: "application/json",
          ...headers,
        },
      },
      (response) => {
        const chunks = [];
        response.on("data", (chunk) => chunks.push(chunk));
        response.on("end", () => {
          const text = Buffer.concat(chunks).toString("utf8");
          let json;
          try {
            json = text ? JSON.parse(text) : undefined;
          } catch {
            json = undefined;
          }
          resolve({ status: response.statusCode, text, json });
        });
      },
    );
    request.setTimeout(30000, () => request.destroy(new Error("timed out")));
    request.on("error", reject);
    if (payload) request.write(payload);
    request.end();
  });
}

const auth = { Authorization: `Bearer ${token}` };
let nextId = 1;
const rpc = (method, params, headers = auth) =>
  send({ headers, body: { jsonrpc: "2.0", id: nextId++, method, ...(params ? { params } : {}) } });

const results = [];
function check(name, passed, detail = "") {
  results.push({ name, passed, detail });
  if (!asJson) console.log(`  ${passed ? "PASS" : "FAIL"}  ${name}${detail ? `  (${detail})` : ""}`);
}

const percentile = (sorted, p) => sorted[Math.min(sorted.length - 1, Math.floor((p / 100) * sorted.length))];
const round = (value) => Math.round(value * 100) / 100;

try {
  // --- Security, on the wire -----------------------------------------------------------------
  const ping = { jsonrpc: "2.0", id: 0, method: "ping" };
  let r = await send({ body: ping });
  check("no token is refused", r.status === 401, `HTTP ${r.status}`);

  r = await send({ body: ping, headers: { Authorization: `Bearer ${"0".repeat(token.length)}` } });
  check("wrong token is refused", r.status === 401, `HTTP ${r.status}`);

  r = await send({ body: ping, headers: { Authorization: `Bearer ${token.slice(0, -1)}` } });
  check("token missing its last character is refused", r.status === 401, `HTTP ${r.status}`);

  r = await send({ body: ping, headers: { ...auth, Host: `evil.example:${url.port}` } });
  check("foreign Host header is refused (DNS rebinding)", r.status === 403, `HTTP ${r.status}`);

  r = await send({ body: ping, headers: { ...auth, Origin: "https://evil.example" } });
  check("foreign Origin is refused (cross-site request)", r.status === 403, `HTTP ${r.status}`);

  r = await send({ method: "GET", headers: auth });
  check("GET is not allowed", r.status === 405, `HTTP ${r.status}`);

  r = await send({ method: "GET" });
  check("GET without a token reveals nothing", r.status === 401, `HTTP ${r.status}`);

  // --- Protocol ------------------------------------------------------------------------------
  // The refused requests above do not count as agent activity, so this is the first request the
  // editor treats as an agent working: it shows what a call costs after the editor has been idle.
  const coldStarted = performance.now();
  r = await rpc("initialize", {
    protocolVersion: "2025-06-18",
    capabilities: {},
    clientInfo: { name: "cursor-unreal-probe", version: "1" },
  });
  const firstCallAfterIdleMs = round(performance.now() - coldStarted);
  const serverInfo = r.json?.result?.serverInfo;
  check("initialize", r.status === 200 && serverInfo?.name === "cursor-unreal", `${serverInfo?.name} ${serverInfo?.version}, protocol ${r.json?.result?.protocolVersion}`);

  r = await send({ headers: auth, body: { jsonrpc: "2.0", method: "notifications/initialized" } });
  check("notification is accepted with no body", r.status === 202 && !r.text, `HTTP ${r.status}`);

  r = await rpc("tools/list");
  const tools = r.json?.result?.tools ?? [];
  const names = tools.map((tool) => tool.name);
  check("tools/list", r.status === 200 && names.includes("batch") && names.includes("actor_list"), `${tools.length} tools`);
  check("every tool has a description and an object schema",
    tools.length > 0 && tools.every((tool) => tool.description && tool.inputSchema?.type === "object"));
  check("file tools for the in-editor agent are not offered over MCP", !names.some((name) => name.startsWith("source_")));

  r = await rpc("tools/call", { name: "editor_get_context", arguments: {} });
  const context = r.json?.result?.structuredContent;
  check("tools/call editor_get_context", r.status === 200 && r.json?.result?.isError === false && !!context?.project,
    `project ${context?.project}, engine ${context?.engine_version}`);

  r = await rpc("tools/call", { name: "no_such_tool", arguments: {} });
  check("unknown tool comes back as a readable tool error", r.status === 200 && r.json?.result?.isError === true);

  // Opt-in, because of what it does to an editor with a person in front of it: the call asks to
  // run Python, so the editor shows an approval prompt and this probe waits on the answer. Use it
  // against a headless editor, where nobody can approve and the call must be refused outright.
  if (checkApproval) {
    r = await rpc("tools/call", { name: "python_exec", arguments: { code: "print('should not run')" } });
    const dangerous = r.json?.result;
    const ranAnyway = dangerous?.isError === false;
    check("dangerous tool does not run unapproved", r.status === 200 && !ranAnyway,
      ranAnyway ? "IT RAN" : (dangerous?.content?.[0]?.text ?? "").slice(0, 90));
  }

  // --- Latency -------------------------------------------------------------------------------
  // A cheap read-only call, so the numbers are transport and scheduling, not tool work.
  const roundTrips = [];
  const serverTimes = [];
  for (let i = 0; i < callCount; i++) {
    const started = performance.now();
    const reply = await rpc("tools/call", {
      name: "editor_log_tail",
      arguments: { lines: 1 },
      _meta: { "cursorunreal/sentAtMs": Date.now() },
    });
    roundTrips.push(performance.now() - started);
    const serverMs = reply.json?.result?._meta?.["cursorunreal/serverMs"];
    if (typeof serverMs === "number") serverTimes.push(serverMs);
    if (reply.status !== 200 || reply.json?.result?.isError !== false) {
      check("latency calls succeed", false, `call ${i} failed: HTTP ${reply.status}`);
      break;
    }
  }
  roundTrips.sort((a, b) => a - b);
  serverTimes.sort((a, b) => a - b);
  const latency = {
    label,
    firstCallAfterIdleMs,
    calls: roundTrips.length,
    roundTripMs: {
      min: round(roundTrips[0]),
      median: round(percentile(roundTrips, 50)),
      p95: round(percentile(roundTrips, 95)),
      max: round(roundTrips[roundTrips.length - 1]),
    },
    serverMs: serverTimes.length
      ? { median: round(percentile(serverTimes, 50)), p95: round(percentile(serverTimes, 95)) }
      : null,
  };

  const failed = results.filter((result) => !result.passed);
  if (asJson) {
    console.log(JSON.stringify({ url: url.href, checks: results, latency }, null, 2));
  } else {
    console.log(`\n  First call after idle: ${latency.firstCallAfterIdleMs} ms`);
    console.log(`  Latency over ${latency.calls} sequential calls${label ? ` [${label}]` : ""}:`);
    console.log(`    round trip  min ${latency.roundTripMs.min} ms   median ${latency.roundTripMs.median} ms   p95 ${latency.roundTripMs.p95} ms   max ${latency.roundTripMs.max} ms`);
    if (latency.serverMs) {
      console.log(`    in the tool median ${latency.serverMs.median} ms   p95 ${latency.serverMs.p95} ms   (the rest is transport and waiting for the editor's next tick)`);
    }
    console.log(`\n  ${results.length - failed.length} passed, ${failed.length} failed.`);
  }
  agent.destroy();
  process.exit(failed.length > 0 ? 1 : 0);
} catch (error) {
  agent.destroy();
  // Never include request headers in the message: they hold the token.
  console.error(`Could not reach the MCP server at ${url.href}: ${error.code ?? error.message}`);
  console.error("Is the editor running with the plugin enabled, and is 'Enable MCP Server' on in Project Settings > Plugins > Cursor Unreal?");
  process.exit(2);
}
