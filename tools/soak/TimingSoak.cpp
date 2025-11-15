#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "retrovue/buffer/FrameRingBuffer.h"
#include "retrovue/decode/FrameProducer.h"
#include "retrovue/renderer/FrameRenderer.h"
#include "retrovue/telemetry/MetricsExporter.h"
#include "playout_service.h"
#include "retrovue/timing/MasterClock.h"

namespace
{
  constexpr int kDefaultDurationSeconds = 300;
  constexpr double kMaxGapP95Ms = 6.0;
  constexpr double kMaxCorrectionsPerSec = 1.0;
  constexpr int kWindowSeconds = 60;

  struct ParsedArgs
  {
    int32_t channel_id = 9001;
    std::string asset_uri = "timing://soak/default";
    int duration_seconds = kDefaultDurationSeconds;
    double cpu_load = 0.0;
    int metrics_port = 0;
  };

  ParsedArgs ParseArgs(int argc, char **argv)
  {
    ParsedArgs args;
    for (int i = 1; i < argc; ++i)
    {
      const std::string_view arg(argv[i]);
      if (arg == "--channel-id" && i + 1 < argc)
      {
        args.channel_id = std::stoi(argv[++i]);
      }
      else if (arg == "--asset" && i + 1 < argc)
      {
        args.asset_uri = argv[++i];
      }
      else if (arg == "--duration-seconds" && i + 1 < argc)
      {
        args.duration_seconds = std::stoi(argv[++i]);
      }
      else if (arg == "--cpu-load" && i + 1 < argc)
      {
        args.cpu_load = std::clamp(std::stod(argv[++i]), 0.0, 100.0);
      }
      else if (arg == "--metrics-port" && i + 1 < argc)
      {
        args.metrics_port = std::stoi(argv[++i]);
      }
    }
    return args;
  }

  double Mean(const std::vector<double> &values)
  {
    if (values.empty())
      return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) /
           static_cast<double>(values.size());
  }

  double P95(std::vector<double> values)
  {
    if (values.empty())
      return 0.0;
    std::sort(values.begin(), values.end());
    const double rank = 0.95 * static_cast<double>(values.size() - 1);
    const auto lo = static_cast<size_t>(std::floor(rank));
    const auto hi = static_cast<size_t>(std::ceil(rank));
    const double fraction = rank - static_cast<double>(lo);
    return values[lo] + (values[hi] - values[lo]) * fraction;
  }

  void SpinCpu(double load_percent)
  {
    if (load_percent <= 0.0)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      return;
    }
    const auto cycle = std::chrono::milliseconds(100);
    const auto busy =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            cycle * (load_percent / 100.0));
    const auto idle = cycle - busy;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < busy)
    {
      std::this_thread::yield();
    }
    if (idle.count() > 0)
    {
      std::this_thread::sleep_for(idle);
    }
  }

} // namespace

int main(int argc, char **argv)
{
  using namespace retrovue;
  const ParsedArgs args = ParseArgs(argc, argv);
  auto metrics = std::make_shared<telemetry::MetricsExporter>(args.metrics_port);
  const auto epoch_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
  auto clock = timing::MakeSystemMasterClock(epoch_us, 0.0);

  playout::PlayoutControlImpl service(metrics, clock);

  // Spin CPU stressor if requested.
  std::atomic<bool> stop_stress{false};
  std::thread stress_thread;
  if (args.cpu_load > 0.0)
  {
    stress_thread = std::thread([&]()
                                {
      while (!stop_stress.load()) {
        SpinCpu(args.cpu_load);
      } });
  }

  // Start channel using the public gRPC implementation.
  grpc::ServerContext ctx;
  playout::StartChannelRequest req;
  playout::StartChannelResponse resp;
  req.set_channel_id(args.channel_id);
  req.set_plan_handle(args.asset_uri);
  req.set_port(0);
  auto status = service.StartChannel(&ctx, &req, &resp);
  if (!status.ok())
  {
    std::cerr << "Failed to start channel: " << status.error_message() << std::endl;
    stop_stress.store(true);
    if (stress_thread.joinable())
      stress_thread.join();
    return EXIT_FAILURE;
  }

  std::vector<double> window_gaps;
  window_gaps.reserve(kWindowSeconds * 60);
  std::vector<uint64_t> correction_samples;
  correction_samples.reserve(kWindowSeconds);

  const auto start_time = std::chrono::steady_clock::now();
  auto last_sample = start_time;

  while (std::chrono::steady_clock::now() - start_time <
         std::chrono::seconds(args.duration_seconds))
  {
    telemetry::ChannelMetrics metrics_snapshot{};
    if (!metrics->GetChannelMetrics(args.channel_id, metrics_snapshot))
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }

    window_gaps.push_back(std::abs(metrics_snapshot.frame_gap_seconds * 1'000.0));
    correction_samples.push_back(metrics_snapshot.corrections_total);

    if (window_gaps.size() > static_cast<size_t>(kWindowSeconds * 60))
    {
      window_gaps.erase(window_gaps.begin());
    }
    if (correction_samples.size() > static_cast<size_t>(kWindowSeconds))
    {
      correction_samples.erase(correction_samples.begin());
    }

    const double mean_gap = Mean(window_gaps);
    const double p95_gap = P95(window_gaps);
    const double corrections_per_sec =
        (correction_samples.size() > 1)
            ? static_cast<double>(correction_samples.back() -
                                  correction_samples.front()) /
                  static_cast<double>(correction_samples.size() - 1)
            : 0.0;

    std::cout << "[timing_soak] mean_gap_ms=" << mean_gap
              << " p95_gap_ms=" << p95_gap
              << " corrections_recent_per_sec=" << corrections_per_sec << std::endl;

    if (p95_gap > kMaxGapP95Ms || corrections_per_sec > kMaxCorrectionsPerSec)
    {
      std::cerr << "[timing_soak] threshold exceeded, aborting" << std::endl;
      stop_stress.store(true);
      if (stress_thread.joinable())
        stress_thread.join();
      grpc::ServerContext stop_ctx;
      playout::StopChannelRequest stop_req;
      playout::StopChannelResponse stop_resp;
      stop_req.set_channel_id(args.channel_id);
      service.StopChannel(&stop_ctx, &stop_req, &stop_resp);

      return EXIT_FAILURE;
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));
    last_sample = std::chrono::steady_clock::now();
  }

  stop_stress.store(true);
  if (stress_thread.joinable())
    stress_thread.join();
  grpc::ServerContext stop_ctx;
  playout::StopChannelRequest stop_req;
  playout::StopChannelResponse stop_resp;
  stop_req.set_channel_id(args.channel_id);
  service.StopChannel(&stop_ctx, &stop_req, &stop_resp);

  std::cout << "[timing_soak] completed successfully" << std::endl;
  return EXIT_SUCCESS;
}
