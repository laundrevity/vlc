# Native Chromecast subtitles

This branch sends text subtitles to the Default Media Receiver as WebVTT tracks.
VLC keeps its existing Chromecast audio/video pipeline. The TV renders captions;
video does not need to be re-encoded just to display them.

## Using the test build

Open the local movie in **VLC Subtitles**, choose its text track in the Subtitle
menu, then select the Chromecast from Playback → Renderer. Subtitle selection
and disabling work during playback, including while paused.

Embedded MP4 text (`tx3g`/`mov_text`), SubRip/SRT and WebVTT are supported. Load
external subtitle files before starting casting. Adding a new external file
after LOAD requires restarting casting so the receiver can discover the new
track. ASS/SSA, bitmap subtitles, network sources and preservation of complex
subtitle layout are outside this first implementation. Native text conversion
preserves text and timings, not all fonts, positioning or animation.

Seeks retain VLC's keyframe placement. Captions follow the video packets actually
sent, even when playback begins later than the requested position.

## Implementation

An independent, unpaced VLC demux pass reads local subtitle tracks, using VLC's
own subtitle decoders. Audio/video decoding is disabled during this pass. It
caches complete text cues before publishing native tracks. Incomplete scans,
oversized tracks and unsupported subtitle codecs are excluded. No external
FFmpeg executable is required by the app.

Cast LOAD includes the track metadata and `activeTrackIds`; later selection
uses `EDIT_TRACKS_INFO` without seeking or resuming the video. Each LOAD gets new
immutable caption URLs, and one previous set remains available for requests
already in flight. The HTTP endpoints provide GET, HEAD and CORS preflight
responses. The core HTTP server now permits explicitly registered per-resource
OPTIONS handlers while retaining its generic response elsewhere.

The decoder passes its original/clocked PTS pair through an optional stream
output control immediately before the corresponding packet. This is serialized
with packet delivery by the existing stream output lock. Chromecast restores
source timestamps before its existing A/V conversion and muxing, so local pause
durations cannot become gaps in the receiver's media clock. It then clips and
shifts captions using the muxer's actual first DTS.
Using the demux seek position was insufficient: startup and seeking can discard
packets until a later keyframe. Compatible video is still copied without
re-encoding, and stream outputs without a handler ignore the new control.

## Building on Apple Silicon

Install Xcode and GNU Make (`brew install make`), then run from the checkout:

```sh
VLC_PATH="$(brew --prefix make)/libexec/gnubin" \
VLC_CONTRIB_OPTIONS="--disable-librist" \
VLC_CONFIGURE_ARGS="--disable-sparkle" \
./extras/package/macosx/build.sh -a aarch64 -j 8 -c -C "$PWD/build-macos"
```

The app is produced at `build-macos/VLC.app`. The source build avoids relying on
an unavailable prebuilt dependency archive. RIST is disabled because its archive
was unavailable; local movie casting does not use it. Sparkle is disabled so a
test build cannot replace itself with an official release without this patch.
The macOS build also disables `pipe2` detection: Xcode 27 can weak-link it while
targeting an older macOS version that does not provide it at runtime.

## Verification

```sh
PATH="$PWD/extras/tools/build/bin:$(brew --prefix make)/libexec/gnubin:$PATH" \
make -C build-macos/test test_modules_chromecast_webvtt test_modules_chromecast_subtitles

cd build-macos/test
./test_modules_chromecast_webvtt
VLC_PLUGIN_PATH=../modules ./test_modules_chromecast_subtitles
```

The native test checks subtitle selection, extraction, immutable seek documents,
HTTP delivery, HEAD, CORS preflight, and the generic OPTIONS fallback. The pure
WebVTT test covers seek clipping, rewinds, timestamp formatting and escaping.

For the end-to-end sender test, use Python 3.11+ and FFmpeg, plus a copy-compatible
H.264 movie with an embedded text track and dialogue after two minutes:

```sh
python3 test/modules/stream_out/chromecast_receiver.py build-macos movie.mp4 /path/to/ffmpeg
```

This starts a TLS Cast simulator on loopback and uses the actual libVLC sender.
It checks activation, subtitle toggling while paused, pause/resume, seeking and
server teardown. It captures bounded A/V samples and compares decoded frames
with the original movie, then verifies that the caption and video clocks agree.
It performs no receiver discovery and never contacts a physical TV. Test media
and captions are saved only in the printed temporary directory.

Local validation includes a complete 50-minute H.264/AC3/MP4-text source: all
696 extracted cues matched the reference text and timestamps. Captured video and
caption clocks agreed to within one millisecond across pause/resume and a seek.
The simulator also accepts a packaged `.app` in place of the build directory.
On 2026-10-01, the installed build was tested on a physical Chromecast. The
receiver reported the English SDH track active during playback, pause/resume,
and a seek to two minutes. The viewer confirmed that captions appeared and
seemed synchronized with speech. Playback was then stopped and the test server
closed. This was a short hardware test; a complete episode has not yet been
validated on the receiver.
