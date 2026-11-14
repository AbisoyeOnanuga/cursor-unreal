---
name: unreal-cpp-gameplay
description: >-
  Author Unreal C++ gameplay classes with cpp_create_class, cpp_build, and the
  native-only source_read / source_search / source_write tools. Use for Actors,
  Components, and gameplay systems that belong in C++ rather than Blueprint.
---

# Unreal C++ gameplay

## Tools

- `cpp_create_class` — writes a `.h`/`.cpp` pair into an existing **project** module under `Source/` (not plugin modules).
- `cpp_build` — Live Coding when available, otherwise a UBT compile; returns parsed errors/warnings.
- `source_read` / `source_search` / `source_write` — **NativeAgentOnly** (in-editor Anthropic path). Cursor SDK agents should use Cursor's own file tools instead.
- `source_write` only under project/plugin `Source/` trees; backups go to `Saved/CursorUnreal/Backups`. It refuses this plugin's own sources and `Content/`.

## Workflow

1. Read the module layout (`source_search` / list `Source/`) before creating a class.
2. `cpp_create_class` with a clear name and native parent (`Actor`, `ActorComponent`, …). Prefer Blueprintable when designers need a BP child.
3. Edit with `source_write` (or Cursor file tools on the Cursor provider). Keep headers minimal; put includes in the `.cpp` when possible.
4. `cpp_build`. Fix reported errors before adding Blueprints that depend on the class.
5. Expose tunable values as `UPROPERTY(EditAnywhere, BlueprintReadWrite)` and events as `UFUNCTION(BlueprintCallable)` / `BlueprintImplementableEvent` when a BP child will use them.

## Conventions

- Distances in centimetres, rotations pitch/yaw/roll in degrees.
- Do not enable Tick unless needed; turn `PrimaryActorTick.bCanEverTick` off when idle.
- Prefer `TObjectPtr`, `UPROPERTY` on UObject references, and engine smart pointers for non-UObject ownership.
