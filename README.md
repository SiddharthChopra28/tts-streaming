# streaming-tts

A low-latency **streaming Text-To-Speech daemon** built on the
[Moonshine](https://github.com/moonshine-ai/moonshine) C++ TTS engine.

The stock Moonshine TTS CLI works in batch mode: text in → one WAV file out.
This daemon instead reads a **stream of text** from a pipe, synthesizes it in
linguistic chunks, plays the audio **live through PipeWire**, and emits the
**phonemes** for each chunk on a second pipe — all pipelined so playback starts
quickly and keeps flowing as more text arrives.

> **Note on "streaming":** Moonshine's synthesis model (Kokoro/Piper) is
> one-shot — each `synthesize()` call returns a whole clip, not sub-utterance
> frames. So streaming here means *chunking the input text on linguistic
> boundaries and pipelining synthesis against playback*, not emitting audio
> mid-utterance.

## Features

- **Pipe in, audio + phonemes out.** Text arrives on an input FIFO; PCM is
  played to the default PipeWire sink; per-chunk IPA phonemes are written
  (newline-delimited) to a second FIFO.
- **Startup benchmark / calibration.** On launch it measures this machine's
  synthesis speed and fits `synth_s ≈ overhead + RTF · audio_s`
  (RTF = real-time factor). These numbers drive chunk sizing.
- **Latency-aware chunking.**
  - *First chunk* is kept short (targets ~500 ms of audio) so the first sound
    plays quickly, breaking at the first punctuation `.,!?;:` in that window,
    else the next whitespace.
  - *Later chunks* break at sentence/clause punctuation `?.,;:`.
  - *Adaptive fallback:* the daemon tracks how much synthesized-but-unplayed
    audio is buffered. If synthesizing the next chunk would take longer than
    that cushion lasts, the chunk is shrunk so synthesis never falls behind
    playback (important when RTF > 1).
- **Multi-threaded pipeline.** Dedicated threads for: reading input, single
  threaded synthesis, audio-cushion bookkeeping, PCM → PipeWire, and
  phonemes → pipe. All shared buffers are guarded by mutexes / condition
  variables.

## Layout

| File | Role |
|------|------|
| `streaming-tts.cpp` | CLI entry point / daemon wiring |
| `tts-streamer.{h,cpp}` | `TTSStreamer`: reader, chunker, synth, and output writer threads |
| `tts-benchmark.{h,cpp}` | Startup calibration (RTF, overhead, chars-per-audio) |
| `pipewire-sink.{h,cpp}` | `PipeWireSink`: plays float PCM via libpipewire |
| `build.sh` | Build script |

## Requirements

- Moonshine built at `../moonshine` (provides `libmoonshine.so` under
  `moonshine/build/core` and the ONNX Runtime libs under
  `moonshine/core/third-party/onnxruntime`).
- `libpipewire-0.3` development package, and a running PipeWire server.
- A C++17 compiler.

## Build

```bash
./build.sh
```

The engine/ONNX library paths are baked in as rpath, so the binary runs
directly without setting `LD_LIBRARY_PATH`.

## Run

```bash
./streaming-tts --in INPUT_FIFO [--phonemes PHONEME_FIFO] \
                [--lang LANG] [--assets DIR] [--voice ID]
```

| Flag | Default | Meaning |
|------|---------|---------|
| `--in` | *(required)* | Input text FIFO (created if absent) |
| `--phonemes` | `/tmp/moonshine-phonemes.pipe` | Output phoneme FIFO (created if absent) |
| `--lang` | `en_us` | Language tag |
| `--assets` | `./moonshine/core/moonshine-tts/data` | G2P / voice asset root |
| `--voice` | *(engine default)* | Voice id, e.g. `kokoro_af_heart` |

On startup it prints the phoneme pipe path to stdout, loads the engine,
calibrates (takes a moment), then waits for text on the input FIFO.

### Example

```bash
ASSETS=../moonshine/core/moonshine-tts/data

# Terminal 1 — start the daemon (wait for "Waiting for text...")
./streaming-tts --in /tmp/tts-in.pipe --assets "$ASSETS"

# Terminal 2 — read the phoneme stream
cat /tmp/moonshine-phonemes.pipe

# Terminal 3 — send a STREAM of text (keep the pipe open across writes)
cat > /tmp/tts-in.pipe          # type lines; Ctrl-D to end the session
# …or feed a producer straight in:
your_text_producer | tee /tmp/tts-in.pipe
```

You'll hear audio on the default PipeWire sink and see IPA phonemes appear in
Terminal 2, one line per synthesized chunk.

> A bare `echo "text" > /tmp/tts-in.pipe` works but opens *and closes* the pipe,
> which signals end-of-session — the daemon synthesizes that one line and
> exits. To stream, keep the writer open (`cat`, a `{ …; }` group, or
> `exec 3>` the FIFO).

## Known limitations

- **One session per run.** Closing the input writer (EOF) ends the session and
  the daemon exits. A persistent reopen-and-wait loop is not yet implemented.
- **Calibration adds startup latency** (it synthesizes a short corpus before
  accepting input).
- On this hardware the default voice runs at **RTF ≈ 1.15** (slower than real
  time), so the adaptive fallback is what keeps long inputs from underrunning;
  a faster voice would give more headroom.
