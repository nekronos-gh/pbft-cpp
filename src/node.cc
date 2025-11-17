#include "pbft/node.hh"
#include "pbft/metrics.hh"

#include "salticidae/msg.h"
#include "salticidae/event.h"
#include "salticidae/network.h"
#include "salticidae/stream.h"
#include "salticidae/conn.h"

using salticidae::DataStream;
using salticidae::MsgNetwork;
using salticidae::NetAddr;
using salticidae::ConnPool;

namespace pbft {

Node::Node(uint32_t replica_id, uint32_t num_replicas,
           std::unique_ptr<ServiceInterface> service)
    : id_(replica_id), service_(std::move(service)),
      net_(new MsgNetwork<uint8_t>(ec_, MsgNetwork<uint8_t>::Config())) {
  service_->initialize();
  metrics_ = std::make_unique<Metrics>("0.0.0.0:9460"); // TODO: Make configurable
  metrics_->set_view(view_);
  register_handlers();
}

void Node::add_replica(uint32_t id, const NetAddr &addr) {
  if (peers_.find(id) != peers_.end())
    return; // If already exists, skip
  peers_[id] = addr;
  n_++;
  f_ = (n_ - 1) / 3;
}

void Node::start(const NetAddr &listen_addr) {
  net_->start();
  net_->listen(listen_addr);
}

void Node::run() { ec_.dispatch(); }

void Node::register_handlers() {
  net_->template reg_handler([this](RequestMsg &&m, const MsgNetwork<uint8_t>::conn_t &c) {
    on_request(std::move(m), c);
  });
  net_->template reg_handler([this](PrePrepareMsg &&m, const MsgNetwork<uint8_t>::conn_t &c) {
    on_preprepare(std::move(m), c);
  });
  net_->template reg_handler([this](PrepareMsg &&m, const MsgNetwork<uint8_t>::conn_t &c) {
    on_prepare(std::move(m), c);
  });
  net_->template reg_handler([this](CommitMsg &&m, const MsgNetwork<uint8_t>::conn_t &c) {
    on_commit(std::move(m), c);
  });
  net_->template reg_handler([this](CheckpointMsg &&m, const MsgNetwork<uint8_t>::conn_t &c) {
    on_checkpoint(std::move(m), c);
  });
  net_->template reg_handler([this](ViewChangeMsg &&m, const MsgNetwork<uint8_t>::conn_t &c) {
    on_viewchange(std::move(m), c);
  });
  net_->template reg_handler([this](NewViewMsg &&m, const MsgNetwork<uint8_t>::conn_t &c) {
    on_newview(std::move(m), c);
  });
}

void Node::on_request(RequestMsg &&m, const MsgNetwork<uint8_t>::conn_t &) {
  metrics_->inc_msg("request");
  // Requests are only processed by primary
  if (is_primary()) {
    uint64_t seq = next_seq_++;
    auto &info = reqlog_[seq]; // Create log entry
    // Begin PRE_PREPARE phase
    std::string digest = m.digest();
    info.op = m.operation;
    info.ts = m.timestamp;
    info.client = m.client_id;
    info.digest = digest;
    info.t_phase = std::chrono::steady_clock::now();
    info.stage = ReqStage::PRE_PREPARE;

    // Broadcast pre_prepare message to replicas
    // Set the sequence number
    broadcast(PrePrepareMsg(view_, seq, digest, m));

    // Only primary measure pre-prepare
    auto now = std::chrono::steady_clock::now();
    auto sec = std::chrono::duration<double>(now - *info.t_phase).count();
    metrics_->observe_phase("preprepare", sec);
    info.t_phase = now;
  } else {
    // Otherwise, forward to primary
    forward_to_primary(m);
  }
}

void Node::on_preprepare(PrePrepareMsg &&m,
                         const MsgNetwork<uint8_t>::conn_t &) {
  metrics_->inc_msg("preprepare");
  // Discard message if not from the same view
  // and not within sequence window
  if (m.view != view_)
    return;
  if (m.seq_num < low_ || m.seq_num > high_)
    return;

  auto it = reqlog_.find(m.seq_num);
  // If already in log, drop the message
  if (it != reqlog_.end())
    return; 
  auto &info = reqlog_[m.seq_num]; // Create log entry
  // Discard if the digest does not match
  if (!info.digest.empty() && info.digest != m.req_digest)
    return; // conflicting digest for same (view, seq), drop

  // Begin PRE_PREPARE phase
  info.op = m.operation;
  info.ts = m.timestamp;
  info.client = m.client_id;
  info.digest = m.req_digest;
  info.t_phase = std::chrono::steady_clock::now();
  info.stage = ReqStage::PRE_PREPARE;

  // Set the node itself as prepared in the set
  info.prepares.insert(id_);
  // Broadcast prepare to all replicas
  broadcast(PrepareMsg(view_, m.seq_num, m.req_digest, id_));
}

void Node::on_prepare(PrepareMsg &&m, const MsgNetwork<uint8_t>::conn_t &) {
  metrics_->inc_msg("prepare");
  // Discard message if not from the same view
  // and not within sequence window
  if (m.view != view_)
    return;
  if (m.seq_num < low_ || m.seq_num > high_)
    return;

  // Prepared is only true if:
  // - The request is in the log
  // - The request is PRE_PREPARE
  // - 2f replicas match the pre_prepare
  auto it = reqlog_.find(m.seq_num);
  if (it == reqlog_.end())
    return; // If not in log, drop the message
  auto &info = it->second;
  // Message must be pre_prepared
  if (info.stage != ReqStage::PRE_PREPARE)
    return;
  // Check that prepare digest matches our stored digest for this (view, seq)
  if (info.digest != m.req_digest)
    return;

  // Count all received prepare from replicas and check
  info.prepares.insert(m.replica_id);
  if (info.prepares.size() >= 2 * f_) {
    // Begin PREPARE Phase (Quorum reached)
    info.stage = ReqStage::PREPARE;
    auto now = std::chrono::steady_clock::now();
    auto sec = std::chrono::duration<double>(now - *info.t_phase).count();
    metrics_->observe_phase("prepare", sec);
    info.t_phase = now;
    // Set the node itself as commited in the set
    info.commits.insert(id_);
    broadcast(CommitMsg(view_, m.seq_num, info.digest, id_));
  }
}

void Node::on_commit(CommitMsg &&m, const MsgNetwork<uint8_t>::conn_t &) {
  metrics_->inc_msg("commit");
  // Discard message if not from the same view
  // and not within sequence window
  if (m.view != view_)
    return;
  if (m.seq_num < low_ || m.seq_num > high_)
    return;

  // Commited-local is only true if:
  // - 2f + 1 commits are accepted
  auto it = reqlog_.find(m.seq_num);
  if (it == reqlog_.end())
    return; // If not in log, drop the message
  auto &info = it->second;
  if (info.stage != ReqStage::PREPARE)
    return;
    // Check that commit digest matches our stored digest
  if (info.digest != m.req_digest)
    return;

  info.commits.insert(m.replica_id);
  if (info.commits.size() >= 2 * f_ + 1) {
    // Begin commit phase
    info.stage = ReqStage::COMMIT;
    auto now = std::chrono::steady_clock::now();
    auto sec = std::chrono::duration<double>(now - *info.t_phase).count();
    metrics_->observe_phase("commit", sec);
    info.t_phase = now;
    // Commited-local 
    try_execute();
  }
}

void Node::on_checkpoint(CheckpointMsg &&m,
                         const MsgNetwork<uint8_t>::conn_t &) {
  // Discard if not in current window
  if (m.seq_num < low_ || m.seq_num > high_)
    return;

  // only treat multiples as checkpoints
  if (m.seq_num % checkpoint_interval_ != 0)
    return; 

  // Record the digest and the replica that sent it
  auto &cp_info = checkpoints_[m.seq_num];
  auto &rep_set = cp_info.digests[m.state_digest];
  rep_set.insert(m.replica_id);

  // If already stable, we can safely ignore
  if (cp_info.stable)
    return;

  // Check if we reached 2f + 1 matching digests and sequence number
  // This is the proof of correctness
  if (rep_set.size() >= 2 * f_ + 1) {
    cp_info.stable = true;

    // Update last stable checkpoint
    last_stable_checkpoint_seq_ = m.seq_num;
    last_stable_checkpoint_digest_ = m.state_digest;

    // Advance low/high watermarks and garbage-collect logs
    advance_watermarks(last_stable_checkpoint_seq_);
    garbage_collect(last_stable_checkpoint_seq_);
  }
}

void Node::try_execute() {
  // Execute in order assigned by leader
  while (reqlog_.count(last_exec_ + 1)) {
    auto &info = reqlog_[last_exec_ + 1];
    // Never execute unless all lower sequence numbers are executed
    if (info.stage != ReqStage::COMMIT)
      break; // Ignore uncommited transactions and skip

    metrics_->set_inflight(reqlog_.size());
    // Execute operation
    auto result = service_->execute(info.op);
    // Send reply to client
    forward_to_client(ReplyMsg(view_, info.ts, info.client, id_, result));
    metrics_->inc_msg("reply");

    last_exec_++;
    if (last_exec_ % checkpoint_interval_ == 0) {
      make_checkpoint();
    }
  }
}

void Node::make_checkpoint() {
  auto digest = service_->get_checkpoint_digest();
  last_stable_checkpoint_digest_ = digest;
  // Include its own checkpoint
  auto &cp_info = checkpoints_[last_exec_];
  auto &rep_set = cp_info.digests[digest];
  rep_set.insert(id_);
  CheckpointMsg cp(last_exec_, digest, id_);
  broadcast(cp);
}

void Node::advance_watermarks(uint64_t stable_seq) {
  low_ = stable_seq;
  high_ = low_ + window_size_;
  metrics_->set_watermarks(low_, high_);
}

void Node::garbage_collect(uint64_t stable_seq) {
  // Drop all pre-prepare/prepare/commit log entries <= stable_seq
  for (auto it = reqlog_.begin(); it != reqlog_.end();) {
    if (it->first <= stable_seq)
      it = reqlog_.erase(it);
    else
      ++it;
  }

  // Drop all checkpoint tracking strictly older than the stable one
  for (auto it = checkpoints_.begin(); it != checkpoints_.end();) {
    if (it->first < stable_seq)
      it = checkpoints_.erase(it);
    else
      ++it;
  }
}

void Node::on_viewchange(ViewChangeMsg &&,
                         const MsgNetwork<uint8_t>::conn_t &) {
  // TODO: implement
}

void Node::on_newview(NewViewMsg &&, const MsgNetwork<uint8_t>::conn_t &) {
  // TODO: implement
}

template <typename M> void Node::broadcast(const M &m) {
  for (const auto &kv : peers_) {
    if (kv.first == id_)
      continue;
    net_->send_msg(m, net_->connect_sync(kv.second));
  }
}

void Node::forward_to_primary(const RequestMsg &m) {
  auto it = peers_.find(primary());
  if (it == peers_.end())
    return;
  net_->send_msg(m, net_->connect_sync(it->second));
}

void Node::forward_to_client(const ReplyMsg &m) {
  auto it = clients_.find(m.client_id);
  if (it == clients_.end())
    return;
  net_->send_msg(m, net_->connect_sync(it->second));
}

} // namespace pbft
