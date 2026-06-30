#ifndef STREAMING_PIPEWIRE_SINK_H
#define STREAMING_PIPEWIRE_SINK_H

// PipeWireSink: plays mono float PCM through the default PipeWire sink.
//
// PipeWire drives audio from a realtime callback (on_process) that runs on a
// thread PipeWire owns (pw_thread_loop). Producers call write() to enqueue
// samples into an internal FIFO; the callback pulls from that FIFO each cycle
// and emits silence on underrun. This decouples our synthesis-paced producer
// from PipeWire's clock-paced consumer.

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

namespace streaming_tts {

class PipeWireSink {
 public:
  explicit PipeWireSink(uint32_t rate = 24000, uint32_t channels = 1);
  ~PipeWireSink();

  PipeWireSink(const PipeWireSink&) = delete;
  PipeWireSink& operator=(const PipeWireSink&) = delete;

  // Create + connect the stream and start the realtime loop. Call once.
  void start();

  // Enqueue samples for playback (interleaved if channels > 1). Thread-safe.
  void write(const std::vector<float>& samples);

  // Block until the FIFO has been fully consumed by the callback (all queued
  // audio has been handed to PipeWire). Use before stop() so trailing audio is
  // not cut off.
  void drain();

  // Disconnect, stop the loop, and free resources. Safe to call more than once.
  void stop();

 private:
  static void onProcess(void* data);

  uint32_t rate_;
  uint32_t channels_;

  pw_thread_loop* loop_;
  pw_stream* stream_;
  pw_stream_events stream_events_;  // must outlive the stream
  spa_hook stream_listener_;
  bool started_;

  std::mutex mutex_;
  std::condition_variable cv_;  // signaled by callback when it drains samples
  std::deque<float> fifo_;      // guarded by mutex_
};

}  // namespace streaming_tts

#endif  // STREAMING_PIPEWIRE_SINK_H
