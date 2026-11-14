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

1. Look first: `blueprint_get_info` and `blueprint_get_nodes` with `pins: true`.
2. Prefer a C++ parent for performance-critical logic; use Blueprint for tuning and high-level flow.
3. Add an event (`BeginPlay`, `Tick`) only once — a second call returns the existing node.
4. For PrintString and other library calls: `kind: call_function`, `class: KismetSystemLibrary`, `function: PrintString`.
5. Wire execution pins (`then` → `execute`) and data pins by the names `blueprint_get_nodes` reports.
6. `blueprint_compile`, then fix errors before claiming done. Save with `asset_save` when the user wants it on disk.

## Limits

Complex custom K2 nodes, AnimBP-specific graphs, and Widget Blueprint designer layout are out of scope — use `python_exec` only when no other tool fits, and say so.
