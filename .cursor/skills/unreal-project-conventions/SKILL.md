---
name: unreal-project-conventions
description: >-
  This project's own rules: asset naming, Content folder layout, C++ coding
  standard, and module boundaries. Load it before creating, naming, moving or
  restructuring anything: it says which folder each kind of asset goes in.
  Folder layout is decided; sections still marked TODO are not.
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

These are decided rules, not examples.

### Three roots, by who made the asset

```text
/Game/Agent/...       everything an agent creates. Real game content, not scratch.
/Game/...             made by people on the team (same type folders, directly under /Game)
/Game/Imported/...    brought in from outside (Fab, Marketplace, vendor packs), one subfolder per source
/Game/Developers/...  personal scratch, never referenced by shipped content
```

- Everything you create goes under `/Game/Agent`, in the type folder from the table below.
  Never create an asset directly under `/Game/<Type>` or under `/Game/Imported`.
- Your own assets under `/Game/Agent` are updated in place. Do not make `_v2` copies of them.
- A team-made asset under `/Game/<Type>` is edited in place, and only when the user names that
  asset or has it selected. Otherwise build something new under `/Game/Agent`.
- An imported asset is never edited. `asset_duplicate` it into the matching `/Game/Agent` folder
  and change the copy.
- Do not move assets between the three roots unless the user asks. Moving is how a person adopts
  an agent-made asset, so it is their call.
- Only when the user says the work is a test or throwaway does it go to
  `/Game/Developers/<user>/` instead.

### Which folder, by what you are making

Paths are relative to `/Game/Agent`. Team-made content uses the same folders under `/Game`.
`<Name>` is the character, prop or feature the asset belongs to, without a prefix.

| You are making | Unreal type | Folder |
| --- | --- | --- |
| Level | World (map) | `Maps/` |
| Level prop: the mesh | StaticMesh | `Props/<Name>/` |
| Level prop with behaviour (door, pickup, lever) | Blueprint of Actor | `Props/<Name>/` |
| Material, instance or texture used by one prop | Material, MaterialInstanceConstant, Texture2D | `Props/<Name>/` |
| Character Blueprint | Blueprint of Character (or Pawn) | `Characters/<Name>/` |
| Character mesh, skeleton, physics asset | SkeletalMesh, Skeleton, PhysicsAsset | `Characters/<Name>/Meshes/` |
| Animation Blueprint | AnimBlueprint | `Characters/<Name>/Animations/` |
| Animation for one character | AnimSequence, AnimMontage, BlendSpace, AimOffset | `Characters/<Name>/Animations/` |
| Animation shared by several characters | same types | `Animations/` |
| Rig | ControlRigBlueprint, IKRigDefinition, IKRetargeter | `Characters/<Name>/Rigs/` |
| Material or texture used by one character | Material, MaterialInstanceConstant, Texture2D | `Characters/<Name>/Materials/` |
| Player or AI controller | Blueprint of PlayerController, AIController | `Blueprints/Controllers/` |
| Movement | Blueprint of CharacterMovementComponent or another MovementComponent | `Blueprints/Movement/` |
| Input | InputAction, InputMappingContext (Enhanced Input) | `Input/` |
| Game rules and framework | Blueprint of GameModeBase, GameStateBase, PlayerState, HUD, GameInstance | `Blueprints/Framework/` |
| Gameplay Blueprint (anything else that acts in the world) | Blueprint of Actor or ActorComponent | `Blueprints/Gameplay/<Name>/` |
| AI | BehaviorTree, BlackboardData, EnvQuery | `AI/` |
| UI | WidgetBlueprint | `UI/` |
| Material shared across the game | Material, MaterialInstanceConstant, MaterialFunction | `Materials/` |
| Texture shared across the game | Texture2D | `Textures/` |
| FX | NiagaraSystem, NiagaraEmitter, and textures only they use | `FX/` |
| Audio | SoundCue, SoundWave, SoundAttenuation, MetaSoundSource | `Audio/` |
| Data | DataAsset, DataTable, curves, Blueprint enums and structs | `Data/` |

- An asset that belongs to one prop or one character lives with its owner. It moves to the shared
  folder (`Materials/`, `Textures/`, `Animations/`) only once a second owner uses it.
- A folder is created by saving the first asset into it. Do not create empty folders.
- If what you are making is not in the table, use the nearest row and say which one you chose in
  your summary. Do not add a new folder directly under `/Game/Agent` without asking.
- State the full path of every asset you created or changed in your summary.
- C++ (a `UCharacterMovementComponent` subclass, a controller base class) goes in the Source tree
  below, not in Content.

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
