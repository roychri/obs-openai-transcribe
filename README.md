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

## Install

No release yet, so take a CI build. The paths below are confirmed working — the plugin
loads and appears in OBS's audio filter list with them.

1. Go to the [Actions tab](https://github.com/roychri/obs-openai-transcribe/actions) and
   open the most recent run with a green tick. Scroll to **Artifacts** at the bottom of the
   page. (If a run shows ✗, check whether it is only the format check that failed — the
   **Build for Windows** job having a green tick is what decides whether the artifact is
   usable.)
2. Download `obs-openai-transcribe-<version>-windows-x64-<hash>`.
3. **It is a zip inside a zip.** GitHub wraps every artifact, so unzip twice. There is no
   installer `.exe`. The inner zip contains:
   ```
   obs-openai-transcribe/
     bin/64bit/obs-openai-transcribe.dll     <- the plugin
     bin/64bit/obs-openai-transcribe.pdb     <- debug symbols, not needed
     data/locale/en-US.ini
     data/roots.pem
   ```
4. Copy the files into your OBS install. **Note the folder names do not match** — the
   artifact says `bin/64bit`, OBS wants `obs-plugins/64bit`, and the `data` contents go
   under a folder you create yourself named after the plugin:
   ```
   C:\Program Files\obs-studio\obs-plugins\64bit\obs-openai-transcribe.dll
   C:\Program Files\obs-studio\data\obs-plugins\obs-openai-transcribe\locale\en-US.ini
   C:\Program Files\obs-studio\data\obs-plugins\obs-openai-transcribe\roots.pem
   ```
   Writing into `Program Files` needs **Administrator** — accept the elevation prompt.
5. Restart OBS. **OpenAI Live Transcription** should now appear in the audio filter list.

### If the filter is missing or misnamed

| Symptom | Cause |
|---|---|
| Filter absent from the `+` list | The DLL is not where OBS looks. Re-check the `obs-plugins\64bit` path. |
| Filter shows as `cloudvocalAudioFilter` | The DLL loaded but `en-US.ini` did not. Check the `data\obs-plugins\obs-openai-transcribe\locale\` path. |
| Filter shows as **CloudVocal Captions** | Both loaded fine, but from a build older than 2026-08-03. Download a newer artifact — older builds also have a caption-source bug that sends captions nowhere. |

To uninstall, delete the DLL and the `obs-openai-transcribe` data folder, then restart OBS.

## Get an API key

1. Sign in at [platform.openai.com](https://platform.openai.com/api-keys) and create a
   secret key.
2. Make sure the account has billing set up — the key authenticates fine without it, but
   the session will fail once audio starts flowing.
3. Copy the key now; OpenAI will not show it again.

## Set it up

1. In OBS, find the audio source you want captioned — your mic under **Audio Mixer**, or
   any source with audio.
2. Click the **⚙ gear → Filters** next to it (or right-click the source → **Filters**).
3. Under **Audio Filters**, click **+** and choose **OpenAI Live Transcription**. Give it
   any name.
4. Paste your key into **API Key**.
5. Leave **Output source** on the default. The plugin creates a text source called
   `OpenAI Captions` in your current scene and points at it, so captions are visible
   immediately. Pick an existing text source instead if you already have one styled.
6. Set **Language** to what will be spoken, or leave it for auto-detect.
7. Click **Close**, then speak. Text should appear within about a second.

The auto-created text source is a plain OBS text source — move, resize, restyle, or
reposition it like any other. It is added to the scene that was active when the filter
was created.

## Settings reference

### OpenAI options

| Setting | What it does |
|---|---|
| **Latency** | Speed vs. stability. `minimal` shows words soonest but revises them more as the model reconsiders; `xhigh` waits longer and rewrites less. Measured first-word times: `minimal` 0.46 s, `low` 0.85–1.17 s, `high` 1.73 s. Default `low`. |
| **Context** | A sentence or two about the stream — "a live Kerbal Space Program run with guest Marie". The model uses it to disambiguate; it is not an instruction. |
| **Keywords** | Proper nouns the model would otherwise mangle — guest handles, game names, product SKUs, jargon. One per line. This is the single biggest accuracy lever. |
| **Disconnect after idle** | Seconds of silence before the socket closes, to stop paying for an idle connection. Default 30. `0` keeps it open forever. |

### General

**Transcription provider** is `OpenAI (gpt-live-transcribe)` — the only one in this fork.
**API Key** is your OpenAI secret key and is the only credential needed.

(Builds before 2026-08-03 also showed a **Secret Key** field. That was inherited from
CloudVocal, where providers like Naver Clova and AWS need a key/secret *pair*. OpenAI
authenticates with a single bearer token, so the field did nothing and has been removed.)

### Other groups

**File output** writes captions to `.txt` or `.srt`, optionally only while recording, and
can rename the file to match the recording. Note the SRT caveat under *Caption
segmentation* below.

**Advanced** has **Caption to stream** (embeds CEA-608 captions into the outgoing RTMP
stream for YouTube/Twitch — off by default), min/max subtitle duration, and **Process
while muted**, which keeps transcribing a muted source. Leaving that off is also what
stops a muted mic from running up cost.

**Partial transcription** controls whether in-progress lines are shown as you speak.

**Logging** sets verbosity; see troubleshooting.

**Translation** and **Timed metadata** are inherited from CloudVocal and are **not tested
in this fork** — v1 is transcription only. Translation in particular needs its own
separate API key for whichever service you pick.

## Note on caption segmentation

`gpt-live-transcribe` never emits a "transcription completed" event — it streams deltas
into one transcript that grows for the life of the session. This plugin therefore decides
where captions end itself: at sentence punctuation (ignoring `Dr.`, `3.5` and friends),
after a 900 ms pause, or at a 240-character cap. Without that, stream captions and SRT
output would never fire at all, since both are gated on a finalised line.

## Troubleshooting

OBS's log is at **Help → Log Files → View Current Log**, or
`%APPDATA%\obs-studio\logs\`. Set **Log level** to `INFO` in the filter first.

The plugin logs a checkpoint at each stage. Find the **last** one present and read the
row below it — that is where the pipeline stopped.

| Log line | Reached | If it is the last one you see |
|---|---|---|
| `OpenAI provider ready (model …)` | Filter constructed | Key is blank, or the filter is disabled. A blank key also logs `OpenAI API key is empty`. |
| `Connected to OpenAI realtime transcription` | Socket open | Audio is not reaching the provider. Check the source is not muted, that the filter is enabled, and try **Process while muted**. |
| `audio is reaching OpenAI` | Audio streaming | The model is not returning anything. Look for `OpenAI realtime error:` — the API's message and code are logged verbatim. |
| `first transcript received from OpenAI` | Transcribing | Text is being produced but not rendered. Look for the `caption target source … does not exist` warning below. |
| `caption target source '<name>' does not exist` | — | **Output source** points at a text source that is not in the scene, or was renamed. Re-pick it in the filter settings. |

Other lines:

| What you see | What it means |
|---|---|
| No filter in the `+` list | Plugin did not load. Check the two install paths and that OBS is 64-bit. |
| `Error connecting to OpenAI` | Bad key, no billing, or no network. The full message follows. |
| Captions stop after a pause, resume later | Expected — the idle timeout closed the socket and the next audio reopens it. |

## Cost, and why the idle timeout matters

`gpt-live-transcribe` bills **$0.017 per minute of session audio** — about **$1.02/hour**
— and it bills by **how long the socket is open, not how much speech it hears**. Silence
costs the same as talking.

So the plugin opens the connection lazily on first audio and drops it after
**Disconnect after idle** seconds of quiet. A 4-hour stream is roughly **$4** if the
socket stays open throughout; the idle timeout is what makes a quiet stream cheaper.
Setting it to `0` keeps the socket open permanently — expect to pay for every minute the
filter is enabled. Watch real spend on the
[OpenAI usage page](https://platform.openai.com/usage).

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

## Status

Builds green on CI (Windows x64 installer artifact), and the wire protocol is verified
against the live API with real audio.

**It has never been run inside OBS.** The setup steps and settings reference above are
written from the source and the locale strings, so the control names are accurate, but
the exact panel layout is inferred rather than observed. Expect small discrepancies until
someone runs it, and please correct them.

See `PLAN_OPENAI_FORK.md` for the API findings and what remains.

## License

GPLv2 — see [LICENSE](LICENSE). Inherited from CloudVocal.
