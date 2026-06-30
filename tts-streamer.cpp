#include "tts-streamer.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>

#include "moonshine-cpp.h"
#include "pipewire-sink.h"
#include "tts-benchmark.h"

namespace streaming_tts {

namespace {
// Write all bytes, retrying short writes and EINTR. Returns false on a real
// error (e.g. EPIPE: the phoneme reader closed its end).
bool writeAll(int fd, const char* data, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t w = ::write(fd, data + off, len - off);
    if (w > 0) {
      off += static_cast<size_t>(w);
    } else if (w < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}
}  // namespace

namespace {
// Characters that end a chunk. Note the two sets differ per the spec:
// the first chunk also breaks on '!', later chunks do not.
constexpr const char* kFirstChunkPunct = ".,!?;:";
constexpr const char* kLaterChunkPunct = "?.,;:";
constexpr const char* kWhitespace = " \t\r\n";
}  // namespace

TTSStreamer::TTSStreamer(int read_fd, moonshine::TextToSpeech& tts,
                         moonshine::GraphemeToPhonemizer& g2p,
                         int phon_write_fd, PipeWireSink* sink)
    : tts_(tts),
      g2p_(g2p),
      read_fd_(read_fd),
      phon_write_fd_(phon_write_fd),
      sink_(sink),
      running_(true),
      first_chunk_(true),
      input_eof_(false),
      stop_(false),
      synth_done_(false),
      audio_left_s_(0.0) {
  // Spawn threads last, after all members are initialized.
  reader_ = std::thread(&TTSStreamer::readerLoop, this);
  synth_ = std::thread(&TTSStreamer::synthLoop, this);
  duration_ = std::thread(&TTSStreamer::durationLoop, this);
  pcm_writer_ = std::thread(&TTSStreamer::pcmWriterLoop, this);
  phon_writer_ = std::thread(&TTSStreamer::phonWriterLoop, this);
}

TTSStreamer::~TTSStreamer() { stop(); }

// ---------------------------------------------------------------------------
// Reader thread: pipe -> buffer_
// ---------------------------------------------------------------------------
void TTSStreamer::readerLoop() {
  char tmp[4096];
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_) break;
    }

    ssize_t n = ::read(read_fd_, tmp, sizeof(tmp));
    if (n > 0) {
      std::lock_guard<std::mutex> lock(mutex_);
      buffer_.append(tmp, static_cast<size_t>(n));
      cv_.notify_all();  // wake the synth thread
    } else if (n == 0) {
      // EOF: all write ends closed.
      std::lock_guard<std::mutex> lock(mutex_);
      input_eof_ = true;
      cv_.notify_all();
      break;
    } else {
      if (errno == EINTR) continue;  // interrupted by signal; retry
      std::lock_guard<std::mutex> lock(mutex_);
      input_eof_ = true;
      cv_.notify_all();
      break;
    }
  }
}

// Boundary-aware chunking. MUST hold mutex_. Returns "" if no complete chunk
// can be formed yet (caller waits for more input). Erases the consumed prefix.
std::string TTSStreamer::extractChunk() {
  // Drop leading whitespace so chunks never start with a space.
  size_t first = buffer_.find_first_not_of(kWhitespace);
  if (first == std::string::npos) {
    buffer_.clear();
    return "";
  }
  if (first > 0) buffer_.erase(0, first);

  const size_t n = buffer_.size();
  const streaming_tts::PerfModel& perf = streaming_tts::g_perf;
  size_t end = std::string::npos;

  if (first_chunk_) {
    // Target character length for ~kFirstChunkTargetAudioMs of audio.
    size_t target = 64;  // fallback if benchmark unavailable
    if (perf.valid && perf.audio_s_per_char > 0.0) {
      double t = perf.chars_for_audio_s(kFirstChunkTargetAudioMs / 1000.0);
      if (t >= 1.0) target = static_cast<size_t>(t);
    }

    // First punctuation within the [0, target) window.
    size_t p = buffer_.find_first_of(kFirstChunkPunct);
    if (p != std::string::npos && p < target) {
      end = p + 1;  // include the punctuation
    } else {
      // No punctuation in window: break at first whitespace after `target`.
      if (n <= target) {
        if (!input_eof_) return "";  // not enough text yet; wait
        end = n;                     // EOF: flush whatever we have
      } else {
        size_t ws = buffer_.find_first_of(kWhitespace, target);
        if (ws == std::string::npos) {
          if (!input_eof_) return "";
          end = n;
        } else {
          end = ws;  // break before the whitespace (exclusive)
        }
      }
    }
  } else {
    // Later chunks: break at the first punctuation mark.
    size_t p = buffer_.find_first_of(kLaterChunkPunct);
    if (p == std::string::npos) {
      if (!input_eof_) return "";  // wait for a sentence boundary
      end = n;                     // EOF: flush remainder
    } else {
      end = p + 1;
    }

    // Fallback limit: if synthesizing this chunk would take longer than the
    // audio cushion we have left to play, shrink it so synth stays ahead.
    if (perf.valid && perf.audio_s_per_char > 0.0) {
      double cand_audio_s =
          static_cast<double>(end) * perf.audio_s_per_char;
      double cand_synth_s = perf.estimate_synth_s(cand_audio_s);
      double left = audioLeft();
      if (left < cand_synth_s) {
        // Largest audio we can afford within the remaining cushion.
        double afford_audio_s = perf.affordable_audio_s(left);
        double corrected_chars = afford_audio_s > 0.0
                                     ? perf.chars_for_audio_s(afford_audio_s)
                                     : 0.0;
        size_t from = corrected_chars > 0.0
                          ? static_cast<size_t>(corrected_chars)
                          : 0;
        // Break at the next whitespace after the affordable length, but only
        // if that yields a strictly smaller (and non-empty) chunk.
        size_t ws = buffer_.find_first_of(kWhitespace, from);
        if (ws != std::string::npos && ws > 0 && ws < end) {
          end = ws;
        }
      }
    }
  }

  if (end == std::string::npos || end == 0) return "";
  std::string chunk = buffer_.substr(0, end);
  buffer_.erase(0, end);
  first_chunk_ = false;  // we are producing a chunk now
  return chunk;
}

double TTSStreamer::audioLeft() const {
  std::lock_guard<std::mutex> lock(dur_mutex_);
  return audio_left_s_;
}

void TTSStreamer::addAudioLeft(double seconds) {
  std::lock_guard<std::mutex> lock(dur_mutex_);
  audio_left_s_ += seconds;
}

// Bleeds the audio cushion down in real time as a proxy for playback progress.
void TTSStreamer::durationLoop() {
  auto prev = std::chrono::steady_clock::now();
  while (running_.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - prev).count();
    prev = now;
    std::lock_guard<std::mutex> lock(dur_mutex_);
    audio_left_s_ = std::max(0.0, audio_left_s_ - dt);
  }
}

// Consumer: PCM queue -> PipeWire sink. Ends when synthesis is done & drained.
void TTSStreamer::pcmWriterLoop() {
  std::vector<float> pcm;
  while (nextPcm(pcm)) {
    if (sink_ != nullptr) sink_->write(pcm);
  }
}

// Consumer: phoneme queue -> phoneme pipe. One chunk's IPA per line.
void TTSStreamer::phonWriterLoop() {
  std::string ph;
  while (nextPhonemes(ph)) {
    if (phon_write_fd_ < 0) continue;
    ph.push_back('\n');  // newline-delimited so a reader can split per chunk
    if (!writeAll(phon_write_fd_, ph.data(), ph.size())) {
      // Reader closed its end; stop writing but keep draining the queue so the
      // synth thread is never blocked on a full queue.
      phon_write_fd_ = -1;
    }
  }
}

// ---------------------------------------------------------------------------
// Synth thread: buffer_ -> (PCM queue, phoneme queue). Single threaded.
// ---------------------------------------------------------------------------
void TTSStreamer::synthLoop() {
  for (;;) {
    std::string chunk;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      // Wait until there is something to do: data buffered, or we're told to
      // stop, or input has ended (so we can flush any remainder then exit).
      cv_.wait(lock, [this] {
        return !buffer_.empty() || stop_ || input_eof_;
      });

      if (stop_) break;

      if (buffer_.empty()) {
        // Nothing buffered. If input is finished, we're done; otherwise this
        // was a spurious wake — loop and wait again.
        if (input_eof_) break;
        continue;
      }

      chunk = extractChunk();  // still holding mutex_
    }

    if (chunk.empty()) continue;

    // --- Synthesis (no input lock held; engine touched only by this thread) -
    moonshine::TtsSynthesisResult audio = tts_.synthesize(chunk);
    std::string phonemes = g2p_.toIpa(chunk);

    // Grow the cushion by this chunk's audio duration.
    if (audio.sampleRateHz > 0) {
      addAudioLeft(static_cast<double>(audio.samples.size()) /
                   audio.sampleRateHz);
    }

    // Publish PCM.
    {
      std::lock_guard<std::mutex> lock(pcm_mutex_);
      pcm_queue_.push(std::move(audio.samples));
      pcm_cv_.notify_one();
    }
    // Publish phonemes.
    {
      std::lock_guard<std::mutex> lock(phon_mutex_);
      phon_queue_.push(std::move(phonemes));
      phon_cv_.notify_one();
    }
  }

  // Synth thread exiting: mark both output queues as finished so blocked
  // consumers wake and observe end-of-stream.
  {
    std::lock_guard<std::mutex> lock(pcm_mutex_);
    synth_done_ = true;
    pcm_cv_.notify_all();
  }
  {
    std::lock_guard<std::mutex> lock(phon_mutex_);
    synth_done_ = true;
    phon_cv_.notify_all();
  }
}

// ---------------------------------------------------------------------------
// Output consumption
// ---------------------------------------------------------------------------
bool TTSStreamer::nextPcm(std::vector<float>& out) {
  std::unique_lock<std::mutex> lock(pcm_mutex_);
  pcm_cv_.wait(lock, [this] { return !pcm_queue_.empty() || synth_done_; });
  if (pcm_queue_.empty()) return false;  // synth_done_ and drained
  out = std::move(pcm_queue_.front());
  pcm_queue_.pop();
  return true;
}

bool TTSStreamer::nextPhonemes(std::string& out) {
  std::unique_lock<std::mutex> lock(phon_mutex_);
  phon_cv_.wait(lock, [this] { return !phon_queue_.empty() || synth_done_; });
  if (phon_queue_.empty()) return false;
  out = std::move(phon_queue_.front());
  phon_queue_.pop();
  return true;
}

bool TTSStreamer::synthFinished() const {
  std::lock_guard<std::mutex> lock(pcm_mutex_);
  return synth_done_;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------
void TTSStreamer::stop() {
  running_.store(false, std::memory_order_relaxed);  // end duration loop
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
    cv_.notify_all();  // wake synth thread if waiting
  }
  // Wake any blocked consumers too.
  { std::lock_guard<std::mutex> lock(pcm_mutex_); pcm_cv_.notify_all(); }
  { std::lock_guard<std::mutex> lock(phon_mutex_); phon_cv_.notify_all(); }

  // The reader thread may be blocked in read(); it returns on EOF, incoming
  // data, or the owner closing the fd. The daemon closes the read end on
  // shutdown to unblock it.
  if (duration_.joinable()) duration_.join();
  if (synth_.joinable()) synth_.join();
  if (reader_.joinable()) reader_.join();
  // Writer threads end once nextPcm/nextPhonemes return false (synth_done_).
  if (pcm_writer_.joinable()) pcm_writer_.join();
  if (phon_writer_.joinable()) phon_writer_.join();
}

}  // namespace streaming_tts
