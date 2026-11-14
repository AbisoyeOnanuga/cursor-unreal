---
name: unreal-project-conventions
description: >-
  This project's own rules: asset naming, Content folder layout, C++ coding
  standard, and module boundaries. Load it before creating, naming, moving or
  restructuring anything. Ships as a template: sections still marked TODO have
  not been decided by the project yet.
---

# Project conventions

<!--
  THIS IS A TEMPLATE. To make it yours:
  1. In the editor: Cursor Unreal panel > Skills > "Customise" on this skill.
     That copies it to <Project>/.cursor/skills/unreal-project-conventions/ and opens the copy.
  2. Replace each "TODO(project)" line with your rule. Delete sections you do not need.
  3. Commit the copy so the team and Cursor IDE agents share it. Your copy is never overwritten.
-->

## How the agent uses this file

- A line starting `TODO(project)` is a rule the project has not written down. Do not treat the
  example after it as a rule.
- Where a section is still TODO, match what the project already does: look with `asset_search`
  (assets) or `source_search` (C++) for the nearest existing example and copy its pattern.
- If nothing comparable exists and the choice would be hard to change later (a new top-level
  folder, a new module, a naming prefix), ask the user before creating it.
- Filled-in rules here win over the general advice in the other `unreal-*` skills.

## Naming

TODO(project): asset prefixes. Example: `BP_` Blueprint, `ABP_` Animation Blueprint, `M_` / `MI_`
material / instance, `NS_` Niagara system, `SC_` sound cue, `T_` texture, `SM_` / `SK_` static /
skeletal mesh.

TODO(project): suffixes and variants. Example: `T_Rock_D` / `_N` / `_ORM`; `_01`, `_02` for variants.

TODO(project): actor labels and World Outliner folders. Example: labels match the asset name
without the prefix; gameplay actors go in the `Gameplay` folder, lighting in `Lighting`.

TODO(project): C++ names beyond Unreal's `A`/`U`/`F`/`E`/`I` prefixes. Example: all project
types carry a `My` prefix (`AMyCharacter`).

## Folder layout

TODO(project): Content tree. Example:

```text
/Game/Maps            levels
/Game/Blueprints      gameplay Blueprints, one subfolder per feature
/Game/Characters      one subfolder per character: meshes, anims, ABP
/Game/Materials       shared materials and functions
/Game/FX              Niagara systems and their textures
/Game/Audio           cues, attenuation, source wavs
/Game/Developers      personal scratch, never referenced by shipped content
/Game/Assets          level design assets
```

TODO(project): where agent-made test or scratch assets go. Example: `/Game/Assets`.

TODO(project): Source tree. Example: `Source/<Module>/Public` for headers other modules include,
`Private` for everything else, one subfolder per feature.

## Coding standard

TODO(project): the standard to follow. Example: Epic's C++ coding standard, with tabs, `b` prefix
on bools, and no `auto` except for lambdas and iterators.

TODO(project): Blueprint rules. Example: no logic in the Level Blueprint; functions over long
event graphs; every variable has a category and a tooltip.

TODO(project): what must be C++ and what may be Blueprint. Example: replication, tick-heavy
code and shared systems in C++; tuning values, designer hooks and one-off level logic in Blueprint.

TODO(project): logging and asserts. Example: one log category per module; `ensure` for
recoverable problems, `check` only for programmer errors.

## Module boundaries

TODO(project): the modules and what each owns. Example:

| Module | Owns | May depend on |
| --- | --- | --- |
| `MyGame` | gameplay classes | `MyGameCore` |
| `MyGameCore` | shared types, interfaces | engine only |
| `MyGameEditor` | editor-only tools | `MyGame`, `MyGameCore` |

TODO(project): rules for crossing them. Example: runtime modules never include editor headers;
no circular dependencies; new dependencies go in `Build.cs` and get mentioned in the summary.

TODO(project): places the agent must not edit. Example: `Plugins/ThirdParty`, generated code,
anything under `/Game/Vendor`.

## Always true, whatever the project decides

- Never write into `/Engine/...` or another plugin's content.
- Units: centimetres; rotation is pitch, yaw, roll in degrees; scale 1 is 100%.
- Look before changing: `asset_get_info`, `blueprint_get_info`, `material_get_info` and the
  other `*_get_info` tools are read-only.
- Compile after structural edits (`blueprint_compile`, `material_compile`, `animbp_compile`,
  `niagara_compile`) and report errors as errors.
- Edits stay in memory until saved. Call `asset_save` only when the user wants them on disk,
  and name the paths rather than using `all_dirty`.
