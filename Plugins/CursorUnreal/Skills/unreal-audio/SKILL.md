---
name: unreal-audio
description: >-
  Build and preview SoundCues with sound_create_cue, attenuation helpers,
  sound_play / sound_stop, and sound_spawn_ambient. Use for cue setup from waves,
  not MetaSound graphs.
---

# Unreal audio

## Tools

- `sound_create_cue` — new cue from `waves` (optional volume, pitch, looping, attenuation)
- `sound_get_info` — duration, multipliers, nodes, attenuation
- `sound_create_attenuation` / `sound_configure_attenuation` / `sound_set_attenuation`
- `sound_play` / `sound_stop` — editor preview
- `sound_spawn_ambient` — place an ambient sound actor in the level

## Workflow

1. Find or import SoundWaves, then `sound_create_cue` with those paths.
2. Create or configure attenuation assets when distance falloff matters.
3. Preview with `sound_play` before placing actors; stop with `sound_stop`.
4. `asset_save` when the user wants assets on disk.

## Limits

MetaSound authoring is out of scope — keep cues practical or use `python_exec` with an explicit note.
