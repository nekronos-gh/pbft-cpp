#pragma once

#include <future>
#include <memory>
#include <thread>
#include <vector>

#include "service.hh"

#define PBFT_TESTING_ACCESS public
#include "byzantine_node.hh"
#include "pbft/client.hh"
#include "pbft/node.hh"

namespace pbft::testing {

using namespace salticidae;

struct ReplicaHandle {
  uint32_t id;
  NetAddr addr;
  bool crashed = false;
  bool byzantine = false;
  FaultMode fault_mode = FaultMode::NONE;

  CounterService *service = nullptr;
  std::unique_ptr<Node> node;
  std::thread thread;

  ReplicaHandle() = default;

  // Non-copyable, movable
  ReplicaHandle(const ReplicaHandle &) = delete;
  ReplicaHandle &operator=(const ReplicaHandle &) = delete;
  ReplicaHandle(ReplicaHandle &&) = default;
  ReplicaHandle &operator=(ReplicaHandle &&) = default;
};

// Orchestrates a local PBFT cluster for integration tests

class Cluster {
public:
  explicit Cluster(uint32_t n,
                   std::initializer_list<ByzantineNodeConfig> faults = {})
      : n_(n), f_((n - 1) / 3), client_id_(0), client_addr_("127.0.0.1:11000"),
        node_config_{n, 200, 100, 2.0, std::nullopt},
        client_config_{n, 2000, std::nullopt}, pending_faults_(faults) {}

  ~Cluster() { stop(); }

  // Non-copyable, non-movable
  Cluster(const Cluster &) = delete;
  Cluster &operator=(const Cluster &) = delete;

  void start() {
    create_replicas();
    create_client();
    connect_all();
  }

  void stop() {
    stop_client();
    stop_replicas();
  }

  // Configuration (must be called before start())
  void set_node_config(uint32_t log_size, uint32_t checkpoint_interval,
                       double vc_timeout = 2.0) {
    node_config_ = {n_, log_size, checkpoint_interval, vc_timeout, {}};
  }

  std::string send_request(const std::string &op) {
    return client_->invoke(op);
  }
  std::future<std::string> send_request_async(const std::string &op) {
    return client_->invoke_async(op);
  }

  void crash_replica(uint32_t id) {
    if (id >= replicas.size())
      return;

    auto &rep = replicas[id];
    rep.crashed = true;
    rep.node->stop();
    if (rep.thread.joinable())
      rep.thread.join();
  }

  uint32_t current_view() const { return client_->current_view_; }

  // Returns true when more than 2f replicas agree on the given counter value
  bool verify_state(int expected_counter) const {
    uint32_t matching = 0;
    for (const auto &rep : replicas) {
      if (rep.service->get_counter() == expected_counter)
        ++matching;
    }
    return matching > 2 * f_;
  }

  // Public access to replica handles
  std::vector<ReplicaHandle> replicas;

private:
  const uint32_t n_;
  const uint32_t f_;
  const uint32_t client_id_;
  const NetAddr client_addr_;

  NodeConfig node_config_;
  ClientConfig client_config_;

  std::vector<ByzantineNodeConfig> pending_faults_; // applied during start()

  std::unique_ptr<Client> client_;
  std::thread client_thread_;
  NetAddr replica_addr(uint32_t id) const {
    return NetAddr("127.0.0.1:" + std::to_string(10000 + id));
  }

  void create_replicas() {
    replicas.reserve(n_);

    // Collect which IDs are byzantine so we can annotate handles
    std::map<uint32_t, FaultMode> fault_map;
    for (auto &[id, mode] : pending_faults_)
      fault_map[id] = mode;

    for (uint32_t i = 0; i < n_; ++i) {
      ReplicaHandle rep;
      rep.id = i;

      auto service = std::make_unique<CounterService>();
      service->initialize();
      rep.service = service.get(); // borrow pointer before moving

      if (auto it = fault_map.find(i); it != fault_map.end()) {
        rep.byzantine = true;
        rep.fault_mode = it->second;
        rep.node = std::make_unique<ByzantineNode>(
            i, node_config_, rep.fault_mode, std::move(service));
      } else {
        rep.node = std::make_unique<Node>(i, node_config_, std::move(service));
      }

      replicas.push_back(std::move(rep));
    }

    for (uint32_t i = 0; i < n_; ++i) {
      auto addr = replica_addr(i);
      replicas[i].thread = std::thread([this, i, addr] {
        replicas[i].addr = addr;
        replicas[i].node->start(addr);
        replicas[i].node->run();
      });
    }
  }

  void create_client() {
    client_ = std::make_unique<Client>(client_id_, client_config_);
    client_->start(client_addr_);
    client_thread_ = std::thread([this] { client_->run(); });
  }

  void connect_all() {
    for (uint32_t i = 0; i < n_; ++i) {
      for (uint32_t j = 0; j < n_; ++j) {
        if (i != j)
          replicas[i].node->add_replica(j, replica_addr(j));
      }
      client_->add_replica(i, replica_addr(i));
      replicas[i].node->add_client(client_id_, client_addr_);
    }
  }

  void stop_client() {
    if (!client_)
      return;
    client_->stop();
    if (client_thread_.joinable())
      client_thread_.join();
    client_.reset();
  }

  void stop_replicas() {
    for (auto &rep : replicas) {
      if (!rep.crashed && rep.node) {
        rep.node->stop();
        if (rep.thread.joinable())
          rep.thread.join();
      }
    }
    replicas.clear();
  }
};

} // namespace pbft::testing
