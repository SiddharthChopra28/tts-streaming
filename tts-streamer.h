#ifndef STREAMING_TTS_STREAMER_H
#define STREAMING_TTS_STREAMER_H

// TTSStreamer: owns the input + synthesis side of the streaming TTS daemon.
//
//   reader thread  -> appends raw bytes from the input pipe into `buffer_`
//   synth  thread  -> chunks `buffer_`, synthesizes each chunk once (single
//                     threaded), and pushes the results into two output
//                     queues: PCM audio and phonemes.
//
// Each buffer/queue has its own mutex + condition_variable. Downstream
// consumers (a PipeWire-sink writer and a phoneme-pipe writer, added later)
// block on the output condition variables and pop results.

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace moonshine {
class TextToSpeech;
class GraphemeToPhonemizer;
}  // namespace moonshine

namespace streaming_tts {

class PipeWireSink;

class TTSStreamer {
 public:
  // `read_fd`: open readable fd (read end of a pipe / FIFO). Caller keeps
  //   ownership and must keep it open until after this object is destroyed.
  // `tts` / `g2p`: the moonshine engine objects. Caller owns them; they must
  //   outlive this streamer. Synthesis is single threaded (only the synth
  //   thread touches them), so they need no external locking.
  // `phon_write_fd`: writable fd (write end of a second pipe) for the phoneme
  //   stream. One chunk's IPA is written per line. -1 disables phoneme output.
  // `sink`: PipeWire sink for PCM playback. Caller owns it (must be started
  //   and must outlive this streamer). nullptr disables audio output.
  TTSStreamer(int read_fd, moonshine::TextToSpeech& tts,
              moonshine::GraphemeToPhonemizer& g2p, int phon_write_fd,
              PipeWireSink* sink);

  ~TTSStreamer();

  // Owns two threads that capture `this`.
  TTSStreamer(const TTSStreamer&) = delete;
  TTSStreamer& operator=(const TTSStreamer&) = delete;

  // ---- Output consumption (used by the two downstream writer threads) ----
  // Block until an item is available, then move it into `out` and return true.
  // Return false when no more items will ever arrive (synthesis finished and
  // the queue is drained, or the streamer was stopped).
  bool nextPcm(std::vector<float>& out);
  bool nextPhonemes(std::string& out);

  // True once EOF has been read AND all buffered text has been synthesized.
  bool synthFinished() const;

  // Signal both threads to stop and join them. Called by the destructor; safe
  // to call explicitly and more than once.
  void stop();

 private:
  void readerLoop();
  void synthLoop();
  void durationLoop();
  void pcmWriterLoop();   // PCM queue  -> PipeWire sink
  void phonWriterLoop();  // phoneme queue -> phoneme pipe fd

  // Pull the next chunk to synthesize out of `buffer_`, erasing the consumed
  // prefix. MUST be called with `mutex_` held. Returns "" when no complete
  // chunk can be formed yet (waits for more input unless EOF is reached).
  //
  //  * First chunk: target ~kFirstChunkTargetAudioMs of audio, then break at
  //    the first punctuation within that window, else at the first whitespace
  //    after it. Keeps time-to-first-sound low.
  //  * Later chunks: break at the first punctuation. If the synth time of that
  //    chunk would exceed the audio cushion currently left to play
  //    (audio_left_s_), shrink it to the next whitespace after the affordable
  //    character length, so synthesis never falls behind playback.
  std::string extractChunk();

  // audio_left_s_ helpers (guarded by dur_mutex_).
  double audioLeft() const;
  void addAudioLeft(double seconds);

  moonshine::TextToSpeech& tts_;
  moonshine::GraphemeToPhonemizer& g2p_;

  int read_fd_;
  int phon_write_fd_;
  PipeWireSink* sink_;

  std::thread reader_;
  std::thread synth_;
  std::thread duration_;
  std::thread pcm_writer_;
  std::thread phon_writer_;

  std::atomic<bool> running_;  // false once stop() begins; ends duration loop

  bool first_chunk_;  // synth-thread-local; guarded by mutex_ in extractChunk

  // ---- Input buffer: reader (producer) -> synth (consumer) ----
  mutable std::mutex mutex_;
  std::condition_variable cv_;  // signaled on append / EOF / stop
  std::string buffer_;          // guarded by mutex_
  bool input_eof_;              // guarded by mutex_: reader saw EOF/error
  bool stop_;                   // guarded by mutex_: stop requested

  // ---- Output PCM queue: synth (producer) -> sink writer (consumer) ----
  mutable std::mutex pcm_mutex_;
  std::condition_variable pcm_cv_;
  std::queue<std::vector<float>> pcm_queue_;  // guarded by pcm_mutex_

  // ---- Output phoneme queue: synth (producer) -> phoneme writer (consumer) -
  mutable std::mutex phon_mutex_;
  std::condition_variable phon_cv_;
  std::queue<std::string> phon_queue_;  // guarded by phon_mutex_

  // Set true when the synth thread has exited (no more outputs will be
  // produced). Guarded by BOTH output mutexes when written so either consumer
  // observes it consistently; readers hold their own queue's mutex.
  bool synth_done_;  // pcm side reads under pcm_mutex_, phon under phon_mutex_

  // ---- Output audio cushion: seconds of synthesized-but-not-yet-played
  // audio. Grows by a chunk's duration each time a synth completes; the
  // duration thread bleeds it down in real time as a proxy for playback. The
  // chunker reads it to avoid synthesizing slower than the cushion drains.
  mutable std::mutex dur_mutex_;
  double audio_left_s_;  // guarded by dur_mutex_

  // First-chunk target audio duration (milliseconds) for low first-sound
  // latency.
  static constexpr double kFirstChunkTargetAudioMs = 500.0;
};

}  // namespace streaming_tts

#endif  // STREAMING_TTS_STREAMER_H
