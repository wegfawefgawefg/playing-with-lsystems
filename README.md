# playing-with-lsystems

reading through algorithmic beauty of plants

## Build and run

```sh
./scripts/run.sh examples/abop-page-27-inspired.lsys
```

The simpler starter example is:

```sh
./scripts/run.sh examples/plant.lsys
```

VS Code F5 uses the release build and opens `examples/abop-page-27-inspired.lsys`. The debug launch
target uses `LSYSTEMS_PRESET=dev` and writes to `build-debug/`.

## L-system files

The first file format is intentionally plain text:

```text
axiom: X
iterations: 6
seed: 1
angle: 25
step: 7
start_angle: -90
angle_jitter: 0
step_jitter: 0

rule X: F+[[X]-X]-F[-FX]+X
rule F: FF
```

`F` and `G` draw forward, `f` moves without drawing, `+`, `-`, `&`, `^`, `/`, and `\` turn by
`angle` in 2D, `|` turns around, and `[` / `]` push and pop turtle state. The app reloads the file
while it is running, so edits to the `.lsys` file update the rendered 2D result automatically.

Multiple rules with the same predecessor are stochastic alternatives. The numeric value is treated
as a weight, so ABOP-style probabilities work when they sum to `1`:

```text
rule F 0.33: F[+F]F[-F]F
rule F 0.33: F[+F]F
rule F 0.34: F[-F]F
```

This also works:

```text
rule F(0.33): F[+F]F[-F]F
rule F 0.33 -> F[+F]F
```

Use `seed` to get repeatable stochastic expansions.

For leaves and other filled shapes, `{` starts a polygon and `}` closes it. Movement inside the
braces records the polygon boundary, so the ABOP leaf style works:

```text
rule L: {-f+f+f-|-f+f+f}
```

`.` is also supported as an explicit "record current vertex" command.

The current color table is built into the renderer: a muted blue background, dark-green stems,
green leaf polygons, and warm white flower polygons. Inside rules, `'` increments the color index
and `` ` `` decrements it, matching the basic ABOP color-table idea.

For per-instance variation, `~` jitters the current turtle angle by up to `angle_jitter` degrees and
`;` multiplies the current step size by a random factor in `1 +/- step_jitter`. Put them inside
brackets when you want the variation scoped to one leaf or flower.

The renderer also applies a small deterministic "wet paint pull" pass after drawing. It renders the
plant to an offscreen texture, reads the pixels back, classifies non-background pixels as stem, leaf,
or flower by palette color, and pulls short vertical gradients downward from sampled plant pixels.

To only generate the expanded command string:

```sh
./build/lsystems --print examples/plant.lsys
```
