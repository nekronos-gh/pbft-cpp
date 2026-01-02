#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#define PBFT_TESTING_ACCESS public
#include "pbft/messages.hh"
#include "pbft/node.hh"
#include "pbft/service_interface.hh"

using namespace pbft;
using namespace salticidae;

#define ASSERT_TRUE(cond)                                                      \
  if (!(cond)) {                                                               \
    std::cerr << "   [FAIL] " << #cond << " at line " << __LINE__              \
              << std::endl;                                                    \
    return false;                                                              \
  }

#define ASSERT_EQ(val1, val2)                                                  \
  if ((val1) != (val2)) {                                                      \
    std::cerr << "   [FAIL] " << #val1 << " (" << (val1) << ") != " << #val2   \
              << " (" << (val2) << ") at line " << __LINE__ << std::endl;      \
    return false;                                                              \
  }

// ============================================================================
// Service Setup
// ============================================================================
using ExecutionHook = std::function<std::string(const std::string &)>;
// Mock service for dummy application
// Manage a global counter and allow for increase, decrease, addition and set
class TestService : public ServiceInterface {
private:
  std::vector<std::string> executed_ops_;
  int counter_;
  uint256_t state_digest_;
  ExecutionHook execution_hook_;

public:
  TestService() : counter_(0) {}

  void initialize() override {
    executed_ops_.clear();
    counter_ = 0;
    update_digest();
  }

  std::string execute(const std::string &operation) override {
    std::string op_to_execute = operation;
    // Used for byzantine execution
    if (execution_hook_) {
      op_to_execute = execution_hook_(operation);
    }

    executed_ops_.push_back(op_to_execute);

    if (op_to_execute == "INC") {
      counter_++;
    } else if (op_to_execute == "DEC") {
      counter_--;
    } else if (op_to_execute.substr(0, 4) == "ADD:") {
      counter_ += std::stoi(op_to_execute.substr(4));
    } else if (op_to_execute.substr(0, 4) == "SET:") {
      counter_ = std::stoi(op_to_execute.substr(4));
    }

    update_digest();
    return "OK:" + std::to_string(counter_);
  }

  uint256_t get_checkpoint_digest() override { return state_digest_; }
  int get_counter() const { return counter_; }
  size_t get_op_count() const { return executed_ops_.size(); }
  void set_execution_hook(ExecutionHook hook) { execution_hook_ = hook; }

private:
  void update_digest() {
    std::string state =
        std::to_string(counter_) + ":" + std::to_string(executed_ops_.size());
    state_digest_ = salticidae::get_hash(state);
  }
};

// ============================================================================
// Cluster Setup
// ============================================================================
// Cluster for local execution of many replicas
class TestCluster {
public:
  struct ReplicaInfo {
    uint32_t id;
    std::unique_ptr<Node> node;
    TestService *service;
    std::thread thread;
    bool crashed = false;
    bool byzantine = false;
    NetAddr addr;
    MsgNetwork<uint8_t>::conn_t conn;
  };

  std::vector<ReplicaInfo> replicas;

private:
  NodeConfig config_;
  uint32_t n_;
  uint32_t f_;
  uint32_t primary_;

  // Network with salticidae
  EventContext client_ec_;
  std::thread client_thread_;
  std::unique_ptr<MsgNetwork<uint8_t>> client_net_;
  uint32_t client_id_;
  NetAddr client_addr_;
  uint64_t request_counter_;

public:
  TestCluster(uint32_t n)
      : n_(n), f_((n - 1) / 3), primary_(0), client_id_(0),
        request_counter_(0) {
    config_ = {n, 200, 100, 2.0, std::nullopt};
    client_addr_ = NetAddr("127.0.0.1:11000");
  }

  void start() {
    replicas.reserve(n_);
    // Initialize replicas
    for (uint32_t i = 0; i < n_; i++) {
      ReplicaInfo info;
      info.id = i;
      auto service = std::make_unique<TestService>();
      service->initialize();
      info.service = service.get();
      info.node = std::make_unique<Node>(i, config_, std::move(service));
      replicas.push_back(std::move(info));
    }

    // Srart replicas
    for (uint32_t i = 0; i < n_; i++) {
      NetAddr listen_addr("127.0.0.1:" + std::to_string(10000 + i));
      replicas[i].thread = std::thread([this, i, listen_addr]() {
        replicas[i].addr = listen_addr;
        replicas[i].node->start(listen_addr);
        replicas[i].node->run();
      });
    }

    // Setup client
    MsgNetwork<uint8_t>::Config net_config;
    client_net_ = std::make_unique<MsgNetwork<uint8_t>>(client_ec_, net_config);
    client_net_->start();
    client_net_->listen(client_addr_);
    client_thread_ = std::thread([this]() { client_ec_.dispatch(); });

    // Connect replicas
    for (uint32_t i = 0; i < n_; i++) {
      for (uint32_t j = 0; j < n_; j++) {
        if (i != j)
          replicas[i].node->add_replica(j, replicas[j].addr);
      }
      replicas[i].node->add_client(client_id_, client_addr_);
    }

    // Connect client
    for (uint32_t i = 0; i < n_; i++) {
      replicas[i].conn = client_net_->connect_sync(replicas[i].addr);
    }
  }

  // Stop the cluster
  void stop() {
    client_ec_.stop();
    if (client_net_) {
      for (uint32_t i = 0; i < n_; i++)
        client_net_->terminate(replicas[i].conn);
      client_net_->stop();
    }
    if (client_thread_.joinable())
      client_thread_.join();

    for (auto &rep : replicas) {
      if (!rep.crashed) {
        rep.node->stop();
        if (rep.thread.joinable())
          rep.thread.join();
      }
    }
    replicas.clear();
  }

  // Update node configuration
  void set_config(uint32_t log_size, uint32_t checkpoint_interval,
                  double vc_timeout = 2.0) {
    config_ = {n_, log_size, checkpoint_interval, vc_timeout, {}};
  }

  // Send request to primary
  void send_request(const std::string &operation) {
    RequestMsg req(operation, request_counter_++, client_id_);
    client_net_->send_msg(req, replicas[primary_].conn);
  }

  // Broadcast request to all replicas
  void broadcast_request(const std::string &operation) {
    RequestMsg req(operation, request_counter_++, client_id_);
    for (uint32_t i = 0; i < n_; i++) {
      client_net_->send_msg(req, replicas[i].conn);
    }
  }

  // Disable replica in the network
  void crash_replica(uint32_t id) {
    if (id >= replicas.size())
      return;
    replicas[id].crashed = true;
    replicas[id].node->stop();
    if (replicas[id].thread.joinable())
      replicas[id].thread.join();
  }

  void set_primary(uint32_t primary) { primary_ = primary; }

  // Inject code into a replica
  void set_byzantine_behavior(uint32_t id, ExecutionHook hook) {
    if (id >= replicas.size())
      return;
    replicas[id].service->set_execution_hook(hook);
    replicas[id].byzantine = true;
  }

  // Send broadcast as one of the replicas
  template <typename M> void forge_broadcast(uint32_t id, M m) {
    replicas[id].node->broadcast(m);
  }

  // Wait until a number of operations is executed
  bool wait_for_operations(size_t expected_ops, uint32_t n_reps,
                           uint32_t timeout_ms = 1000) {
    auto start = std::chrono::steady_clock::now();
    while (true) {
      uint32_t correct_count = 0;
      for (const auto &rep : replicas) {
        if (rep.crashed || rep.byzantine)
          continue;
        if (rep.service->get_op_count() == expected_ops)
          correct_count++;
      }
      if (correct_count >= n_reps)
        return true;

      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count();
      if (elapsed > timeout_ms)
        return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  // Wait until a viewchange occurs
  bool wait_for_viewchange(uint32_t expected_view,
                           uint32_t timeout_ms = 10000) {
    auto start = std::chrono::steady_clock::now();
    while (true) {
      bool all_match = true;
      for (const auto &rep : replicas) {
        if (!rep.crashed && rep.node->view_ != expected_view) {
          all_match = false;
          break;
        }
      }
      if (all_match)
        return true;

      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count();
      if (elapsed > timeout_ms)
        return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  // Verify that 2f+1 nodes agree on exectuion order
  bool verify_consensus() {
    std::map<uint32_t, uint32_t> counter_votes;
    std::map<size_t, uint32_t> op_votes;
    uint32_t correct_count = 0;

    for (const auto &rep : replicas) {
      if (rep.crashed || rep.byzantine)
        continue;

      counter_votes[rep.service->get_counter()]++;
      op_votes[rep.service->get_op_count()]++;
      correct_count++;
    }

    if (correct_count == 0)
      return false;

    uint32_t max_votes = 0;
    for (const auto &[val, votes] : counter_votes)
      if (votes > max_votes)
        max_votes = votes;

    uint32_t max_op_votes = 0;
    for (const auto &[val, votes] : op_votes)
      if (votes > max_op_votes)
        max_op_votes = votes;

    // We need 2f+1 matching responses in a real client, but here we just check
    // if the majority of correct nodes agree.
    bool safe_majority = (correct_count > f_);
    bool consensus = (max_votes >= correct_count - f_) &&
                     (max_op_votes >= correct_count - f_);
    return consensus && safe_majority;
  }
};

// ============================================================================
// Tests
// ============================================================================

bool test_normal_operation() {
  TestCluster cluster(4);
  cluster.start();

  cluster.send_request("INC");
  cluster.send_request("INC");
  cluster.send_request("ADD:40");

  ASSERT_TRUE(cluster.wait_for_operations(3, 4));
  ASSERT_TRUE(cluster.verify_consensus());

  // Verify state
  ASSERT_EQ(cluster.replicas[0].service->get_counter(), 42);

  cluster.stop();
  return true;
}

bool test_f_crashed() {
  TestCluster cluster(7);
  cluster.start();

  cluster.crash_replica(3);
  cluster.crash_replica(5);
  cluster.send_request("SET:42");

  // Consensus should still happen
  ASSERT_TRUE(cluster.wait_for_operations(1, 5));
  ASSERT_TRUE(cluster.verify_consensus());

  cluster.stop();
  return true;
}

bool test_no_consensus() {
  TestCluster cluster(4);
  cluster.start();

  cluster.crash_replica(1);
  cluster.crash_replica(2);

  cluster.send_request("INC");
  cluster.send_request("INC");

  // Should timeout (fail to reach consensus)
  ASSERT_TRUE(!cluster.wait_for_operations(2, 4, 1500));

  cluster.stop();
  return true;
}

bool test_byzantine() {
  TestCluster cluster(4);
  cluster.start();
  uint64_t byz_node = 1;

  // Replica 1 will modify SET commands
  cluster.set_byzantine_behavior(
      byz_node, [](const std::string &original) -> std::string {
        if (original.substr(0, 4) == "SET:")
          return "SET:666";
        return original;
      });

  auto fake_digest = salticidae::get_hash("SET:666");

  cluster.send_request("SET:42"); // Should be seq 1

  // Byzantine node broadcasts fake Prepare/Commit matching the fake digest
  cluster.forge_broadcast(byz_node, PrepareMsg(0, 1, fake_digest, byz_node));
  cluster.forge_broadcast(byz_node, CommitMsg(0, 1, fake_digest, byz_node));

  // Correct nodes should ignore the byzantine noise and execute SET:42
  ASSERT_TRUE(cluster.wait_for_operations(1, 3));
  ASSERT_TRUE(cluster.verify_consensus());
  for (const auto &rep : cluster.replicas) {
    if (!rep.byzantine) {
      ASSERT_EQ(rep.service->get_counter(), 42);
    }
  }

  cluster.stop();
  return true;
}

bool test_view_change() {
  TestCluster cluster(4);
  cluster.start();

  cluster.crash_replica(0);       // Crash Primary
  cluster.send_request("SET:42"); // Send to crashed primary

  // First set should be ignored, now send to the rest of the replicas
  cluster.broadcast_request("SET:42");

  ASSERT_TRUE(cluster.wait_for_viewchange(1));

  cluster.set_primary(1);
  cluster.send_request("SET:42");

  ASSERT_TRUE(cluster.wait_for_operations(1, 3));
  ASSERT_TRUE(cluster.verify_consensus());

  cluster.stop();
  return true;
}

bool test_checkpoint() {
  TestCluster cluster(4);
  uint32_t K = 10;
  uint32_t L = 20;
  cluster.set_config(L, K);
  cluster.start();

  const int total_ops = 25;

  // Send bursts, so the are commited locally
  for (uint32_t i = 0; i < total_ops; i++) {
    cluster.send_request("INC");
    if (i % 5 == 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  ASSERT_TRUE(cluster.wait_for_operations(total_ops, 4));

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
  ASSERT_TRUE(cluster.verify_consensus());

  cluster.stop();
  return true;
}

// ============================================================================
// Main
// ============================================================================

int main() {
  std::cout << "Running PBFT Integration Tests..." << std::endl;

  std::vector<std::pair<std::string, bool (*)()>> tests = {
      {"Normal Operation", test_normal_operation},
      {"Crashed f Nodes", test_f_crashed},
      {"No Consensus", test_no_consensus},
      {"Byzantine Execution", test_byzantine},
      {"Force View Change", test_view_change},
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
