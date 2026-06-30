// streaming-tts: streaming Text-To-Speech daemon.
//
// Reads text from an input pipe (FIFO), synthesizes it in linguistic chunks,
// plays the PCM audio through the default PipeWire sink, and writes the
// per-chunk IPA phonemes to a second pipe (FIFO).
//
// Usage:
//   streaming-tts --in INPUT_FIFO [--phonemes PHONEME_FIFO]
//                 [--lang LANG] [--assets DIR] [--voice ID]
//
// The phoneme FIFO path is printed to stdout at startup so a caller can attach
// a reader to it. Both FIFOs are created if they do not already exist.
//
// Example:
//   ./streaming-tts --in /tmp/tts-in.pipe &
//   cat /tmp/moonshine-phonemes.pipe &        # read phonemes
//   echo "Hello there. Streaming speech." > /tmp/tts-in.pipe

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "moonshine-cpp.h"
#include "pipewire-sink.h"
#include "tts-benchmark.h"
#include "tts-streamer.h"

namespace {

constexpr int kSampleRateHz = 24000;  // MoonshineTTS native rate

void usage(const char* argv0) {
  std::fprintf(
      stderr,
      "Usage: %s --in INPUT_FIFO [--phonemes PHONEME_FIFO]\n"
      "          [--lang LANG] [--assets DIR] [--voice ID]\n"
      "  --in        Path to the input text FIFO (created if absent). Required.\n"
      "  --phonemes  Path to the output phoneme FIFO (default: "
      "/tmp/moonshine-phonemes.pipe).\n"
      "  --lang      Language tag (default: en_us).\n"
      "  --assets    G2P/voice asset root (default: "
      "./moonshine/core/moonshine-tts/data).\n"
      "  --voice     Voice id (e.g. kokoro_af_heart). Optional.\n",
      argv0);
}

// Ensure a FIFO exists at `path`. Returns false on a real error.
bool ensureFifo(const std::string& path) {
  if (mkfifo(path.c_str(), 0666) == 0) return true;
  if (errno == EEXIST) return true;  // already there; reuse it
  std::fprintf(stderr, "mkfifo(%s): %s\n", path.c_str(), std::strerror(errno));
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  std::string in_path;
  std::string phon_path = "/tmp/moonshine-phonemes.pipe";
  std::string lang = "en_us";
  std::string assets = "./moonshine/core/moonshine-tts/data";
  std::string voice;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Missing value for %s\n", name);
        usage(argv[0]);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--in") in_path = next("--in");
    else if (a == "--phonemes") phon_path = next("--phonemes");
    else if (a == "--lang") lang = next("--lang");
    else if (a == "--assets") assets = next("--assets");
    else if (a == "--voice") voice = next("--voice");
    else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
    else { std::fprintf(stderr, "Unknown arg: %s\n", a.c_str()); usage(argv[0]); return 2; }
  }

  if (in_path.empty()) {
    std::fprintf(stderr, "Error: --in is required.\n");
    usage(argv[0]);
    return 2;
  }

  // Writing to a phoneme pipe whose reader has gone away must yield EPIPE, not
  // a SIGPIPE that kills the daemon.
  std::signal(SIGPIPE, SIG_IGN);

  if (!ensureFifo(in_path) || !ensureFifo(phon_path)) return 1;

  // Tell the caller where to read phonemes from, before the slow startup work.
  std::printf("phonemes: %s\n", phon_path.c_str());
  std::fflush(stdout);

  try {
    // ---- Engine + audio sink ----
    std::vector<moonshine_option_t> options = {{"g2p_root", assets.c_str()}};
    if (!voice.empty()) options.push_back({"voice", voice.c_str()});

    std::fprintf(stderr, "Loading engine (lang=%s, assets=%s)...\n",
                 lang.c_str(), assets.c_str());
    moonshine::TextToSpeech tts(lang, options);
    moonshine::GraphemeToPhonemizer g2p(lang, options);

    std::fprintf(stderr, "Calibrating (this takes a moment)...\n");
    streaming_tts::calibrate(tts);

    streaming_tts::PipeWireSink sink(kSampleRateHz, 1);
    sink.start();

    // ---- Open the FIFOs ----
    // Phonemes: O_RDWR so open() succeeds immediately even with no reader yet
    //   (we never read it; this just avoids blocking on a reader).
    int phon_fd = ::open(phon_path.c_str(), O_RDWR);
    if (phon_fd < 0) {
      std::fprintf(stderr, "open(%s): %s\n", phon_path.c_str(),
                   std::strerror(errno));
      return 1;
    }
    // Input: O_RDONLY blocks until a writer connects; when all writers close,
    //   read() returns EOF, which drives end-of-utterance.
    std::fprintf(stderr, "Waiting for text on %s ...\n", in_path.c_str());
    int in_fd = ::open(in_path.c_str(), O_RDONLY);
    if (in_fd < 0) {
      std::fprintf(stderr, "open(%s): %s\n", in_path.c_str(),
                   std::strerror(errno));
      ::close(phon_fd);
      return 1;
    }

    // ---- Run ----
    streaming_tts::TTSStreamer streamer(in_fd, tts, g2p, phon_fd, &sink);

    // Wait until all input has been read and synthesized.
    while (!streamer.synthFinished()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    sink.drain();  // let queued audio finish playing
    sink.stop();
    streamer.stop();  // joins all worker threads

    ::close(in_fd);
    ::close(phon_fd);
    std::fprintf(stderr, "Done.\n");
  } catch (const moonshine::MoonshineException& e) {
    std::fprintf(stderr, "Moonshine error: %s\n", e.what());
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Error: %s\n", e.what());
    return 1;
  }
  return 0;
}
