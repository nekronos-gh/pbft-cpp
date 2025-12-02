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
using salticidae::_1;
using salticidae::_2;

namespace pbft {

Node::Node(uint32_t replica_id, uint32_t num_replicas,
           std::unique_ptr<ServiceInterface> service)
  : id_(replica_id), n_(num_replicas), service_(std::move(service)),
  net_(new MsgNetwork<uint8_t>(ec_, MsgNetwork<uint8_t>::Config())),
  view_change_timer_(ec_, [this](salticidae::TimerEvent &) { start_view_change(); }) {

  f_ = (n_ - 1) / 3;
  service_->initialize();
  metrics_ = std::make_unique<Metrics>("0.0.0.0:9460"); // TODO: Make configurable
  metrics_->set_view(view_);
  register_handlers();
}

void Node::add_replica(uint32_t id, const NetAddr &addr) {
  peers_[id] = addr;
}

void Node::add_client(uint32_t id, const NetAddr &addr) {
  clients_[id] = addr;
}

void Node::start(const NetAddr &listen_addr) {
  net_->start();
  net_->listen(listen_addr);
  reset_timer();
}

void Node::run() { ec_.dispatch(); }

void Node::register_handlers() {
  net_->reg_handler(salticidae::generic_bind(&Node::on_request, this, _1, _2));
  net_->reg_handler(salticidae::generic_bind(&Node::on_preprepare, this, _1, _2));
  net_->reg_handler(salticidae::generic_bind(&Node::on_prepare, this, _1, _2));
  net_->reg_handler(salticidae::generic_bind(&Node::on_commit, this, _1, _2));
  net_->reg_handler(salticidae::generic_bind(&Node::on_checkpoint, this, _1, _2));
  net_->reg_handler(salticidae::generic_bind(&Node::on_viewchange, this, _1, _2));
  net_->reg_handler(salticidae::generic_bind(&Node::on_newview, this, _1, _2));
}

// --------------------------------------------------------------------------
// REQUEST
// --------------------------------------------------------------------------
void Node::on_request(RequestMsg &&m, 
                      const MsgNetwork<uint8_t>::conn_t &) {
  metrics_->inc_msg("request");
  // Requests are only processed by primary
  if (is_primary()) {
    if (view_changing_) return;

    // Check for duplicate request by (client_id, timestamp)
    auto last_reply_it = last_replies_.find(m.client_id);
    if (last_reply_it != last_replies_.end()) {
      if (last_reply_it->second.timestamp == m.timestamp) {
        // Resend last reply
        send_to_client(m.client_id, ReplyMsg(view_, m.timestamp, m.client_id, id_, 
                                      last_reply_it->second.result));
        return;
      }
    }

    // Verify sequence within watermarks
    seq_num_++;
    if (seq_num_ <= h_ || seq_num_ > H_) return;

    auto &request = reqlog_[seq_num_]; // Create log entry
    // Begin PRE_PREPARE phase
    request.view = view_;
    request.seq = seq_num_;
    request.req = m;
    request.digest = salticidae::get_hash(m);
    request.has_preprepare = true;
    request.stage = ReqStage::PRE_PREPARED;

    // Broadcast preprepare message to replicas
    broadcast(PrePrepareMsg(view_, seq_num_, request.digest, m));

    // Primary also considers itself as having received pre-prepare
    request.prepares.insert(id_);
    metrics_->set_inflight(reqlog_.size());
  } else {
    // Otherwise, broadcast
    broadcast(m);
    reset_timer();
  }
}

// --------------------------------------------------------------------------
// PRE-PREPARE
// --------------------------------------------------------------------------
void Node::on_preprepare(PrePrepareMsg &&m,
                         const MsgNetwork<uint8_t>::conn_t &) {
  metrics_->inc_msg("preprepare");

  // Discard message criteria
  if (view_changing_) return;
  if (m.view != view_) return;
  if (m.seq_num <= h_ || m.seq_num > H_) return;

  auto &req_entry = reqlog_[m.seq_num];

  // Check for digest mismatch
  if (req_entry.has_preprepare && req_entry.digest != m.req_digest) {
    return;
  }

  // Begin PRE_PREPARE phase
  req_entry.view = m.view;
  req_entry.seq = m.seq_num;
  req_entry.digest = m.req_digest;
  req_entry.req = m.req;
  req_entry.has_preprepare = true;
  req_entry.stage = ReqStage::PRE_PREPARED;

  // Broadcast prepare to all replicas
  broadcast(PrepareMsg(view_, m.seq_num, m.req_digest, id_));

  // Set the node itself as prepared in the set
  req_entry.prepares.insert(id_);
  reset_timer();
  try_prepare(req_entry);
}

// --------------------------------------------------------------------------
// PREPARE
// --------------------------------------------------------------------------
void Node::on_prepare(PrepareMsg &&m, 
                      const MsgNetwork<uint8_t>::conn_t &) {
  metrics_->inc_msg("prepare");

  // Discard message criteria
  if (view_changing_) return;
  if (m.view != view_) return;
  if (m.seq_num <= h_ || m.seq_num > H_) return;

  auto &req_entry = reqlog_[m.seq_num];
  // Buffer the prepare even if we haven't received PrePrepare yet (Out-of-Order handling)
  // But we only count it if digests match
  if (req_entry.has_preprepare && req_entry.digest != m.req_digest)
    return;

  req_entry.prepares.insert(m.replica_id);
  try_prepare(req_entry);
}

void Node::try_prepare(ReqLogEntry &req_entry) {
  // Total needed matching PrePrepare: 2f from others + 1 (Primary's PrePrepare).
  if (req_entry.stage == ReqStage::PRE_PREPARED && 
    req_entry.has_preprepare && 
    req_entry.prepares.size() >= 2 * f_ + 1) { // 2f others + me 

    req_entry.stage = ReqStage::PREPARED;

    // Broadcast Commit
    broadcast(CommitMsg(view_, req_entry.seq, req_entry.digest, id_));
    req_entry.commits.insert(id_);

    try_commit(req_entry);
  }
}

// --------------------------------------------------------------------------
// COMMIT
// --------------------------------------------------------------------------
void Node::on_commit(CommitMsg &&m, const MsgNetwork<uint8_t>::conn_t &) {
  metrics_->inc_msg("commit");

  // Discard message criteria
  if (view_changing_) return;
  if (m.view != view_) return;
  if (m.seq_num <= h_ || m.seq_num > H_) return;

  auto &req_entry = reqlog_[m.seq_num];
  // Check digest
  if (req_entry.digest != m.req_digest) return;

  req_entry.commits.insert(m.replica_id);
  try_commit(req_entry);
}

void Node::try_commit(ReqLogEntry &entry) {
  // Prepared AND 2f+1 commits (including self)
  if (entry.stage == ReqStage::PREPARED && 
    entry.commits.size() >= 2 * f_ + 1) {
    entry.stage = ReqStage::COMMITTED;
    try_execute();
  }
}

void Node::try_execute() {
  // Execute in order assigned by leader
  while (true) {
    auto it = reqlog_.find(last_exec_ + 1);
    if (it == reqlog_.end()) break;
    auto &req_entry = it->second;

    // Never execute unless all lower sequence numbers are executed
    if (req_entry.stage != ReqStage::COMMITTED)
      break; // Ignore uncommited transactions and skip
    metrics_->set_inflight(reqlog_.size());

    // Execute operation
    auto result = service_->execute(req_entry.req.operation);

    // Send reply to client
    send_to_client(
      req_entry.req.client_id,
      ReplyMsg(view_, req_entry.req.timestamp, 
                req_entry.req.client_id, id_, result));
    metrics_->inc_msg("reply");

    last_exec_++;
    if (last_exec_ % K == 0) make_checkpoint();
  }
}

// --------------------------------------------------------------------------
// CHECKPOINT
// --------------------------------------------------------------------------
void Node::make_checkpoint() {
  // Insert count for a execution checkpoint
  uint256_t digest = service_->get_checkpoint_digest();
  broadcast(CheckpointMsg(last_exec_, digest, id_));
  checkpoints_[last_exec_].votes[digest].insert(id_);
}

void Node::on_checkpoint(CheckpointMsg &&m,
                         const MsgNetwork<uint8_t>::conn_t &) {
  if (m.seq_num <= h_) return; // Old checkpoint
  auto &cp_data = checkpoints_[m.seq_num];
  // Save vote for the checkpoint
  cp_data.votes[m.state_digest].insert(m.replica_id);

  // Check stability: 2f+1 votes
  if (!cp_data.stable && cp_data.votes[m.state_digest].size() >= 2 * f_ + 1) {
    cp_data.stable = true;
    last_stable_digest_ = m.state_digest;
    advance_watermarks(m.seq_num);
    garbage_collect();
  }
}

void Node::advance_watermarks(uint64_t stable_seq) {
  h_ = stable_seq;
  H_ = h_ + L;
  metrics_->set_watermarks(h_, H_);
}

void Node::garbage_collect() {
  // Delete anything that is not in current window
  // Drop all pre-prepare/prepare/commit log entries <= stable_seq
  for (auto it = reqlog_.begin(); it != reqlog_.end();) {
    if (it->first <= h_)
      it = reqlog_.erase(it);
    else
      ++it;
  }

  // Drop all checkpoint tracking strictly older than the stable one
  for (auto it = checkpoints_.begin(); it != checkpoints_.end();) {
    if (it->first < h_)
      it = checkpoints_.erase(it);
    else
      ++it;
  }
}

// --------------------------------------------------------------------------
// VIEW CHANGE
// --------------------------------------------------------------------------
void Node::start_view_change() {
  view_changing_ = true;
  uint64_t next_view = view_++; 
  view_change_timeout_count_++;

  uint32_t last_stable_checkpoint = h_;
  // Create C set (Valid Checkpoints) 
  std::vector<CheckpointMsg> C;
  if (checkpoints_.count(last_stable_checkpoint)) {
    for (const auto &digest_votes : checkpoints_[last_stable_checkpoint].votes) {
      for (uint32_t rid : digest_votes.second) {
        C.emplace_back(last_stable_checkpoint, digest_votes.first, rid);
      }
      if (digest_votes.second.size() >= 2 * f_ + 1) break;
    }
  }

  // Create P set (Prepared requests in current window)
  std::vector<PrepareProof> P;
  for (auto &pair : reqlog_) {
    if (pair.second.stage == ReqStage::PREPARED) {
      P.emplace_back(pair.second.view, pair.second.seq, pair.second.digest);
    }
  }
  ViewChangeMsg vc(next_view, last_stable_checkpoint, id_, C, P);
  broadcast(vc);
  on_viewchange(std::move(vc), MsgNetwork<uint8_t>::conn_t{});

  // Set timer for next view change (exponential backoff)
  reset_timer();
}

void Node::on_viewchange(ViewChangeMsg &&m, const MsgNetwork<uint8_t>::conn_t &) {
  // Ignore previous views
  if (m.next_view < view_) return;
  // If not the primary for v + 1, ignore
  if ((m.next_view % n_) != id_) return;

  view_change_store_[m.next_view][m.replica_id] = m;
  auto &vcs = view_change_store_[m.next_view];
  if (vcs.size() >= 2 * f_ + 1) {
    // Create V set (Valid View Change messages received)
    std::vector<ViewChangeMsg> V;
    for (auto &kv : vcs) V.push_back(kv.second);

    // Create O set (recalculate PrePrepares)
    std::vector<PrePrepareMsg> O;
    // 1. Find min_s (last stable checkpoint agreed)
    // 2. Find max_s (furthest ceiling agreed)
    uint64_t min_s = 0;
    uint64_t max_s = 0;
    for (auto &vcm : V) {
      min_s = std::max(min_s, vcm.last_stable_checkpoint);
      for (auto &prep : vcm.prepared_proofs) {
        max_s = std::max(max_s, prep.seq_num);
      }
    }
    // 3. For every seq between min_s and max_s, create PrePrepare
    for (uint64_t n = min_s + 1; n <= max_s; n++) {
      // Check if any view change message has a prepare for this seq_num
      uint32_t highest_view = 0;
      uint256_t req_digest;
      RequestMsg null_req("", 0, 0); // Default null request (No piggybacking)
      bool found = false;

      for (auto cvm : V) {
        for (auto &prep : cvm.prepared_proofs) {
          // The primary must choose the request from the most recent (highest) view
          if (prep.seq_num == n && prep.view > highest_view) {
            highest_view = prep.view;
            req_digest = prep.digest;
            found = true;
          }
        }
      }

      if (found) {
        // Create pre-prepare with the request digest from highest view
        O.emplace_back(m.next_view, n, req_digest, null_req);
      } else {
        // Create null pre-prepare
        null_req.operation = ""; // Null operation
        O.emplace_back(m.next_view, n, salticidae::get_hash(""), null_req);
      }
    }

    NewViewMsg nv(m.next_view, V, O);
    broadcast(nv);

    view_changing_ = false;
    view_ = m.next_view;
    view_change_timeout_count_ = 0;
    reset_timer();
  }
}

void Node::on_newview(NewViewMsg &&m, const salticidae::MsgNetwork<uint8_t>::conn_t &) {
  if (!view_changing_ || m.next_view <= view_) {
    return; // Ignore: either not waiting, or message is stale
  }
  // Validate V set
  if (m.view_changes.size() < 2 * f_ + 1) return;

  // Validate O set
  uint64_t min_s = 0;
  for (auto &vc : m.view_changes) {
    min_s = std::max(min_s, vc.last_stable_checkpoint);
  }
  uint64_t max_s = min_s;
  for (auto &vc : m.view_changes) {
    for (auto &prep : vc.prepared_proofs) {
      max_s = std::max(max_s, prep.seq_num);
    }
  }
  // O set has wrong size
  if (m.pre_prepares.size() != (max_s - min_s)) {
    return; 
  }

  // Update View
  view_ = m.next_view;
  view_changing_ = false;
  view_change_timeout_count_ = 0;
  reset_timer();

  // Process PrePrepares in O set
  for (auto &pp : m.pre_prepares) {
    on_preprepare(std::move(pp), salticidae::MsgNetwork<uint8_t>::conn_t{});
  }
}

void Node::reset_timer() {  view_change_timer_.del();
  view_change_timer_.del();
  // Exponential backoff: timeout doubles on each timeout
  double timeout = vc_timeout_ * (1u << view_change_timeout_count_);
  timeout = std::min(timeout, 300.0); // Cap at 5 minutes
  view_change_timer_.add(timeout);
}

template <typename M>
void Node::broadcast(const M &m) {
  for (const auto &kv : peers_) {
    if (kv.first == id_)
      continue;
    net_->send_msg(m, net_->connect_sync(kv.second));
  }
}

template <typename M> 
void Node::send_to_replica(uint32_t replica_id, const M& m) {
  auto it = peers_.find(primary());
  if (it == peers_.end())
    return;
  net_->send_msg(m, net_->connect_sync(it->second));
}

template <typename M> 
void Node::send_to_client(uint32_t client_id, const M& m) {
  auto it = clients_.find(primary());
if (it == clients_.end())
    return;
  net_->send_msg(m, net_->connect_sync(it->second));
}

} // namespace pbft
