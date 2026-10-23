# Modding

The game's models can be replaced with your own, in glTF (`.glb` or `.gltf`) from Blender
or any other 3D tool. The project ships no mods and no game content: you make your own,
from your own copy of the game. Only use models you have the right to use and share (see
[Where to find models](#where-to-find-models)).

## How it works

Mods live in a `mods/` folder next to the `game` folder (beside `save/` and `cache/`). Each
mod is a folder with a `mod.toml` and its models:

```
mods/
  my_bear/
    mod.toml
    bear.glb
```

Every mesh the game draws has a **mesh ID**, a hash of its vertex data and the part of it
that's drawn, so it's the same in every level and every run. A mod lists the mesh IDs it
replaces and the model to draw instead.

A replacement is drawn with the game's own shaders, so it gets the game's lighting, fur and
effects. It doesn't need its own skeleton: each vertex of your model takes its bone
weights (and colours and tangents) from the nearest vertex of the game's mesh, so it
animates with the game's skeleton. That works best when your model is shaped and posed like
the original: same size, a T-pose with the arms where the original's arms are, and so on.
Position, normals and texture coordinates come from your model.

## Making a mod

### 1. Find the mesh

Export the meshes drawn in a frame where the character is on screen:

```bash
NB_MOD_EXPORT=6600 ./build/game/NaughtyBear
```

Each mesh of that frame is written to `mods/_export/frame6600/<mesh ID>.glb`, with a list in
`meshes.txt`: vertex and triangle counts, whether it's skinned (animated by a skeleton),
its size and its textures. Characters are the skinned ones; open their `.glb` files to see
which is which.

In our testing, Naughty's default costume is mesh `a62ef2ebbde0f1ab` (2520 vertices). Other
costumes and characters have their own IDs.

These exports come from your copy of the game. Use them as references and don't
redistribute them.

### 2. Line your model up

Import the exported mesh into Blender (File > Import > glTF 2.0). It's in the game's bind
pose: Naughty stands in a T-pose, Y up in glTF (so upright in Blender), facing +Z.

Import your model, then scale, rotate and pose it over the reference until the two
overlap: head over head, arms along the arms, feet on the feet. Then delete the reference
and export your model (File > Export > glTF 2.0, `.glb`) into your mod's folder. Its
armature and skin weights, if it has any, are ignored.

To try a model without Blender, `fit = "bounds"` (below) scales and centres it on the game
mesh automatically. It doesn't rotate the model, so the model must already be upright and
facing the same way.

### 3. Write mod.toml

```toml
name = "My bear"

[[mesh]]
replace = "a62ef2ebbde0f1ab"   # a mesh ID, or a list: ["...", "..."]
model = "bear.glb"
texture_slot = 1               # Naughty's colour texture is slot 1
```

Then start the game. The log shows what each mod loaded and when a replacement is first
drawn.

## mod.toml reference

| Key | Default | Meaning |
|-|-|-|
| `name` | folder name | Shown in the log |
| `[[mesh]]` | | One entry per replacement; a mod can have any number |
| `replace` | | Mesh ID(s) from `NB_MOD_EXPORT`, as a string or a list of strings |
| `model` | | `.glb`/`.gltf` file in the mod's folder; all its meshes are drawn as one |
| `texture_slot` | `0` | Which of the game mesh's textures (the numbers in `meshes.txt`) your model's base colour texture replaces; `-1` keeps the game's textures |
| `fit` | `"none"` | `"bounds"`: scale and centre the model onto the game mesh |
| `scale`, `rotate`, `offset` | `1`, `[0, 0, 0]`, `[0, 0, 0]` | Applied to the model in that order; `rotate` is degrees about X, Y, then Z |
| `hide` | `false` | `true`: draw nothing for these meshes (no `model` needed) |

When two mods replace the same mesh, the one whose folder name sorts last wins.

## Limits

- Only the model's base colour texture is used (PNG or JPEG, embedded or next to the
  file). The game's other textures for the mesh (fur, normal maps, etc.) stay, which is
  why replaced characters still look furry.
- The skeleton is the game's: a model can't add bones or animations, and parts far from
  any original vertex follow whatever bone is nearest.
- The first time a replacement is drawn, its weights are matched to the game mesh, which
  can take a moment for large models.
- Menus and HUD (drawn by Scaleform) can't be replaced this way.

## Where to find models

Use models you made, or ones whose license lets you change and share them. CC0 is the
easiest; CC-BY needs credit; avoid "no derivatives" licenses. Good sources include
[Quaternius](https://quaternius.com), [Kenney](https://kenney.nl),
[Poly Pizza](https://poly.pizza), [Poly Haven](https://polyhaven.com),
[OpenGameArt](https://opengameart.org) and [Sketchfab](https://sketchfab.com) (filter by
downloadable and check each license). Don't use models taken from other games.
