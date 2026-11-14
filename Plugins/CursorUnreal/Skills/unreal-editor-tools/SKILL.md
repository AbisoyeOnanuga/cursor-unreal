---
name: unreal-editor-tools
description: >-
  Drive the Unreal Editor with editor_*, actor_*, selection_*, object_*,
  asset_*, and console_command tools. Use for level layout, selection,
  property edits, asset housekeeping, and light scripting via python_exec.
---

# Unreal editor tools

## Tools

- `editor_get_context` — map, selection, play state, engine version
- `actor_list` / `actor_get` / `actor_spawn` / `actor_update` / `actor_duplicate` / `actor_delete`
- `selection_set`
- `object_get_properties` / `object_set_properties`
- `asset_search` / `asset_get_info` / `asset_save` / `asset_duplicate` / `asset_rename` / `asset_delete`
- `console_command` — engine console (high risk; prefer narrower tools)
- `python_exec` — last resort when no dedicated tool exists
- `editor_log_tail` — recent log lines after a failed compile or PIE issue

## Workflow

1. Start with `editor_get_context` so paths and selection match the open map.
2. Find assets with `asset_search` before inventing paths.
3. Spawn and place with `actor_spawn` + `actor_update` (location/rotation/scale). Prefer centimetres.
4. Batch related property changes in one `object_set_properties` call.
5. Save only what you changed (`asset_save` with paths, or `save: true` on tools that support it).
6. Prefer pack tools over `python_exec`. If you must use Python, keep scripts short and say why.

## Limits

Do not mass-delete Content or run destructive console commands unless the user asked. Engine/plugin assets are read-only for write tools.
