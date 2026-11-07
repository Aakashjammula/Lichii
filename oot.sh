#!/usr/bin/env bash
# Launch Lichii with the settings tuned for Ocarina of Time on this machine (branch oot-latest).
#   ./oot.sh          play (proven settings)
#   ./oot.sh new      proven settings + the newer opt-in optimizations
#   ./oot.sh pace     proven settings + Frizti display-locked frame pacing (60 FPS)
#   ./oot.sh pace30   same, locked low-latency 30 FPS
#   ./oot.sh --perf   proven settings while perf samples the CPU (data in ~/lichii-perf/)
set -euo pipefail
cd "$(dirname "$0")"

export EDEN_NX2_PRELOAD_ALL_PIPELINES=1     # build every cached shader at boot
export EDEN_NX2_PIPELINE_EVICT_FRAMES=0     # never drop built shaders mid-session
export EDEN_NX2_FS_READ_MBPS=0              # no artificial 40 MB/s game-file read limit
export EDEN_NX2_FAST_DRAIN=1                # fewer waits between the two GPU threads
export EDEN_NX2_TC_MIN_AGE=3600             # keep textures used in the last ~minute (no re-upload thrash)
export EDEN_NX2_LINUX_THREAD_PLACEMENT=1    # game/GPU threads on P-cores, compiles on E-cores (fights 19 -> 37-52 fps)
export EDEN_NX2_TC_LOG=1                    # texture cache summary every 600 frames (cheap)
export EDEN_DKB_FRAME_TRACE=1               # sampled frame timing (cheap)
export __GL_SHADER_DISK_CACHE_SIZE=10737418240  # 10 GB NVIDIA shader disk cache

case "${1:-}" in
new)
    export EDEN_NX2_ASYNC_DISCARD=1         # new shaders: brief pop-in instead of ~2 s freezes (zortos #224)
    export EDEN_NX2_CLAIM_HELD_BUILD=1      # a waiting draw compiles its own shader (zortos #321)
    export EDEN_NX2_OCCLUSION_READ_WAIT=1   # targeted lighting fix (zortos #267's approach, not #215)
    export EDEN_NX2_FRONT_REPORT_READS=1    # answer macro report reads on the front (zortos #243 idea)
    export EDEN_NX2_UNSAFE_TIC_READ=1       # no flushing descriptor read during shader translation (Zeus)
    ;;
pace|pace30)
    # Frizti display-locked frame pacer (off unless EDEN_NX2_HOST_PACE=1).
    export EDEN_NX2_HOST_PACE=1
    export EDEN_NX2_OCCLUSION_READ_WAIT=1   # fixes the flickering rocks (targeted occlusion wait)
    export EDEN_NX2_ASYNC_DISCARD=1         # new shaders: brief pop-in instead of freezing game + audio
    # The pacer can't ask the compositor on Linux: pass the active display's refresh rate.
    hz=$(xrandr --current 2>/dev/null | awk '/\*/{for(i=2;i<=NF;i++) if($i ~ /\*/){gsub(/[*+]/,"",$i); print $i; exit}}')
    export EDEN_NX2_DISPLAY_HZ="${EDEN_NX2_DISPLAY_HZ:-${hz:-60}}"
    echo "host pacer: display ${EDEN_NX2_DISPLAY_HZ} Hz"
    [[ "$1" == pace30 ]] && export EDEN_NX2_SWAP_INTERVAL=2   # low-latency locked 30 FPS
    exec ./build/bin/lichii
    ;;
--perf)
    mkdir -p "$HOME/lichii-perf"
    out="$HOME/lichii-perf/perf-$(date +%Y%m%d-%H%M%S).data"
    echo "perf data: $out"
    # No -g: call-stack sampling fails to open for Lichii; DEBUGINFOD off so exit is quick.
    DEBUGINFOD_URLS= exec perf record -F 199 -e cpu-clock -o "$out" -- ./build/bin/lichii
    ;;
esac
exec ./build/bin/lichii
