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

void Node::remove_replica(uint32_t id) {
  if (peers_.find(id) == peers_.end())
    return; // If it does not exists, skip
  peers_.erase(id);
  n_--;
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
}

void Node::on_request(RequestMsg &&m, const MsgNetwork<uint8_t>::conn_t &) {
  metrics_->inc_msg("request");
  // Requests are only processed by primary
  if (is_primary()) {
    uint64_t seq = next_seq_++;
    auto &info = reqlog_[seq]; // Create log entry
    // Begin PRE_PREPARE phase
    // TODO: generate digest
    std::string digest = "Digest";
    info.op = m.operation;
    info.ts = m.timestamp;
    info.client = m.client_id;
    info.digest = digest;
    info.t_phase = std::chrono::steady_clock::now();

    // Broadcast pre_prepare message to replicas
    broadcast(PrePrepareMsg(view_, seq, digest, m));

    // Only primary measure pre-prepare
    auto now = std::chrono::steady_clock::now();
    auto sec = std::chrono::duration<double>(now - *info.t_phase).count();
    metrics_->observe_phase("preprepare", sec);
    info.t_phase = now;
  } else {
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

  // TODO: check_digest

  // Check if received duplicated
  auto it = reqlog_.find(m.seq_num);
  if (it == reqlog_.end())
    return; // If not in log, drop the message
  auto &info = it->second;
  if (info.stage == ReqStage::PRE_PREPARE)
    return;

  // Begin PRE_PREPARE phase
  info.op = m.operation;
  info.ts = m.timestamp;
  info.client = m.client_id;
  info.digest = m.req_digest;
  info.t_phase = std::chrono::steady_clock::now();

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

  // TODO: Check digest

  // Prepared is only true if:
  // - The request is in the log
  // - The request is PRE_PREPARE
  // - 2f replicas match the pre_prepare
  auto it = reqlog_.find(m.seq_num);
  if (it == reqlog_.end())
    return; // If not in log, drop the message
  auto &info = it->second;
  if (info.stage != ReqStage::PRE_PREPARE)
    return;

  // Count all received prepare from replicas and check
  info.prepares.insert(m.replica_id);
  if (info.prepares.size() >= 2 * f_) {
    // Begin PREPARE Phase
    info.stage = ReqStage::PREPARE;
    auto now = std::chrono::steady_clock::now();
    auto sec = std::chrono::duration<double>(now - *info.t_phase).count();
    metrics_->observe_phase("prepare", sec);
    info.t_phase = now;
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

  // TODO: Check digest

  // Commited-local is only true if:
  // - 2f + 1 commits are accepted
  auto it = reqlog_.find(m.seq_num);
  if (it == reqlog_.end())
    return; // If not in log, drop the message
  auto &info = it->second;
  if (info.stage != ReqStage::PREPARE)
    return;

  info.commits.insert(m.replica_id);
  if (info.commits.size() >= 2 * f_ + 1) {
    // Begin commit phase
    info.stage = ReqStage::COMMIT;
    auto now = std::chrono::steady_clock::now();
    auto sec = std::chrono::duration<double>(now - *info.t_phase).count();
    metrics_->observe_phase("commit", sec);
    info.t_phase = now;
    try_execute();
  }
}

void Node::try_execute() {
  // Execute in order assigned by leader
  while (reqlog_.count(last_exec_ + 1)) {
    auto &info = reqlog_[last_exec_ + 1];
    if (info.stage != ReqStage::COMMIT)
      break; // Ignore uncommited transactions

    metrics_->set_inflight(reqlog_.size());
    // Execute operation
    auto result = service_->execute(info.op);
    // Send reply to client
    forward_to_client(ReplyMsg(view_, info.ts, info.client, id_, result));
    metrics_->inc_msg("reply");

    last_exec_++;
    if (last_exec_ % checkpoint_interval_ == 0)
      take_checkpoint();
  }
}

void Node::take_checkpoint() {
  auto digest = service_->get_checkpoint_digest();
  CheckpointMsg cp(last_exec_, digest, id_);
  broadcast(cp);
}

void Node::on_checkpoint(CheckpointMsg &&,
                         const MsgNetwork<uint8_t>::conn_t &) {
  // TODO: collect 2f+1 matching digests and advance watermarks.
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
