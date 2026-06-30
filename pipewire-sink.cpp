#include "pipewire-sink.h"

#include <algorithm>
#include <cstring>

namespace streaming_tts {

PipeWireSink::PipeWireSink(uint32_t rate, uint32_t channels)
    : rate_(rate),
      channels_(channels),
      loop_(nullptr),
      stream_(nullptr),
      stream_events_{},
      started_(false) {
  pw_init(nullptr, nullptr);
  loop_ = pw_thread_loop_new("moonshine-tts-sink", nullptr);
}

PipeWireSink::~PipeWireSink() { stop(); }

// Realtime callback: fill one PipeWire buffer from our FIFO.
void PipeWireSink::onProcess(void* data) {
  auto* self = static_cast<PipeWireSink*>(data);
  pw_buffer* b = pw_stream_dequeue_buffer(self->stream_);
  if (b == nullptr) return;  // no buffer available this cycle

  spa_buffer* buf = b->buffer;
  auto* dst = static_cast<float*>(buf->datas[0].data);
  if (dst == nullptr) {
    pw_stream_queue_buffer(self->stream_, b);
    return;
  }

  const uint32_t stride = sizeof(float) * self->channels_;
  uint32_t n_frames = buf->datas[0].maxsize / stride;
  // Honor the amount PipeWire actually wants this cycle, if it told us.
  if (b->requested != 0) {
    n_frames = std::min<uint32_t>(n_frames, b->requested);
  }
  const uint32_t n_samples = n_frames * self->channels_;

  uint32_t filled = 0;
  {
    std::lock_guard<std::mutex> lock(self->mutex_);
    while (filled < n_samples && !self->fifo_.empty()) {
      dst[filled++] = self->fifo_.front();
      self->fifo_.pop_front();
    }
  }
  self->cv_.notify_all();  // wake drain()/backpressure waiters

  // Underrun: pad the rest with silence so playback stays continuous.
  for (uint32_t i = filled; i < n_samples; ++i) dst[i] = 0.0f;

  buf->datas[0].chunk->offset = 0;
  buf->datas[0].chunk->stride = static_cast<int32_t>(stride);
  buf->datas[0].chunk->size = n_frames * stride;

  pw_stream_queue_buffer(self->stream_, b);
}

void PipeWireSink::start() {
  if (started_) return;

  pw_thread_loop_start(loop_);
  pw_thread_loop_lock(loop_);

  stream_events_.version = PW_VERSION_STREAM_EVENTS;
  stream_events_.process = &PipeWireSink::onProcess;

  stream_ = pw_stream_new_simple(
      pw_thread_loop_get_loop(loop_), "moonshine-tts",
      pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                        "Playback", PW_KEY_MEDIA_ROLE, "Music", nullptr),
      &stream_events_, this);

  // Describe our audio format: F32, mono, rate_.
  uint8_t buffer[1024];
  spa_pod_builder pod = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  spa_audio_info_raw info = {};
  info.format = SPA_AUDIO_FORMAT_F32;
  info.rate = rate_;
  info.channels = channels_;
  const spa_pod* params[1];
  params[0] = spa_format_audio_raw_build(&pod, SPA_PARAM_EnumFormat, &info);

  pw_stream_connect(
      stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                   PW_STREAM_FLAG_MAP_BUFFERS |
                                   PW_STREAM_FLAG_RT_PROCESS),
      params, 1);

  pw_thread_loop_unlock(loop_);
  started_ = true;
}

void PipeWireSink::write(const std::vector<float>& samples) {
  if (samples.empty()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  fifo_.insert(fifo_.end(), samples.begin(), samples.end());
}

void PipeWireSink::drain() {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this] { return fifo_.empty(); });
}

void PipeWireSink::stop() {
  if (loop_ != nullptr) {
    // Stop the loop before destroying the stream so no callback races us.
    pw_thread_loop_stop(loop_);
  }
  if (stream_ != nullptr) {
    pw_stream_destroy(stream_);
    stream_ = nullptr;
  }
  if (loop_ != nullptr) {
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
  }
  started_ = false;
}

}  // namespace streaming_tts
