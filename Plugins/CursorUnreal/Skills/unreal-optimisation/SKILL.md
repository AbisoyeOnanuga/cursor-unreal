---
name: unreal-optimisation
description: >-
  Read-only content cost checks with optim_level_stats, optim_list_heavy_actors,
  optim_mesh_info, and optim_suggest_lods. Use when hunting heavy StaticMesh
  actors or missing LODs.
---

# Unreal optimisation

## Tools

- `optim_level_stats` — summary counts for the open level
- `optim_list_heavy_actors` — actors ranked by estimated mesh cost
- `optim_mesh_info` — LOD count and triangles per LOD for one mesh
- `optim_suggest_lods` — LOD advice for a StaticMesh; does not build LODs

## Workflow

1. Start with `optim_level_stats`, then `optim_list_heavy_actors`.
2. Drill into offenders with `optim_mesh_info` / `optim_suggest_lods`.
3. Fix LODs in the Static Mesh Editor or your pipeline; re-check after changes.

## Limits

These are content-side estimates, not GPU counters. Skeletal meshes, Niagara, and UI overdraw need other tools.
