#include <iostream>
#include <vector>
#include <memory>
#include <thread>
#include <chrono>
#include <map>

#define PBFT_TESTING_ACCESS public
#include "pbft/node.hh"
#include "pbft/messages.hh"
#include "pbft/service_interface.hh"

using namespace pbft;
using namespace salticidae;


// ============================================================================
// Service Setup
// ============================================================================
// Simple state machine to verify execution
using ExecutionHook = std::function<std::string(const std::string&)>;
class TestService : public ServiceInterface {
private:
  std::vector<std::string> executed_ops_;
  int counter_;
  uint256_t state_digest_;

  // For byzantine simulation
  ExecutionHook execution_hook_;
public:
  TestService() : counter_(0) {}

  void initialize() override {
    executed_ops_.clear();
    counter_ = 0;
    update_digest();
  }

  std::string execute(const std::string& operation) override {
    std::string op_to_execute = operation;
    if (execution_hook_) {
      // The hook takes the original op and returns the (possibly evil) op
      op_to_execute = execution_hook_(operation);
    }

    executed_ops_.push_back(op_to_execute);

    if (op_to_execute == "INC") {
      counter_++;
    } else if (op_to_execute == "DEC") {
      counter_--;
    } else if (op_to_execute.substr(0, 4) == "ADD:") {
      int value = std::stoi(op_to_execute.substr(4));
      counter_ += value;
    } else if (op_to_execute.substr(0, 4) == "SET:") {
      int value = std::stoi(op_to_execute.substr(4));
      counter_ = value;
    }

    update_digest();
    return "OK:" + std::to_string(counter_);
  }

  uint256_t get_checkpoint_digest() override {
    return state_digest_;
  }

  int get_counter() const { return counter_; }
  size_t get_op_count() const { return executed_ops_.size(); }

  void set_execution_hook(ExecutionHook hook) {
    execution_hook_ = hook;
  }


private:
  void update_digest() {
    std::string state = std::to_string(counter_) + ":" + 
      std::to_string(executed_ops_.size());
    state_digest_ = salticidae::get_hash(state);
  }
};

// ============================================================================
// Node Setup
// ============================================================================
class TestCluster {
private:
  struct ReplicaInfo {
    uint32_t id;
    std::unique_ptr<Node> node;
    TestService* service;
    std::thread thread;
    bool crashed = false;
    NetAddr addr;
    MsgNetwork<uint8_t>::conn_t conn;
    bool byzantine = false;
  };

  std::vector<ReplicaInfo> replicas_;
  uint32_t n_;
  uint32_t f_;
  uint32_t primary_;

  const double vc_timeout_{2.0};

  // Client network
  EventContext client_ec_;
  std::thread client_thread_; // New thread for client EC
  std::unique_ptr<MsgNetwork<uint8_t>> client_net_;
  uint32_t client_id_;
  NetAddr client_addr_;
  uint64_t request_counter_;

public:
  TestCluster(uint32_t n) : n_(n), f_((n - 1) / 3), primary_(0), client_id_(0), request_counter_(0) {
    client_addr_ = NetAddr("127.0.0.1:11000");
  }

  void start() {
    std::cout << "[Cluster] Starting..." << std::endl;
    // Initialize all replicas with their respective services
    replicas_.reserve(n_);
    for (uint32_t i = 0; i < n_; i++) {
      ReplicaInfo info;
      info.id = i;
      auto service = std::make_unique<TestService>();
      service->initialize();
      info.service = service.get();
      info.node = std::make_unique<Node>(i, n_, std::move(service), std::nullopt, vc_timeout_);
      replicas_.push_back(std::move(info));
    }

    // Start Node Threads
    for (uint32_t i = 0; i < n_; i++) {
      NetAddr listen_addr("127.0.0.1:" + std::to_string(10000 + i));
      replicas_[i].thread = std::thread([this, i, listen_addr]() {
        replicas_[i].addr = listen_addr;
        replicas_[i].node->start(listen_addr);
        replicas_[i].node->run();
      });
      std::cout << "[Replica " << i << "] Started on port " << (10000 + i) << std::endl;
    }

    // Setup Client
    MsgNetwork<uint8_t>::Config net_config;
    client_net_ = std::make_unique<MsgNetwork<uint8_t>>(client_ec_, net_config);
    client_net_->start(); // Starts worker threads
    client_net_->listen(client_addr_);
    client_net_->reg_handler(salticidae::generic_bind(&TestCluster::on_reply, this, _1, _2));
    client_thread_ = std::thread([this]() {
      client_ec_.dispatch();
    });

    // Configure peers
    for (uint32_t i = 0; i < n_; i++) {
      for (uint32_t j = 0; j < n_; j++) {
        if (i != j) {
          replicas_[i].node->add_replica(j, replicas_[j].addr);
        }
      }
      replicas_[i].node->add_client(client_id_, client_addr_);
    }

    // Connect client to all replicas
    for (uint32_t i = 0; i < n_; i++) {
      replicas_[i].conn = client_net_->connect_sync(replicas_[i].addr);
    }

    std::cout << "[Cluster] Started" << std::endl;
  }

  void stop() {
    std::cout << "[Cluster] Stopping..." << std::endl;
    // Stop client
    client_ec_.stop();
    if (client_net_) {
      for (uint32_t i = 0; i < n_; i++) {
        client_net_->terminate(replicas_[i].conn);
      }
      client_net_->stop();
    }
    if (client_thread_.joinable()) {
        client_thread_.join();
    }
    std::cout << "[Cluster] Client stopped" << std::endl;

    // Stop replicas
    for (auto& rep : replicas_) {
      if (!rep.crashed) { 
        rep.node->stop();
        if (rep.thread.joinable()) {
            rep.thread.join();
        }
      }
    }

    replicas_.clear();
    std::cout << "[Cluster] Stopped\n" << std::endl;
  }

  void send_request(const std::string& operation) {
    RequestMsg req(operation, request_counter_++, client_id_);
    std::cout << "[Client] Sending: " << operation << " to primary cluster" << std::endl;
    client_net_->send_msg(req, replicas_[primary_].conn);
  }

  void broadcast_request(const std::string& operation) {
    RequestMsg req(operation, request_counter_++, client_id_);
    std::cout << "[Client] Sending broadcast: " << operation << " to cluster" << std::endl;
    for (uint32_t i = 0; i < n_; i++) {
      client_net_->send_msg(req, replicas_[i].conn);
    }
  }

  void on_reply(ReplyMsg &&m, const MsgNetwork<uint8_t>::conn_t &) {
    std::cout << "[Client] Received Reply from Replica " << m.replica_id 
      << ": " << m.result << std::endl;
  }

  void crash_replica(uint32_t id) {
    if (id >= replicas_.size()) return;
    replicas_[id].crashed = true;
    replicas_[id].node->stop(); 
    if (replicas_[id].thread.joinable()) {
      replicas_[id].thread.join();
    }
    std::cout << "[Cluster] Replica " << id << " crashed" << std::endl;
  }

  void set_primary(uint32_t primary) {
    primary_ = primary;
  }

  void set_byzantine_behavior(uint32_t id, ExecutionHook hook) {
    if (id >= replicas_.size()) return;
    replicas_[id].service->set_execution_hook(hook);
    replicas_[id].byzantine = true; 
    std::cout << "[Cluster] Injected byzantine behavior into replica " << id << std::endl;
  }

  template <typename M> 
  void forge_broadcast(uint32_t id, M m) {
    // Create a fake digest 
    uint256_t fake_digest = salticidae::get_hash("EvilPayload");
    replicas_[id].node->broadcast(m);
  }

  // Wait untill a number of executions have been observed in the service for each node
  bool wait_for_operations(size_t expected_ops, uint32_t n_reps, uint32_t timeout_ms = 1000) {
    auto start = std::chrono::steady_clock::now();

    while (true) {
      uint32_t correct_count = 0;

      for (const auto& rep : replicas_) {
        if (rep.crashed || rep.byzantine) {
          continue;
        }

        size_t ops = rep.service->get_op_count();

        if (ops == expected_ops) {
          correct_count++;
        }
      }

      if (correct_count >= n_reps) {
        return true;
      }

      // Check timeout
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

      if (elapsed > timeout_ms) {
        std::cout << "[Wait] Timeout waiting for " << expected_ops << " operations" << std::endl;
        return false;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  // Wait until view change
  bool wait_for_viewchange(uint32_t expected_view, uint32_t timeout_ms = 10000) {
    std::cout << "[Wait] Waiting for view change..." << std::endl;

    auto start = std::chrono::steady_clock::now();

    while (true) {
      bool all_match = true;

      // Check if all non-crashed replicas are in expected view
      for (const auto& rep : replicas_) {
        if (!rep.crashed && rep.node->view_ != expected_view) {
          all_match = false;
          break;
        }
      }

      if (all_match) {
        std::cout << "[Wait] View changed correctly!" << std::endl;
        return true;
      }

      // Check timeout
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

      if (elapsed > timeout_ms) {
        std::cout << "[Wait] Timeout waiting for view " << expected_view << std::endl;

        // Show current state
        for (const auto& rep : replicas_) {
          if (!rep.crashed) {
            std::cout << "  Replica " << rep.id << ": view=" 
              << rep.node->view_ << std::endl;
          } else {
            std::cout << "  Replica " << rep.id << ": CRASHED" << std::endl;
          }
        }
        return false;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  // Check that all services are in the same state
  bool verify_consensus() {
    std::cout << "[Verify] Checking consensus..." << std::endl;
    std::map<uint32_t, uint32_t> counter_votes;
    std::map<size_t, uint32_t> op_votes;

    int correct_count = 0;
    for (const auto& rep : replicas_) {
      if (rep.crashed) {
        std::cout << "  Replica " << rep.id << ": CRASHED" << std::endl;
        continue;
      }

      int counter = rep.service->get_counter();
      size_t ops = rep.service->get_op_count();

      std::cout << "  Replica " << rep.id << ": counter=" << counter 
        << ", ops=" << ops;

      if (rep.byzantine) {
        std::cout << " (BYZANTINE)" << std::endl;
      } else {
        counter_votes[counter]++;
        op_votes[ops]++;
        correct_count++;
        std::cout << std::endl;
      }
    }

    if (correct_count == 0) {
      std::cout << "[Verify] No correct replicas!" << std::endl;
      return false;
    }

    uint32_t max_votes = 0;
    uint32_t agreed_counter = 0;
    for (const auto& [counter, votes] : counter_votes) {
      if (votes > max_votes) {
        max_votes = votes;
        agreed_counter = counter;
      }
    }

    uint32_t max_op_votes = 0;
    uint32_t agreed_ops = 0;
    for (const auto& [ops, votes] : op_votes) {
      if (votes > max_op_votes) {
        max_op_votes = votes;
        agreed_ops = ops;
      }
    }

    bool consensus = (max_votes >= correct_count - f_) && (max_op_votes >= correct_count - f_);

    if (consensus) {
      std::cout << "[Verify] CONSENSUS: Enough correct replicas agree (" << agreed_counter << ", " << agreed_ops << ")" << std::endl;
    } else {
      std::cout << "[Verify] NO CONSENSUS: Replicas disagree" << std::endl;
    }

    return consensus;
  }

  bool verify_current_view(uint32_t view) {
    for (const auto& rep : replicas_) {
      if (!rep.crashed && rep.node->view_ != view) {
        std::cout << "[Verify] VIEW: System is not in view: " << view << std::endl;
        for (const auto& rep : replicas_) {
          if (!rep.crashed) {
            std::cout << "  Replica " << rep.id << ": view=" << rep.node->view_ << std::endl;
          } else {
            std::cout << "  Replica " << rep.id << ": CRASHED" << std::endl;
          }
        }
        return false;
      }
    }
    std::cout << "[Verify] VIEW: System is in view: " << view << std::endl;
    return true;
  }
};

// ============================================================================
// Tests
// ============================================================================

bool test_normal_operation() {
  std::cout << "╔══════════════════════════════════════════════╗" << std::endl;
  std::cout << "║  TEST: Normal Operation                      ║" << std::endl;
  std::cout << "╚══════════════════════════════════════════════╝" << std::endl;
  bool passed = true;

  TestCluster cluster(4);
  cluster.start();

  cluster.send_request("INC");
  cluster.send_request("INC");
  cluster.send_request("ADD:40");

  // 4 replicas should execute
  if (!cluster.wait_for_operations(3, 4)) {
    passed = false;
  }
  if (!cluster.verify_consensus()) {
    passed = false;
  }
  cluster.stop();

  return passed;
}

bool test_f_crashed() {
  std::cout << "╔══════════════════════════════════════════════╗" << std::endl;
  std::cout << "║  TEST: f=2 Crashed Node                      ║" << std::endl;
  std::cout << "╚══════════════════════════════════════════════╝" << std::endl;
  bool passed = true;

  TestCluster cluster(7);
  cluster.start();

  // Crash two replicas
  cluster.crash_replica(3);
  cluster.crash_replica(5);

  cluster.send_request("SET:42");

  // 5 replicas should execute
  if (!cluster.wait_for_operations(1, 5)) {
    passed = false;
  }
  if (!cluster.verify_consensus()) {
    passed = false;
  }
  cluster.stop();

  return passed;
}

bool test_no_consensus() {
  std::cout << "╔══════════════════════════════════════════════╗" << std::endl;
  std::cout << "║  TEST: No Consensus (> f)                    ║" << std::endl;
  std::cout << "╚══════════════════════════════════════════════╝" << std::endl;
  bool passed = true;

  TestCluster cluster(4);
  cluster.start();

  // Crash two replicas
  cluster.crash_replica(1);
  cluster.crash_replica(2);

  cluster.send_request("INC");
  cluster.send_request("INC");
  cluster.send_request("ADD:20");
  cluster.send_request("ADD:20");

  // No replicas should execute
  if (!cluster.wait_for_operations(0, 2)) {
    passed = false;
  }
  // No replicas should execute
  if (!cluster.verify_consensus()) {
    passed = false;
  }
  cluster.stop();

  return passed;
}

bool test_byzantine() {
  std::cout << "╔══════════════════════════════════════════════╗" << std::endl;
  std::cout << "║  TEST: Byzantine Execution                   ║" << std::endl;
  std::cout << "╚══════════════════════════════════════════════╝" << std::endl;
  bool passed = true;

  TestCluster cluster(4);
  cluster.start();
  uint64_t byzanite_node = 1;

  // We want Replica 2 to turn any "SET" command into a "SET:666" command.
  cluster.set_byzantine_behavior(byzanite_node, 
  [](const std::string& original_op) -> std::string {
      if (original_op.substr(0, 4) == "SET:") {
          // Modify on rutime
          return "SET:666";
      }
      return original_op;
  });


  // Fun begins: mess up the cluster
  // Send a valid PrePrepare to everybody
  auto forged_request = RequestMsg("SET:666", 0, 0);
  auto fake_digest = salticidae::get_hash("SET:666");
  uint64_t view = 0;
  uint64_t seq_num = 1;

  // Start consensus on SET:42
  cluster.send_request("SET:42");
  // Send a valid Prepare message as executing SET:666
  cluster.forge_broadcast(byzanite_node, 
                          PrepareMsg(view, seq_num, fake_digest, byzanite_node));

  // Send a valid Commit message and execute
  cluster.forge_broadcast(byzanite_node, 
                          CommitMsg(view, seq_num, fake_digest, byzanite_node));

  if (!cluster.wait_for_operations(1, 3)) {
    passed = false;
  }
  if (!cluster.verify_consensus()) {
     passed = false;
  }
  
  cluster.stop();
  return passed;
}

bool test_view_change() {
  std::cout << "╔══════════════════════════════════════════════╗" << std::endl;
  std::cout << "║  TEST: View Change (Primary Failure)         ║" << std::endl;
  std::cout << "╚══════════════════════════════════════════════╝" << std::endl;
  bool passed = true;

  TestCluster cluster(4);
  cluster.start();

  // Crash Primary in view 0
  cluster.crash_replica(0);

  cluster.send_request("SET:42");
  // Nothing should be executed
  if (cluster.wait_for_operations(1, 4)) {
    passed = false;
  }
  // Timeout, no response, broadcast the nodes
  cluster.broadcast_request("SET:42");

  // Validate that view change happenned
  if (!cluster.wait_for_viewchange(1)) {
    passed = false;
  }

  // Send new request to primary
  cluster.set_primary(1);
  cluster.send_request("SET:42");
  // Everything executed in new view
  if (!cluster.wait_for_operations(1, 3)) {
    passed = false;
  }
  if (!cluster.verify_consensus()) {
     passed = false;
  }

  cluster.stop();
  return passed;
}

// ============================================================================
// Main
// ============================================================================

int main() {
  std::cout << "╔══════════════════════════════════════════════╗" << std::endl;
  std::cout << "║                                              ║" << std::endl;
  std::cout << "║    PBFT Integration Test Suite               ║" << std::endl;
  std::cout << "║    n=4, f=1                                  ║" << std::endl;
  std::cout << "║                                              ║" << std::endl;
  std::cout << "╚══════════════════════════════════════════════╝" << std::endl;

  std::vector<std::pair<std::string, bool(*)()>> tests = {
    {"Normal Operation", test_normal_operation},
    {"Crashed f Nodes", test_f_crashed},
    {"No Consensus", test_no_consensus},
    {"Byzantine Execution", test_byzantine},
    {"Force View Change", test_view_change},
  };

  int passed = 0;
  int failed = 0;

  for (auto& [name, test_fn] : tests) {
    try {
      if (test_fn()) {
        std::cout << name << " PASSED" << std::endl;
        passed++;
      } else {
        std::cout << name << " FAILED" << std::endl;
        failed++;
      }
    } catch (const std::exception& e) {
      std::cout << name << " EXCEPTION: " << e.what() << std::endl;
      failed++;
    }
  }

  std::cout << "\n╔══════════════════════════════════════════════╗" << std::endl;
  std::cout << "║           FINAL RESULTS                      ║" << std::endl;
  std::cout << "╠══════════════════════════════════════════════╣" << std::endl;
  std::cout << "║  Passed: " << passed << " / " << (passed + failed) << "                               ║" << std::endl;
  std::cout << "║  Failed: " << failed << " / " << (passed + failed) << "                               ║" << std::endl;
  std::cout << "╚══════════════════════════════════════════════╝\n" << std::endl;

  return (failed == 0) ? 0 : 1;
}
