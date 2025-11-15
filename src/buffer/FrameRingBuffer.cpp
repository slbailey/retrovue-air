// Repository: Retrovue-playout
// Component: Frame Ring Buffer
// Purpose: Thread-safe circular buffer implementation for decoded frames.
// Copyright (c) 2025 RetroVue

#include "retrovue/buffer/FrameRingBuffer.h"

#include <algorithm>

namespace retrovue::buffer {

FrameRingBuffer::FrameRingBuffer(size_t capacity)
    : capacity_(capacity + 1),  // +1 to distinguish full from empty
      buffer_(new Frame[capacity + 1]),
      audio_buffer_(new AudioFrame[capacity + 1]),
      write_index_(0),
      read_index_(0),
      audio_write_index_(0),
      audio_read_index_(0) {
}

FrameRingBuffer::~FrameRingBuffer() {
  // Unique_ptr handles cleanup
}

bool FrameRingBuffer::Push(const Frame& frame) {
  const uint32_t current_write = write_index_.load(std::memory_order_relaxed);
  const uint32_t next_write = (current_write + 1) % capacity_;
  
  // Check if buffer is full
  if (next_write == read_index_.load(std::memory_order_acquire)) {
    return false;  // Buffer full
  }
  
  // Copy frame data
  buffer_[current_write] = frame;
  
  // Update write index
  write_index_.store(next_write, std::memory_order_release);
  
  return true;
}

const Frame* FrameRingBuffer::Peek() const {
  const uint32_t current_read = read_index_.load(std::memory_order_acquire);
  
  // Check if buffer is empty
  if (current_read == write_index_.load(std::memory_order_acquire)) {
    return nullptr;  // Buffer empty
  }
  
  // Return pointer to frame (non-destructive read)
  return &buffer_[current_read];
}

bool FrameRingBuffer::Pop(Frame& frame) {
  const uint32_t current_read = read_index_.load(std::memory_order_relaxed);
  
  // Check if buffer is empty
  if (current_read == write_index_.load(std::memory_order_acquire)) {
    return false;  // Buffer empty
  }
  
  // Copy frame data
  frame = buffer_[current_read];
  
  // Update read index
  const uint32_t next_read = (current_read + 1) % capacity_;
  read_index_.store(next_read, std::memory_order_release);
  
  return true;
}

size_t FrameRingBuffer::Size() const {
  const uint32_t write = write_index_.load(std::memory_order_acquire);
  const uint32_t read = read_index_.load(std::memory_order_acquire);
  
  if (write >= read) {
    return write - read;
  } else {
    return capacity_ - (read - write);
  }
}

bool FrameRingBuffer::IsEmpty() const {
  return read_index_.load(std::memory_order_acquire) == 
         write_index_.load(std::memory_order_acquire);
}

bool FrameRingBuffer::IsFull() const {
  const uint32_t write = write_index_.load(std::memory_order_acquire);
  const uint32_t read = read_index_.load(std::memory_order_acquire);
  const uint32_t next_write = (write + 1) % capacity_;
  
  return next_write == read;
}

bool FrameRingBuffer::PushAudioFrame(const AudioFrame& audio_frame) {
  const uint32_t current_write = audio_write_index_.load(std::memory_order_relaxed);
  const uint32_t next_write = (current_write + 1) % capacity_;
  
  // Check if buffer is full
  if (next_write == audio_read_index_.load(std::memory_order_acquire)) {
    return false;  // Buffer full
  }
  
  // Copy audio frame data
  audio_buffer_[current_write] = audio_frame;
  
  // Update write index
  audio_write_index_.store(next_write, std::memory_order_release);
  
  return true;
}

bool FrameRingBuffer::PopAudioFrame(AudioFrame& audio_frame) {
  const uint32_t current_read = audio_read_index_.load(std::memory_order_relaxed);
  
  // Check if buffer is empty
  if (current_read == audio_write_index_.load(std::memory_order_acquire)) {
    return false;  // Buffer empty
  }
  
  // Copy audio frame data
  audio_frame = audio_buffer_[current_read];
  
  // Update read index
  const uint32_t next_read = (current_read + 1) % capacity_;
  audio_read_index_.store(next_read, std::memory_order_release);
  
  return true;
}

const AudioFrame* FrameRingBuffer::PeekAudioFrame() const {
  const uint32_t current_read = audio_read_index_.load(std::memory_order_acquire);
  
  // Check if buffer is empty
  if (current_read == audio_write_index_.load(std::memory_order_acquire)) {
    return nullptr;  // Buffer empty
  }
  
  // Return pointer to audio frame (non-destructive read)
  return &audio_buffer_[current_read];
}

void FrameRingBuffer::Clear() {
  // Reset indices - caller must ensure no concurrent access
  write_index_.store(0, std::memory_order_release);
  read_index_.store(0, std::memory_order_release);
  audio_write_index_.store(0, std::memory_order_release);
  audio_read_index_.store(0, std::memory_order_release);
}

}  // namespace retrovue::buffer

