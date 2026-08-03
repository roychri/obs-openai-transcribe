# Plan: OBS plugin for `gpt-live-transcribe`

Fork + strip `royshil/cloudvocal` into an OpenAI-only realtime captions plugin for **OBS 32.2.1, Windows x64**.

## Context

OpenAI shipped `gpt-live-transcribe` on 2026-07-28 — a streaming STT model on
`wss://api.openai.com/v1/realtime?intent=transcription`, $0.017/min of session audio,
with a tunable `delay` knob and three context inputs (`prompt`, `keywords`, `languages`)
that are a strong fit for live streaming (game names, guest handles, jargon).

`cloudvocal` already solves every hard OBS-side problem — audio-filter tap, resampling,
caption delivery to a text source, CEA-608 embedding into the RTMP stream, SRT writing,
settings UI — and its `DeepgramProvider` is a working Boost.Beast WSS provider that the
OpenAI one can be modelled on directly.

But the repo is stale: **last commit 2024-12-19**, `buildspec.json` pins **OBS 30.1.2**,
and its Conan stack pulls gRPC + protobuf + abseil + aws-sdk-cpp purely for the
Clova/Google/AWS providers. Stripping those is most of the de-risking.

**v1 scope: transcription only.** No translation.

## Baseline facts (verified in the clone)

| Thing | Where |
|---|---|
| Provider ABC (`init` / `sendAudioBufferToTranscription` / `readResultsFromTranscription` / `shutdown`) | `src/cloud-providers/cloud-provider.h:13` |
| Provider factory | `src/cloud-providers/cloud-provider.cpp:9` |
| Closest template — WSS + TLS + JSON results | `src/cloud-providers/deepgram/deepgram-provider.cpp` |
| Resample target **hardcoded 16 kHz** | `src/cloudvocal-data.h:14`, consumed at `src/cloudvocal.cpp:308` |
| Caption → text source | `src/cloudvocal-callbacks.cpp:23` |
| Caption → RTMP (CEA-608) | `src/cloudvocal-callbacks.cpp:191` (`obs_output_output_caption_text2`) |
| Provider dropdown | `src/cloudvocal-properties.cpp:278` |
| Windows CI (already exists) | `.github/workflows/build-project.yaml`, `.github/scripts/Build-Windows.ps1` |

Boost 1.86 (Beast) + OpenSSL are already Conan deps and stay. `nlohmann/json` is fetched
by `cmake/FetchNlohmannJSON.cmake` and stays.

## Build strategy

This box is **aarch64 Linux with no OBS** — a Windows x64 plugin cannot be built here.

- **Primary loop: build locally on the Windows machine that runs OBS.**
  `.github/scripts/Build-Windows.ps1` is the same script CI runs and works standalone with
  VS 2022 Build Tools + CMake + Conan. Seconds per iteration instead of a CI round-trip,
  and it costs no Actions minutes.
- **CI is for release artifacts.** Keep the repo **public** — Actions on standard runners
  is unmetered for public repos, and the GPLv2 fork is going to be public anyway. On a
  private repo the free plan's 2,000 min/month drain at a **2× multiplier** on Windows
  (~1,000 real minutes), and a cold build here is 20–35 min.
- **The macOS job is disabled** (`if: false`). At a 10× multiplier it would consume the
  entire allowance on a build this fork does not target.
- Nothing sensitive lives in the repo: the OpenAI key is an OBS settings field, not a CI
  secret, so there is no reason to keep the repo private.

## Steps

### 1. Strip ✅ done
- Delete `src/cloud-providers/{clova,google,aws,revai,deepgram}/`, drop them from the
  factory in `cloud-provider.cpp`, and remove `add_subdirectory(src/cloud-providers/aws)`
  plus `BuildClovaAPIs.cmake` / `BuildGoogleAPIs.cmake` includes from `CMakeLists.txt`.
- Reduce `conanfile.txt` to `boost/1.86.0`, `openssl/3.3.2`, `zlib/1.3.1`. Delete the
  gRPC/protobuf/abseil/c-ares/aws-sdk-cpp requires and the `transcribestreaming` option.
- Keep `src/cloud-translation/` compiling but unwired from the UI (v1 is transcription
  only; leaving it intact keeps the door open without extra work now).
- Drop `src/timed-metadata/` if it pulls aws-sdk — check before deleting.

### 2. Rebase onto OBS 32.2.x — ✅ done (on 31.1.1, see below)
`buildspec.json` was moved from **OBS 30.1.2 → 31.1.1** with obs-deps/Qt6 bumped to
`2025-07-11`, matching `obsproject/obs-plugintemplate` master (checked 2026-08-03).

**It is deliberately not on 32.2.1.** Upstream's template has not been bumped past 31.1.1
(last buildspec change 2025-07-14), so 32.2.1 would mean inventing dependency hashes and
building against a source tree the template's cmake has never been tested with. Compiling
fresh against 31.1.1 is the low-risk move, and 32.2's breakage was about plugins needing
recompilation and new Windows DLL policy — not new headers.

**Open question, resolved by testing:** if the plugin fails to load in OBS 32.2.1, bump
`obs-studio.version` to 32.2.1 and compute the tarball SHA-256 ourselves. Don't do this
speculatively.

### 3. Make the sample rate provider-driven ✅ done
`TRANSCRIPTION_SAMPLE_RATE` is baked in at 16 kHz; OpenAI wants **24 kHz PCM16 mono**.
Replace the macro with a rate carried on `cloudvocal_data` (set from the selected
provider) and used when building `dst` at `cloudvocal.cpp:308`. Re-create the resampler on
provider change — `restart_cloud_provider()` is the natural hook.

### 4. `OpenAIProvider` ✅ done
New `src/cloud-providers/openai/openai-provider.{h,cpp}`, copying `DeepgramProvider`'s
Beast/SSL member layout (`ioc`, `ssl_ctx`, `resolver`, `ws`) and `needs_results_thread = true`.

- **`init()`** — resolve `api.openai.com:443`, SNI, TLS handshake, then decorate the WS
  upgrade with `Authorization: Bearer <api_key>` and `OpenAI-Beta: realtime=v1`, and
  handshake on `/v1/realtime?intent=transcription`. Immediately write one `session.update`:
  ```json
  {"type":"session.update","session":{"type":"transcription","audio":{"input":{
     "format":{"type":"audio/pcm","rate":24000},
     "transcription":{"model":"gpt-live-transcribe","delay":"low",
                      "prompt":"...","keywords":"...","languages":["en"]},
     "turn_detection":{"type":"server_vad"}}}}}
  ```
  Keep server VAD on — unlike the doc's manual-commit example, OBS has no turn boundaries.
- **`sendAudioBufferToTranscription()`** — float → clamped int16 exactly as Deepgram does,
  then **base64** (`boost/beast/core/detail/base64.hpp`) into a *text* frame
  `{"type":"input_audio_buffer.append","audio":"<b64>"}`. This is the key divergence from
  Deepgram, which writes raw binary. Set `ws.text(true)`.
- **`readResultsFromTranscription()`** — blocking `ws.read()`; map
  `conversation.item.input_audio_transcription.delta` → `DETECTION_RESULT_PARTIAL` and
  `...completed` → `DETECTION_RESULT_SPEECH`. Accumulate deltas per item id so partials
  render as a growing line rather than fragments. Log and ignore `error` events; treat a
  closed socket as `running = false` so the filter can reconnect.
- **`shutdown()`** — `ws.close(normal)`.

Register `"openai"` in the factory and add it to the dropdown at `properties.cpp:278`.

### 5. Settings UI ✅ done
Add to the general group, shown only when provider == `openai`:
`delay` (combo: minimal/low/medium/high/xhigh), `prompt` (multiline), `keywords`
(multiline, one per line), and reuse the existing API-key password field. The existing
single-language selector maps to a one-element `languages` array — good enough for v1.

### 6. Cost guard ✅ done
Billing is **session wall-clock, not speech** — an idle open socket bills $1.02/hr. Add an
idle timeout that closes the WS after N seconds with no audio (source muted / scene
inactive) and reconnects lazily on the next buffer. Also honour the existing
`process_while_muted` flag by not opening the socket at all while muted.

## API key handling

The key is **an OBS filter setting**, not a repo artefact. Concretely:

- **Not in source**, not in `.env`, not a GitHub secret. Nothing to configure before build.
- Entered at runtime in the OBS filter's **API Key** field
  (`transcription_cloud_provider_api_key`, rendered `OBS_TEXT_PASSWORD` so it is masked in
  the UI). Read into `gf->cloud_provider_api_key` and sent as
  `Authorization: Bearer <key>` on the WebSocket upgrade.
- OBS persists it **in plaintext** in the scene-collection JSON under
  `%APPDATA%\obs-studio\basic\scenes\*.json`. Same as every other OBS plugin credential,
  but worth knowing before sharing a scene collection or a screen recording of settings.
- CI never needs it — there are no integration tests in the build.
- The only other place it is needed is the off-OBS smoke client (verification step 2),
  which reads `OPENAI_API_KEY` from the environment.

## Verified against the live API (2026-08-03)

Run with `tools/smoke_test.py` against `nemotron-asr/sample1.wav` (13.7 s of speech).
Transcription came back clean and accurate. Four things contradicted the plan as written:

1. **`OpenAI-Beta: realtime=v1` is fatal.** The server closes with
   `4000 invalid_request_error.beta_api_shape_disabled`. Send `Authorization` only. (That
   header came from a March-2025 write-up, not the current docs.)
2. **`turn_detection` must be `null`.** Any object — including the default `server_vad` —
   is rejected with *"Turn detection is not supported for this transcription model"*, and
   because that rejection fails the whole `session.update`, the session silently stays
   unconfigured and you get VAD events but **no transcription at all**. Omitting the field
   does not help either.
3. **No `completed` event, ever.** Waited 40 s past end-of-audio: only
   `...transcription.delta`, all under a single `item_id` that never rotates. The transcript
   just grows. This is the big one — `set_text_callback` gates stream captions
   (`cloudvocal-callbacks.cpp:266`) and file/SRT output (`:271`) on
   `DETECTION_RESULT_SPEECH`, so with partials alone **RTMP captions and SRT would be
   silently dead** and the text source would render one unbounded line.
   → Provider now segments client-side: on sentence punctuation (with an abbreviation
   guard and a 12-char minimum), on a 900 ms pause, or at a 240-char hard cap.
4. **Latency to first word**, measured, since OpenAI publishes no figures:

   | `delay` | time to first delta |
   |---|---|
   | `minimal` | 0.46 s |
   | `low` | 0.85 – 1.17 s |
   | `high` | 1.73 s |

   Connect handshake is 0.3–0.7 s. Default is `low`; `minimal` is worth trying for live
   captions if the extra revision churn is tolerable.

## Verification

1. **CI**: green Windows build producing an installer artifact — the gate for everything else.
2. **Smoke, off-OBS**: before touching the plugin, confirm the wire protocol with a throwaway
   Python client on this box (key from env, feed a WAV at 24 kHz) so protocol bugs are
   debugged in minutes, not CI cycles.
3. **In OBS 32.2.1**: add the filter to a mic source → captions appear in a text source
   within ~1 s. Check `delay: minimal` vs `high` for revision churn.
4. **Keywords**: say 5 unusual proper nouns with and without them in the `keywords` box;
   accuracy should visibly improve.
5. **Stream path**: start an RTMP stream with "caption to stream" enabled, confirm CEA-608
   captions appear on the platform side.
6. **Cost**: 10-minute session should bill ≈ $0.17; verify the idle timeout by muting for
   2 minutes and confirming the socket closes.

## Known risks

- **OBS 32.2 template rebase** is the biggest unknown — Windows DLL-loading and mitigation
  changes broke other third-party plugins. Mitigation: take the current plugintemplate's
  cmake/CI wholesale rather than patching the 2024 one.
- `delay` tier semantics are undocumented in ms; tune empirically (step 3).
- No word timestamps from `gpt-live-transcribe`, so SRT output will fall back to
  filter-side wall-clock timing rather than model timestamps. Acceptable for live captions;
  if real SRT timing matters, that's a `whisper-1` post-pass on the recording.

## Build status (2026-08-03)

CI is green on Windows x64 and uploads an installer artifact. Getting there took nine
runs; five failures were build-system, one was a real bug in the new code, and two were
latent cloudvocal bugs that warnings-as-errors exposed.

Worth knowing before touching the build again:

- **The template sync reverts every cloudvocal-specific setting.** Taking
  `obs-plugintemplate` wholesale fixed the OBS SDK, but silently dropped the Conan install
  step from `.github/actions/build-plugin` and flipped `ENABLE_FRONTEND_API` back to false
  in `CMakePresets.json`. Re-check both after any future sync.
- **`.gitignore` is an allowlist** (`/*` plus exceptions). Anything new — `tools/`,
  `.gersemirc`, docs — is silently untracked until explicitly un-ignored. This bit three
  times; check `git status --ignored` after adding files.
- **gersemi is pinned to 0.21.0** in `.github/actions/run-gersemi`, installed from PyPI
  because the `obsproject/tools` tap formula fails on current Homebrew. The pin is
  load-bearing: 0.28.0 reformats files that 0.21.0 considers clean.
- **Keep `CMAKE_COMPILE_WARNING_AS_ERROR`.** It caught a refcount trap around the
  deprecated `obs_scene_sceneitem_from_source` and a `size_t`→`int` narrowing in the HMAC
  path. `LNK4099` from Conan's PDB-less OpenSSL is suppressed narrowly instead.

### Still open

- **Never run inside OBS.** Compiling and linking says nothing about whether captions
  render, the idle disconnect behaves, or CEA-608 embedding works.
- **Built against OBS 31.1.1, not 32.2.1** — see step 2. If the plugin fails to load in
  32.2.1, that is the first thing to change.
- **curl is still in the build** only because `cloud-translation` and `timed-metadata`
  still compile. Neither is reachable in v1. Dropping them removes curl and its prebuilt
  third-party fetch entirely.
