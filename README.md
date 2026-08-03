# OBS OpenAI Live Transcription

Real-time OBS captions powered by OpenAI's [`gpt-live-transcribe`](https://developers.openai.com/api/docs/models/gpt-live-transcribe).

This is a fork of [locaal-ai/cloudvocal](https://github.com/locaal-ai/cloudvocal), stripped
down to a single provider. All the OBS-side plumbing — audio filter, resampling, caption
delivery, SRT writing, CEA-608 embedding into the RTMP stream — is CloudVocal's work.
GPLv2, same as upstream.

## What it does

Adds an audio filter that streams your source's audio to OpenAI over a WebSocket and
renders the returned transcript into a text source, a file, and/or the outgoing stream's
caption track.

- Partial captions that grow as you speak, finalised at sentence boundaries
- Measured time-to-first-word: **0.46 s** (`minimal`) → **1.73 s** (`high`)
- **Latency tier** (`minimal` → `xhigh`) trading first-word speed against caption stability
- **Context** and **keywords** fields — the model uses these to get proper nouns, jargon
  and product names right, which is the main reason to prefer it over Whisper
- Caption output to a text source, `.txt`/`.srt` file, or embedded CEA-608 for
  YouTube/Twitch

## Cost, and why the idle timeout matters

`gpt-live-transcribe` bills **$0.017 per minute of session audio** — roughly **$1.02/hour**
— and it bills by how long the socket is open, *not* by how much speech it hears.

This plugin therefore opens the connection lazily on first audio and drops it after
`Disconnect after idle` seconds of silence (default 30). Set it to 0 to keep the socket
open permanently, and expect to pay for every minute the filter is enabled.

## Configuration

1. Add **OpenAI Live Transcription** as a filter on an audio source
2. Paste your OpenAI API key into **API Key**
3. Pick a text source in **Output source** (one is created for you if you have none)
4. Optionally fill in **Context** (what the stream is about) and **Keywords** (names,
   games, jargon — one per line)

The API key is stored by OBS in its scene-collection JSON in plain text, like every other
OBS plugin credential. Treat that file accordingly.

## Building

Only **Windows x64** is built by CI; the macOS job is disabled (see
`.github/workflows/build-project.yaml`) because this fork does not target it.

Dependencies come from Conan — Boost (for Beast's WebSocket client), OpenSSL, and zlib:

```powershell
> pip install conan
> conan profile detect --force
> conan install . --output-folder=./build_conan --build=missing -g CMakeDeps
```

Build:

```powershell
> .\.github\scripts\Build-Windows.ps1 -Configuration Release
```

Build and deploy straight into OBS while developing:

```powershell
> pwsh -ExecutionPolicy Bypass -File .\.github\scripts\Build-Windows.ps1 -Configuration RelWithDebInfo -SkipDeps
> Copy-Item -Force -Recurse .\release\RelWithDebInfo\* "C:\Program Files\obs-studio\"
```

Linux and macOS build scripts are inherited from upstream and should still work, but are
untested in this fork.

## Note on caption segmentation

`gpt-live-transcribe` never emits a "transcription completed" event — it streams deltas
into one transcript that grows for the life of the session. This plugin therefore decides
where captions end itself: at sentence punctuation (ignoring `Dr.`, `3.5` and friends),
after a 900 ms pause, or at a 240-character cap. Without that, stream captions and SRT
output would never fire at all, since both are gated on a finalised line.

## Status

The wire protocol is verified against the live API; the C++ has not been compiled yet.
See `PLAN_OPENAI_FORK.md` for details and what remains.

## License

GPLv2 — see [LICENSE](LICENSE). Inherited from CloudVocal.
