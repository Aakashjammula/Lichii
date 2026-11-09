#!/usr/bin/env python3
"""FPS summary of a Lichii log run with EDEN_DKB_FRAME_TRACE=1 (oot.sh sets it).

  python3 -I tools/nx2/oot_fps.py [log] [--skip SECONDS]

Each sampled "DKB renderer renderer_frame=N" line gives the frames presented since the previous
one; frames / elapsed time between two samples is that window's fps. Prints the median and the
10th percentile (worst 10%) over the windows after --skip seconds (default 60, boot and menus).
"""
import pathlib
import re
import statistics
import sys

LINE = re.compile(r"^\[\s*([\d.]+)\].*DKB renderer renderer_frame=(\d+)")


def main() -> int:
    args = sys.argv[1:]
    skip = 60.0
    if "--skip" in args:
        i = args.index("--skip")
        skip = float(args[i + 1])
        del args[i : i + 2]
    log = pathlib.Path(args[0] if args else "~/.local/share/eden/log/eden_log.txt").expanduser()
    samples = []
    for line in log.read_text(errors="replace").splitlines():
        m = LINE.match(line)
        if m:
            samples.append((float(m.group(1)), int(m.group(2))))
    fps = []
    for (t0, f0), (t1, f1) in zip(samples, samples[1:]):
        if t0 >= skip and t1 > t0 and f1 > f0:
            fps.append((f1 - f0) / (t1 - t0))
    if len(fps) < 5:
        print(f"{log}: only {len(fps)} windows after {skip:.0f} s; was EDEN_DKB_FRAME_TRACE=1 set?")
        return 1
    fps.sort()
    worst = fps[: max(1, len(fps) // 10)]
    print(f"{log}")
    print(f"  windows: {len(fps)} over {samples[-1][0] - skip:.0f} s")
    print(f"  median fps:    {statistics.median(fps):.1f}")
    print(f"  worst 10% fps: {statistics.mean(worst):.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
