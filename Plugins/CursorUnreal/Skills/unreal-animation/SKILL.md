---
name: unreal-animation
description: >-
  Create and inspect Animation Blueprints with animbp_create, animbp_get_info,
  animbp_list_graphs, animbp_list_nodes, and animbp_compile. Use for AnimBP
  scaffolding against a skeleton, not deep pose-graph authoring.
---

# Unreal animation

## Tools

- `animbp_create` — new AnimBP for a `skeleton` (optional `parent_class`, `preview_mesh`)
- `animbp_get_info` — skeleton, parent, graphs, compile status
- `animbp_list_graphs` / `animbp_list_nodes` — inspect graphs and nodes
- `animbp_compile` — compile and report errors

## Workflow

1. Resolve the skeleton with `asset_search` before creating.
2. `animbp_create` with path `/Game/.../ABP_Name` and the skeleton path.
3. Inspect with `animbp_list_nodes`, then `animbp_compile`.
4. Save with `asset_save` when the user wants it on disk.

## Limits

State machines, blend spaces, and AnimGraph node authoring are out of scope — use `python_exec` only when no other tool fits, and say so.
