// Repository: Retrovue-playout
// Component: Frame Ring Buffer
// Purpose: Thread-safe circular buffer for decoded frames with atomic read/write indices.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_BUFFER_FRAME_RING_BUFFER_H_
#define RETROVUE_BUFFER_FRAME_RING_BUFFER_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace retrovue::buffer
{

  // FrameMetadata carries timing and provenance information for a decoded frame.
  struct FrameMetadata
  {
    int64_t pts;           // Presentation timestamp (in stream timebase units)
    int64_t dts;           // Decode timestamp (in stream timebase units)
    double duration;       // Frame duration in seconds
    std::string asset_uri; // Source asset identifier

    FrameMetadata()
        : pts(0), dts(0), duration(0.0) {}

    FrameMetadata(int64_t p, int64_t d, double dur, const std::string &uri)
        : pts(p), dts(d), duration(dur), asset_uri(uri) {}
  };

  // Frame holds the actual decoded frame data along with metadata.
  struct Frame
  {
    FrameMetadata metadata;
    std::vector<uint8_t> data; // Raw frame data (YUV420, etc.)
    int width;
    int height;

    Frame() : width(0), height(0) {}
  };

  // AudioFrame holds decoded audio samples (PCM) along with timing metadata.
  struct AudioFrame
  {
    std::vector<uint8_t> data;   // PCM samples (interleaved, S16 format)
    int sample_rate;             // Sample rate (e.g., 48000)
    int channels;                // Number of channels (e.g., 2 for stereo)
    int64_t pts_us;              // Presentation timestamp in microseconds
    int nb_samples;              // Number of PCM samples in this frame

    AudioFrame() : sample_rate(0), channels(0), pts_us(0), nb_samples(0) {}
  };

  // FrameRingBuffer is a lock-free circular buffer for producer-consumer frame streaming.
  //
  // Design:
  // - Fixed-size circular buffer (default: 60 frames)
  // - Atomic read/write indices for thread safety
  // - Non-blocking push/pop operations
  // - Returns success/failure instead of blocking
  // - Supports both video frames and audio frames
  //
  // Thread Model:
  // - Single producer (decode thread)
  // - Single consumer (Renderer or frame staging thread)
  //
  // Capacity Management:
  // - Buffer is full when: (write_index + 1) % capacity == read_index
  // - Buffer is empty when: write_index == read_index
  class FrameRingBuffer
  {
  public:
    // Constructs a ring buffer with the specified capacity.
    // capacity: Number of frames the buffer can hold (default: 60)
    explicit FrameRingBuffer(size_t capacity = 60);

    ~FrameRingBuffer();

    // Disable copy and move
    FrameRingBuffer(const FrameRingBuffer &) = delete;
    FrameRingBuffer &operator=(const FrameRingBuffer &) = delete;

    // Attempts to push a video frame into the buffer.
    // Returns true if successful, false if buffer is full.
    // Thread-safe for single producer.
    bool Push(const Frame &frame);

    // Attempts to push an audio frame into the buffer.
    // Returns true if successful, false if buffer is full.
    // Thread-safe for single producer.
    bool PushAudioFrame(const AudioFrame &audio_frame);

    // Attempts to pop a video frame from the buffer.
    // Returns true if successful, false if buffer is empty.
    // Thread-safe for single consumer.
    bool Pop(Frame &frame);

    // Attempts to pop an audio frame from the buffer.
    // Returns true if successful, false if buffer is empty.
    // Thread-safe for single consumer.
    bool PopAudioFrame(AudioFrame &audio_frame);

    // Peeks at the next video frame without removing it.
    // Returns pointer to frame if available, nullptr if buffer is empty.
    // Thread-safe for single consumer.
    // Note: The returned pointer is only valid until the next Pop() or Push().
    const Frame* Peek() const;

    // Peeks at the next audio frame without removing it.
    // Returns pointer to audio frame if available, nullptr if buffer is empty.
    // Thread-safe for single consumer.
    // Note: The returned pointer is only valid until the next PopAudioFrame() or PushAudioFrame().
    const AudioFrame* PeekAudioFrame() const;

    // Returns the current number of frames in the buffer.
    // This is an approximate count due to concurrent access.
    size_t Size() const;

    // Returns the maximum capacity of the buffer.
    size_t Capacity() const { return capacity_; }

    // Returns true if the buffer is empty.
    bool IsEmpty() const;

    // Returns true if the buffer is full.
    bool IsFull() const;

    // Clears all frames from the buffer.
    // Not thread-safe - caller must ensure no concurrent access.
    void Clear();

  private:
    const size_t capacity_;
    std::unique_ptr<Frame[]> buffer_;

    // Audio frame buffer (separate from video buffer)
    std::unique_ptr<AudioFrame[]> audio_buffer_;
    std::atomic<uint32_t> audio_write_index_;
    std::atomic<uint32_t> audio_read_index_;

    // Atomic indices for lock-free operation
    std::atomic<uint32_t> write_index_;
    std::atomic<uint32_t> read_index_;
  };

} // namespace retrovue::buffer

#endif // RETROVUE_BUFFER_FRAME_RING_BUFFER_H_
