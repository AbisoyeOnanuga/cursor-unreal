---
name: unreal-blueprints
description: >-
  Create and edit Unreal Blueprints with the CursorUnreal blueprint_* tools —
  parent class, variables, components, common graph nodes, pin wiring, and
  compile. Use when the user asks for Blueprint gameplay, UI logic in BP, or
  to inspect or change an existing Blueprint graph.
---

# Unreal Blueprints

## Tools

- `blueprint_create` — new asset under `/Game/...` from a parent class (`Actor`, `Pawn`, a C++ class, another Blueprint).
- `blueprint_get_info` / `blueprint_list_graphs` / `blueprint_get_nodes` — inspect before editing.
- `blueprint_add_variable`, `blueprint_add_component`, `blueprint_add_function`
- `blueprint_add_node` — `event`, `custom_event`, `call_function`, `variable_get`/`set`, `branch`, `sequence`, `self`
- `blueprint_connect_pins`, `blueprint_set_pin_default`, `blueprint_remove_node`
- `blueprint_compile` — always compile after structural changes; report status and errors
- `asset_save` — Blueprints stay in memory until saved

## Workflow

1. Look first: `blueprint_get_info` and `blueprint_get_nodes` (pins are included unless you pass `include_pins: false`).
2. Prefer a C++ parent for performance-critical logic; use Blueprint for tuning and high-level flow.
3. Add an event (`BeginPlay`, `Tick`) only once — a second call returns the existing node.
4. For PrintString and other library calls: `kind: call_function`, `class: KismetSystemLibrary`, `function: PrintString`.
5. Wire execution pins (`then` → `execute`) and data pins by the names `blueprint_get_nodes` reports.
6. `blueprint_compile`, then fix errors before claiming done. Save with `asset_save` when the user wants it on disk.
7. Moving platforms, triggers, pressure plates, kill zones, jump pads and goals have tested recipes: `load_skill` `unreal-level-design` with `file` `gameplay-kit.md`. A playable character with keyboard and gamepad controls: the same skill, `file` `player.md`.

## Pin values

`blueprint_set_pin_default` reports the value as stored; read that, not what you sent.

- Key pins take a plain key name: `D`, `SpaceBar`, `LeftShift`, `Gamepad_LeftX`, `Gamepad_FaceButton_Bottom`. A name that is not a key is refused with the close matches.
- Vectors: `(X=1,Y=0,Z=0)` or `1,0,0`. Rotators: `(Pitch=0,Yaw=90,Roll=0)`. Other structs: the engine's text form, `(X=3,Y=4)` for a Vector2D; text the struct cannot read is refused.
- Object and class pins take an asset path or class name.
- A pin reported as hidden cannot be set or wired; the function is meant for a different node. Use another function.

## Player input

Read keys from the player controller: `IsInputKeyDown`, `WasInputKeyJustPressed`, `GetInputAnalogKeyState` (class `PlayerController`), with the controller from `GameplayStatics` `GetPlayerController`. `GetInputAxisValue` and `GetInputAxisKeyValue` return 0 outside the editor's own input nodes and are refused. Do not edit the project's input settings to make controls work.

## When a function is not found

The error names the class that does have it, says when the name is a property rather than a function, and lists similar functions. Use those before guessing another name.

## Limits

- No input-event nodes, casts, timelines, events bound to a component, or nodes that get or set a property on another object. The Level Blueprint is not reachable.
- You cannot start Play or see the game. After `blueprint_compile`, check your work with `blueprint_get_nodes`, and say that it has not been played.
- Complex custom K2 nodes, AnimBP-specific graphs, and Widget Blueprint designer layout are out of scope — use `python_exec` only when no other tool fits, and say so.
