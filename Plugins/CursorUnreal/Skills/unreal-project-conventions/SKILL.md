---
name: unreal-project-conventions
description: >-
  Shared Unreal project conventions for CursorUnreal agents: Content layout,
  naming, units, save discipline, and when to prefer C++ vs Blueprint vs Niagara.
  Use on every non-trivial edit so work matches the rest of the project.
---

# Project conventions

## Content layout

- Game assets under `/Game/...` (for example `/Game/Blueprints`, `/Game/Materials`, `/Game/FX`, `/Game/Audio`).
- Prefer creating folders that already exist in the project over inventing new top-level trees.
- Never write into `/Engine/...` or this plugin's own packages.

## Naming

- Blueprints: `BP_Name`
- Animation Blueprints: `ABP_Name`
- Materials / instances: `M_Name` / `MI_Name`
- Niagara systems: `NS_Name`
- Sound cues: `SC_Name`
- C++ types: `A`/`U`/`F` prefixes; no spaces or leading digits

## Units and transforms

- Distance: centimetres. Rotation: pitch, yaw, roll in degrees. Scale: 1 = 100%.

## Edit discipline

1. Inspect with `*_get_info` / `asset_get_info` before mutating.
2. Compile Blueprints and materials after structural edits; report errors, do not claim success.
3. Keep work in memory until the user wants disk writes, then `asset_save`.
4. Prefer one pack's tools over `python_exec` or `console_command`.

## C++ vs Blueprint vs FX

- Hot paths, replication, and reusable systems → C++ (`cpp_*` / `source_*`).
- Tuning, designer hooks, and high-level flow → Blueprint (`blueprint_*`).
- Particles → Niagara pack when available; do not fake FX with Material tricks unless asked.
