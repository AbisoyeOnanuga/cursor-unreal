#!/usr/bin/env node
// Sidecar started by the CursorUnreal editor plugin. It runs a local Cursor agent through
// @cursor/sdk and relays between it and the editor over stdin and stdout, one JSON object per
// line. The Cursor API key arrives in the first message on stdin. It is never read from the
// command line or the environment, and is never written anywhere.

import { readFile } from "node:fs/promises";
import { createRequire } from "node:module";
import { pathToFileURL } from "node:url";
import process from "node:process";

import { createBridge } from "./lib/bridge-core.mjs";
import { createLineReader, encode, PROTOCOL_VERSION } from "./lib/protocol.mjs";

const MIN_NODE = [22, 13];

function nodeIsNewEnough() {
  const [major, minor] = process.versions.node.split(".").map(Number);
  return major > MIN_NODE[0] || (major === MIN_NODE[0] && minor >= MIN_NODE[1]);
}

function writeMessage(message) {
  process.stdout.write(encode(message));
}

function log(level, text) {
  writeMessage({ type: "log", level, text });
}

// stdout carries the protocol and nothing else. Anything a dependency prints goes to stderr,
// which the editor keeps for troubleshooting.
for (const method of ["log", "info", "debug", "warn", "error", "trace"]) {
  console[method] = (...parts) => {
    process.stderr.write(`${parts.map((part) => (typeof part === "string" ? part : JSON.stringify(part))).join(" ")}\n`);
  };
}

/** `--sdk-module=<path>` swaps in a stand-in for @cursor/sdk. Used by the tests; the editor never passes it. */
function sdkModuleOverride() {
  const prefix = "--sdk-module=";
  const arg = process.argv.slice(2).find((value) => value.startsWith(prefix));
  return arg ? arg.slice(prefix.length) : undefined;
}

async function loadSdk() {
  const override = sdkModuleOverride();
  if (override) {
    return { sdk: await import(pathToFileURL(override).href), version: "stand-in" };
  }
  const sdk = await import("@cursor/sdk");
  let version;
  try {
    const require = createRequire(import.meta.url);
    const entry = require.resolve("@cursor/sdk");
    const marker = entry.replaceAll("\\", "/").lastIndexOf("/node_modules/@cursor/sdk/");
    if (marker !== -1) {
      const manifest = `${entry.slice(0, marker)}/node_modules/@cursor/sdk/package.json`;
      version = JSON.parse(await readFile(manifest, "utf8")).version;
    }
  } catch {
    // The version is only shown in the Setup tab.
  }
  return { sdk, version };
}

let exiting = false;
function exit(code) {
  if (exiting) {
    return;
  }
  exiting = true;
  // Let queued stdout drain before the process goes, but do not wait for a reader that has gone.
  setTimeout(() => process.exit(code), 500).unref();
  process.stdout.write("", () => process.exit(code));
}

if (!nodeIsNewEnough()) {
  writeMessage({
    type: "fatal",
    message: `Node.js ${MIN_NODE.join(".")} or newer is needed for the Cursor SDK. This is ${process.version}.`,
  });
  exit(1);
} else {
  const bridge = createBridge({ loadSdk, send: writeMessage, log, onShutdown: () => exit(0) });

  const reader = createLineReader(
    (message) => {
      void bridge.handle(message);
    },
    (problem) => log("warn", `From the editor: ${problem}.`),
  );

  process.stdin.on("data", (chunk) => reader.push(chunk));
  // The editor closing its end, or dying, is the signal to stop. Never outlive it.
  process.stdin.on("end", () => void bridge.shutdown());
  process.stdin.on("error", () => void bridge.shutdown());
  process.stdout.on("error", () => exit(0));

  process.on("uncaughtException", (error) => {
    // State cannot be trusted after this. Stopping lets the editor end the turn with this message
    // instead of waiting on a turn that may never finish; it starts a new bridge for the next one.
    writeMessage({ type: "fatal", message: `Unexpected error: ${error instanceof Error ? error.message : String(error)}` });
    exit(1);
  });
  process.on("unhandledRejection", (reason) => {
    log("error", `Unexpected rejection: ${reason instanceof Error ? reason.message : String(reason)}`);
  });

  writeMessage({ type: "hello", protocol: PROTOCOL_VERSION, node: process.version, pid: process.pid });
}
