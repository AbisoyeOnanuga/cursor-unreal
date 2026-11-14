---
name: unreal-materials
description: >-
  Build Unreal materials and instances with material_create, expression graph
  tools, material_set_parameter, and material_compile. Use for shading setup,
  simple master materials, and cheap instance variants.
---

# Unreal materials

## Tools

- `material_create` / `material_create_instance`
- `material_get_info` — expressions, parameters, connected inputs
- `material_add_expression` / `material_remove_expression` / `material_connect`
- `material_set_parameter` — defaults on a material, overrides on an instance
- `material_compile` — rebuild shaders; `save: true` writes the package
- `asset_save` — alternative save path

## Workflow

1. Prefer a **master material + instances** over many unique materials.
2. Add parameters (`ScalarParameter`, `VectorParameter`, `TextureSampleParameter2D`) for anything artists will tune.
3. Connect expression **names** from `material_get_info` / `material_add_expression` into material inputs (`BaseColor`, `Metallic`, `Roughness`, `Normal`, `EmissiveColor`, …) or into other expression inputs (`A`, `B`).
4. `material_compile` after graph or default changes. Check the `errors` array.
5. Create instances with `material_create_instance`, then override with `material_set_parameter`.

## Limits

Material functions, layers, and custom HLSL nodes are out of scope for these tools — say so and use `python_exec` only if the user accepts that path.
