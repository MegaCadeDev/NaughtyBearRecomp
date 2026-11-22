# Releasing

`tools/package-release.sh` builds the PC, PS5 and Switch 2 versions from a clean recompile
and packages them in `out/release`:

| File | Contents |
|-|-|
| `NaughtyBearRecomp-<version>-linux-x64.tar.gz` | The PC version: `NaughtyBear`, `libdxcompiler.so`, README, NOTICE, modding guide, an empty `game` folder |
| `NaughtyBearRecomp-<version>-ps5-PPSA99862.zip` | The PS5 title folder, for `/data/homebrew/PPSA99862` |
| `NaughtyBearRecomp-<version>-switch2.zip` | The Switch 2 NRO in its folder, for the SD card's `/switch` |
| `SHA256SUMS`, `RELEASE_NOTES.md` | Checksums, and the release's description |

No package contains the game's files. The script fails if one holds an executable,
package, shader cache, save or mod. Players copy in their own game.

```bash
tools/package-release.sh v1.0.0
```

`--pc-only`, `--ps5-only` or `--switch2-only` builds one of them.

## The release workflow

[`.github/workflows/release.yml`](../.github/workflows/release.yml) runs the script and puts
the packages in a **draft** release. Check the draft and publish it from the Releases page.

It runs on a self-hosted runner, so building happens on your own machine. GitHub's own
machines would need the game's `Default.xex` uploaded to them; this way it stays on yours.

### Setting up the runner (once)

1. On GitHub: **Settings → Actions → Runners → New self-hosted runner**, Linux x64. Follow
   its download and `./config.sh` steps on the build machine. When it asks for labels,
   add `naughtybear`.
2. Start it with `./run.sh`, or install it as a service with `sudo ./svc.sh install && sudo
   ./svc.sh start`.
3. **Settings → Secrets and variables → Actions → Variables** gets two repository
   variables:
   - `NB_GAME_DIR`: the folder with the game's `Default.xex` on that machine, for example
     `/home/you/NaughtyBearRecomp/game/Naughty_Bear`.
   - `NB_PS5_DEPS` (optional, recommended): a folder that keeps the PS5 build's dependencies
     between runs, for example `/home/you/.cache/nb-ps5-deps`. Without it, every release
     rebuilds them (RADV, FFmpeg, DXC: an hour or more). You can copy an existing `ps5/.deps`
     there to start.
4. The machine needs what the PC, PS5 and Switch 2 builds need already (see the main README,
   `ps5/README.md` and `switch2/README.md`), plus `zip` and Docker.

The workflow only starts from a pushed tag or by hand, never from pull requests, so other
people's code doesn't run on your machine. Keep it that way: don't add `pull_request`
triggers to a workflow that runs on this runner.

### Making a release

```bash
git tag v1.0.0
git push origin v1.0.0
```

Or use **Actions → Release → Run workflow** and enter the version. The draft appears under
Releases when the run finishes.
