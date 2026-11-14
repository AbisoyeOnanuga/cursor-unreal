---
name: unreal-niagara
description: >-
  Create and inspect Niagara systems with niagara_* tools — emitters, user
  parameters, compile, and level spawn. Use for VFX setup when the Niagara pack
  is loaded.
---

# Unreal Niagara

## Tools

- `niagara_create_system` — new system; optional template emitter names or paths
- `niagara_get_info` — emitters, modules (shallow), readiness
- `niagara_add_emitter` / `niagara_remove_emitter` / `niagara_set_emitter_enabled`
- `niagara_list_parameters` / `niagara_add_user_parameter` / `niagara_set_parameter`
- `niagara_compile` — rebuild; `save: true` writes the package
- `niagara_spawn_in_level` — place a Niagara actor with the system assigned

## Workflow

1. Create with `niagara_create_system` under `/Game/FX/...`.
2. Prefer shipped template emitter names when you only need a starting stack.
3. Inspect with `niagara_get_info` before changing parameters.
4. Set existing User parameters with `niagara_set_parameter`; add new ones only when needed.
5. `niagara_compile`, then `asset_save` or `save: true` when the user wants disk writes.

## Limits

Adding or rewiring individual modules in the stack is out of scope — say so and use `python_exec` only if the user accepts that path.
