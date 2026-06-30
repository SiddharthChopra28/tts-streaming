// Temporary driver: initialize the TTS engine, run the startup calibration,
// and print the fitted performance model. This will later be folded into the
// streaming daemon's startup path.

#include <cstdio>
#include <string>
#include <vector>

#include "moonshine-cpp.h"
#include "tts-benchmark.h"

int main(int argc, char** argv) {
  // Asset root containing kokoro/, en_us/, etc. Override with argv[1].
  std::string asset_root =
      "../moonshine/core/moonshine-tts/data";
  std::string language = "en_us";
  if (argc > 1) asset_root = argv[1];
  if (argc > 2) language = argv[2];

  try {
    std::vector<moonshine_option_t> options = {
        {"g2p_root", asset_root.c_str()},
    };
    std::fprintf(stderr, "Loading TTS (lang=%s, assets=%s)...\n",
                 language.c_str(), asset_root.c_str());
    moonshine::TextToSpeech tts(language, options);

    const streaming_tts::PerfModel& perf = streaming_tts::calibrate(tts);
    if (!perf.valid) {
      std::fprintf(stderr, "Calibration failed.\n");
      return 1;
    }

    std::printf("rtf=%.4f overhead=%.4fs (n=%d)\n", perf.rtf, perf.overhead_s,
                perf.num_samples);
    // Sanity: predicted synth time for a 2-second clip.
    std::printf("predicted synth_s for 2.0s audio: %.3fs\n",
                perf.estimate_synth_s(2.0));
  } catch (const moonshine::MoonshineException& e) {
    std::fprintf(stderr, "Moonshine error: %s\n", e.what());
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Error: %s\n", e.what());
    return 1;
  }
  return 0;
}
