#ifndef STREAMING_TTS_BENCHMARK_H
#define STREAMING_TTS_BENCHMARK_H

// Startup benchmark for the streaming TTS engine.
//
// Models per-call synthesis cost as a line:
//
//     synth_s ~= overhead + rtf * audio_s
//
//   * rtf (real-time factor): seconds of compute per second of audio. < 1.0
//     means synthesis outruns playback, which is what lets us stay ahead of
//     the PipeWire sink. This is the slope of the fit.
//   * overhead: fixed per-call cost (model invocation, G2P, allocation) that
//     does not depend on the chunk length. This is the intercept.
//
// These are hardware-specific, so we measure them once at startup on the
// actual target machine rather than hard-coding a guess.

#include <string>

namespace streaming_tts {

// Forward declaration: the engine wrapper from moonshine-cpp.h.
}  // namespace streaming_tts

namespace moonshine {
class TextToSpeech;
}

namespace streaming_tts {

// Result of the startup fit. Stored globally (see g_perf) so the pipeline can
// estimate synth_s for any chunk via estimate_synth_s().
struct PerfModel {
  double rtf = 0.0;         // slope: compute-seconds per audio-second
  double overhead_s = 0.0;  // intercept: fixed per-call cost in seconds
  int num_samples = 0;      // number of (audio_s, synth_s) pairs fitted
  bool valid = false;       // true once calibrate() has run successfully

  // Average seconds of synthesized audio produced per input character, learned
  // from the same calibration corpus. Lets the chunker turn a target audio
  // duration (or an affordable audio budget) into a character length.
  double audio_s_per_char = 0.0;

  // Predicted wall-clock synthesis time for a clip of `audio_s` seconds.
  double estimate_synth_s(double audio_s) const {
    return overhead_s + rtf * audio_s;
  }

  // Character count whose synthesized audio is about `audio_s` seconds long.
  double chars_for_audio_s(double audio_s) const {
    return audio_s_per_char > 0.0 ? audio_s / audio_s_per_char : 0.0;
  }

  // Largest audio duration (seconds) whose synthesis fits within `budget_s` of
  // wall-clock time: synth_s = overhead + rtf*audio_s <= budget_s. May be <= 0
  // if the budget cannot even cover the fixed overhead.
  double affordable_audio_s(double budget_s) const {
    return rtf > 0.0 ? (budget_s - overhead_s) / rtf : 0.0;
  }
};

// Global, process-wide performance model. Populated once by calibrate().
extern PerfModel g_perf;

// Run the startup benchmark against `tts`, fit the line, and store the result
// in g_perf (also returned). Synthesizes a handful of chunks of varying
// length, times each synthesize() call, and does a least-squares fit of
// synth_s against audio_s.
//
// A warm-up call (excluded from the fit) is made first so lazy one-time model
// initialization does not contaminate the per-call overhead intercept.
const PerfModel& calibrate(moonshine::TextToSpeech& tts);

}  // namespace streaming_tts

#endif  // STREAMING_TTS_BENCHMARK_H
