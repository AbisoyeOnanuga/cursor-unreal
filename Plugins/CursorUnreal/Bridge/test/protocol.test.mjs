import assert from "node:assert/strict";
import { test } from "node:test";

import { createLineReader, encode, MAX_LINE_BYTES } from "../lib/protocol.mjs";

function collect() {
  const messages = [];
  const problems = [];
  const reader = createLineReader(
    (message) => messages.push(message),
    (problem) => problems.push(problem),
  );
  return { messages, problems, reader };
}

test("encode writes one line per message, whatever the text contains", () => {
  const line = encode({ type: "text_delta", text: "first\nsecond\r\n\u2028third" });
  assert.equal(line.endsWith("\n"), true);
  assert.equal(line.indexOf("\n"), line.length - 1);
  assert.deepEqual(JSON.parse(line), { type: "text_delta", text: "first\nsecond\r\n\u2028third" });
});

test("a message split across chunks is put back together", () => {
  const { messages, reader } = collect();
  const line = encode({ type: "send", id: "1", text: "hello" });
  reader.push(line.slice(0, 5));
  assert.equal(messages.length, 0);
  reader.push(line.slice(5, 12));
  reader.push(line.slice(12));
  assert.deepEqual(messages, [{ type: "send", id: "1", text: "hello" }]);
});

test("several messages in one chunk all arrive, in order", () => {
  const { messages, reader } = collect();
  reader.push(encode({ n: 1 }) + encode({ n: 2 }) + encode({ n: 3 }).slice(0, 4));
  assert.deepEqual(messages, [{ n: 1 }, { n: 2 }]);
  reader.push(encode({ n: 3 }).slice(4));
  assert.deepEqual(messages, [{ n: 1 }, { n: 2 }, { n: 3 }]);
});

test("a multi-byte character split between chunks survives", () => {
  const { messages, reader } = collect();
  const bytes = Buffer.from(encode({ text: "naïve — 日本語 🎮" }), "utf8");
  // One byte at a time is the worst case: every multi-byte character is split.
  for (const byte of bytes) {
    reader.push(Buffer.from([byte]));
  }
  assert.deepEqual(messages, [{ text: "naïve — 日本語 🎮" }]);
});

test("CRLF line endings and blank lines are accepted", () => {
  const { messages, problems, reader } = collect();
  reader.push('{"a":1}\r\n\r\n\n{"a":2}\r\n');
  assert.deepEqual(messages, [{ a: 1 }, { a: 2 }]);
  assert.deepEqual(problems, []);
});

test("lines that are not JSON objects are reported and skipped, and reading carries on", () => {
  const { messages, problems, reader } = collect();
  reader.push('not json\n[1,2]\n"text"\nnull\n{"ok":true}\n');
  assert.deepEqual(messages, [{ ok: true }]);
  assert.equal(problems.length, 4);
});

test("a line over the limit is dropped without being kept in memory, and the next line still arrives", () => {
  const { messages, problems, reader } = collect();
  const chunk = Buffer.alloc(1024 * 1024, 0x61);
  for (let sent = 0; sent <= MAX_LINE_BYTES; sent += chunk.length) {
    reader.push(chunk);
  }
  assert.equal(problems.length, 1);
  // More of the same oversized line, then its end, then a good line.
  reader.push(chunk);
  reader.push(`tail of the long line\n${encode({ after: true })}`);
  assert.deepEqual(messages, [{ after: true }]);
  assert.equal(problems.length, 1);
});
