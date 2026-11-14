import assert from "node:assert/strict";
import { test } from "node:test";

import { createBridge } from "../lib/bridge-core.mjs";
import { PROTOCOL_VERSION } from "../lib/protocol.mjs";
import * as fakeSdk from "./helpers/fake-sdk.mjs";

function makeBridge() {
  fakeSdk.resetState();
  const out = [];
  const logs = [];
  let clock = 1_000;
  const bridge = createBridge({
    loadSdk: async () => ({ sdk: fakeSdk, version: "stand-in" }),
    send: (message) => out.push(message),
    log: (level, text) => logs.push({ level, text }),
    now: () => clock++,
  });
  return { bridge, out, logs };
}

async function flush() {
  await new Promise((resolve) => setImmediate(resolve));
}

async function init(bridge, extra = {}) {
  await bridge.handle({
    type: "init",
    apiKey: "cursor_test_key",
    cwd: "D:/project",
    tools: [
      {
        name: "editor_get_context",
        description: "What is open in the editor.",
        inputSchema: { type: "object", properties: {} },
        readOnly: true,
      },
    ],
    builtinTools: "files",
    settingSources: ["project"],
    prewarm: true,
    ...extra,
  });
}

test("init loads the SDK, answers ready, and starts a workspace prewarm", async () => {
  const { bridge, out } = makeBridge();
  await init(bridge);
  assert.equal(out[0].type, "ready");
  assert.equal(out[0].protocol, PROTOCOL_VERSION);
  assert.equal(out[0].tools, 1);
  await flush();
  assert.equal(fakeSdk.state.prewarmed.length, 1);
  assert.equal(fakeSdk.state.prewarmed[0].apiKey, "cursor_test_key");
  assert.equal(fakeSdk.state.prewarmed[0].local.cwd, "D:/project");
  assert.ok(out.some((message) => message.type === "prewarmed"));
});

test("init without a key fails cleanly", async () => {
  const { bridge, out } = makeBridge();
  await bridge.handle({ type: "init", cwd: "D:/project", tools: [] });
  assert.equal(out[0].type, "init_error");
  assert.match(out[0].message, /API key/i);
});

test("a plain send streams thinking, text, usage, and done", async () => {
  const { bridge, out } = makeBridge();
  await init(bridge, { prewarm: false });
  out.length = 0;
  await bridge.handle({ type: "send", id: "t1", text: "Say hello", model: { id: "stand-in-large" } });
  const types = out.map((message) => message.type);
  assert.deepEqual(
    types.filter((type) => type !== "log"),
    ["started", "thinking_delta", "text_delta", "text_delta", "usage", "done"],
  );
  assert.equal(out.find((message) => message.type === "thinking_delta").text, "A scripted thought.");
  assert.equal(
    out
      .filter((message) => message.type === "text_delta")
      .map((message) => message.text)
      .join(""),
    "Hello from the stand-in.",
  );
  assert.deepEqual(out.find((message) => message.type === "usage").usage, {
    inputTokens: 120,
    outputTokens: 30,
    cacheReadTokens: 400,
    cacheWriteTokens: 10,
  });
  assert.equal(out.find((message) => message.type === "done").outcome, "done");
});

test("an editor tool call is forwarded and answered through tool_result", async () => {
  const { bridge, out } = makeBridge();
  await init(bridge, { prewarm: false });
  out.length = 0;

  const turn = bridge.handle({
    type: "send",
    id: "t2",
    text: "[[tool:editor_get_context]]",
    model: { id: "stand-in-large" },
  });
  await flush();
  const call = out.find((message) => message.type === "tool_call");
  assert.ok(call);
  assert.equal(call.name, "editor_get_context");
  assert.equal(call.id, "t2");
  assert.ok(typeof call.sentAtMs === "number");

  await bridge.handle({
    type: "tool_result",
    callId: call.callId,
    text: '{"project":"Demo"}',
    isError: false,
  });
  await turn;

  const reply = out
    .filter((message) => message.type === "text_delta")
    .map((message) => message.text)
    .join("");
  assert.match(reply, /Tool said: \{"project":"Demo"\}/);
  assert.equal(out.find((message) => message.type === "done").outcome, "done");
});

test("Cursor's own tools are reported as builtin_tool, not tool_call", async () => {
  const { bridge, out } = makeBridge();
  await init(bridge, { prewarm: false });
  out.length = 0;
  await bridge.handle({ type: "send", id: "t3", text: "[[builtin]]", model: { id: "stand-in-small" } });
  assert.ok(out.some((message) => message.type === "builtin_tool" && message.name === "cursor:read"));
  assert.equal(out.some((message) => message.type === "tool_call"), false);
});

test("cancel stops a waiting turn", async () => {
  const { bridge, out } = makeBridge();
  await init(bridge, { prewarm: false });
  out.length = 0;
  const turn = bridge.handle({ type: "send", id: "t4", text: "[[wait]]", model: { id: "stand-in-large" } });
  await flush();
  await bridge.handle({ type: "cancel", id: "t4" });
  await turn;
  assert.equal(out.find((message) => message.type === "done").outcome, "cancelled");
});

test("a bad key on list_models is returned as models_error", async () => {
  const { bridge, out } = makeBridge();
  await init(bridge, { apiKey: "bad-key", prewarm: false });
  out.length = 0;
  await bridge.handle({ type: "list_models", id: "m1" });
  assert.equal(out[0].type, "models_error");
  assert.match(out[0].message, /API key/i);
});

test("list_models returns the stand-in catalogue", async () => {
  const { bridge, out } = makeBridge();
  await init(bridge, { prewarm: false });
  out.length = 0;
  await bridge.handle({ type: "list_models", id: "m2" });
  assert.equal(out[0].type, "models");
  assert.equal(out[0].models.length, 2);
  assert.equal(out[0].models[0].id, "stand-in-large");
});

test("read-only built-in mode keeps shell and delete off the agent", async () => {
  const { bridge, out } = makeBridge();
  await init(bridge, { builtinTools: "read-only", prewarm: false });
  out.length = 0;
  await bridge.handle({ type: "send", id: "t5", text: "[[report]]", model: { id: "stand-in-large" } });
  const report = JSON.parse(out.find((message) => message.type === "text_delta").text);
  assert.deepEqual(report.tools, ["read", "grep", "glob", "ls", "semSearch", "readLints", "readTodos", "updateTodos", "mcp"]);
  assert.ok(report.disallowedTools.includes("askQuestion"));
  assert.deepEqual(report.toolNames, ["editor_get_context"]);
  assert.equal(report.apiKeyLength > 0, true);
});

test("a run that cannot start reports error without a done", async () => {
  const { bridge, out } = makeBridge();
  await init(bridge, { prewarm: false });
  out.length = 0;
  await bridge.handle({ type: "send", id: "t6", text: "[[throw]]", model: { id: "stand-in-large" } });
  assert.equal(out.some((message) => message.type === "error"), true);
  assert.equal(out.some((message) => message.type === "done"), false);
  assert.equal(out.find((message) => message.type === "error").started, false);
});

test("pictures attached to a message reach the SDK as images beside the text", async () => {
  const { bridge, out, logs } = makeBridge();
  await init(bridge, { prewarm: false });
  out.length = 0;
  await bridge.handle({
    type: "send",
    id: "t7",
    text: "[[images]] What is in these?",
    model: { id: "stand-in-large" },
    images: [
      { data: "QUJDRA==", mimeType: "image/png", width: 640, height: 360 },
      { data: "RUZHSA==", mimeType: "image/jpeg" },
      // Dropped: not a format the editor sends, no data, not an object.
      { data: "SUpLTA==", mimeType: "image/svg+xml" },
      { data: "", mimeType: "image/png" },
      "not an image",
    ],
  });
  const report = JSON.parse(out.find((message) => message.type === "text_delta").text);
  assert.deepEqual(report, [
    { mimeType: "image/png", base64Chars: 8, dimension: { width: 640, height: 360 } },
    { mimeType: "image/jpeg", base64Chars: 8, dimension: null },
  ]);
  assert.equal(fakeSdk.state.sends[0].images, 2);
  // Picture data is never echoed back to the editor or written to a log.
  const everything = JSON.stringify(out) + JSON.stringify(logs);
  assert.equal(everything.includes("QUJDRA=="), false);
});

test("a message without pictures is still sent as plain text", async () => {
  const { bridge, out } = makeBridge();
  await init(bridge, { prewarm: false });
  out.length = 0;
  await bridge.handle({ type: "send", id: "t8", text: "[[images]]", model: { id: "stand-in-large" }, images: [] });
  assert.equal(out.find((message) => message.type === "text_delta").text, "[]");
  assert.equal(fakeSdk.state.sends[0].images, 0);
});

test("a picture in a tool result is handed to the model as image content", async () => {
  const { bridge, out } = makeBridge();
  await init(bridge, { prewarm: false });
  out.length = 0;
  const turn = bridge.handle({ type: "send", id: "t9", text: "[[tool:editor_get_context]]", model: { id: "stand-in-large" } });
  await flush();
  const call = out.find((message) => message.type === "tool_call");
  await bridge.handle({
    type: "tool_result",
    callId: call.callId,
    text: '{"width":640}',
    isError: false,
    images: [{ data: "QUJDRA==", mimeType: "image/jpeg" }],
  });
  await turn;
  const reply = out
    .filter((message) => message.type === "text_delta")
    .map((message) => message.text)
    .join("");
  assert.equal(reply, 'Tool said: {"width":640}[image image/jpeg 8 base64 chars]');
});
