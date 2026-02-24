#pragma once

#include "pbft/messages.hh"
#include "pbft/metrics.hh"

#include "salticidae/network.h"

#include "spdlog/spdlog.h"

#include <future>
#include <memory>
#include <optional>

namespace pbft {

struct ClientTLSConfig {
  std::string cert_file;
  std::string key_file;
};

struct ClientConfig {
  uint32_t num_replicas;
  uint32_t request_timeout_;
  std::optional<ClientTLSConfig> tls_config;
};

class Client {
public:
  Client(uint32_t client_id, const ClientConfig &config);
  ~Client();

  void add_replica(uint32_t id, const salticidae::NetAddr &addr);
  void start(const salticidae::NetAddr &listen_addr);
  void stop();
  // Run should be executed on another thread to be able to use invoke
  void run();

  std::string invoke(const std::string &operation);
  std::future<std::string> invoke_async(const std::string &operation);

#ifndef PBFT_TESTING_ACCESS
private:
#else
public:
#endif

  // Threading protection
  std::mutex mu_;

  // Config
  uint32_t id_;
  uint32_t n_;
  uint32_t f_;
  uint32_t timeout_;

  // Internal state;
  uint32_t current_view_;
  uint64_t timestamp_;
  struct InflightRequest {
    uint64_t timestamp;
    std::promise<std::string> done;
    std::unordered_set<uint32_t> seen_replicas;
    std::unordered_map<std::string, size_t> result_counts;
  };
  std::unordered_map<uint64_t, InflightRequest> inflight_requests_;

  // Network
  salticidae::EventContext ec_;
  std::unique_ptr<salticidae::MsgNetwork<uint8_t>> net_;
  std::unordered_map<uint32_t, salticidae::MsgNetwork<uint8_t>::conn_t>
      replicas_;

  // Protocol Handlers
  void register_handlers();
  void on_reply(ReplyMsg &&m, const salticidae::MsgNetwork<uint8_t>::conn_t &);

  // Metrics
  std::unique_ptr<Metrics> metrics_;
  // Logger
  std::shared_ptr<spdlog::logger> logger_;

  // Helpers
  inline uint32_t primary_hint() { return current_view_ % n_; }
  inline void broadcast_request(const RequestMsg &m) {
    for (const auto &kv : replicas_) {
      auto &conn = kv.second;
      if (conn && !conn->is_terminated()) {
        net_->send_msg(m, conn);
        metrics_->inc_msg("request", m.serialized.size(), true);
      }
    }
  }
};

} // namespace pbft
