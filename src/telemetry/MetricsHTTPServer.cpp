// Repository: Retrovue-playout
// Component: Metrics HTTP Server
// Purpose: Real HTTP server for Prometheus metrics endpoint.
// Copyright (c) 2025 RetroVue

#include "retrovue/telemetry/MetricsHTTPServer.h"

#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>

// Platform-specific socket headers
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef int socklen_t;
  #define CLOSE_SOCKET closesocket
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  #define INVALID_SOCKET -1
  #define SOCKET_ERROR -1
  #define CLOSE_SOCKET close
#endif

namespace retrovue::telemetry {

MetricsHTTPServer::MetricsHTTPServer(int port)
    : port_(port),
      running_(false),
      stop_requested_(false),
      server_socket_(INVALID_SOCKET) {
}

MetricsHTTPServer::~MetricsHTTPServer() {
  Stop();
}

void MetricsHTTPServer::SetMetricsCallback(MetricsCallback callback) {
  metrics_callback_ = std::move(callback);
}

bool MetricsHTTPServer::Start() {
  if (running_.load(std::memory_order_acquire)) {
    std::cerr << "[MetricsHTTPServer] Already running" << std::endl;
    return false;
  }

  if (!metrics_callback_) {
    std::cerr << "[MetricsHTTPServer] Metrics callback not set" << std::endl;
    return false;
  }

#ifdef _WIN32
  // Initialize Winsock
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    std::cerr << "[MetricsHTTPServer] WSAStartup failed" << std::endl;
    return false;
  }
#endif

  stop_requested_.store(false, std::memory_order_release);
  
  server_thread_ = std::make_unique<std::thread>(&MetricsHTTPServer::ServerLoop, this);
  
  // Wait a bit for server to start
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  
  std::cout << "[MetricsHTTPServer] Started on port " << port_ << std::endl;
  return true;
}

void MetricsHTTPServer::Stop() {
  if (!running_.load(std::memory_order_acquire) && !server_thread_) {
    return;
  }

  std::cout << "[MetricsHTTPServer] Stopping..." << std::endl;
  stop_requested_.store(true, std::memory_order_release);

  // Close server socket to unblock accept()
  if (server_socket_ != INVALID_SOCKET) {
    CLOSE_SOCKET(server_socket_);
    server_socket_ = INVALID_SOCKET;
  }

  if (server_thread_ && server_thread_->joinable()) {
    server_thread_->join();
  }

  server_thread_.reset();
  running_.store(false, std::memory_order_release);

#ifdef _WIN32
  WSACleanup();
#endif
  
  std::cout << "[MetricsHTTPServer] Stopped" << std::endl;
}

void MetricsHTTPServer::ServerLoop() {
  std::cout << "[MetricsHTTPServer] Server loop started" << std::endl;

  // Create socket
  server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
  if (server_socket_ == INVALID_SOCKET) {
    std::cerr << "[MetricsHTTPServer] Failed to create socket" << std::endl;
    return;
  }

  // Set socket options
  int opt = 1;
#ifdef _WIN32
  setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
#else
  setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

  // Set non-blocking mode for accept timeout
#ifdef _WIN32
  u_long mode = 1;
  ioctlsocket(server_socket_, FIONBIO, &mode);
#else
  int flags = fcntl(server_socket_, F_GETFL, 0);
  fcntl(server_socket_, F_SETFL, flags | O_NONBLOCK);
#endif

  // Bind socket
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port_);

  if (bind(server_socket_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
    std::cerr << "[MetricsHTTPServer] Failed to bind socket to port " << port_ << std::endl;
    CLOSE_SOCKET(server_socket_);
    server_socket_ = INVALID_SOCKET;
    return;
  }

  // Listen for connections
  if (listen(server_socket_, 5) == SOCKET_ERROR) {
    std::cerr << "[MetricsHTTPServer] Failed to listen" << std::endl;
    CLOSE_SOCKET(server_socket_);
    server_socket_ = INVALID_SOCKET;
    return;
  }

  running_.store(true, std::memory_order_release);
  std::cout << "[MetricsHTTPServer] Listening on port " << port_ << std::endl;

  // Accept loop
  while (!stop_requested_.load(std::memory_order_acquire)) {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    
    int client_socket = accept(server_socket_, (sockaddr*)&client_addr, &client_len);
    
    if (client_socket == INVALID_SOCKET) {
#ifdef _WIN32
      int err = WSAGetLastError();
      if (err == WSAEWOULDBLOCK) {
        // No pending connection - sleep and retry
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
#else
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        // No pending connection - sleep and retry
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
#endif
      // Real error or server closing
      break;
    }

    // Handle connection
    HandleConnection(client_socket);
    CLOSE_SOCKET(client_socket);
  }

  // Cleanup
  if (server_socket_ != INVALID_SOCKET) {
    CLOSE_SOCKET(server_socket_);
    server_socket_ = INVALID_SOCKET;
  }

  running_.store(false, std::memory_order_release);
  std::cout << "[MetricsHTTPServer] Server loop exited" << std::endl;
}

void MetricsHTTPServer::HandleConnection(int client_socket) {
  // Read request (with timeout)
  char buffer[4096] = {0};
  
#ifdef _WIN32
  // Set receive timeout
  DWORD timeout = 5000;  // 5 seconds
  setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
#else
  struct timeval timeout;
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif

  int bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
  if (bytes_read <= 0) {
    return;
  }

  buffer[bytes_read] = '\0';
  std::string request(buffer);

  // Parse request and generate response
  std::string path = ParseRequest(request);
  std::string response = GenerateResponse(path);

  // Send response
  send(client_socket, response.c_str(), static_cast<int>(response.length()), 0);
}

std::string MetricsHTTPServer::ParseRequest(const std::string& request) {
  // Parse first line: GET /path HTTP/1.1
  size_t space1 = request.find(' ');
  if (space1 == std::string::npos) {
    return "/";
  }

  size_t space2 = request.find(' ', space1 + 1);
  if (space2 == std::string::npos) {
    return "/";
  }

  return request.substr(space1 + 1, space2 - space1 - 1);
}

std::string MetricsHTTPServer::GenerateResponse(const std::string& path) {
  std::ostringstream response;

  if (path == "/metrics") {
    // Generate metrics via callback
    std::string metrics = metrics_callback_();

    response << "HTTP/1.1 200 OK\r\n";
    response << "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n";
    response << "Content-Length: " << metrics.length() << "\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << metrics;
  } else if (path == "/") {
    // Root path - return simple info page
    std::string body = "RetroVue Playout Engine - Metrics Server\n"
                       "Metrics available at: /metrics\n";
    
    response << "HTTP/1.1 200 OK\r\n";
    response << "Content-Type: text/plain\r\n";
    response << "Content-Length: " << body.length() << "\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << body;
  } else {
    // Not found
    std::string body = "404 Not Found\n";
    
    response << "HTTP/1.1 404 Not Found\r\n";
    response << "Content-Type: text/plain\r\n";
    response << "Content-Length: " << body.length() << "\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << body;
  }

  return response.str();
}

}  // namespace retrovue::telemetry

