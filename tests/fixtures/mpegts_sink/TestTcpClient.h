// Repository: Retrovue-playout
// Component: Test TCP Client
// Purpose: Helper to connect to sink's TCP server for testing.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_TESTS_FIXTURES_MPEGTS_SINK_TEST_TCP_CLIENT_H_
#define RETROVUE_TESTS_FIXTURES_MPEGTS_SINK_TEST_TCP_CLIENT_H_

#include <cstdint>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>

namespace retrovue::tests::fixtures::mpegts_sink {

// TestTcpClient connects to the sink's TCP server to unblock accept().
// RAII wrapper that closes socket on destruction.
class TestTcpClient {
 public:
  TestTcpClient() : fd_(-1) {}

  ~TestTcpClient() {
    Close();
  }

  // Non-copyable, movable
  TestTcpClient(const TestTcpClient&) = delete;
  TestTcpClient& operator=(const TestTcpClient&) = delete;
  TestTcpClient(TestTcpClient&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }

  // Connect to sink on specified port
  // Returns true on success, false on failure
  bool Connect(uint16_t port) {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
      return false;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
      close(fd_);
      fd_ = -1;
      return false;
    }

    return true;
  }

  // Close the connection
  void Close() {
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
  }

  // Get the file descriptor (for testing)
  int GetFd() const {
    return fd_;
  }

  // Check if connected
  bool IsConnected() const {
    return fd_ >= 0;
  }

 private:
  int fd_;
};

// Helper function to connect to sink
inline TestTcpClient ConnectTo(uint16_t port) {
  TestTcpClient client;
  client.Connect(port);
  return client;
}

}  // namespace retrovue::tests::fixtures::mpegts_sink

#endif  // RETROVUE_TESTS_FIXTURES_MPEGTS_SINK_TEST_TCP_CLIENT_H_





