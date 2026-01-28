#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#define PBFT_TESTING_ACCESS public
#include "pbft/client.hh"
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
    return "OK";
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
  NodeConfig node_config_;
  ClientConfig client_config_;
  uint32_t n_;
  uint32_t f_;

  // Client
  std::unique_ptr<Client> client_;
  std::thread client_thread_;
  uint32_t client_id_;
  NetAddr client_addr_;

  void create_nodes() {
    replicas.reserve(n_);
    for (uint32_t i = 0; i < n_; i++) {
      ReplicaInfo info;
      info.id = i;
      auto service = std::make_unique<TestService>();
      service->initialize();
      info.service = service.get();
      info.node = std::make_unique<Node>(i, node_config_, std::move(service));
      replicas.push_back(std::move(info));
    }

    // Start replicas
    for (uint32_t i = 0; i < n_; i++) {
      NetAddr listen_addr("127.0.0.1:" + std::to_string(10000 + i));
      replicas[i].thread = std::thread([this, i, listen_addr]() {
        replicas[i].addr = listen_addr;
        replicas[i].node->start(listen_addr);
        replicas[i].node->run();
      });
    }
  }

  void create_client() {
    client_ = std::make_unique<Client>(client_id_, client_config_);
    client_->start(client_addr_);
    client_thread_ = std::thread([this]() { client_->run(); });
  }

  void enable_conections() {
    for (uint32_t i = 0; i < n_; i++) {
      // Other replica connnections
      for (uint32_t j = 0; j < n_; j++) {
        if (i != j) {
          NetAddr replica_addr("127.0.0.1:" + std::to_string(10000 + j));
          replicas[i].node->add_replica(j, replica_addr);
        }
      }
      NetAddr replica_addr("127.0.0.1:" + std::to_string(10000 + i));
      // Client connections
      client_->add_replica(i, replica_addr);
      replicas[i].node->add_client(client_id_, client_addr_);
    }
  }

public:
  TestCluster(uint32_t n) : n_(n), f_((n - 1) / 3), client_id_(0) {
    node_config_ = {n, 200, 100, 2.0, std::nullopt};
    client_config_ = {n, 2000, std::nullopt};
    client_addr_ = NetAddr("127.0.0.1:11000");
  }

  void start() {
    // Initialize replicas
    create_nodes();
    // Setup client
    create_client();
    // Stablish connections
    enable_conections();
  }

  // Stop the cluster
  void stop() {
    if (client_) {
      client_->stop();
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
  void set_node_config(uint32_t log_size, uint32_t checkpoint_interval,
                       double vc_timeout = 2.0) {
    node_config_ = {n_, log_size, checkpoint_interval, vc_timeout, {}};
  }

  // Send request to cluster
  std::string send_request(const std::string &operation) {
    return client_->invoke(operation);
  }
  std::future<std::string> send_request_async(const std::string &operation) {
    return client_->invoke_async(operation);
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

  uint32_t current_view() { return client_->current_view_; }

  // Check that enough services agree on a counter
  bool verify_state(int counter) {
    uint32_t correct = 0;
    for (const auto &rep : replicas) {
      if (rep.service->get_counter() == counter) {
        correct++;
      }
    }
    return (correct > 2 * f_);
  }
};

// ============================================================================
// Tests
// ============================================================================

bool test_normal_operation() {
  TestCluster cluster(4);
  cluster.start();

  ASSERT_EQ(cluster.send_request("INC"), "OK");
  ASSERT_EQ(cluster.send_request("INC"), "OK");
  ASSERT_EQ(cluster.send_request("ADD:40"), "OK");

  ASSERT_EQ(cluster.verify_state(42), true);

  cluster.stop();
  return true;
}

bool test_f_crashed() {
  TestCluster cluster(7);
  cluster.start();

  cluster.crash_replica(3);
  cluster.crash_replica(5);
  ASSERT_EQ(cluster.send_request("SET:42"), "OK");

  ASSERT_EQ(cluster.verify_state(42), true);

  cluster.stop();
  return true;
}

bool test_no_consensus() {
  TestCluster cluster(4);
  cluster.start();

  cluster.crash_replica(1);
  cluster.crash_replica(2);

  cluster.send_request_async("INC");
  ASSERT_EQ(cluster.verify_state(0), true);

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

  // Byzantine node broadcasts fake Prepare/Commit matching the fake digest
  auto fut = cluster.send_request_async("SET:42");
  cluster.forge_broadcast(byz_node, PrepareMsg(0, 1, fake_digest, byz_node));
  cluster.forge_broadcast(byz_node, CommitMsg(0, 1, fake_digest, byz_node));

  // Correct nodes should ignore the byzantine noise and execute SET:42
  ASSERT_EQ(fut.get(), "OK");
  ASSERT_EQ(cluster.verify_state(42), true);

  cluster.stop();
  return true;
}

bool test_view_change() {
  TestCluster cluster(4);
  cluster.start();

  cluster.crash_replica(0); // Crash Primary
  ASSERT_EQ(cluster.send_request("SET:42"),
            "OK"); // Send to crashed primary

  // After a timeout it should broadcast all other replicas
  // View must have changed
  ASSERT_EQ(cluster.verify_state(42), true);
  ASSERT_EQ(cluster.current_view(), 1);

  cluster.stop();
  return true;
}

bool test_checkpoint() {
  TestCluster cluster(4);
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

  cluster.stop();
  return true;
}

// ============================================================================
// Main
// ============================================================================

int main() {
  std::cout << " === Running PBFT Integration Tests === " << std::endl;

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
