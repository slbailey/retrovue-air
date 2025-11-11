#include <gtest/gtest.h>

#include "BaseContractTest.h"
#include "retrovue/runtime/PlayoutControlStateMachine.h"

namespace retrovue::tests::contracts {

namespace {

int64_t MsToUs(double value_ms) {
  return static_cast<int64_t>(value_ms * 1'000.0);
}

}  // namespace

class PlayoutControlContractTest : public BaseContractTest {
 protected:
  [[nodiscard]] std::string DomainName() const override { return "PlayoutControl"; }

  [[nodiscard]] std::vector<std::string> CoveredRuleIds() const override {
    return {"CTL_001", "CTL_002", "CTL_003"};
  }
};

TEST_F(PlayoutControlContractTest, CTL_001_DeterministicStateTransitions) {
  runtime::PlayoutControlStateMachine controller;
  const int64_t start_time = 1'700'000'000'000'000LL;

  ASSERT_TRUE(controller.BeginSession("begin", start_time));
  controller.OnBufferDepth(5, 60, start_time + MsToUs(10));
  EXPECT_EQ(controller.state(), runtime::PlayoutControlStateMachine::State::kPlaying);

  ASSERT_TRUE(controller.Pause("pause",
                               start_time + MsToUs(50),
                               start_time + MsToUs(70),
                               0.2));
  EXPECT_EQ(controller.state(), runtime::PlayoutControlStateMachine::State::kPaused);

  ASSERT_TRUE(controller.Resume("resume",
                                start_time + MsToUs(100),
                                start_time + MsToUs(130)));
  EXPECT_EQ(controller.state(), runtime::PlayoutControlStateMachine::State::kPlaying);

  ASSERT_TRUE(controller.Seek("seek-forward",
                              start_time + MsToUs(150),
                              start_time + MsToUs(500),
                              start_time + MsToUs(200)));
  EXPECT_EQ(controller.state(), runtime::PlayoutControlStateMachine::State::kPlaying);

  ASSERT_TRUE(controller.Stop("stop",
                              start_time + MsToUs(400),
                              start_time + MsToUs(500)));
  EXPECT_EQ(controller.state(), runtime::PlayoutControlStateMachine::State::kIdle);

  // Attempt illegal transition - resume while idle.
  EXPECT_FALSE(controller.Resume("illegal-resume",
                                 start_time + MsToUs(510),
                                 start_time + MsToUs(515)));

  const auto snapshot = controller.Snapshot();
  EXPECT_EQ(snapshot.transitions.at({runtime::PlayoutControlStateMachine::State::kIdle,
                                     runtime::PlayoutControlStateMachine::State::kBuffering}),
            1u);
  EXPECT_EQ(snapshot.illegal_transition_total, 1u);
}

TEST_F(PlayoutControlContractTest, CTL_002_ControlActionLatencyCompliance) {
  runtime::PlayoutControlStateMachine controller;
  const int64_t start_time = 1'700'000'100'000'000LL;

  ASSERT_TRUE(controller.BeginSession("begin", start_time));
  controller.OnBufferDepth(4, 60, start_time + MsToUs(15));

  ASSERT_TRUE(controller.Pause("pause-ok",
                               start_time + MsToUs(50),
                               start_time + MsToUs(75),
                               0.1));
  ASSERT_TRUE(controller.Resume("resume-ok",
                                start_time + MsToUs(100),
                                start_time + MsToUs(140)));
  ASSERT_TRUE(controller.Seek("seek-ok",
                              start_time + MsToUs(150),
                              start_time + MsToUs(800),
                              start_time + MsToUs(380)));
  ASSERT_TRUE(controller.Stop("stop-ok",
                              start_time + MsToUs(400),
                              start_time + MsToUs(820)));

  auto snapshot = controller.Snapshot();
  EXPECT_EQ(snapshot.latency_violation_total, 0u);
  EXPECT_LE(snapshot.pause_latency_p95_ms, 33.0);
  EXPECT_LE(snapshot.resume_latency_p95_ms, 50.0);
  EXPECT_LE(snapshot.seek_latency_p95_ms, 250.0);
  EXPECT_LE(snapshot.stop_latency_p95_ms, 500.0);

  // Introduce violations.
  ASSERT_TRUE(controller.BeginSession("begin2", start_time + MsToUs(900)));
  controller.OnBufferDepth(3, 60, start_time + MsToUs(910));
  EXPECT_TRUE(controller.Pause("pause-breach",
                               start_time + MsToUs(920),
                               start_time + MsToUs(1'020),
                               0.0));
  snapshot = controller.Snapshot();
  EXPECT_GE(snapshot.latency_violation_total, 1u);
}

TEST_F(PlayoutControlContractTest, CTL_003_CommandIdempotencyAndFailureTelemetry) {
  runtime::PlayoutControlStateMachine controller;
  const int64_t base_time = 1'700'000'200'000'000LL;

  ASSERT_TRUE(controller.BeginSession("begin", base_time));
  controller.OnBufferDepth(3, 60, base_time + MsToUs(10));

  // First seek succeeds.
  ASSERT_TRUE(controller.Seek("seek-1",
                              base_time + MsToUs(20),
                              base_time + MsToUs(300),
                              base_time + MsToUs(220)));
  // Duplicate seek is acknowledged without mutation.
  EXPECT_TRUE(controller.Seek("seek-1",
                              base_time + MsToUs(40),
                              base_time + MsToUs(310),
                              base_time + MsToUs(250)));

  // External timeout forces error state.
  controller.OnExternalTimeout(base_time + MsToUs(260));
  EXPECT_EQ(controller.state(), runtime::PlayoutControlStateMachine::State::kError);
  auto snapshot = controller.Snapshot();
  EXPECT_EQ(snapshot.timeout_total, 1u);

  // Recovery returns to buffering.
  ASSERT_TRUE(controller.Recover("recover",
                                 base_time + MsToUs(270)));
  EXPECT_EQ(controller.state(), runtime::PlayoutControlStateMachine::State::kBuffering);

  controller.OnQueueOverflow();
  snapshot = controller.Snapshot();
  EXPECT_EQ(snapshot.queue_overflow_total, 1u);

  // Late seek should record violation.
  EXPECT_FALSE(controller.Seek("seek-backwards",
                               base_time + MsToUs(300),
                               base_time + MsToUs(100),
                               base_time + MsToUs(320)));
  snapshot = controller.Snapshot();
  EXPECT_EQ(snapshot.late_seek_total, 1u);
}

}  // namespace retrovue::tests::contracts

