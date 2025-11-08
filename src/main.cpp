// Repository: Retrovue-playout
// Component: PlayoutControl gRPC Server
// Purpose: Main entry point for the RetroVue playout engine.
// Copyright (c) 2025 RetroVue

#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>

#include "playout_service.h"
#include "retrovue/telemetry/MetricsExporter.h"

namespace {

// Parse command-line arguments
struct ServerConfig {
  std::string server_address = "0.0.0.0:50051";
  bool enable_reflection = true;
};

ServerConfig ParseArgs(int argc, char** argv) {
  ServerConfig config;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    
    if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
      config.server_address = std::string("0.0.0.0:") + argv[++i];
    } else if (arg == "--address" || arg == "-a") {
      if (i + 1 < argc) {
        config.server_address = argv[++i];
      }
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "RetroVue Playout Engine\n\n"
                << "Usage: retrovue_playout [OPTIONS]\n\n"
                << "Options:\n"
                << "  -p, --port PORT        Listen port (default: 50051)\n"
                << "  -a, --address ADDRESS  Full listen address (default: 0.0.0.0:50051)\n"
                << "  -h, --help             Show this help message\n"
                << std::endl;
      std::exit(0);
    }
  }

  return config;
}

void RunServer(const ServerConfig& config) {
  // Create and start metrics exporter
  auto metrics_exporter = std::make_shared<retrovue::telemetry::MetricsExporter>(9308);
  if (!metrics_exporter->Start()) {
    std::cerr << "Failed to start metrics exporter" << std::endl;
    return;
  }

  // Create the service implementation
  retrovue::playout::PlayoutControlImpl service(metrics_exporter);

  // Enable health checking and reflection
  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();

  // Build the server
  grpc::ServerBuilder builder;
  builder.AddListeningPort(config.server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  // Start the server
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  
  std::cout << "==============================================================" << std::endl;
  std::cout << "RetroVue Playout Engine (Phase 3)" << std::endl;
  std::cout << "==============================================================" << std::endl;
  std::cout << "gRPC Server: " << config.server_address << std::endl;
  std::cout << "API Version: 1.0.0" << std::endl;
  std::cout << "gRPC Health Check: Enabled" << std::endl;
  std::cout << "gRPC Reflection: " << (config.enable_reflection ? "Enabled" : "Disabled") << std::endl;
  std::cout << "Metrics Endpoint: http://localhost:9308/metrics" << std::endl;
  std::cout << "==============================================================" << std::endl;
  std::cout << "\nComponents:" << std::endl;
  std::cout << "  ✓ FFmpegDecoder (real video decoding)" << std::endl;
  std::cout << "  ✓ FrameRingBuffer (lock-free circular buffer)" << std::endl;
  std::cout << "  ✓ FrameRenderer (headless mode)" << std::endl;
  std::cout << "  ✓ MetricsHTTPServer (Prometheus format)" << std::endl;
  std::cout << "\nPress Ctrl+C to shutdown...\n" << std::endl;

  // Wait for the server to shutdown
  server->Wait();
  
  // Cleanup metrics exporter
  metrics_exporter->Stop();
}

}  // namespace

int main(int argc, char** argv) {
  try {
    ServerConfig config = ParseArgs(argc, argv);
    RunServer(config);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Fatal error: " << e.what() << std::endl;
    return 1;
  }
}

