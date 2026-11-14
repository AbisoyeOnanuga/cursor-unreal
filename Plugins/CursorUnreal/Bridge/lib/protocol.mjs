// Wire format between the Unreal Editor and this process: one JSON object per line, UTF-8, on
// stdin (editor to bridge) and stdout (bridge to editor). Nothing else may be written to stdout.

export const PROTOCOL_VERSION = 1;

/** Longest line accepted from the editor. A tool result is capped well below this on the editor side. */
export const MAX_LINE_BYTES = 16 * 1024 * 1024;

/** One message as a line ready to write. */
export function encode(message) {
  // JSON.stringify never emits a raw newline, so the line break below is the only one.
  return JSON.stringify(message) + "\n";
}

/**
 * Turns a stream of chunks into parsed messages. Chunks can split a line, or a multi-byte
 * character, anywhere.
 *
 * @param {(message: object) => void} onMessage
 * @param {(problem: string) => void} onProblem called for a line that is not a JSON object
 */
export function createLineReader(onMessage, onProblem = () => {}) {
  /** @type {Buffer[]} */
  let pending = [];
  let pendingBytes = 0;
  let skipping = false;

  function finishLine(line) {
    const text = line.toString("utf8").trim();
    if (text === "") {
      return;
    }
    let message;
    try {
      message = JSON.parse(text);
    } catch {
      onProblem("a line that is not JSON was ignored");
      return;
    }
    if (message === null || typeof message !== "object" || Array.isArray(message)) {
      onProblem("a line that is not a JSON object was ignored");
      return;
    }
    onMessage(message);
  }

  return {
    /** @param {Buffer | string} chunk */
    push(chunk) {
      let rest = typeof chunk === "string" ? Buffer.from(chunk, "utf8") : chunk;
      for (;;) {
        const newline = rest.indexOf(0x0a);
        if (newline === -1) {
          break;
        }
        const head = rest.subarray(0, newline);
        rest = rest.subarray(newline + 1);
        if (skipping) {
          // The end of a line that was already thrown away for being too long.
          skipping = false;
        } else {
          finishLine(pending.length === 0 ? head : Buffer.concat([...pending, head]));
        }
        pending = [];
        pendingBytes = 0;
      }
      if (rest.length === 0 || skipping) {
        return;
      }
      pending.push(rest);
      pendingBytes += rest.length;
      if (pendingBytes > MAX_LINE_BYTES) {
        pending = [];
        pendingBytes = 0;
        skipping = true;
        onProblem("a line longer than the limit was ignored");
      }
    },
  };
}
