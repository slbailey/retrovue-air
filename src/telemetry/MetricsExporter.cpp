// Repository: Retrovue-playout
// Component: Metrics Exporter
// Purpose: Exposes Prometheus metrics at /metrics HTTP endpoint.
// Copyright (c) 2025 RetroVue

#include "retrovue/telemetry/MetricsExporter.h"
#include "retrovue/telemetry/MetricsHTTPServer.h"

#include <algorithm>
#include <iostream>
#include <sstream>

namespace retrovue::telemetry {

const char* ChannelStateToString(ChannelState state) {
  switch (state) {
    case ChannelState::STOPPED:
      return "stopped";
    case ChannelState::BUFFERING:
      return "buffering";
    case ChannelState::READY:
      return "ready";
    case ChannelState::ERROR_STATE:
      return "error";
    default:
      return "unknown";
  }
}

int ChannelStateToValue(ChannelState state) {
  return static_cast<int>(state);
}

MetricsExporter::EventQueue::EventQueue(size_t capacity)
    : capacity_(capacity),
      buffer_(capacity),
      head_(0),
      tail_(0) {}

bool MetricsExporter::EventQueue::Push(const Event& event) {
  size_t tail = tail_.load(std::memory_order_relaxed);
  size_t head = head_.load(std::memory_order_acquire);
  size_t next = (tail + 1) % capacity_;
  if (next == head) {
    return false;
  }
  buffer_[tail] = event;
  tail_.store(next, std::memory_order_release);
  return true;
}

bool MetricsExporter::EventQueue::Pop(Event& event) {
  size_t head = head_.load(std::memory_order_relaxed);
  size_t tail = tail_.load(std::memory_order_acquire);
  if (head == tail) {
    return false;
  }
  event = buffer_[head];
  head_.store((head + 1) % capacity_, std::memory_order_release);
  return true;
}

bool MetricsExporter::EventQueue::Empty() const {
  return head_.load(std::memory_order_acquire) ==
         tail_.load(std::memory_order_acquire);
}

MetricsExporter::MetricsExporter(int port, bool enable_http)
    : port_(port),
      enable_http_(enable_http),
      running_(false),
      stop_requested_(false),
      http_server_(enable_http ? std::make_unique<MetricsHTTPServer>(port) : nullptr),
      queue_overflow_total_(0),
      event_queue_(1024),
      submitted_events_(0),
      processed_events_(0) {
  if (http_server_) {
    http_server_->SetMetricsCallback([this]() { return this->GenerateMetricsText(); });
  }
}

MetricsExporter::~MetricsExporter() {
  Stop();
}

bool MetricsExporter::Start(bool start_http_server) {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return false;
  }

  stop_requested_.store(false, std::memory_order_release);

  if (enable_http_ && start_http_server && http_server_) {
    if (!http_server_->Start()) {
      std::cerr << "[MetricsExporter] Failed to start HTTP server" << std::endl;
      running_.store(false, std::memory_order_release);
      return false;
    }

    std::cout << "[MetricsExporter] Started HTTP server on port " << port_
              << " - metrics at http://localhost:" << port_ << "/metrics" << std::endl;
  }

  worker_thread_ = std::thread(&MetricsExporter::WorkerLoop, this);
  return true;
}

void MetricsExporter::Stop() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false)) {
    return;
  }

  stop_requested_.store(true, std::memory_order_release);
  queue_cv_.notify_all();

  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }

  if (enable_http_ && http_server_) {
    http_server_->Stop();
  }
}

bool MetricsExporter::SubmitChannelMetrics(int32_t channel_id,
                                           const ChannelMetrics& metrics) {
  if (!running_.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    channel_metrics_[channel_id] = metrics;
    std::cout << "[MetricsExporter] (sync) snapshot written for channel "
              << channel_id << std::endl;
    return true;
  }

  Event event{};
  event.type = Event::Type::kUpdateChannel;
  event.channel_id = channel_id;
  event.channel_metrics = metrics;

  if (!event_queue_.Push(event)) {
    queue_overflow_total_.fetch_add(1, std::memory_order_acq_rel);
    std::cerr << "[MetricsExporter] Queue overflow while enqueuing channel metrics for channel "
              << channel_id << std::endl;
    return false;
  }

  std::cout << "[MetricsExporter] Enqueued metrics update for channel "
            << channel_id << std::endl;
  submitted_events_.fetch_add(1, std::memory_order_acq_rel);
  queue_cv_.notify_one();
  return true;
}

void MetricsExporter::SubmitChannelRemoval(int32_t channel_id) {
  if (!running_.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    channel_metrics_.erase(channel_id);
    std::cout << "[MetricsExporter] (sync) channel " << channel_id
              << " removed from metrics" << std::endl;
    return;
  }

  Event event{};
  event.type = Event::Type::kRemoveChannel;
  event.channel_id = channel_id;

  if (!event_queue_.Push(event)) {
    queue_overflow_total_.fetch_add(1, std::memory_order_acq_rel);
    std::cerr << "[MetricsExporter] Queue overflow while enqueuing channel removal for channel "
              << channel_id << std::endl;
    return;
  }

  std::cout << "[MetricsExporter] Enqueued metrics removal for channel "
            << channel_id << std::endl;
  submitted_events_.fetch_add(1, std::memory_order_acq_rel);
  queue_cv_.notify_one();
}

void MetricsExporter::RegisterMetricDescriptor(const std::string& name,
                                               const std::string& version) {
  if (!running_.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    descriptor_versions_[name] = version;
    descriptor_deprecated_[name] = false;
    std::cout << "[MetricsExporter] (sync) registered descriptor "
              << name << " version " << version << std::endl;
    return;
  }

  Event event{};
  event.type = Event::Type::kRegisterDescriptor;
  event.descriptor_name = name;
  event.descriptor_version = version;

  if (!event_queue_.Push(event)) {
    queue_overflow_total_.fetch_add(1, std::memory_order_acq_rel);
    std::cerr << "[MetricsExporter] Queue overflow while registering descriptor "
              << name << std::endl;
    return;
  }

  submitted_events_.fetch_add(1, std::memory_order_acq_rel);
  queue_cv_.notify_one();
}

void MetricsExporter::DeprecateMetricDescriptor(const std::string& name) {
  if (!running_.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    descriptor_deprecated_[name] = true;
    std::cout << "[MetricsExporter] (sync) deprecated descriptor " << name << std::endl;
    return;
  }

  Event event{};
  event.type = Event::Type::kDeprecateDescriptor;
  event.descriptor_name = name;

  if (!event_queue_.Push(event)) {
    queue_overflow_total_.fetch_add(1, std::memory_order_acq_rel);
    std::cerr << "[MetricsExporter] Queue overflow while deprecating descriptor "
              << name << std::endl;
    return;
  }

  submitted_events_.fetch_add(1, std::memory_order_acq_rel);
  queue_cv_.notify_one();
}

void MetricsExporter::RecordDeliveryStatus(Transport transport,
                                           bool success,
                                           double latency_ms) {
  if (!running_.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    auto& data = transport_data_[transport];
    data.latencies_ms.push_back(latency_ms);
    if (success) {
      data.deliveries++;
    } else {
      data.failures++;
    }
    std::cout << "[MetricsExporter] (sync) recorded transport delivery (transport="
              << static_cast<int>(transport) << ", success=" << std::boolalpha << success
              << ", latency_ms=" << latency_ms << ")" << std::endl;
    return;
  }

  Event event{};
  event.type = Event::Type::kRecordTransport;
  event.transport = transport;
  event.transport_success = success;
  event.transport_latency_ms = latency_ms;

  if (!event_queue_.Push(event)) {
    queue_overflow_total_.fetch_add(1, std::memory_order_acq_rel);
    std::cerr << "[MetricsExporter] Queue overflow while recording delivery status for transport "
              << static_cast<int>(transport) << std::endl;
    return;
  }

  submitted_events_.fetch_add(1, std::memory_order_acq_rel);
  queue_cv_.notify_one();
}

bool MetricsExporter::GetChannelMetrics(int32_t channel_id,
                                        ChannelMetrics& metrics) const {
  std::cout << "[MetricsExporter] GetChannelMetrics requested for channel "
            << channel_id << std::endl;
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  auto it = channel_metrics_.find(channel_id);
  if (it == channel_metrics_.end()) {
    return false;
  }
  metrics = it->second;
  return true;
}

MetricsExporter::Snapshot MetricsExporter::SnapshotForTest() const {
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  Snapshot snapshot;
  snapshot.channel_metrics = channel_metrics_;
  snapshot.descriptor_versions = descriptor_versions_;
  snapshot.descriptor_deprecated = descriptor_deprecated_;
  for (const auto& [transport, data] : transport_data_) {
    TransportSnapshot ts;
    ts.deliveries = data.deliveries;
    ts.failures = data.failures;
    ts.latency_p95_ms = ComputePercentile(data.latencies_ms, 0.95);
    snapshot.transport_stats.emplace(transport, ts);
  }
  snapshot.queue_overflow_total = queue_overflow_total_.load(std::memory_order_acquire);
  return snapshot;
}

bool MetricsExporter::WaitUntilDrainedForTest(std::chrono::milliseconds timeout) {
  if (!running_.load(std::memory_order_acquire)) {
    return true;
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (processed_events_.load(std::memory_order_acquire) >=
            submitted_events_.load(std::memory_order_acquire) &&
        event_queue_.Empty()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

void MetricsExporter::WorkerLoop() {
  while (!stop_requested_.load(std::memory_order_acquire) ||
         !event_queue_.Empty()) {
    Event event;
    if (event_queue_.Pop(event)) {
      ProcessEvent(event);
      processed_events_.fetch_add(1, std::memory_order_acq_rel);
      continue;
    }

    std::unique_lock<std::mutex> lock(queue_mutex_);
    queue_cv_.wait_for(lock, std::chrono::milliseconds(50));
  }
}

void MetricsExporter::ProcessEvent(const Event& event) {
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  switch (event.type) {
    case Event::Type::kUpdateChannel:
      channel_metrics_[event.channel_id] = event.channel_metrics;
      std::cout << "[MetricsExporter] Snapshot written for channel "
                << event.channel_id << std::endl;
      break;
    case Event::Type::kRemoveChannel:
      channel_metrics_.erase(event.channel_id);
      std::cout << "[MetricsExporter] Channel " << event.channel_id
                << " removed from metrics" << std::endl;
      break;
    case Event::Type::kRegisterDescriptor:
      descriptor_versions_[event.descriptor_name] = event.descriptor_version;
      descriptor_deprecated_[event.descriptor_name] = false;
      std::cout << "[MetricsExporter] Registered descriptor "
                << event.descriptor_name << " version "
                << event.descriptor_version << std::endl;
      break;
    case Event::Type::kDeprecateDescriptor:
      descriptor_deprecated_[event.descriptor_name] = true;
      std::cout << "[MetricsExporter] Deprecated descriptor "
                << event.descriptor_name << std::endl;
      break;
    case Event::Type::kRecordTransport: {
      auto& data = transport_data_[event.transport];
      data.latencies_ms.push_back(event.transport_latency_ms);
      if (event.transport_success) {
        data.deliveries++;
      } else {
        data.failures++;
      }
      std::cout << "[MetricsExporter] Recorded transport delivery (transport="
                << static_cast<int>(event.transport)
                << ", success=" << std::boolalpha << event.transport_success
                << ", latency_ms=" << event.transport_latency_ms << ")" << std::endl;
      break;
    }
  }
}

double MetricsExporter::ComputePercentile(const std::vector<double>& values,
                                          double percentile) {
  if (values.empty()) {
    return 0.0;
  }

  std::vector<double> sorted = values;
  std::sort(sorted.begin(), sorted.end());
  const double rank = percentile * static_cast<double>(sorted.size() - 1);
  const auto lower_index = static_cast<std::size_t>(std::floor(rank));
  const auto upper_index = static_cast<std::size_t>(std::ceil(rank));
  const double lower = sorted[lower_index];
  if (upper_index == lower_index) {
    return lower;
  }
  const double upper = sorted[upper_index];
  const double fraction = rank - static_cast<double>(lower_index);
  return lower + (upper - lower) * fraction;
}

std::string MetricsExporter::GenerateMetricsText() const {
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  std::ostringstream oss;

  oss << "# HELP retrovue_metrics_overflow_total Number of dropped metric events due to queue overflow\n";
  oss << "# TYPE retrovue_metrics_overflow_total counter\n";
  oss << "retrovue_metrics_overflow_total " << queue_overflow_total_.load(std::memory_order_acquire) << "\n\n";

  oss << "# HELP retrovue_playout_channel_state Current state of playout channel\n";
  oss << "# TYPE retrovue_playout_channel_state gauge\n";
  for (const auto& [channel_id, metrics] : channel_metrics_) {
    oss << "retrovue_playout_channel_state{channel=\"" << channel_id
        << "\",state=\"" << ChannelStateToString(metrics.state) << "\"} "
        << ChannelStateToValue(metrics.state) << "\n";
  }

  oss << "\n# HELP retrovue_playout_buffer_depth_frames Number of frames in buffer\n";
  oss << "# TYPE retrovue_playout_buffer_depth_frames gauge\n";
  for (const auto& [channel_id, metrics] : channel_metrics_) {
    oss << "retrovue_playout_buffer_depth_frames{channel=\"" << channel_id
        << "\"} " << metrics.buffer_depth_frames << "\n";
  }

  oss << "\n# HELP retrovue_playout_frame_gap_seconds Timing deviation from MasterClock\n";
  oss << "# TYPE retrovue_playout_frame_gap_seconds gauge\n";
  for (const auto& [channel_id, metrics] : channel_metrics_) {
    oss << "retrovue_playout_frame_gap_seconds{channel=\"" << channel_id
        << "\"} " << metrics.frame_gap_seconds << "\n";
  }

  oss << "\n# HELP retrovue_playout_decode_failure_count Total decode failures\n";
  oss << "# TYPE retrovue_playout_decode_failure_count counter\n";
  for (const auto& [channel_id, metrics] : channel_metrics_) {
    oss << "retrovue_playout_decode_failure_count{channel=\"" << channel_id
        << "\"} " << metrics.decode_failure_count << "\n";
  }

  oss << "\n# HELP retrovue_playout_corrections_total Total timing corrections applied\n";
  oss << "# TYPE retrovue_playout_corrections_total counter\n";
  for (const auto& [channel_id, metrics] : channel_metrics_) {
    oss << "retrovue_playout_corrections_total{channel=\"" << channel_id << "\"} "
        << metrics.corrections_total << "\n";
  }

  oss << "\n# HELP retrovue_metrics_descriptor_version Metric descriptor version\n";
  oss << "# TYPE retrovue_metrics_descriptor_version gauge\n";
  for (const auto& [name, version] : descriptor_versions_) {
    const bool deprecated =
        descriptor_deprecated_.count(name) ? descriptor_deprecated_.at(name) : false;
    oss << "retrovue_metrics_descriptor_version{name=\"" << name
        << "\",version=\"" << version << "\",deprecated=\"" << (deprecated ? "true" : "false") << "\"} 1\n";
  }

  oss << "\n# HELP retrovue_metrics_delivery_failures_total Delivery failures per transport\n";
  oss << "# TYPE retrovue_metrics_delivery_failures_total counter\n";
  for (const auto& [transport, data] : transport_data_) {
    const char* transport_name = "";
    switch (transport) {
      case Transport::kGrpcStream:
        transport_name = "grpc_stream";
        break;
      case Transport::kScrape:
        transport_name = "scrape";
        break;
      case Transport::kFile:
        transport_name = "file";
        break;
    }
    oss << "retrovue_metrics_delivery_failures_total{transport=\"" << transport_name << "\"} "
        << data.failures << "\n";
  }

  oss << "\n# HELP retrovue_metrics_delivery_latency_ms Delivery latency p95 per transport\n";
  oss << "# TYPE retrovue_metrics_delivery_latency_ms gauge\n";
  for (const auto& [transport, data] : transport_data_) {
    const char* transport_name = "";
    switch (transport) {
      case Transport::kGrpcStream:
        transport_name = "grpc_stream";
        break;
      case Transport::kScrape:
        transport_name = "scrape";
        break;
      case Transport::kFile:
        transport_name = "file";
        break;
    }
    oss << "retrovue_metrics_delivery_latency_ms{transport=\"" << transport_name << "\"} "
        << ComputePercentile(data.latencies_ms, 0.95) << "\n";
  }

  return oss.str();
}

}  // namespace retrovue::telemetry
