#!/usr/bin/env python3
"""Exercise the gpt-live-transcribe wire protocol without OBS in the way.

The C++ provider and this script speak the same session config, so a protocol
mistake shows up here in seconds instead of a Windows CI round-trip.

    export OPENAI_API_KEY=sk-...
    python3 tools/smoke_test.py speech.wav --delay low --keywords keywords.txt

Any WAV works; it is converted to the 24 kHz mono PCM16 the model expects and
streamed in real time so the pacing matches what OBS would produce.

Requires: pip install websockets
"""

import argparse
import asyncio
import audioop
import base64
import json
import os
import sys
import time
import wave

import websockets

WS_URL = "wss://api.openai.com/v1/realtime?intent=transcription"
MODEL = "gpt-live-transcribe"
SAMPLE_RATE = 24000
CHUNK_MS = 100


def load_wav_as_pcm24k(path):
    """Return 24 kHz mono signed-16 PCM bytes for any reasonable input WAV."""
    with wave.open(path, "rb") as wav:
        channels = wav.getnchannels()
        width = wav.getsampwidth()
        rate = wav.getframerate()
        frames = wav.readframes(wav.getnframes())

    if width != 2:
        frames = audioop.lin2lin(frames, width, 2)
        width = 2
    if channels > 1:
        frames = audioop.tomono(frames, width, 0.5, 0.5)
    if rate != SAMPLE_RATE:
        frames, _ = audioop.ratecv(frames, width, 1, rate, SAMPLE_RATE, None)

    return frames


def build_session(args, keywords):
    transcription = {"model": MODEL, "delay": args.delay}
    if args.prompt:
        transcription["prompt"] = args.prompt
    if keywords:
        transcription["keywords"] = keywords
    if args.language:
        transcription["languages"] = [args.language]

    return {
        "type": "session.update",
        "session": {
            "type": "transcription",
            "audio": {
                "input": {
                    "format": {"type": "audio/pcm", "rate": SAMPLE_RATE},
                    "transcription": transcription,
                    "turn_detection": {"type": "server_vad"},
                }
            },
        },
    }


async def pump_audio(ws, pcm, realtime):
    """Feed audio in CHUNK_MS slices, pacing to wall-clock like a live source."""
    bytes_per_chunk = int(SAMPLE_RATE * 2 * CHUNK_MS / 1000)
    started = time.monotonic()

    for i in range(0, len(pcm), bytes_per_chunk):
        chunk = pcm[i : i + bytes_per_chunk]
        await ws.send(
            json.dumps(
                {
                    "type": "input_audio_buffer.append",
                    "audio": base64.b64encode(chunk).decode("ascii"),
                }
            )
        )
        if realtime:
            # keep the send schedule anchored to the start so we don't drift
            target = started + ((i + bytes_per_chunk) / (SAMPLE_RATE * 2))
            await asyncio.sleep(max(0.0, target - time.monotonic()))

    print("\n[audio sent, waiting for trailing transcripts...]", file=sys.stderr)


async def read_events(ws, first_delta):
    partial = ""
    async for message in ws:
        event = json.loads(message)
        etype = event.get("type", "")

        if etype == "conversation.item.input_audio_transcription.delta":
            if first_delta["at"] is None:
                first_delta["at"] = time.monotonic()
            partial += event.get("delta", "")
            print(f"\r  partial: {partial}", end="", flush=True)
        elif etype == "conversation.item.input_audio_transcription.completed":
            text = event.get("transcript", partial)
            print(f"\r  FINAL:   {text}")
            partial = ""
        elif etype == "session.updated":
            print("[session configured]", file=sys.stderr)
        elif etype == "error":
            err = event.get("error", {})
            print(
                f"[error] {err.get('message', event)} ({err.get('code', '')})",
                file=sys.stderr,
            )
        elif etype:
            print(f"[{etype}]", file=sys.stderr)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav", help="input WAV file")
    ap.add_argument(
        "--delay",
        default="low",
        choices=["minimal", "low", "medium", "high", "xhigh"],
        help="latency/stability tier",
    )
    ap.add_argument("--prompt", default="", help="free-form context about the audio")
    ap.add_argument("--keywords", help="file with domain terms, one per line")
    ap.add_argument("--language", default="", help="ISO 639-1 hint, e.g. en (default: auto)")
    ap.add_argument(
        "--fast",
        action="store_true",
        help="send as fast as possible instead of pacing to real time",
    )
    args = ap.parse_args()

    api_key = os.environ.get("OPENAI_API_KEY")
    if not api_key:
        sys.exit("OPENAI_API_KEY is not set")

    keywords = []
    if args.keywords:
        with open(args.keywords) as fh:
            keywords = [line.strip() for line in fh if line.strip()]

    pcm = load_wav_as_pcm24k(args.wav)
    duration = len(pcm) / (SAMPLE_RATE * 2)
    print(
        f"[{args.wav}: {duration:.1f}s @ {SAMPLE_RATE} Hz mono, "
        f"delay={args.delay}, {len(keywords)} keywords]",
        file=sys.stderr,
    )

    connect_started = time.monotonic()
    async with websockets.connect(
        WS_URL,
        additional_headers={
            "Authorization": f"Bearer {api_key}",
            "OpenAI-Beta": "realtime=v1",
        },
        max_size=None,
    ) as ws:
        await ws.send(json.dumps(build_session(args, keywords)))

        first_delta = {"at": None}
        audio_started = time.monotonic()
        reader = asyncio.create_task(read_events(ws, first_delta))
        await pump_audio(ws, pcm, realtime=not args.fast)

        # give the tail of the audio time to come back before tearing down
        try:
            await asyncio.wait_for(reader, timeout=15)
        except asyncio.TimeoutError:
            reader.cancel()

        print(f"\n[connect: {audio_started - connect_started:.2f}s]", file=sys.stderr)
        if first_delta["at"]:
            print(
                f"[first delta: {first_delta['at'] - audio_started:.2f}s "
                f"after audio start]",
                file=sys.stderr,
            )
        else:
            print("[no transcript deltas received]", file=sys.stderr)


if __name__ == "__main__":
    asyncio.run(main())
