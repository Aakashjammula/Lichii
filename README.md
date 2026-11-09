<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->

<p align="center">
  <img src="dist/branding/lichii_icon_with_logo.png" alt="Lichii" width="600">
</p>

# Lichii

Lichii is an experimental, open-source Nintendo Switch and Switch 2 emulator written in C++. It builds on Eglantine, Eden, yuzu and Sudachi, preserving their contributions while developing this fork's game compatibility and renderer fixes.

This is an independent fork, not an official release of those upstream projects. Lichii is not affiliated with Nintendo.

Visit the [official Lichii website](https://lichii.zortos.me) to find games and compare community compatibility reports.

## This fork: Ocarina of Time on Linux (branch `oot-latest`)

This fork tracks [zortos293/Lichii](https://github.com/zortos293/Lichii) and adds fixes and tuning for **The Legend of Zelda: Ocarina of Time** (`0400486024DEC000`) on **Linux with NVIDIA** laptops. It is tested on an ASUS TUF F15: i7-12700H, RTX 4060 Laptop GPU (8 GB), 16 GB RAM, CachyOS.

Results on that machine, same route in Hyrule Field, both builds PGO:

| Build | Median fps | Worst 10% |
|---|---|---|
| Upstream release, build 117 (PGO) | 46.4 | 31.8 |
| **This branch (PGO)** | **49.9** | **42.5** |

The first visit to a new area, boss or cutscene still stutters while its shaders compile (see *Known issues*).

### Build (Linux, PGO)

Build with upstream's Linux PGO profile, as the release builds do. It needs clang 23 (the profile's LLVM version) and `lld`.

```sh
git clone -b oot-latest https://github.com/Aakashjammula/Lichii.git && cd Lichii
P="-fprofile-use=$PWD/eglantine.profdata -Wno-backend-plugin -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      "-DCMAKE_C_FLAGS=$P" "-DCMAKE_CXX_FLAGS=$P" -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld \
      -DENABLE_WERROR=OFF
cmake --build build --parallel 6   # 6 jobs: more can run a 16 GB machine out of memory
```

### Play

`oot.sh` starts `build/bin/lichii` with the tuned switches:

| Command | What it does |
|---|---|
| `EDEN_NX2_COMPACT_GLOBAL_LOADS=1 ./oot.sh` | **Recommended.** Tuned settings plus faster-compiling shaders |
| `./oot.sh` | Tuned settings |
| `./oot.sh upstream` | Only switches upstream also has, to compare this branch with upstream |
| `./oot.sh smooth` | Display-locked 60 fps on a high-refresh display, without async shader discard (no pop-in) |
| `./oot.sh pace` / `pace30` | Optional: display-locked frame pacing at 60 or 30 fps (adds async shader discard, so new objects can pop in) |
| `./oot.sh --perf` | Tuned settings with `perf` CPU sampling (output in `~/lichii-perf/`) |

`LICHII_BIN=<path> ./oot.sh` runs another build with the same settings, for example the upstream release AppImage.

Recommended emulator settings (Emulation → Configure → Graphics):
- **Vulkan, resolution 1x, window filter Bilinear, anti-aliasing None.** The GPU is close to its limit at 1x.
- **Ocarina of Time anti-aliasing: No sharpening.** Skips the game's sharpening network, which is faster and removes edge halos. It does nothing while NVIDIA DLSS is on.
- **NVIDIA DLSS off.** It leaves white streaks at night.
- **GPU mode Fast**, **asynchronous shaders** and **asynchronous presentation** on. Accurate turns off the split GPU thread.
- **Power profile `performance`** (`powerprofilesctl set performance`). On `balanced` the laptop holds the CPU near 1.2 GHz; `performance` lets it reach 4.1 GHz, which the CPU-bound emulator needs. It returns to `power-saver` on battery automatically.
- **Graphics → Extras → "Pin GPU threads to their own cores"** on hybrid Intel CPUs.
- Add-ons **Max Resolution** and **Dynamic FPS** on (both built in and on by default).

### 16 GB machines

The game fills its 12 GiB of console RAM, so Lichii needs more memory than a 16 GB laptop has. zram compresses this memory only about 2x. Add a swap file on the SSD with a higher priority than zram, so the game's cold memory goes there:

```sh
sudo fallocate -l 16G /swapfile && sudo chmod 600 /swapfile && sudo mkswap /swapfile
sudo swapon --priority 200 /swapfile
echo '/swapfile none swap defaults,pri=200 0 0' | sudo tee -a /etc/fstab
```

Close heavy programs while playing. In this repository, `.clangd` with `Index: Background: Skip` keeps a background C++ indexer from taking 4–7 GB.

### What this fork adds

| Change | Effect |
|---|---|
| *(always on)* Texture-gather shaders | Ocarina's gathers with offsets that can't be proven constant are lowered instead of rejected. Without this the 3D scene was black. |
| *(always on)* Cache work | GPU-write invalidations skip pages with nothing cached (thibaut), and read traps arm only pages not armed yet (Kajcsi) |
| *(always on)* Dynamic FPS | The game keeps real speed down to 10 fps (CTGL) |
| `EDEN_NX2_COMPACT_GLOBAL_LOADS=1` | Global loads through one loaded pointer read that pointer's buffer, so Ocarina's slowest shaders compile 5–7x faster |
| `EDEN_NX2_LINUX_THREAD_PLACEMENT=1` | Linux hybrid CPUs: guest and GPU threads on P-cores, shader compiles on E-cores |
| `EDEN_NX2_FAST_DRAIN=1` | Fewer waits between the split GPU threads |
| `EDEN_NX2_FS_READ_MBPS=<n>` | Forces the emulated game-card read speed (upstream now leaves Switch 2 titles unthrottled) |
| `pin_gpu_threads` setting | Linux implementation of zortos #418: the GPU front and back threads each get a physical core |
| `EDEN_NX2_HOST_PACE=1` + `EDEN_NX2_DISPLAY_HZ` | Frizti's display-locked frame pacer, made to work on Linux, where the refresh rate is passed in |
| `EDEN_NX2_OCCLUSION_READ_WAIT=1` | Waits only for the occlusion report a macro read overlaps. Fixes flickering rocks without zortos #215's 15 fps cost; #215 is reverted on this branch |
| `EDEN_NX2_ASYNC_DISCARD=1` | Skips a draw whose pipeline isn't built yet, so the object pops in late instead of the game freezing |
| `EDEN_NX2_OOT_MOVIE_COPY=0` | Runs Ocarina's movie network natively instead of zortos #721's input copy |
| `EDEN_NX2_CLAIM_HELD_BUILD=1`, `EDEN_NX2_FRONT_REPORT_READS=1`, `EDEN_NX2_UNSAFE_TIC_READ=1` | Smaller stall reductions (zortos #321, an idea from #243, Keeflo541/Zeus) |
| `EDEN_NX2_GPL_STAGE_BINDINGS=1`, `EDEN_NX2_GPL_FIXED_INTERFACES=1` | Work in progress: VK_EXT_graphics_pipeline_library steps 1–2 (see `docs/nx2/gpl-feasibility.md`). Off by default. |

Switches that hurt on an 8 GB GPU with 16 GB of RAM (off in `oot.sh`):
- `EDEN_NX2_TC_MIN_AGE=3600` kept 18.7 GB of textures, spilled 11 GB into system RAM, drew unbacked (black) textures and got Lichii OOM-killed.
- `EDEN_NX2_PRELOAD_ALL_PIPELINES=1` and `EDEN_NX2_PIPELINE_EVICT_FRAMES=0` hold every pipeline, about 2 GB.

### Known issues

- **First-time stutter.** New shaders are translated on the GPU thread before the driver compiles them, so a new area can drop to a few fps for several seconds. Later visits are smooth once the shaders are cached.
- **Water and window reflections flicker** (zortos #340). zortos #354 fixes them but drew foreground foliage as sky-coloured holes, so it is not on this branch.
- With the GPL switches on, a brief lighting glitch has been seen during cutscene freezes. Keep those switches off for normal play.

## Downloads and setup

Download Lichii only from this repository's [releases page](https://github.com/zortos293/Lichii/releases). Lichii has no other download site: archives offered elsewhere, such as lichii.org, are not built by this project, and one has been reported to install malware ([#545](https://github.com/zortos293/Lichii/issues/545)).

On Windows, extract the entire build archive and run **lichii.exe**. Only use games and system files you have legally obtained. Games, keys and firmware are not included.

### Existing saves and settings

The rebrand intentionally retains the existing Eden/Eglantine data directories and configuration keys. It does not move or delete saves, settings or shader caches. Back up your saves before testing experimental builds.

## Compatibility and issue reports

Compatibility and performance vary by game, hardware and driver. Rendering errors, stutters and crashes remain possible. Passing automated tests does not establish that a game renders correctly, and this project makes no universal frame-rate guarantee.

Browse [community compatibility reports](https://lichii.zortos.me/reports) for results with each player's build and setup.

Desktop reporting requires a build containing [PR #303](https://github.com/zortos293/Lichii/pull/303) and is not available in build 88. In builds that include it, play and stop a Switch 2 game, then right-click that game and choose **Submit compatibility report**. Only the latest session in the current Lichii process is eligible.

Confirm the matching website catalog entry and review the editable machine, game, setup, and enabled add-on details prefilled where available. Check the public preview and give explicit consent before submitting. Nothing uploads automatically, and the native v1 reporter does not read or upload logs or files.

When [reporting an issue](https://github.com/zortos293/Eglantine/issues), include:

- The exact build, game version, CPU, GPU and driver version.
- Relevant settings and steps to reproduce the problem.
- A screenshot and the log from the affected session, after checking it for personal information.

Do not upload game files, keys, firmware or private account information.

## Building and contributing

See the [build guide](docs/Build.md), [user documentation](docs/user/README.md) and [coding guidelines](docs/Coding.md). Some inherited documentation and internal build target names still use upstream names. The GUI build target remains `yuzu`; its output is `lichii.exe` on Windows and `lichii` on other desktop platforms.

Keep changes focused and distinguish code-level checks from actual gameplay testing.

## Artwork
Art created by YOB (yobslab)
The supplied Lichii logo, icon and library artwork are in [dist/branding](dist/branding). Windows, Linux and macOS icon resources are generated from that artwork by [Generate-LichiiIcons.ps1](tools/Generate-LichiiIcons.ps1).

## Support Lichii

Lichii is free and stays free. If you want to help, you can donate in crypto. Donations help pay for the AI tools and build machines used to develop this open-source project faster.

**Solana (SOL)**
```text
JepEjfyWjGjhJvHSrqj52KtLjcbbbmUh1iGqerYBF4w
```

**Ethereum (ETH)**
```text
0x055ABa1b80Bc35C836Ce7E9F47c2b3d6C75b5534
```

**Bitcoin (BTC)**
```text
bc1qcvxdpf4qvdhqp8ee4s355cjmxrzsphsdwpl8w4
```

**BNB Smart Chain (BNB)**
```text
0x055ABa1b80Bc35C836Ce7E9F47c2b3d6C75b5534
```

**Polygon**
```text
0x055ABa1b80Bc35C836Ce7E9F47c2b3d6C75b5534
```

Send each coin on its matching network. Coins sent on the wrong network can't be recovered.

> [!NOTE]
> Donations are voluntary gifts and are non-refundable. A donation does not buy any product, service, perk, early access, exclusive build, priority support or say in the project. Every release stays free and public for everyone.

## Credits and license

Lichii inherits the work of the Eglantine, Eden, yuzu and Sudachi projects and their contributors, including Eglantine's original developer Camille LaVey. Original copyright notices and third-party licenses are retained.

See [LICENSE.txt](LICENSE.txt) and the source files for applicable license terms.
