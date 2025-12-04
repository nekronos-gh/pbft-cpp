#pragma once
#ifndef PBFT_TESTING_ACCESS
    #define PBFT_TESTING_ACCESS private
#endif
#include "pbft/messages.hh"
#include "pbft/metrics.hh"
#include "pbft/service_interface.hh"
#include "salticidae/crypto.h"
#include "salticidae/event.h"
#include "salticidae/network.h"

#include <chrono>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>

namespace pbft {

class Node {
public:
  Node(uint32_t replica_id, uint32_t num_replicas,
       std::unique_ptr<ServiceInterface> service);

  void add_replica(uint32_t id, const salticidae::NetAddr &addr);
  void add_client(uint32_t id, const salticidae::NetAddr &addr);
  void start(const salticidae::NetAddr &listen_addr);
  void stop();
  void run();

PBFT_TESTING_ACCESS:
  // Config
  uint32_t id_;
  uint32_t n_{0};
  uint32_t f_{0};

  // Network
  salticidae::EventContext ec_;
  std::unique_ptr<salticidae::MsgNetwork<uint8_t>> net_;
  std::unordered_map<uint32_t, salticidae::NetAddr> peers_;
  std::unordered_map<uint32_t, salticidae::NetAddr> clients_;

  // State (Sane defaults)
  uint32_t view_{0};
  uint64_t seq_num_{0};          // Last assigned seq num
  uint64_t last_exec_{0};        // Last executed request
  static const uint32_t L = 200; // Log window size
  static const uint32_t K = 100; // Checkpoint interval
  uint64_t h_{0};
  uint64_t H_{L};

  // Logs
  enum class ReqStage { NONE, PRE_PREPARED, PREPARED, COMMITTED };
  struct ReqLogEntry {
    uint32_t view;
    uint64_t seq;
    uint256_t digest;
    RequestMsg req; // The actual operation

    ReqStage stage{ReqStage::NONE};

    bool has_preprepare = false;
    std::set<uint32_t> prepares;
    std::set<uint32_t> commits;
  };
  std::unordered_map<uint64_t, ReqLogEntry> reqlog_;

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
  void on_request(RequestMsg &&m,
                  const salticidae::MsgNetwork<uint8_t>::conn_t &);
  void on_preprepare(PrePrepareMsg &&m,
                     const salticidae::MsgNetwork<uint8_t>::conn_t &);
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
  constexpr static const double vc_timeout_{2.0};
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
  void garbage_collect();

  // Timer management
  bool timer_running_;
  void start_timer_if_not_running();
  void stop_timer();
  void manage_timer();
  bool is_waiting_for_request();

  // Helpers
  bool is_primary() const { return id_ == (view_ % n_); }
  uint32_t primary() const { return view_ % n_; }
  template <typename M> void broadcast(const M &m);
  template <typename M> void send_to_replica(uint32_t replica_id, const M& msg);
  template <typename M> void send_to_client(uint32_t client_id, const M& msg);

  // Service
  std::unique_ptr<ServiceInterface> service_;
  // Metrics
  std::unique_ptr<Metrics> metrics_;
};

} // namespace pbft
