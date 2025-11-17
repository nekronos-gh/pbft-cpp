#pragma once
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

struct CheckpointInfo {
    // digest -> set of replica ids that sent this digest at this seq
    std::unordered_map<std::string, std::unordered_set<uint32_t>> digests;
    bool stable = false;
};

class Node {
public:
  Node(uint32_t replica_id, uint32_t num_replicas,
       std::unique_ptr<ServiceInterface> service);

  void add_replica(uint32_t id, const salticidae::NetAddr &addr);
  void start(const salticidae::NetAddr &listen_addr);
  void run();

private:
  // Config
  uint32_t id_;
  uint32_t n_{0};
  uint32_t f_{0};

  // State (Sane defaults)
  uint32_t view_{0};
  uint64_t next_seq_{1};
  uint64_t last_exec_{0};
  uint64_t window_size_{200};
  uint64_t low_{0};
  uint64_t high_{low_ + window_size_};
  const uint64_t checkpoint_interval_{100};
  uint64_t last_stable_checkpoint_seq_{0};
  std::string last_stable_checkpoint_digest_;

  // Service
  std::unique_ptr<ServiceInterface> service_;

  // Metrics
  std::unique_ptr<Metrics> metrics_;

  // Network
  salticidae::EventContext ec_;
  std::unique_ptr<salticidae::MsgNetwork<uint8_t>> net_;
  std::unordered_map<uint32_t, salticidae::NetAddr> peers_;
  std::unordered_map<uint32_t, salticidae::NetAddr> clients_;

  enum class ReqStage { PRE_PREPARE, PREPARE, COMMIT };

  // Logs
  struct ReqInfo {
    std::string op;
    uint64_t ts{0};
    uint32_t client{0};
    std::string digest;

    ReqStage stage{ReqStage::PRE_PREPARE};

    std::set<uint32_t> prepares;
    std::set<uint32_t> commits;
    std::string result;

    std::optional<std::chrono::steady_clock::time_point> t_phase;
  };
  std::unordered_map<uint64_t, ReqInfo> reqlog_;

  // Handlers
  void register_handlers();

  // Helpers
  bool is_primary() const { return id_ == (view_ % n_); }
  uint32_t primary() const { return view_ % n_; }

  // Protocol
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

  void try_execute();

  // For collecting checkpoint messages
  std::map<uint64_t, CheckpointInfo> checkpoints_;
  void make_checkpoint();
  void advance_watermarks(uint64_t stable_seq);
  void garbage_collect(uint64_t stable_seq);

  // IO
  template <typename M> void broadcast(const M &m);
  void forward_to_primary(const RequestMsg &m);
  void forward_to_client(const ReplyMsg &m);
};

} // namespace pbft
