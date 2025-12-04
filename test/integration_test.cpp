#include <iostream>
#include <vector>
#include <memory>
#include <thread>
#include <chrono>
#include <map>

#include "pbft/node.hh"
#include "pbft/messages.hh"
#include "pbft/service_interface.hh"

using namespace pbft;
using namespace salticidae;


// ============================================================================
// Service Setup
// ============================================================================
// Simple state machine to verify execution
class TestService : public ServiceInterface {
private:
  std::vector<std::string> executed_ops_;
  int counter_;
  uint256_t state_digest_;

public:
  TestService() : counter_(0) {}

  void initialize() override {
    executed_ops_.clear();
    counter_ = 0;
    update_digest();
  }

  std::string execute(const std::string& operation) override {
    executed_ops_.push_back(operation);

    if (operation == "INC") {
      counter_++;
    } else if (operation == "DEC") {
      counter_--;
    } else if (operation.substr(0, 4) == "ADD:") {
      int value = std::stoi(operation.substr(4));
      counter_ += value;
    } else if (operation.substr(0, 4) == "SET:") {
      int value = std::stoi(operation.substr(4));
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
    bool byzantine = false;
  };

  std::vector<ReplicaInfo> replicas_;
  uint32_t n_;
  uint32_t f_;

  // Client network
  EventContext client_ec_;
  std::thread client_thread_; // New thread for client EC
  std::unique_ptr<MsgNetwork<uint8_t>> client_net_;
  uint32_t client_id_;
  NetAddr client_addr_;
  uint64_t request_counter_;

public:
  TestCluster(uint32_t n) : n_(n), f_((n - 1) / 3), client_id_(100), request_counter_(0) {
    client_addr_ = NetAddr("127.0.0.1:11000");
  }

  void start() {
    std::cout << "[Cluster] Starting..." << std::endl;
    // Initialize all replicas with their respective services
    replicas_.reserve(n_);
    for (uint32_t i = 0; i < n_; ++i) {
      ReplicaInfo info;
      info.id = i;

      auto service = std::make_unique<TestService>();
      service->initialize();
      info.service = service.get();
      info.node = std::make_unique<Node>(i, n_, std::move(service));

      replicas_.push_back(std::move(info));
    }

    // Configure peers
    for (uint32_t i = 0; i < n_; ++i) {
      for (uint32_t j = 0; j < n_; ++j) {
        if (i != j) {
          NetAddr peer("127.0.0.1:" + std::to_string(10000 + j));
          replicas_[i].node->add_replica(j, peer);
        }
      }
      replicas_[i].node->add_client(client_id_, client_addr_);
    }

    // Start Node Threads
    for (uint32_t i = 0; i < n_; ++i) {
      replicas_[i].thread = std::thread([this, i]() {
        NetAddr listen_addr("127.0.0.1:" + std::to_string(10000 + i));
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

    std::cout << "[Cluster] Started" << std::endl;
  }

  void stop() {
    std::cout << "[Cluster] Stopping..." << std::endl;
    // Force stopping as 
    // Stop client
    client_ec_.stop();
    if (client_thread_.joinable()) {
        pthread_cancel(client_thread_.native_handle());
        client_thread_.join();
    }

    // Stop replicas
    for (auto& rep : replicas_) {
        rep.node->stop();
        if (rep.thread.joinable()) {
            pthread_cancel(rep.thread.native_handle());
            rep.thread.join();
        }
    }

    if (client_net_) client_net_.reset();
    
    replicas_.clear();
    std::cout << "[Cluster] Stopped\n" << std::endl;
  }

  void send_request(const std::string& operation) {
    RequestMsg req(operation, request_counter_++, client_id_);

    std::cout << "[Client] Sending: " << operation << " to cluster" << std::endl;

    salticidae::NetAddr access_node("127.0.0.1:10000");

    auto conn = client_net_->connect_sync(access_node);

    if (conn) {
      client_net_->send_msg(req, conn);
    } else {
      std::cerr << "[Client] Failed to connect cluster" << std::endl;
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
    std::cout << "[Cluster] Replica " << id << " crashed"<< std::endl;
  }

  void mark_byzantine(uint32_t id) {
    if (id >= replicas_.size()) return;
    std::cout << "[Cluster] Marking replica " << id << " as Byzantine" << std::endl;
    replicas_[id].byzantine = true;
  }

  // Wait untill a number of executions have been observed in the service for each node
  void wait_for_exec(size_t expected_ops, uint32_t timeout_ms = 5000) {
    auto start = std::chrono::steady_clock::now();

    while (true) {
      bool all_reached = true;
      int correct_count = 0;

      for (const auto& rep : replicas_) {
        if (rep.crashed || rep.byzantine) {
          continue;
        }

        correct_count++;
        size_t ops = rep.service->get_op_count();

        if (ops < expected_ops) {
          all_reached = false;
          break;
        }
      }

      if (correct_count > 0 && all_reached) {
        return;
      }

      // Check timeout
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

      if (elapsed > timeout_ms) {
        std::cout << "[Wait] Timeout waiting for " << expected_ops << " operations" << std::endl;
        return;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  // Check that all services are in the same state
  bool verify_consensus() {
    std::cout << "[Verify] Checking consensus ..." << std::endl;
    std::map<int, int> counter_votes;
    std::map<size_t, int> op_votes;

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

    int max_votes = 0;
    int agreed_counter = 0;
    for (const auto& [counter, votes] : counter_votes) {
      if (votes > max_votes) {
        max_votes = votes;
        agreed_counter = counter;
      }
    }

    int max_op_votes = 0;
    size_t agreed_ops = 0;
    for (const auto& [ops, votes] : op_votes) {
      if (votes > max_op_votes) {
        max_op_votes = votes;
        agreed_ops = ops;
      }
    }

    bool consensus = (max_votes == correct_count) && (max_op_votes == correct_count);

    if (consensus) {
      std::cout << "[Verify] CONSENSUS: All correct replicas agree (" << agreed_counter << ", " << agreed_ops << ")" << std::endl;
    } else {
      std::cout << "[Verify] NO CONSENSUS: Replicas disagree" << std::endl;
    }

    return consensus;
  }
};

// ============================================================================
// Tests
// ============================================================================

bool test_normal_operation() {
  std::cout << "╔══════════════════════════════════════════════╗" << std::endl;
  std::cout << "║  TEST: Normal Operation                      ║" << std::endl;
  std::cout << "╚══════════════════════════════════════════════╝" << std::endl;

  TestCluster cluster(4);
  cluster.start();

  cluster.send_request("INC");
  cluster.send_request("INC");
  cluster.send_request("ADD:5");

  cluster.wait_for_exec(3);
  bool passed = cluster.verify_consensus();
  cluster.stop();

  return passed;
}

bool test_f_crashed() {
  std::cout << "╔══════════════════════════════════════════════╗" << std::endl;
  std::cout << "║  TEST: f=1 Crashed Node                      ║" << std::endl;
  std::cout << "╚══════════════════════════════════════════════╝" << std::endl;

  TestCluster cluster(4);
  cluster.start();

  cluster.crash_replica(3);

  cluster.send_request("SET:10");
  cluster.send_request("INC");

  cluster.wait_for_exec(2);
  bool passed = cluster.verify_consensus();
  cluster.stop();

  return passed;
}

bool test_byzantine() {
  std::cout << "╔══════════════════════════════════════════════╗" << std::endl;
  std::cout << "║  TEST: f=1 Byzantine Node                    ║" << std::endl;
  std::cout << "╚══════════════════════════════════════════════╝" << std::endl;

  TestCluster cluster(4);
  cluster.start();

  cluster.mark_byzantine(2);

  cluster.send_request("SET:100");
  cluster.send_request("ADD:50");

  cluster.wait_for_exec(2);
  bool passed = cluster.verify_consensus();
  cluster.stop();

  return passed;
}

#include <iostream>
#include <filesystem>
#include <string>

bool test_view_change() {
  std::cout << "╔══════════════════════════════════════════════╗" << std::endl;
  std::cout << "║  TEST: View Change (Primary Failure)         ║" << std::endl;
  std::cout << "╚══════════════════════════════════════════════╝" << std::endl;

  TestCluster cluster(4);
  cluster.start();

  cluster.send_request("SET:42");

  cluster.crash_replica(0);

  cluster.send_request("INC");

  cluster.wait_for_exec(2);
  bool passed = cluster.verify_consensus();
  cluster.stop();

  return passed;
}

bool test_sequential() {
  std::cout << "╔══════════════════════════════════════════════╗" << std::endl;
  std::cout << "║  TEST: Sequential Operations                 ║" << std::endl;
  std::cout << "╚══════════════════════════════════════════════╝" << std::endl;

  TestCluster cluster(4);
  cluster.start();

  // Fix CONNECT SYNC, and create one connection per client, not many
  for (int i = 0; i < 10; ++i) {
    cluster.send_request("INC");
  }

  cluster.wait_for_exec(10);
  bool passed = cluster.verify_consensus();
  cluster.stop();

  return passed;
}

bool test_mixed() {
  std::cout << "╔══════════════════════════════════════════════╗" << std::endl;
  std::cout << "║  TEST: Mixed Operations                      ║" << std::endl;
  std::cout << "╚══════════════════════════════════════════════╝" << std::endl;

  TestCluster cluster(4);
  cluster.start();

  cluster.send_request("SET:0");
  cluster.send_request("INC");
  cluster.send_request("INC");
  cluster.send_request("ADD:10");
  cluster.send_request("DEC");

  cluster.wait_for_exec(5);
  bool passed = cluster.verify_consensus();
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
    {"f Crashed Nodes", test_f_crashed},
    {"Byzantine Nodes", test_byzantine},
    {"View Change", test_view_change},
    {"Sequential Operations", test_sequential},
    {"Mixed Operations", test_mixed}
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

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }

  std::cout << "\n╔══════════════════════════════════════════════╗" << std::endl;
  std::cout << "║           FINAL RESULTS                      ║" << std::endl;
  std::cout << "╠══════════════════════════════════════════════╣" << std::endl;
  std::cout << "║  Passed: " << passed << " / " << (passed + failed) << "                               ║" << std::endl;
  std::cout << "║  Failed: " << failed << " / " << (passed + failed) << "                               ║" << std::endl;
  std::cout << "╚══════════════════════════════════════════════╝\n" << std::endl;

  return (failed == 0) ? 0 : 1;
}
