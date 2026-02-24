#include <csignal>
#include <cstdlib>
#include <iostream>

#include "cluster.hh"

// ============================================================================
// Helpers
// ============================================================================

#define ASSERT_EQ(val1, val2)                                                  \
  if ((val1) != (val2)) {                                                      \
    std::cerr << "\n [FAIL] " << #val1 << " (" << (val1) << ") != " << #val2   \
              << " (" << (val2) << ") at line " << __LINE__ << std::endl;      \
    return false;                                                              \
  }

// ============================================================================
// Tests
// ============================================================================

using namespace pbft::testing;

bool test_normal_operation() {
  Cluster cluster(4);
  cluster.start();

  ASSERT_EQ(cluster.send_request("INC"), "OK");
  ASSERT_EQ(cluster.send_request("INC"), "OK");
  ASSERT_EQ(cluster.send_request("ADD:40"), "OK");

  ASSERT_EQ(cluster.verify_state(42), true);

  return true;
}

bool test_f_crashed() {
  Cluster cluster(7);
  cluster.start();

  cluster.crash_replica(3);
  cluster.crash_replica(5);
  ASSERT_EQ(cluster.send_request("SET:42"), "OK");

  ASSERT_EQ(cluster.verify_state(42), true);

  return true;
}

bool test_no_consensus() {
  Cluster cluster(4);
  cluster.start();

  cluster.crash_replica(1);
  cluster.crash_replica(2);

  cluster.send_request_async("INC");
  ASSERT_EQ(cluster.verify_state(0), true);

  return true;
}

bool test_silent_byzantine() {
  Cluster cluster(4, {{1, FaultMode::DROP_ALL}});
  cluster.start();

  ASSERT_EQ(cluster.send_request("SET:42"), "OK");

  ASSERT_EQ(cluster.verify_state(42), true);

  return true;
}

bool test_equivocate_preprepare() {
  Cluster cluster(4, {{0, FaultMode::EQUIVOCATE_PREPREPARE}});
  cluster.start();

  // Expect eventually success (after view change)
  ASSERT_EQ(cluster.send_request("INC"), "OK");
  ASSERT_EQ(cluster.verify_state(1), true);

  ASSERT_EQ((cluster.current_view() >= 1), true);

  return true;
}

bool test_double_prepare() {
  Cluster cluster(4, {{1, FaultMode::DOUBLE_PREPARE}});
  cluster.start();

  ASSERT_EQ(cluster.send_request("INC"), "OK");
  ASSERT_EQ(cluster.verify_state(1), true);

  return true;
}

bool test_double_commit() {
  Cluster cluster(4, {{1, FaultMode::DOUBLE_COMMIT}});
  cluster.start();

  ASSERT_EQ(cluster.send_request("INC"), "OK");
  ASSERT_EQ(cluster.verify_state(1), true);

  return true;
}

bool test_corrupt_checkpoint() {
  Cluster cluster(4, {{1, FaultMode::CORRUPT_CHECKPOINT}});
  uint32_t K = 10;
  uint32_t L = 20;
  cluster.set_node_config(L, K);
  cluster.start();

  // Trigger checkpoints
  for (int i = 0; i < 15; i++) {
    cluster.send_request("INC");
  }

  ASSERT_EQ(cluster.verify_state(15), true);

  return true;
}

bool test_selective_silence() {
  Cluster cluster(4, {{0, FaultMode::SELECTIVE_SILENCE}});
  cluster.start();

  // Should trigger view change
  ASSERT_EQ(cluster.send_request("INC"), "OK");
  ASSERT_EQ(cluster.verify_state(1), true);
  ASSERT_EQ(cluster.send_request("INC"), "OK");
  ASSERT_EQ(cluster.verify_state(2), true);
  // View 0 failed, so view should be at least 1
  ASSERT_EQ((cluster.current_view() >= 1), true);

  return true;
}

bool test_fake_viewchange() {
  Cluster cluster(4, {{1, FaultMode::FAKE_VIEWCHANGE}});
  cluster.start();

  // Force view change
  cluster.crash_replica(0);

  ASSERT_EQ(cluster.send_request("INC"), "OK");
  ASSERT_EQ(cluster.verify_state(1), true);

  return true;
}

bool test_view_change() {
  Cluster cluster(4);
  cluster.start();

  cluster.crash_replica(0); // Crash Primary
  ASSERT_EQ(cluster.send_request("SET:42"),
            "OK"); // Send to crashed primary

  // After a timeout it should broadcast all other replicas
  // View must have changed
  ASSERT_EQ(cluster.verify_state(42), true);
  ASSERT_EQ(cluster.current_view(), 1);

  return true;
}

bool test_multiple_view_changes() {
  Cluster cluster(7);
  cluster.start();

  cluster.crash_replica(0); // Primary of view 0
  ASSERT_EQ(cluster.send_request("SET:10"), "OK");
  ASSERT_EQ(cluster.verify_state(10), true);
  ASSERT_EQ(cluster.current_view(), 1);

  cluster.crash_replica(1); // Primary of view 1
  ASSERT_EQ(cluster.send_request("SET:20"), "OK");
  ASSERT_EQ(cluster.current_view(), 2);

  ASSERT_EQ(cluster.verify_state(20), true);
  return true;
}

bool test_checkpoint() {
  Cluster cluster(4);
  uint32_t K = 10;
  uint32_t L = 20;
  cluster.set_node_config(L, K);
  cluster.start();

  const int total_ops = 25;

  // Send bursts, so the are commited locally
  for (uint32_t i = 0; i < total_ops; i++) {
    cluster.send_request("INC");
  }

  int stable_replicas = 0;
  for (const auto &rep : cluster.replicas) {
    uint64_t low_watermark = rep.node->h_;
    size_t log_size = rep.node->reqlog_.size();

    // With 25 ops and K=10, we should have checkpointed at 10 and 20
    // So h should be 20
    // Log should also be truncated
    if (low_watermark == 20 && log_size == 5) {
      stable_replicas++;
    }
  }

  // All replicas should be stable (watermark + log size)
  ASSERT_EQ(stable_replicas, 4);

  return true;
}

int main() {
  std::cout << " === Running PBFT Integration Tests === " << std::endl;

  std::vector<std::pair<std::string, bool (*)()>> tests = {
      {"Normal Operation", test_normal_operation},
      {"Crashed f Nodes", test_f_crashed},
      {"No Consensus", test_no_consensus},
      {"Silent Byzantine", test_silent_byzantine},
      {"Equivocating Primary Safety", test_equivocate_preprepare},
      {"Double Prepare", test_double_prepare},
      {"Double Commit", test_double_commit},
      {"Corrupt Checkpoint", test_corrupt_checkpoint},
      {"Selective Silence", test_selective_silence},
      {"Fake ViewChange Certificate", test_fake_viewchange},
      {"Force View Change", test_view_change},
      {"Force Multiple View Changes", test_multiple_view_changes},
      {"Checkpointing and Garbage Collection", test_checkpoint},
  };

  int passed = 0;
  int failed = 0;

  for (auto &[name, test_fn] : tests) {
    std::cout << "[RUN] " << name << " ... " << std::flush;
    try {
      if (test_fn()) {
        std::cout << "PASSED" << std::endl;
        passed++;
      } else {
        std::cout << "FAILED" << std::endl;
        failed++;
      }
    } catch (const std::exception &e) {
      std::cout << "EXCEPTION: " << e.what() << std::endl;
      failed++;
    }
  }

  std::cout << "\nResults: " << passed << " Passed, " << failed << " Failed."
            << std::endl;
  return (failed == 0) ? 0 : 1;
}
