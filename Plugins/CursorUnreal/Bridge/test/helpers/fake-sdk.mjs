// A stand-in for @cursor/sdk with the same shapes the bridge uses, driven by the text of the
// message it is sent. It never touches the network. The bridge loads it when started with
// --sdk-module=<this file>, which is how the bridge tests and the editor's automation tests run
// a whole turn without a Cursor account.
//
// What a message does, by a marker that will not appear in ordinary Unreal prompts:
//   [[tool:<name>]] or [[tool:<name>:{...}]]   call that editor tool
//   [[builtin]]         reports one of Cursor's own tools running and finishing
//   [[fail]]            the run starts, then ends with an error
//   [[throw]]           send() itself throws, as it does when the run cannot start
//   [[wait]]            the run keeps going until it is cancelled
//   [[report]]          replies with the options the agent was created with (never the key)
//   [[images]]          replies with what it was told about the pictures sent with the message
//   anything else       a short thinking note and a two-part reply

export const state = {
  created: [],
  sends: [],
  disposed: 0,
  prewarmed: [],
  prewarmReleased: 0,
  modelListCalls: 0,
};

export function resetState() {
  state.created.length = 0;
  state.sends.length = 0;
  state.disposed = 0;
  state.prewarmed.length = 0;
  state.prewarmReleased = 0;
  state.modelListCalls = 0;
}

function sdkError(message, code, isRetryable = false) {
  return Object.assign(new Error(message), { code, isRetryable });
}

function checkKey(apiKey) {
  if (typeof apiKey !== "string" || apiKey === "" || apiKey === "bad-key") {
    throw sdkError("Invalid API key.", "unauthenticated");
  }
}

function describePart(part) {
  // Sizes only: a picture's bytes must never end up in a reply or a log.
  return part.type === "image" ? `[image ${part.mimeType} ${part.data.length} base64 chars]` : part.text;
}

function createRun(agent, text, sendOptions, images = []) {
  const runId = `run-${state.sends.length}`;
  const base = { agent_id: agent.agentId, run_id: runId };
  const customTools = sendOptions?.local?.customTools ?? agent.options.local?.customTools ?? {};

  let status = "running";
  let error;
  let cancelSignal;
  const cancelled = new Promise((resolve) => {
    cancelSignal = resolve;
  });
  let finish;
  const finished = new Promise((resolve) => {
    finish = resolve;
  });

  async function* script() {
    const toolMatch = /\[\[tool:(\w+)(?::(\{.*\}))?\]\]/s.exec(text);
    if (toolMatch) {
      const [, name, argsJson] = toolMatch;
      const args = argsJson ? JSON.parse(argsJson) : {};
      const call = { ...base, type: "tool_call", call_id: "sdk-call-1", name: "mcp" };
      const mcpArgs = { providerIdentifier: "custom-user-tools", toolName: name, args };
      yield { ...call, status: "running", args: mcpArgs };
      const tool = customTools[name];
      const result = tool
        ? await tool.execute(args, { toolCallId: "sdk-call-1", sessionId: agent.agentId })
        : { content: [{ type: "text", text: `No tool named ${name}.` }], isError: true };
      yield { ...call, status: result.isError ? "error" : "completed", args: mcpArgs, result };
      const shown = result.content.map(describePart).join("");
      yield { ...base, type: "assistant", message: { role: "assistant", content: [{ type: "text", text: `${result.isError ? "Tool failed" : "Tool said"}: ${shown}` }] } };
    } else if (text.includes("[[builtin]]")) {
      const call = { ...base, type: "tool_call", call_id: "sdk-call-2", name: "read", args: { path: "Source/Game.cpp" } };
      yield { ...call, status: "running" };
      yield { ...call, status: "completed", result: { status: "success", value: { content: "// file" } } };
      yield { ...base, type: "assistant", message: { role: "assistant", content: [{ type: "text", text: "Read one file." }] } };
    } else if (text.includes("[[fail]]")) {
      yield { ...base, type: "assistant", message: { role: "assistant", content: [{ type: "text", text: "Starting." }] } };
      status = "error";
      error = { message: "The stand-in was asked to fail.", code: "stand_in_failure" };
      return;
    } else if (text.includes("[[wait]]")) {
      yield { ...base, type: "assistant", message: { role: "assistant", content: [{ type: "text", text: "Waiting." }] } };
      await cancelled;
      return;
    } else if (text.includes("[[images]]")) {
      const report = images.map((image) => ({
        mimeType: image.mimeType,
        base64Chars: typeof image.data === "string" ? image.data.length : 0,
        dimension: image.dimension ?? null,
      }));
      yield { ...base, type: "assistant", message: { role: "assistant", content: [{ type: "text", text: JSON.stringify(report) }] } };
    } else if (text.includes("[[report]]")) {
      const options = agent.options;
      const report = {
        apiKeyLength: options.apiKey.length,
        cwd: options.local?.cwd,
        settingSources: options.local?.settingSources,
        tools: options.tools ?? null,
        disallowedTools: options.disallowedTools ?? null,
        toolNames: Object.keys(customTools).sort(),
        model: sendOptions?.model ?? options.model,
        sends: state.sends.length,
      };
      yield { ...base, type: "assistant", message: { role: "assistant", content: [{ type: "text", text: JSON.stringify(report) }] } };
    } else {
      yield { ...base, type: "system", subtype: "init", model: sendOptions?.model };
      yield { ...base, type: "thinking", text: "A scripted thought." };
      yield { ...base, type: "thinking", text: "", thinking_duration_ms: 5 };
      yield { ...base, type: "assistant", message: { role: "assistant", content: [{ type: "text", text: "Hello " }] } };
      yield { ...base, type: "assistant", message: { role: "assistant", content: [{ type: "text", text: "from the stand-in." }] } };
    }
    yield { ...base, type: "usage", usage: { inputTokens: 120, outputTokens: 30, cacheReadTokens: 400, cacheWriteTokens: 10, totalTokens: 560 } };
  }

  return {
    id: runId,
    agentId: agent.agentId,
    get status() {
      return status;
    },
    supports: () => true,
    unsupportedReason: () => undefined,
    async *stream() {
      try {
        yield* script();
        if (status === "running") {
          status = "finished";
        }
      } finally {
        finish();
      }
    },
    async wait() {
      await finished;
      return { id: runId, status, durationMs: 7, ...(error ? { error } : {}) };
    },
    async cancel() {
      if (status === "running") {
        status = "cancelled";
        cancelSignal();
      }
    },
  };
}

export const Agent = {
  async create(options) {
    checkKey(options.apiKey);
    if (!options.model || typeof options.model.id !== "string") {
      throw sdkError("A model is required for local agents.", "invalid_argument");
    }
    const agent = {
      agentId: `agent-${state.created.length + 1}`,
      options,
      model: options.model,
      async send(message, sendOptions) {
        const text = typeof message === "string" ? message : message.text;
        const images = typeof message === "string" ? [] : (message.images ?? []);
        state.sends.push({ agentId: agent.agentId, text, model: sendOptions?.model, images: images.length });
        if (text.includes("[[throw]]")) {
          throw sdkError("The stand-in refused to start the run.", "unavailable", true);
        }
        return createRun(agent, text, sendOptions, images);
      },
      close() {
        state.disposed += 1;
      },
      async [Symbol.asyncDispose]() {
        state.disposed += 1;
      },
    };
    state.created.push(options);
    return agent;
  },
};

export const Cursor = {
  models: {
    async list(options) {
      state.modelListCalls += 1;
      checkKey(options?.apiKey);
      return [
        { id: "stand-in-large", displayName: "Stand-in Large", description: "Scripted replies.", parameters: [{ id: "effort", values: [{ value: "low" }, { value: "high" }] }] },
        { id: "stand-in-small", displayName: "Stand-in Small" },
      ];
    },
  },
};

export async function createAgentPlatform() {
  return {
    async prewarmLocalWorkspace(options) {
      checkKey(options.apiKey);
      state.prewarmed.push(options);
      return async () => {
        state.prewarmReleased += 1;
      };
    },
  };
}
