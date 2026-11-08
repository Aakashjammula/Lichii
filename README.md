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

This fork tracks [zortos293/Lichii](https://github.com/zortos293/Lichii) and adds fixes and tuning for **The Legend of Zelda: Ocarina of Time** (`0400486024DEC000`) on **Linux with NVIDIA** hybrid laptops. It was tested on an i7-12700H with an RTX 4060 Laptop GPU, a 180 Hz external monitor and CachyOS. Every change is opt-in through an environment switch. With all switches unset, the build behaves like zortos's code, except for the shader fix, which is always on.

Results in Ocarina on that machine, in areas already visited: **52–60 fps** with even frame pacing. The first visit to a new area, boss or cutscene still stutters while its shaders compile (see *Known issues*).

### Build (Linux)

```sh
git clone -b oot-latest https://github.com/Aakashjammula/Lichii.git && cd Lichii
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DYUZU_TESTS=ON
cmake --build build
./build/bin/tests        # optional: unit tests
```

### Play

`oot.sh` starts `build/bin/lichii` with the tuned switches:

| Command | What it does |
|---|---|
| `./oot.sh pace` | **Recommended.** Frame pacing locked to your display's refresh rate (read from `xrandr`), plus the occlusion fix and async shader discard |
| `./oot.sh pace30` | Same, locked at a low-latency 30 fps |
| `./oot.sh new` | The newer opt-in optimizations, without frame pacing |
| `./oot.sh` | Baseline tuned settings |
| `./oot.sh --perf` | Baseline settings with `perf` CPU sampling (output in `~/lichii-perf/`) |

Recommended emulator settings:
- **GPU mode Fast.** Accurate turns off the split GPU thread.
- **Asynchronous shaders and asynchronous presentation on.**
- **Graphics → Extras → "Pin GPU threads to their own cores"** on hybrid Intel CPUs.

### What this fork adds

| Switch | Effect |
|---|---|
| *(always on)* | Ocarina's texture-gather shaders with offsets that can't be proven constant now lower instead of being rejected. Without this the 3D scene was black. |
| `EDEN_NX2_TC_MIN_AGE=3600` | Stops the texture cache from evicting and re-uploading textures unused for about one second |
| `EDEN_NX2_PRELOAD_ALL_PIPELINES=1`, `EDEN_NX2_PIPELINE_EVICT_FRAMES=0` | Builds every cached pipeline at boot and never evicts built pipelines mid-session |
| `EDEN_NX2_FS_READ_MBPS=0` | Removes the emulated 40 MB/s game-card read limit, which froze area loads |
| `EDEN_NX2_FAST_DRAIN=1` | Fewer waits between the split GPU threads (port of Frizti's drain work) |
| `EDEN_NX2_LINUX_THREAD_PLACEMENT=1` | Linux hybrid CPUs: guest and GPU threads on P-cores, shader compiles on E-cores (fights went from about 19 to 37–52 fps) |
| `pin_gpu_threads` setting | Linux implementation of zortos #418: the GPU front and back threads each get a physical core |
| `EDEN_NX2_HOST_PACE=1` + `EDEN_NX2_DISPLAY_HZ` | Frizti's display-locked frame pacer, made to work on Linux, where the refresh rate is passed in because there is no compositor query |
| `EDEN_NX2_OCCLUSION_READ_WAIT=1` | Waits only for the occlusion report a macro read overlaps. Fixes flickering rocks without zortos #215's 15 fps cost; #215 is reverted on this branch |
| `EDEN_NX2_ASYNC_DISCARD=1` | Skips a draw whose pipeline isn't built yet, so the object pops in late instead of the game and audio freezing |
| `EDEN_NX2_CLAIM_HELD_BUILD=1`, `EDEN_NX2_FRONT_REPORT_READS=1`, `EDEN_NX2_UNSAFE_TIC_READ=1` | Smaller stall reductions (zortos #321, an idea from #243, Keeflo541/Zeus) |
| `EDEN_NX2_GPL_STAGE_BINDINGS=1`, `EDEN_NX2_GPL_FIXED_INTERFACES=1` | Work in progress: VK_EXT_graphics_pipeline_library steps 1–2 (see `docs/nx2/gpl-feasibility.md`). Off by default. |

### Known issues

- **First-time stutter.** Each new shader takes the NVIDIA driver about 65 ms, and up to 3 s, to compile. Async discard hides most of it, but big new scenes still hitch, and audio cuts out during those hitches. The structural fix is graphics pipeline library steps 3–4, which are in progress.
- With the GPL switches on, a brief lighting glitch has been seen during cutscene freezes. Keep those switches off for normal play.
- The shader cache version is 54, so caches from public builds (53) are not used.

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
