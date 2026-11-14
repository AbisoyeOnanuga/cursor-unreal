# Writing a skill

Skills are Agent Skill folders with a `SKILL.md` (YAML frontmatter + Markdown body). CursorUnreal ships them under `Plugins/CursorUnreal/Skills/`. The same format works in Cursor IDE.

## Frontmatter

```yaml
---
name: unreal-example
description: >-
  One or two sentences: what tools it covers and when the agent should load it.
---
```

`name` is the id passed to `load_skill`. `description` is what the agent sees in the skill index — make it specific enough to trigger on the right tasks.

## Body

- List the **real** tool names this skill expects.
- Give a short workflow (inspect → mutate → compile/save).
- State hard limits so the agent does not invent unsupported APIs.

## Worked example

`Plugins/CursorUnreal/Skills/unreal-example/SKILL.md`:

```markdown
---
name: unreal-example
description: >-
  Place a StaticMeshActor at a point with actor_spawn and actor_update.
  Use for a one-asset layout smoke check.
---

# Example placement

## Tools

- `asset_search` — find the mesh
- `actor_spawn` / `actor_update` — place and move it
- `selection_set` — select the result for the user

## Workflow

1. `asset_search` for the mesh path.
2. `actor_spawn` with that class/asset and a label.
3. `actor_update` location in centimetres.
4. `selection_set` so the viewport focuses it.

## Limits

Do not delete other actors. Prefer project content under `/Game`.
```

## Layers and overrides

| Layer | Location | Who sees it |
|-------|----------|-------------|
| Built-in | `Plugins/CursorUnreal/Skills/<name>/` | In-editor agent (and Cursor after install) |
| User | `~/.cursor/skills/<name>/` | Cursor; in-editor only if enabled in Setup |
| Project | `<Project>/.cursor/skills/<name>/` | Team + Cursor IDE / Cursor SDK |

Higher layers override lower ones with the same `name`. Use **Install all into this project** on the Skills tab so Cursor IDE / Cursor SDK agents can read them. The installer never overwrites a project skill that was edited after install.

### Customise one skill

1. Skills tab → **Customise** on a built-in (copies into the project layer).
2. Edit the project copy’s `SKILL.md`.
3. Refresh — the project version wins for that name.
