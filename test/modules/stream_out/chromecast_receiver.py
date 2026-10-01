#!/usr/bin/env python3
"""Exercise VLC's real Cast sender against a loopback-only TLS receiver.

Usage: python3 chromecast_receiver.py BUILD_DIRECTORY_OR_APP VIDEO_FILE [FFMPEG]
Uses Python's standard library plus FFmpeg for the captured frame checks.
No discovery or physical receiver access. Use a copy-compatible H.264 movie
with an embedded text track, distinct frames and dialogue after two minutes.
"""

import asyncio
import ctypes
import json
import os
import socket
import ssl
import struct
import sys
import tempfile
import time
from pathlib import Path
from urllib.parse import urlsplit

from chromecast_timing import check_sync


def varint(value):
    result = bytearray()
    while value > 127:
        result.append((value & 127) | 128)
        value >>= 7
    return bytes(result) + bytes([value])


def field(number, value):
    if isinstance(value, int):
        return varint(number << 3) + varint(value)
    if isinstance(value, str):
        value = value.encode()
    return varint((number << 3) | 2) + varint(len(value)) + value


def envelope(namespace, payload, binary=False):
    return b"".join(
        [
            field(1, 0),
            field(2, "receiver-0"),
            field(3, "sender-vlc"),
            field(4, namespace),
            field(5, int(binary)),
            field(7 if binary else 6, payload),
        ]
    )


def decode(data):
    position = 0

    def integer():
        nonlocal position
        value = shift = 0
        while True:
            byte = data[position]
            position += 1
            value |= (byte & 127) << shift
            if byte < 128:
                return value
            shift += 7

    fields = {}
    while position < len(data):
        tag = integer()
        if tag & 7 == 0:
            value = integer()
        elif tag & 7 == 2:
            size = integer()
            value = data[position : position + size]
            position += size
        else:
            raise ValueError("Unexpected protobuf wire type")
        fields[tag >> 3] = value
    return fields


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Receiver:
    def __init__(self):
        self.loads = []
        self.captions = []
        self.video_samples = []
        self.tasks = []
        self.active = []
        self.state = "PAUSED"
        self.session = 0
        self.started = time.monotonic()
        self.position = 0.0
        self.errors = []
        self.writer = None

    async def send(self, namespace, payload, binary=False):
        if not binary:
            payload = json.dumps(payload)
        data = envelope(namespace, payload, binary)
        self.writer.write(struct.pack("!I", len(data)) + data)
        await self.writer.drain()

    async def status(self, request_id):
        await self.send(
            "urn:x-cast:com.google.cast.media",
            {
                "type": "MEDIA_STATUS",
                "requestId": request_id,
                "status": [
                    {
                        "mediaSessionId": self.session,
                        "playerState": self.state,
                        "currentTime": self.position
                        + (time.monotonic() - self.started if self.state == "PLAYING" else 0),
                        "supportedMediaCommands": 15,
                        "activeTrackIds": self.active,
                    }
                ],
            },
        )

    async def fetch(self, url, video=False):
        parsed = urlsplit(url)
        assert parsed.hostname == "127.0.0.1", "Test must never contact a physical receiver"
        reader, writer = await asyncio.open_connection(parsed.hostname, parsed.port)
        try:
            writer.write(
                (
                    f"GET {parsed.path} HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                    "Origin: https://www.gstatic.com\r\nConnection: close\r\n\r\n"
                ).encode()
            )
            await writer.drain()
            headers = await reader.readuntil(b"\r\n\r\n")
            assert b" 200 " in headers.split(b"\r\n", 1)[0], headers
            if not video:
                assert b"Access-Control-Allow-Origin: https://www.gstatic.com" in headers
                body = await reader.read()
                assert body.startswith(b"WEBVTT\n\n") and b" --> " in body
                self.captions.append((url, body))
            else:
                sample = bytearray()
                self.video_samples.append(sample)
                while chunk := await reader.read(65536):
                    if len(sample) < 12_000_000:
                        sample.extend(chunk)
                    await asyncio.sleep(0.01)
        except (ConnectionError, asyncio.IncompleteReadError):
            if not video:
                raise
        finally:
            writer.close()
            await writer.wait_closed()

    async def connection(self, reader, writer):
        self.writer = writer
        try:
            while True:
                size = struct.unpack("!I", await reader.readexactly(4))[0]
                message = decode(await reader.readexactly(size))
                namespace = message[4].decode()
                if namespace.endswith("deviceauth"):
                    await self.send(namespace, b"\x12\x04\x0a\x00\x12\x00", True)
                    continue
                payload = json.loads(message.get(6, b"{}"))
                command = payload.get("type")
                request_id = payload.get("requestId", 0)
                if namespace.endswith("heartbeat") and command == "PING":
                    await self.send(namespace, {"type": "PONG"})
                elif namespace.endswith("receiver") and command in {"GET_STATUS", "LAUNCH"}:
                    await self.send(
                        namespace,
                        {
                            "type": "RECEIVER_STATUS",
                            "requestId": request_id,
                            "status": {
                                "applications": [
                                    {
                                        "appId": "CC1AD845",
                                        "transportId": "test-app",
                                        "sessionId": "test-session",
                                        "namespaces": [
                                            {"name": "urn:x-cast:com.google.cast.media"}
                                        ],
                                    }
                                ]
                            },
                        },
                    )
                elif namespace.endswith("media"):
                    if command == "LOAD":
                        media = payload["media"]
                        assert media["tracks"], "LOAD omitted native subtitle tracks"
                        self.loads.append(payload)
                        self.active = payload["activeTrackIds"]
                        self.session += 1
                        self.state = "PLAYING"
                        self.started = time.monotonic()
                        self.position = 0.0
                        await self.status(request_id)
                        for track in media["tracks"]:
                            assert (
                                track["type"] == "TEXT" and track["trackContentType"] == "text/vtt"
                            )
                            self.tasks.append(
                                asyncio.create_task(self.fetch(track["trackContentId"]))
                            )
                        self.tasks.append(asyncio.create_task(self.fetch(media["contentId"], True)))
                    elif command in {"PLAY", "PAUSE", "GET_STATUS", "EDIT_TRACKS_INFO", "STOP"}:
                        if command == "PLAY":
                            self.started = time.monotonic()
                            self.state = "PLAYING"
                        elif command == "PAUSE":
                            self.position += time.monotonic() - self.started
                            self.state = "PAUSED"
                        elif command == "EDIT_TRACKS_INFO":
                            self.active = payload["activeTrackIds"]
                        elif command == "STOP":
                            self.state = "IDLE"
                        await self.status(request_id)
        except (asyncio.IncompleteReadError, ConnectionError):
            pass
        except Exception as exc:
            self.errors.append(exc)
        finally:
            writer.close()
            await writer.wait_closed()

    async def until(self, predicate):
        async with asyncio.timeout(15):
            while not predicate():
                if self.errors:
                    raise self.errors[0]
                await asyncio.sleep(0.05)


async def main():
    build = Path(sys.argv[1]).resolve()
    video = Path(sys.argv[2]).resolve()
    source = Path(__file__).resolve().parents[3]
    artifacts = Path(tempfile.mkdtemp(prefix="vlc-cast-test-"))
    print(f"Test artifacts: {artifacts}", flush=True)
    if build.suffix == ".app":
        runtime = build / "Contents/MacOS"
        os.environ["VLC_PLUGIN_PATH"] = str(runtime / "plugins")
        core_library = runtime / "lib/libvlccore.dylib"
        player_library = runtime / "lib/libvlc.dylib"
    else:
        os.environ["VLC_PLUGIN_PATH"] = str(build / "modules")
        core_library = build / "src/.libs/libvlccore.dylib"
        player_library = build / "lib/.libs/libvlc.dylib"
    ctypes.CDLL(str(core_library), mode=ctypes.RTLD_GLOBAL)
    vlc = ctypes.CDLL(str(player_library))

    def api(name, result, *parameters):
        function = getattr(vlc, "libvlc_" + name)
        function.restype = result
        function.argtypes = parameters
        return function

    ptr = ctypes.c_void_p
    new = api("new", ptr, ctypes.c_int, ctypes.POINTER(ctypes.c_char_p))
    release = api("release", None, ptr)
    media_new = api("media_new_path", ptr, ptr, ctypes.c_char_p)
    option = api("media_add_option", None, ptr, ctypes.c_char_p)
    media_release = api("media_release", None, ptr)
    player_new = api("media_player_new_from_media", ptr, ptr)
    play = api("media_player_play", ctypes.c_int, ptr)
    pause = api("media_player_set_pause", None, ptr, ctypes.c_int)
    seek = api("media_player_set_time", None, ptr, ctypes.c_longlong)
    stop = api("media_player_stop", None, ptr)
    get_subtitle = api("video_get_spu", ctypes.c_int, ptr)
    set_subtitle = api("video_set_spu", ctypes.c_int, ptr, ctypes.c_int)
    player_release = api("media_player_release", None, ptr)
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.load_cert_chain(source / "test/samples/certs/certkey.pem")
    receiver = Receiver()
    server = await asyncio.start_server(receiver.connection, "127.0.0.1", 0, ssl=tls)
    cast_port = server.sockets[0].getsockname()[1]
    arguments = [
        b"--ignore-config",
        b"--no-media-library",
        b"--no-metadata-network-access",
        b"--aout=adummy",
        b"--vout=vdummy",
        b"-vv",
    ]
    instance = new(len(arguments), (ctypes.c_char_p * len(arguments))(*arguments))
    assert instance
    media = media_new(instance, os.fsencode(video))
    assert media
    option(
        media, f"sout=#chromecast{{ip=127.0.0.1,port={cast_port},http-port={free_port()}}}".encode()
    )
    option(media, b"demux-filter=cc_demux")
    option(media, b"sub-track=0")
    option(media, b"sout-chromecast-show-perf-warning=0")
    player = player_new(media)
    media_release(media)
    try:
        assert await asyncio.to_thread(play, player) == 0
        await receiver.until(
            lambda: (
                receiver.loads
                and receiver.captions
                and receiver.active
                and receiver.state == "PLAYING"
                and receiver.video_samples
                and len(receiver.video_samples[0]) >= 4_000_000
            )
        )
        print("Initial native subtitle LOAD and activation passed", flush=True)
        await asyncio.to_thread(pause, player, 1)
        await receiver.until(lambda: receiver.state == "PAUSED")
        assert receiver.active
        subtitle_id = get_subtitle(player)
        assert subtitle_id >= 0
        assert await asyncio.to_thread(set_subtitle, player, -1) == 0
        await receiver.until(lambda: not receiver.active)
        assert receiver.state == "PAUSED" and len(receiver.loads) == 1
        assert await asyncio.to_thread(set_subtitle, player, subtitle_id) == 0
        await receiver.until(lambda: bool(receiver.active))
        assert receiver.state == "PAUSED" and len(receiver.loads) == 1
        print("Subtitle toggle while paused did not reload or resume video", flush=True)
        await asyncio.sleep(1)
        await asyncio.to_thread(pause, player, 0)
        await receiver.until(lambda: receiver.state == "PLAYING")
        print("Pause/resume retained native subtitles", flush=True)
        await receiver.until(lambda: len(receiver.video_samples[0]) >= 12_000_000)
        initial = receiver.captions[0][1]
        await asyncio.to_thread(seek, player, 120000)
        await receiver.until(
            lambda: (
                len(receiver.loads) >= 2
                and len(receiver.captions) >= 2
                and len(receiver.video_samples) >= 2
                and len(receiver.video_samples[1]) >= 2_000_000
            )
        )
        assert receiver.loads[-1]["activeTrackIds"]
        assert receiver.captions[-1][1] != initial
        assert receiver.captions[-1][0] != receiver.captions[0][0]
        print("Seek reload published new captions with changed timestamps", flush=True)
    finally:
        for index, sample in enumerate(receiver.video_samples):
            if sample:
                (artifacts / f"sample-{index}.mkv").write_bytes(sample)
        for index, (_, body) in enumerate(receiver.captions):
            (artifacts / f"captions-{index}.vtt").write_bytes(body)
        await asyncio.to_thread(stop, player)
        player_release(player)
        release(instance)
        for task in receiver.tasks:
            task.cancel()
        await asyncio.gather(*receiver.tasks, return_exceptions=True)
        server.close()
        await server.wait_closed()
    if receiver.errors:
        raise receiver.errors[0]
    await check_sync(sys.argv[3] if len(sys.argv) > 3 else "ffmpeg", video, artifacts)
    parsed = urlsplit(receiver.loads[-1]["media"]["contentId"])
    try:
        reader, writer = await asyncio.open_connection(parsed.hostname, parsed.port)
    except ConnectionError:
        print("Playback cleanup closed the local media server", flush=True)
    else:
        writer.close()
        await writer.wait_closed()
        raise AssertionError("Media server survived playback cleanup")


if __name__ == "__main__":
    asyncio.run(main())
