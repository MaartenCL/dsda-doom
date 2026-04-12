# Viewcam Script

Use `-viewcam_script <path>` to load a scripted playback camera timeline.

- The script is only evaluated during demo playback.
- Each non-empty, non-comment line defines one instruction.
- Tic ranges are inclusive (`first_tic` through `last_tic`).
- If ranges overlap, the last matching line in the file wins.
- Outside scripted ranges, normal player view is used.
- Pitch is always `0`.

## Line Format

General form:

`first_tic last_tic action ...params... [orientation]`

Comments:

- Full-line comments: start with `#`
- Inline comments: any token starting with `#` ends parsing for the line

## Actions

### Static

`first last static x y z angle`

Camera remains fixed between `first` and `last`.

### Linear

`first last linear x1 y1 z1 a1 x2 y2 z2 a2 [orientation]`

- Position interpolates linearly from point 1 to point 2.
- Angle is interpolated from `a1` to `a2`.
- `orientation` is optional and defaults to `absolute`.
- Allowed orientations:
   - `absolute`: use interpolated angle directly
   - `movement`: camera heading follows movement direction, with `a1..a2` used as angle offsets

### Arc

`first last arc cx cy radius z1 z2 rot_start rot_delta a1 a2 [orientation]`

- Camera moves on a circle centered at `(cx, cy)` with `radius`.
- Rotation in degrees starts at `rot_start` and progresses by `rot_delta` over the range.
- `z` interpolates from `z1` to `z2`.
- `a1..a2` are interpolated angle values.
- `orientation` is optional and defaults to `absolute`.
- Allowed orientations:
   - `absolute`: use interpolated angle directly
   - `movement`: heading follows tangent direction, plus `a1..a2` offset
   - `center`: heading points toward circle center, plus `a1..a2` offset

## Example

```text
# Static opening shot
0 175 static 1024 2048 64 90

# Linear fly-through, look along movement
176 700 linear 1024 2048 64 0 2048 2048 96 0 movement

# Orbit around arena center while looking at center
701 1400 arc 1536 1536 512 96 96 0 360 0 0 center
```

## Errors

Script parsing is strict.

- Malformed lines are fatal.
- Errors include file path and line number.
