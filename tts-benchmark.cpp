#include "tts-benchmark.h"

#include <chrono>
#include <cstdio>
#include <vector>

#include "moonshine-cpp.h"

namespace streaming_tts {

// Definition of the global model declared in the header.
PerfModel g_perf;

namespace {

// Calibration corpus: chunks chosen to span short clauses to long sentences so
// the fit has leverage at both ends of the audio-duration range. The exact
// wording does not matter; what matters is the spread of resulting clip
// lengths. ~10 phrases is plenty for a stable two-parameter fit.
const std::vector<std::string>& calibration_chunks() {
  static const std::vector<std::string> chunks = {
      "Yes.",
      "Hello there.",
      "The robot is ready.",
      "Please wait one moment.",
      "I am synthesizing your text now.",
      "The quick brown fox jumps over the lazy dog.",
      "Streaming speech keeps the conversation feeling natural and responsive.",
      "When the model generates audio faster than real time, playback never has "
      "to wait for the next chunk.",
      "Calibration measures how this particular machine performs, because real "
      "time factor and fixed overhead are hardware specific and no formula can "
      "substitute for measuring them directly.",
      "Low latency text to speech depends on overlapping synthesis with "
      "playback, splitting incoming text on linguistic boundaries, and keeping "
      "the synthesizer comfortably ahead of the audio sink so that the listener "
      "hears a continuous, uninterrupted stream of natural sounding speech.",
  };
  return chunks;
}

}  // namespace

const PerfModel& calibrate(moonshine::TextToSpeech& tts) {
  const std::vector<std::string>& chunks = calibration_chunks();

  // Warm-up: the first synthesize() call may trigger lazy model loading and
  // allocator warm-up. Excluding it keeps that one-time cost out of the
  // per-call overhead intercept.
  if (!chunks.empty()) {
    tts.synthesize(chunks.front());
  }

  // Collect (audio_s, synth_s) pairs.
  std::vector<double> audio;   // x
  std::vector<double> synth;   // y
  audio.reserve(chunks.size());
  synth.reserve(chunks.size());
  double total_chars = 0.0;    // for the chars<->audio relationship
  double total_audio_s = 0.0;

  for (const std::string& chunk : chunks) {
    auto t0 = std::chrono::steady_clock::now();
    moonshine::TtsSynthesisResult r = tts.synthesize(chunk);
    auto t1 = std::chrono::steady_clock::now();

    double synth_s = std::chrono::duration<double>(t1 - t0).count();
    double audio_s =
        r.sampleRateHz > 0
            ? static_cast<double>(r.samples.size()) / r.sampleRateHz
            : 0.0;

    if (audio_s <= 0.0) {
      continue;  // skip degenerate clips so they don't skew the fit
    }
    audio.push_back(audio_s);
    synth.push_back(synth_s);
    total_chars += static_cast<double>(chunk.size());
    total_audio_s += audio_s;
    std::fprintf(stderr,
                 "[calibrate] audio=%.3fs synth=%.3fs (rtf~=%.3f)\n",
                 audio_s, synth_s, synth_s / audio_s);
  }

  const int n = static_cast<int>(audio.size());
  if (n < 2) {
    std::fprintf(stderr,
                 "[calibrate] not enough usable samples (%d); model invalid\n",
                 n);
    g_perf = PerfModel{};
    return g_perf;
  }

  // Ordinary least-squares fit of y = a + b*x.
  //   b (slope/rtf)      = (n*Sxy - Sx*Sy) / (n*Sxx - Sx^2)
  //   a (intercept/over) = (Sy - b*Sx) / n
  double Sx = 0, Sy = 0, Sxx = 0, Sxy = 0;
  for (int i = 0; i < n; ++i) {
    Sx += audio[i];
    Sy += synth[i];
    Sxx += audio[i] * audio[i];
    Sxy += audio[i] * synth[i];
  }
  double denom = n * Sxx - Sx * Sx;
  if (denom == 0.0) {
    // All x identical (shouldn't happen with varied chunks): fall back to the
    // mean ratio as rtf and zero overhead.
    g_perf.rtf = Sy / Sx;
    g_perf.overhead_s = 0.0;
  } else {
    g_perf.rtf = (n * Sxy - Sx * Sy) / denom;
    g_perf.overhead_s = (Sy - g_perf.rtf * Sx) / n;
  }
  g_perf.num_samples = n;
  g_perf.audio_s_per_char =
      total_chars > 0.0 ? total_audio_s / total_chars : 0.0;
  g_perf.valid = true;

  std::fprintf(stderr,
               "[calibrate] fit over %d points: rtf=%.4f overhead=%.4fs "
               "audio_s_per_char=%.5f\n",
               g_perf.num_samples, g_perf.rtf, g_perf.overhead_s,
               g_perf.audio_s_per_char);
  return g_perf;
}

}  // namespace streaming_tts
