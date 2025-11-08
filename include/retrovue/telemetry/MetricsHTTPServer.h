// Repository: Retrovue-playout
// Component: Metrics HTTP Server
// Purpose: Real HTTP server for Prometheus metrics endpoint.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_TELEMETRY_METRICS_HTTP_SERVER_H_
#define RETROVUE_TELEMETRY_METRICS_HTTP_SERVER_H_

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace retrovue::telemetry {

// MetricsCallback is called to generate metrics text for each request.
using MetricsCallback = std::function<std::string()>;

// MetricsHTTPServer serves Prometheus metrics over HTTP.
//
// Features:
// - Simple HTTP/1.1 server
// - GET /metrics endpoint
// - Prometheus text exposition format
// - Non-blocking accept with select()
// - Thread-safe metric updates via callback
//
// Design:
// - Runs in dedicated server thread
// - Calls user-provided callback to generate metrics
// - Returns 200 OK with text/plain for /metrics
// - Returns 404 for other paths
//
// Thread Model:
// - Server runs in its own thread
// - Accepts connections and handles requests
// - Callback is called on server thread
//
// Usage:
// 1. Construct with port number
// 2. Set metrics callback with SetMetricsCallback()
// 3. Call Start() to begin serving
// 4. Call Stop() to shutdown
class MetricsHTTPServer {
 public:
  // Constructs a server that will listen on the specified port.
  // Default port 9090 matches Prometheus convention.
  explicit MetricsHTTPServer(int port = 9090);
  
  ~MetricsHTTPServer();

  // Disable copy and move
  MetricsHTTPServer(const MetricsHTTPServer&) = delete;
  MetricsHTTPServer& operator=(const MetricsHTTPServer&) = delete;

  // Sets the callback function that generates metrics text.
  // Must be called before Start().
  void SetMetricsCallback(MetricsCallback callback);

  // Starts the HTTP server.
  // Returns true if started successfully.
  bool Start();

  // Stops the HTTP server.
  void Stop();

  // Returns true if server is currently running.
  bool IsRunning() const { return running_.load(std::memory_order_acquire); }

  // Gets the port number server is listening on.
  int GetPort() const { return port_; }

 private:
  // Main server loop (runs in server thread).
  void ServerLoop();

  // Handles a single client connection.
  void HandleConnection(int client_socket);

  // Parses HTTP request and returns path.
  std::string ParseRequest(const std::string& request);

  // Generates HTTP response.
  std::string GenerateResponse(const std::string& path);

  int port_;
  std::atomic<bool> running_;
  std::atomic<bool> stop_requested_;
  
  std::unique_ptr<std::thread> server_thread_;
  MetricsCallback metrics_callback_;
  
  // Server socket (platform-specific)
  int server_socket_;
};

}  // namespace retrovue::telemetry

#endif  // RETROVUE_TELEMETRY_METRICS_HTTP_SERVER_H_

