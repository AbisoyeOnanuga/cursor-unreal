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
   Building or blocking out a level, or adding gameplay pieces to one: load `unreal-level-design` first for sizes and recipes.
4. Batch related property changes in one `object_set_properties` call.
5. Save only what you changed (`asset_save` with paths, or `save: true` on tools that support it).
6. Prefer pack tools over `python_exec`. If you must use Python, keep scripts short and say why.

## Addressing objects

The `object` argument of `object_get_properties` / `object_set_properties` takes any of these, so class defaults and level settings do not need `python_exec`:

- An actor by label or name; a component as `Label.ComponentName`.
- `WorldSettings` — the open level's settings actor, whatever the level calls it (game mode override, kill Z).
- An asset path: `/Game/Folder/Asset`.
- A Blueprint's class defaults: `/Game/Folder/BP_X.Default__BP_X_C`. A native class's: `/Script/Engine.Default__Character`.
- A component's defaults on a Blueprint: `/Game/Folder/BP_X.BP_X_C:Mesh_GEN_VARIABLE` for one the Blueprint added (after a compile), `/Game/Folder/BP_X.Default__BP_X_C:CharMoveComp` for one from the C++ parent. `...Default__BP_X_C.Mesh` and `...BP_X_C:Mesh` reach the same objects.
- Project settings: `/Script/EngineSettings.Default__GameMapsSettings` and the like.

A property name can go through a struct (`RelativeLocation.Z`) or through something the object owns (`CharacterMovement.MaxWalkSpeed` on a character). It cannot go through a reference to a shared asset; the error gives the asset's own path to use instead.

## Limits

Do not mass-delete Content or run destructive console commands unless the user asked. Engine/plugin assets are read-only for write tools.
