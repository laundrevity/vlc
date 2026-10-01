"""Compare captured Cast frames and captions with the original local movie."""

import asyncio
import html
import re
import statistics
from collections import defaultdict
from pathlib import Path


async def ffmpeg_output(ffmpeg, *args):
    process = await asyncio.create_subprocess_exec(
        ffmpeg,
        "-v",
        "error",
        *args,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    output, error = await process.communicate()
    if process.returncode:
        raise RuntimeError(error.decode(errors="replace")[-2000:])
    return output.decode()


async def frames(ffmpeg, path, start=None, duration=35):
    options = ["-copyts"] if start is None else ["-ss", str(start)]
    output = await ffmpeg_output(
        ffmpeg,
        *options,
        "-i",
        str(path),
        "-t",
        str(duration),
        "-map",
        "0:v:0",
        "-vf",
        "scale=160:68",
        "-fps_mode",
        "passthrough",
        "-f",
        "framemd5",
        "-",
    )
    timebase = next(
        line.split(": ", 1)[1] for line in output.splitlines() if line.startswith("#tb 0:")
    )
    numerator, denominator = map(int, timebase.split("/"))
    result = []
    for line in output.splitlines():
        if line.startswith("#"):
            continue
        fields = line.split(",")
        if len(fields) == 6:
            result.append(
                (int(fields[2]) * numerator / denominator + (start or 0), fields[-1].strip())
            )
    return result


def captions(text):
    result = []
    for block in re.split(r"\n\s*\n", text.replace("\r\n", "\n")):
        match = re.search(r"(?m)^((?:\d+:)?\d{2}:\d{2}\.\d{3}) --> [^\n]+\n(.*)", block, re.S)
        if not match:
            continue
        start = sum(
            float(part) * 60**power for power, part in enumerate(reversed(match[1].split(":")))
        )
        words = " ".join(html.unescape(re.sub(r"<[^>]*>", "", match[2])).split())
        result.append((start, words))
    return result


async def check_sync(ffmpeg, source, artifacts):
    source_captions = captions(
        await ffmpeg_output(
            ffmpeg,
            "-i",
            str(source),
            "-map",
            "0:s:0",
            "-f",
            "webvtt",
            "-",
        )
    )
    by_text = defaultdict(list)
    for start, text in source_captions:
        by_text[text].append(start)
    for number, reference_start in [(0, 0), (1, 100)]:
        reference = await frames(
            ffmpeg, source, reference_start, duration=80 if number == 0 else 60
        )
        captured = await frames(
            ffmpeg, Path(artifacts) / f"sample-{number}.mkv", duration=60 if number == 0 else 18
        )
        by_hash = defaultdict(list)
        for timestamp, digest in reference:
            by_hash[digest].append(timestamp)
        offsets = [
            by_hash[digest][0] - timestamp
            for timestamp, digest in captured
            if len(by_hash[digest]) == 1
        ]
        assert len(offsets) >= 5, (
            "Need copied H.264 video with distinct frames to check synchronization"
        )
        assert max(offsets) - min(offsets) < 0.04, "Video clock changed across pause/resume"
        document = (Path(artifacts) / f"captions-{number}.vtt").read_text()
        shifts = [
            by_text[text][0] - start
            for start, text in captions(document)
            if start > 0 and len(by_text[text]) == 1
        ]
        assert len(shifts) >= 2, (
            "Need at least two distinct subtitle cues for synchronization check"
        )
        origin = statistics.median(offsets)
        drift = max(abs(shift - origin) for shift in shifts)
        # Matroska uses millisecond timestamps; the framehash time base can
        # additionally quantize at a frame boundary. Reject a one-frame drift.
        assert drift < 0.04, f"Caption/video mismatch on LOAD {number}: {drift:.3f}s"
        print(
            f"LOAD {number}: {len(offsets)} matching video frames, "
            f"{len(shifts)} matching cues, caption drift {drift * 1000:.3f} ms",
            flush=True,
        )
