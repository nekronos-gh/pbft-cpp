#pragma once

#include "pbft/messages.hh"
#include "pbft/metrics.hh"
#include "pbft/service_interface.hh"

#include "salticidae/network.h"

#include "spdlog/spdlog.h"

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>

namespace pbft {

struct NodeTLSConfig {
  std::string cert_file;
  std::string key_file;
};

struct NodeConfig {
  uint32_t num_replicas;
  uint32_t log_size;
  uint32_t checkpoint_interval;

  double vc_timeout = 2.0;
  std::optional<NodeTLSConfig> tls_config;
};

class Node {
public:
  Node(uint32_t replica_id, const NodeConfig &config,
       std::unique_ptr<ServiceInterface> service);

  void add_replica(uint32_t id, const salticidae::NetAddr &addr);
  void add_client(uint32_t id, const salticidae::NetAddr &addr);
  void start(const salticidae::NetAddr &listen_addr);
  void stop();
  void run();

#ifndef PBFT_TESTING_ACCESS
protected:
#else
public:
#endif

  // Config
  uint32_t id_;
  uint32_t n_;
  uint32_t f_;

  // Network
  salticidae::EventContext ec_;
  std::unique_ptr<salticidae::MsgNetwork<uint8_t>> net_;
  std::unordered_map<uint32_t, salticidae::MsgNetwork<uint8_t>::conn_t> peers_;
  std::unordered_map<salticidae::MsgNetwork<uint8_t>::Conn *, uint32_t>
      conn_to_peer_;
  std::unordered_map<uint32_t, salticidae::MsgNetwork<uint8_t>::conn_t>
      clients_;

  // State (Sane defaults)
  uint32_t view_{0};
  uint64_t seq_num_{0};   // Last assigned seq num
  uint64_t last_exec_{0}; // Last executed request
  uint32_t L_;            // Log window size
  uint32_t K_;            // Checkpoint interval
  uint64_t h_{0};
  uint64_t H_;

  // Logs
  enum class ReqStage { NONE, PRE_PREPARED, PREPARED, COMMITTED };
  struct ReqLogEntry {
    uint32_t view;
    uint64_t seq;
    uint256_t digest;
    RequestMsg req; // The actual operation

    ReqStage stage{ReqStage::NONE};

    std::chrono::steady_clock::time_point t_preprepare_start;
    std::chrono::steady_clock::time_point t_commit_start;

    bool has_preprepare = false;
    std::set<uint32_t> prepares;
    std::set<uint32_t> commits;
  };
  std::map<uint64_t, ReqLogEntry> reqlog_;

  // Client management
  struct ClientReplyInfo {
    uint64_t timestamp;
    uint32_t replica_id;
    std::string result;
  };
  // client_id -> last reply
  std::map<uint32_t, ClientReplyInfo> last_replies_;

  // Protocol Handlers
  void register_handlers();
  void on_handshake(HandshakeMsg &&m,
                    const salticidae::MsgNetwork<uint8_t>::conn_t &);
  void on_request(RequestMsg &&m,
                  const salticidae::MsgNetwork<uint8_t>::conn_t &);
  void on_preprepare(PrePrepareMsg &&m,
                     const salticidae::MsgNetwork<uint8_t>::conn_t &);
  void on_preprepare_impl(PrePrepareMsg &&m,
                          const salticidae::MsgNetwork<uint8_t>::conn_t &,
                          bool from_self = false);
  void on_prepare(PrepareMsg &&m,
                  const salticidae::MsgNetwork<uint8_t>::conn_t &);
  void on_commit(CommitMsg &&m,
                 const salticidae::MsgNetwork<uint8_t>::conn_t &);
  void on_checkpoint(CheckpointMsg &&m,
                     const salticidae::MsgNetwork<uint8_t>::conn_t &);
  void on_viewchange(ViewChangeMsg &&m,
                     const salticidae::MsgNetwork<uint8_t>::conn_t &);
  void on_newview(NewViewMsg &&m,
                  const salticidae::MsgNetwork<uint8_t>::conn_t &);

  // State transitions
  void try_prepare(ReqLogEntry &entry);
  void try_commit(ReqLogEntry &entry);
  void try_execute();
  void do_checkpoint();
  void start_view_change();

  // Liveness
  const double vc_timeout_;
  salticidae::TimerEvent view_change_timer_;
  bool view_changing_{false};
  // (new view) -> (replica_id -> View Change message)
  std::map<uint32_t, std::map<uint32_t, ViewChangeMsg>> view_change_store_;
  uint32_t view_change_timeout_count_{0}; // For exponential backoff

  // Checkpointing
  struct CheckpointVotes {
    // Checkpoint storage: seq -> (digest -> {replicas})
    std::unordered_map<uint256_t, std::set<uint32_t>> votes;
    bool stable = false;
  };
  std::map<uint64_t, CheckpointVotes> checkpoints_;
  uint256_t last_stable_digest_;

  void make_checkpoint();
  void advance_watermarks(uint64_t stable_seq);
  uint64_t recompute_highest_sequence() const;
  void garbage_collect();

  // Timer management
  bool timer_running_;
  void start_timer_if_not_running();
  void stop_timer();
  void manage_timer();
  bool is_waiting_for_request();

  // Helpers
  inline bool is_primary() const { return id_ == (view_ % n_); }

  // Primary in current view
  inline bool comes_from_primary(
      const salticidae::MsgNetwork<uint8_t>::conn_t &conn) const {
    auto it = conn_to_peer_.find(conn.get());
    if (it == conn_to_peer_.end())
      return false;
    if (it->second == primary())
      return true;
    return false;
  }
  // Primary in a specific view
  inline bool
  comes_from_primary(const salticidae::MsgNetwork<uint8_t>::conn_t &conn,
                     uint32_t new_view) const {
    auto it = conn_to_peer_.find(conn.get());
    if (it == conn_to_peer_.end())
      return false;
    if (it->second == new_view % n_)
      return true;
    return false;
  }
  inline uint32_t primary() const { return view_ % n_; }

  template <typename M> inline void broadcast(const M &m) {
    if (metrics_) {
      metrics_->inc_msg(get_msg_type<M>(), m.serialized.size(), true);
    }
    for (const auto &kv : peers_) {
      if (kv.first == id_)
        continue;
      auto &conn = kv.second;
      if (conn && !conn->is_terminated()) {
        net_->send_msg(m, conn);
      }
    }
  }

  template <typename M>
  inline void send_to_replica(uint32_t replica_id, const M &m) {
    if (metrics_) {
      metrics_->inc_msg(get_msg_type<M>(), m.serialized.size(), true);
    }
    auto it = peers_.find(replica_id);
    if (it == peers_.end())
      return;
    auto &conn = it->second;
    if (conn && !conn->is_terminated()) {
      net_->send_msg(m, conn);
    }
  }

  template <typename M>
  inline void send_to_client(uint32_t client_id, const M &m) {
    if (metrics_) {
      metrics_->inc_msg(get_msg_type<M>(), m.serialized.size(), true);
    }
    auto it = clients_.find(client_id);
    if (it == clients_.end())
      return;
    auto &conn = it->second;
    if (conn && !conn->is_terminated()) {
      net_->send_msg(m, conn);
    }
  }

  // Service
  std::unique_ptr<ServiceInterface> service_;
  // Metrics
  std::unique_ptr<Metrics> metrics_;
  // Logger
  std::shared_ptr<spdlog::logger> logger_;
};

} // namespace pbft
