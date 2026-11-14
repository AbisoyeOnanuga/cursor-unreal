# Getting started

## Install into a project

1. Copy `Packaged/<EngineVersion>/CursorUnreal/` into your project’s `Plugins/` folder (or enable this repo’s `Plugins/CursorUnreal`).
2. Open the `.uproject` in the matching Unreal Editor and rebuild when prompted.
3. Click **Agent** on the level editor toolbar, or Tools → **Cursor Unreal**.

### Cursor provider bridge

```powershell
cd Plugins/CursorUnreal/Bridge
npm install
```

Then choose Provider **Cursor** on the Setup tab and store your API key.

### Anthropic provider

Choose Provider **Anthropic**, store the key on Setup. No Node install required.

## First agent turn (smoke)

1. Open any map.
2. Chat: ask for the open level context (the agent should call `editor_get_context`).
3. Ask it to spawn a cube and move it one metre on X (`actor_spawn` / `actor_update`, centimetres).
4. Open **Activity** to see the tool trail; **Performance** for timings.

## Skills for Cursor IDE

In the Skills tab, **Install all into this project**. That copies `Plugins/CursorUnreal/Skills/*` into `<Project>/.cursor/skills/` so Cursor IDE / Cursor SDK agents can load the same guides.

## MCP (optional)

Not required for shipping the plugin. When you want Cursor IDE as a client:

1. Start the editor so the MCP server is listening.
2. On the Setup tab, **Add to my Cursor config** (writes the `cursor-unreal` entry into `~/.cursor/mcp.json`), or **Copy config** and paste it yourself. The entry contains this editor's access token.
3. Reload MCP servers in Cursor and list tools.

See [Providers](Providers.md) and [measurements/phase1-mcp-latency.md](measurements/phase1-mcp-latency.md).

## Package a release yourself

```powershell
./Scripts/Build-Plugin.ps1 -Mode Package
```

Output: `Packaged/<EngineVersion>/CursorUnreal/`. Confirm `Skills/` and `Bridge/` (without `node_modules`) are present after packaging.
