// The bridge's logic, kept apart from stdio and from the real SDK so it can be tested with a fake
// of each. See ../README.md for the message reference.

import { PROTOCOL_VERSION } from "./protocol.mjs";

/** The MCP server name the SDK serves `local.customTools` under. */
const CUSTOM_TOOLS_SERVER = "custom-user-tools";

/** Longest args or result of one of Cursor's own tools passed on for display. */
const MAX_SHOWN_CHARS = 4000;

/**
 * Which of Cursor's own tools the agent keeps, by the mode the editor asks for. The editor's
 * approval prompt covers editor tools only: Cursor's built-in tools run inside this process and
 * cannot ask, so the modes that must not change anything take the changing tools away instead.
 *
 * "task" goes with them because subagents keep their own toolsets, shell included.
 * "askQuestion" is always off: there is nobody on this side to answer it.
 */
const BUILTIN_TOOL_MODES = {
  // Shell included. The user turned this on in the settings.
  all: { disallowedTools: ["askQuestion"] },
  // Read, search and edit files. No shell, no file deletion, no subagents.
  files: { disallowedTools: ["askQuestion", "shell", "delete", "task"] },
  // Read and search only.
  "read-only": {
    tools: ["read", "grep", "glob", "ls", "semSearch", "readLints", "readTodos", "updateTodos", "mcp"],
    disallowedTools: ["askQuestion"],
  },
  // Editor tools only.
  none: { tools: ["mcp"], disallowedTools: ["askQuestion"] },
};

const SETTING_SOURCES = new Set(["project", "user", "team", "mdm", "plugins", "all"]);

function isRecord(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function clip(value) {
  if (value === undefined) {
    return undefined;
  }
  let text;
  try {
    text = typeof value === "string" ? value : JSON.stringify(value);
  } catch {
    text = String(value);
  }
  if (text === undefined) {
    return undefined;
  }
  return text.length > MAX_SHOWN_CHARS
    ? `${text.slice(0, MAX_SHOWN_CHARS)}... (${text.length - MAX_SHOWN_CHARS} more characters)`
    : text;
}

/** A message for the user from whatever was thrown. Never includes the stack. */
function describeError(error) {
  if (error instanceof Error && error.message) {
    return error.message;
  }
  return typeof error === "string" && error !== "" ? error : "Unknown error.";
}

function errorFields(error) {
  const fields = { message: describeError(error) };
  if (isRecord(error) || error instanceof Error) {
    if (typeof error.code === "string") {
      fields.code = error.code;
    }
    if (typeof error.isRetryable === "boolean") {
      fields.retryable = error.isRetryable;
    }
    if (typeof error.status === "number") {
      fields.status = error.status;
    }
  }
  return fields;
}

function normalizeModel(model) {
  if (!isRecord(model) || typeof model.id !== "string" || model.id.trim() === "") {
    return undefined;
  }
  const selection = { id: model.id.trim() };
  if (Array.isArray(model.params)) {
    const params = model.params
      .filter((p) => isRecord(p) && typeof p.id === "string" && typeof p.value === "string")
      .map((p) => ({ id: p.id, value: p.value }));
    if (params.length > 0) {
      selection.params = params;
    }
  }
  return selection;
}

function normalizeTools(tools) {
  if (!Array.isArray(tools)) {
    return [];
  }
  return tools
    .filter((tool) => isRecord(tool) && typeof tool.name === "string" && tool.name !== "")
    .map((tool) => ({
      name: tool.name,
      description: typeof tool.description === "string" ? tool.description : "",
      inputSchema: isRecord(tool.inputSchema) ? tool.inputSchema : { type: "object", properties: {} },
      readOnly: tool.readOnly === true,
      destructive: tool.destructive === true,
    }));
}

/**
 * @param {object} deps
 * @param {() => Promise<{ sdk: object, version?: string }>} deps.loadSdk
 * @param {(message: object) => void} deps.send writes one message to the editor
 * @param {(level: "info" | "warn" | "error", text: string) => void} [deps.log]
 * @param {() => void} [deps.onShutdown] called once everything is released
 * @param {() => number} [deps.now]
 */
export function createBridge({ loadSdk, send, log = () => {}, onShutdown = () => {}, now = () => Date.now() }) {
  let sdk = null;
  let sdkVersion;
  /** Set by `init`. */
  let config = null;
  let tools = [];
  let agent = null;
  /** Resolves when the last agent has been let go, so a new one is not created over it. */
  let settling = Promise.resolve();
  let releasePrewarm = null;
  /** Counts `init` messages, so work started under an earlier one can tell it is stale. */
  let epoch = 0;
  /** The turn in progress: { id, run, cancelRequested }. */
  let active = null;
  /** Editor tool calls waiting for the editor: callId -> { resolve, turnId }. */
  const pendingCalls = new Map();
  let nextCallNumber = 0;
  let closed = false;

  function emit(message) {
    if (!closed) {
      send(message);
    }
  }

  function settlePendingCalls(turnId, text) {
    for (const [callId, pending] of pendingCalls) {
      if (turnId === undefined || pending.turnId === turnId) {
        pendingCalls.delete(callId);
        pending.resolve({ content: [{ type: "text", text }], isError: true });
      }
    }
  }

  function callEditor(name, args) {
    const turnId = active?.id;
    if (turnId === undefined) {
      return Promise.resolve({
        content: [{ type: "text", text: "Not run: no turn is in progress in the editor." }],
        isError: true,
      });
    }
    if (active.cancelRequested) {
      return Promise.resolve({ content: [{ type: "text", text: "Not run: the user stopped the turn." }], isError: true });
    }
    const callId = `call-${++nextCallNumber}`;
    return new Promise((resolve) => {
      pendingCalls.set(callId, { resolve, turnId });
      emit({ type: "tool_call", id: turnId, callId, name, args: isRecord(args) ? args : {}, sentAtMs: now() });
    });
  }

  function buildCustomTools() {
    const customTools = {};
    for (const tool of tools) {
      customTools[tool.name] = {
        description: tool.description,
        inputSchema: tool.inputSchema,
        annotations: { readOnlyHint: tool.readOnly, destructiveHint: tool.destructive },
        execute: (args) => callEditor(tool.name, args),
      };
    }
    return customTools;
  }

  /** Options shared by prewarming and agent creation. They have to match for the prewarmed workspace to be reused. */
  function baseOptions() {
    return {
      apiKey: config.apiKey,
      local: { cwd: config.cwd, settingSources: config.settingSources },
    };
  }

  function agentOptions(model) {
    const base = baseOptions();
    const mode = BUILTIN_TOOL_MODES[config.builtinTools];
    const options = {
      ...base,
      name: "Unreal Editor",
      model,
      local: { ...base.local, customTools: buildCustomTools() },
      disallowedTools: [...mode.disallowedTools],
    };
    if (mode.tools) {
      options.tools = [...mode.tools];
    }
    return options;
  }

  async function disposeAgent() {
    const old = agent;
    agent = null;
    if (!old) {
      return;
    }
    try {
      if (typeof old[Symbol.asyncDispose] === "function") {
        await old[Symbol.asyncDispose]();
      } else if (typeof old.close === "function") {
        old.close();
      }
    } catch (error) {
      log("warn", `Closing the agent failed: ${describeError(error)}`);
    }
  }

  async function dropPrewarm() {
    const release = releasePrewarm;
    releasePrewarm = null;
    if (release) {
      try {
        await release();
      } catch (error) {
        log("warn", `Releasing the prewarmed workspace failed: ${describeError(error)}`);
      }
    }
  }

  async function prewarm(forEpoch) {
    if (typeof sdk.createAgentPlatform !== "function") {
      return;
    }
    const started = now();
    try {
      const platform = await sdk.createAgentPlatform();
      const release = await platform.prewarmLocalWorkspace(baseOptions());
      if (closed || forEpoch !== epoch) {
        await release();
        return;
      }
      releasePrewarm = release;
      emit({ type: "prewarmed", ms: now() - started });
    } catch (error) {
      // An optimisation only: the first turn does the same work if this did not.
      log("info", `Prewarming the workspace did not finish: ${describeError(error)}`);
    }
  }

  async function handleInit(message) {
    if (typeof message.apiKey !== "string" || message.apiKey.trim() === "") {
      emit({ type: "init_error", message: "No Cursor API key was given." });
      return;
    }
    if (typeof message.cwd !== "string" || message.cwd === "") {
      emit({ type: "init_error", message: "No project folder was given." });
      return;
    }
    const builtinTools = typeof message.builtinTools === "string" ? message.builtinTools : "files";
    if (!Object.hasOwn(BUILTIN_TOOL_MODES, builtinTools)) {
      emit({ type: "init_error", message: `Unknown built-in tool mode '${builtinTools}'.` });
      return;
    }

    const myEpoch = ++epoch;
    if (active) {
      await cancelActive();
    }
    settling = settling.then(disposeAgent).then(dropPrewarm);
    await settling;
    if (myEpoch !== epoch) {
      return;
    }

    config = {
      apiKey: message.apiKey.trim(),
      cwd: message.cwd,
      builtinTools,
      settingSources: Array.isArray(message.settingSources)
        ? message.settingSources.filter((source) => SETTING_SOURCES.has(source))
        : [],
    };
    tools = normalizeTools(message.tools);

    if (!sdk) {
      try {
        const loaded = await loadSdk();
        sdk = loaded.sdk;
        sdkVersion = loaded.version;
      } catch (error) {
        config = null;
        emit({
          type: "init_error",
          message: `The Cursor SDK could not be loaded: ${describeError(error)} Run "npm install" in the plugin's Bridge folder.`,
        });
        return;
      }
    }
    if (myEpoch !== epoch) {
      return;
    }

    emit({ type: "ready", protocol: PROTOCOL_VERSION, node: process.version, sdk: sdkVersion ?? "unknown", tools: tools.length });
    if (message.prewarm === true) {
      void prewarm(myEpoch);
    }
  }

  function isOwnToolCall(message) {
    if (message.name !== "mcp" || !isRecord(message.args)) {
      return false;
    }
    const { providerIdentifier, toolName } = message.args;
    return (
      providerIdentifier === CUSTOM_TOOLS_SERVER ||
      (typeof toolName === "string" && toolName.startsWith(`${CUSTOM_TOOLS_SERVER}-`))
    );
  }

  function builtinToolName(message) {
    if (message.name === "mcp" && isRecord(message.args)) {
      const { providerIdentifier, toolName } = message.args;
      if (typeof toolName === "string") {
        return typeof providerIdentifier === "string" ? `mcp:${providerIdentifier}/${toolName}` : `mcp:${toolName}`;
      }
    }
    return `cursor:${message.name}`;
  }

  function translate(turnId, message) {
    if (!isRecord(message)) {
      return;
    }
    switch (message.type) {
      case "assistant":
        for (const block of message.message?.content ?? []) {
          if (block?.type === "text" && typeof block.text === "string" && block.text !== "") {
            emit({ type: "text_delta", id: turnId, text: block.text });
          }
        }
        break;
      case "thinking":
        if (typeof message.text === "string" && message.text !== "") {
          emit({ type: "thinking_delta", id: turnId, text: message.text });
        }
        break;
      case "tool_call":
        // Editor tools are reported from callEditor, with the editor's own timing.
        if (!isOwnToolCall(message) && typeof message.call_id === "string") {
          emit({
            type: "builtin_tool",
            id: turnId,
            callId: message.call_id,
            name: builtinToolName(message),
            status: message.status,
            args: clip(message.args),
            result: clip(message.result),
          });
        }
        break;
      case "usage":
        if (isRecord(message.usage)) {
          emit({
            type: "usage",
            id: turnId,
            usage: {
              inputTokens: Number(message.usage.inputTokens) || 0,
              outputTokens: Number(message.usage.outputTokens) || 0,
              cacheReadTokens: Number(message.usage.cacheReadTokens) || 0,
              cacheWriteTokens: Number(message.usage.cacheWriteTokens) || 0,
            },
          });
        }
        break;
      case "summary":
        if (message.status === "running") {
          emit({ type: "notice", id: turnId, text: "Cursor is summarising the conversation to free up context." });
        } else if (message.status === "error") {
          emit({ type: "notice", id: turnId, text: "Cursor could not summarise the conversation. It is unchanged." });
        } else if (message.fallback === true) {
          emit({ type: "notice", id: turnId, text: "The conversation was shortened to free up context. Some earlier detail may be lost." });
        }
        break;
      default:
        // Message types this version does not know are skipped: minor SDK releases add new ones.
        break;
    }
  }

  async function cancelRun(turn) {
    const run = turn.run;
    if (!run) {
      return;
    }
    try {
      if (typeof run.supports !== "function" || run.supports("cancel")) {
        await run.cancel();
      } else {
        log("warn", "This run cannot be cancelled; it will finish on its own.");
      }
    } catch (error) {
      log("warn", `Cancelling the run failed: ${describeError(error)}`);
    }
  }

  async function cancelActive() {
    const turn = active;
    if (!turn || turn.cancelRequested) {
      return;
    }
    turn.cancelRequested = true;
    settlePendingCalls(turn.id, "Not run: the user stopped the turn.");
    await cancelRun(turn);
  }

  async function handleSend(message) {
    const id = message.id;
    if (typeof id !== "string" || id === "") {
      log("warn", "A send without an id was ignored.");
      return;
    }
    if (!config || !sdk) {
      emit({ type: "error", id, message: "The bridge has not been set up yet." });
      return;
    }
    if (active) {
      emit({ type: "error", id, message: "A turn is already running." });
      return;
    }
    if (typeof message.text !== "string" || message.text.trim() === "") {
      emit({ type: "error", id, message: "The message is empty." });
      return;
    }
    const model = normalizeModel(message.model);
    if (!model) {
      emit({ type: "error", id, message: "No model was chosen." });
      return;
    }

    const turn = { id, run: null, cancelRequested: false };
    active = turn;
    const started = now();
    try {
      await settling;
      if (!agent) {
        agent = await sdk.Agent.create(agentOptions(model));
      }
      if (turn.cancelRequested) {
        emit({ type: "done", id, outcome: "cancelled", durationMs: now() - started });
        return;
      }

      // Tools are passed on every send as well, so a pack switched on or off in the editor applies to the next message.
      turn.run = await agent.send(message.text, { model, local: { customTools: buildCustomTools() } });
      emit({ type: "started", id, agentId: turn.run.agentId ?? agent.agentId, runId: turn.run.id });
      if (turn.cancelRequested) {
        await cancelRun(turn);
      }

      for await (const sdkMessage of turn.run.stream()) {
        translate(id, sdkMessage);
      }
      const result = await turn.run.wait();

      if (result.status === "finished") {
        emit({ type: "done", id, outcome: "done", durationMs: result.durationMs ?? now() - started });
      } else if (result.status === "cancelled") {
        emit({ type: "done", id, outcome: "cancelled", durationMs: result.durationMs ?? now() - started });
      } else {
        emit({
          type: "error",
          id,
          message: result.error?.message || "The run failed without saying why.",
          ...(typeof result.error?.code === "string" ? { code: result.error.code } : {}),
          started: true,
        });
      }
    } catch (error) {
      if (turn.cancelRequested) {
        emit({ type: "done", id, outcome: "cancelled", durationMs: now() - started });
      } else {
        // Thrown rather than returned: the run did not start (key, model, network) or the stream broke.
        emit({ type: "error", id, ...errorFields(error), started: turn.run !== null });
      }
    } finally {
      settlePendingCalls(id, "Not run: the turn ended.");
      if (active === turn) {
        active = null;
      }
    }
  }

  async function handleListModels(message) {
    const id = typeof message.id === "string" ? message.id : "";
    if (!config || !sdk) {
      emit({ type: "models_error", id, message: "The bridge has not been set up yet." });
      return;
    }
    try {
      const models = await sdk.Cursor.models.list({ apiKey: config.apiKey });
      emit({
        type: "models",
        id,
        models: (Array.isArray(models) ? models : [])
          .filter((model) => isRecord(model) && typeof model.id === "string")
          .map((model) => ({
            id: model.id,
            displayName: typeof model.displayName === "string" ? model.displayName : model.id,
            ...(typeof model.description === "string" ? { description: model.description } : {}),
            ...(Array.isArray(model.parameters) ? { parameters: model.parameters } : {}),
          })),
      });
    } catch (error) {
      emit({ type: "models_error", id, ...errorFields(error) });
    }
  }

  async function shutdown() {
    if (closed) {
      return;
    }
    await cancelActive();
    settlePendingCalls(undefined, "Not run: the bridge is shutting down.");
    settling = settling.then(disposeAgent).then(dropPrewarm);
    await settling;
    closed = true;
    onShutdown();
  }

  /**
   * Handles one message from the editor. Returns a promise for tests; the caller does not need
   * to wait for it, and must not: a `send` stays pending for the whole turn.
   */
  async function handle(message) {
    if (closed || !isRecord(message)) {
      return;
    }
    try {
      switch (message.type) {
        case "init":
          await handleInit(message);
          break;
        case "tools":
          tools = normalizeTools(message.tools);
          break;
        case "send":
          await handleSend(message);
          break;
        case "cancel":
          if (active && (message.id === undefined || message.id === active.id)) {
            await cancelActive();
          }
          break;
        case "tool_result": {
          const pending = pendingCalls.get(message.callId);
          if (pending) {
            pendingCalls.delete(message.callId);
            pending.resolve({
              content: [{ type: "text", text: typeof message.text === "string" && message.text !== "" ? message.text : "(no output)" }],
              isError: message.isError === true,
            });
          }
          break;
        }
        case "reset":
          if (active) {
            await cancelActive();
          }
          settling = settling.then(disposeAgent);
          await settling;
          break;
        case "list_models":
          await handleListModels(message);
          break;
        case "shutdown":
          await shutdown();
          break;
        default:
          log("warn", `A message of unknown type '${String(message.type)}' was ignored.`);
          break;
      }
    } catch (error) {
      log("error", `Handling '${String(message.type)}' failed: ${describeError(error)}`);
    }
  }

  return { handle, shutdown };
}
