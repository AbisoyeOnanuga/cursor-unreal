import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import path from "node:path";
import { test } from "node:test";

import { createLineReader, encode } from "../lib/protocol.mjs";

const bridgePath = fileURLToPath(new URL("../bridge.mjs", import.meta.url));
const fakeSdkPath = fileURLToPath(new URL("./helpers/fake-sdk.mjs", import.meta.url));
const bridgeDir = path.dirname(bridgePath);

function startBridge() {
  const child = spawn(process.execPath, [bridgePath, `--sdk-module=${fakeSdkPath}`], {
    cwd: bridgeDir,
    stdio: ["pipe", "pipe", "pipe"],
    env: { ...process.env, FORCE_COLOR: "0" },
  });
  const messages = [];
  const stderr = [];
  const reader = createLineReader((message) => messages.push(message));
  child.stdout.on("data", (chunk) => reader.push(chunk));
  child.stderr.on("data", (chunk) => stderr.push(chunk.toString("utf8")));
  const write = (message) => child.stdin.write(encode(message));
  const waitFor = async (predicate, label, ms = 5000) => {
    const deadline = Date.now() + ms;
    for (;;) {
      const hit = messages.find(predicate);
      if (hit) {
        return hit;
      }
      if (Date.now() > deadline) {
        throw new Error(`Timed out waiting for ${label}. Saw: ${JSON.stringify(messages)}\nstderr: ${stderr.join("")}`);
      }
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
  };
  return { child, messages, stderr, write, waitFor };
}

test("the real process says hello, answers init, runs a turn, and shuts down", async () => {
  const { child, write, waitFor, messages } = startBridge();
  try {
    await waitFor((message) => message.type === "hello", "hello");
    write({
      type: "init",
      apiKey: "cursor_test_key",
      cwd: bridgeDir,
      tools: [],
      builtinTools: "none",
      prewarm: false,
    });
    await waitFor((message) => message.type === "ready", "ready");
    write({ type: "send", id: "p1", text: "hi", model: { id: "stand-in-large" } });
    await waitFor((message) => message.type === "done" && message.id === "p1", "done");
    const text = messages
      .filter((message) => message.type === "text_delta" && message.id === "p1")
      .map((message) => message.text)
      .join("");
    assert.equal(text, "Hello from the stand-in.");
    write({ type: "shutdown" });
    await new Promise((resolve, reject) => {
      child.once("exit", (code) => (code === 0 ? resolve() : reject(new Error(`exit ${code}`))));
      setTimeout(() => reject(new Error("shutdown timed out")), 5000);
    });
  } finally {
    if (!child.killed) {
      child.kill();
    }
  }
});
