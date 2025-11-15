#include <gtest/gtest.h>

#include <chrono>
#include <memory>

#include "BaseContractTest.h"
#include "retrovue/telemetry/MetricsExporter.h"
#include "../ContractRegistryEnvironment.h"

namespace retrovue::tests::contracts {

  using retrovue::tests::RegisterExpectedDomainCoverage;

  const bool kRegisterCoverage = []()
  {
    RegisterExpectedDomainCoverage("MetricsExport",
                                   {"MET-001",
                                    "MET-002",
                                    "MET-003"});
    return true;
  }();

class MetricsExportContractTest : public BaseContractTest {
 protected:
  [[nodiscard]] std::string DomainName() const override { return "MetricsExport"; }

  [[nodiscard]] std::vector<std::string> CoveredRuleIds() const override {
    return {"MET-001", "MET-002", "MET-003"};
  }
};

TEST_F(MetricsExportContractTest, MET_001_NonBlockingExportSemantics) {
  telemetry::MetricsExporter exporter(0, /*enable_http=*/false);
  ASSERT_TRUE(exporter.Start(/*start_http_server=*/false));

  telemetry::ChannelMetrics sample;
  sample.state = telemetry::ChannelState::READY;
  sample.buffer_depth_frames = 3;
  sample.frame_gap_seconds = 0.002;

  constexpr int kIterations = 512;
  for (int i = 0; i < kIterations; ++i) {
    EXPECT_TRUE(exporter.SubmitChannelMetrics(100, sample));
  }

  ASSERT_TRUE(exporter.WaitUntilDrainedForTest(std::chrono::milliseconds(500)));

  auto snapshot = exporter.SnapshotForTest();
  EXPECT_EQ(snapshot.queue_overflow_total, 0u);
  auto it = snapshot.channel_metrics.find(100);
  ASSERT_NE(it, snapshot.channel_metrics.end());
  EXPECT_EQ(it->second.buffer_depth_frames, 3u);

  exporter.Stop();
}

TEST_F(MetricsExportContractTest, MET_002_SchemaVersionIntegrity) {
  telemetry::MetricsExporter exporter(0, /*enable_http=*/false);
  ASSERT_TRUE(exporter.Start(/*start_http_server=*/false));

  exporter.RegisterMetricDescriptor("retrovue_playout_channel_state", "1.0.0");
  exporter.RegisterMetricDescriptor("retrovue_playout_channel_state", "1.1.0");
  exporter.DeprecateMetricDescriptor("retrovue_playout_channel_state");

  ASSERT_TRUE(exporter.WaitUntilDrainedForTest(std::chrono::milliseconds(200)));

  auto snapshot = exporter.SnapshotForTest();
  ASSERT_EQ(snapshot.descriptor_versions["retrovue_playout_channel_state"], "1.1.0");
  EXPECT_TRUE(snapshot.descriptor_deprecated["retrovue_playout_channel_state"]);

  exporter.Stop();
}

TEST_F(MetricsExportContractTest, MET_003_DeliveryReliabilityPerTransport) {
  telemetry::MetricsExporter exporter(0, /*enable_http=*/false);
  ASSERT_TRUE(exporter.Start(/*start_http_server=*/false));

  exporter.RecordDeliveryStatus(telemetry::MetricsExporter::Transport::kGrpcStream,
                                true, 120.0);
  exporter.RecordDeliveryStatus(telemetry::MetricsExporter::Transport::kGrpcStream,
                                false, 180.0);
  exporter.RecordDeliveryStatus(telemetry::MetricsExporter::Transport::kScrape,
                                true, 80.0);
  exporter.RecordDeliveryStatus(telemetry::MetricsExporter::Transport::kFile,
                                true, 400.0);

  ASSERT_TRUE(exporter.WaitUntilDrainedForTest(std::chrono::milliseconds(200)));

  auto snapshot = exporter.SnapshotForTest();
  const auto& grpc_stats =
      snapshot.transport_stats[telemetry::MetricsExporter::Transport::kGrpcStream];
  EXPECT_EQ(grpc_stats.deliveries, 1u);
  EXPECT_EQ(grpc_stats.failures, 1u);
  EXPECT_NEAR(grpc_stats.latency_p95_ms, 177.0, 5.0);

  const auto& scrape_stats =
      snapshot.transport_stats[telemetry::MetricsExporter::Transport::kScrape];
  EXPECT_EQ(scrape_stats.deliveries, 1u);
  EXPECT_EQ(scrape_stats.failures, 0u);

  exporter.Stop();
}

}  // namespace retrovue::tests::contracts

