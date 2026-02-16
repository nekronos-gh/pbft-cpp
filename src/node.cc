#include <arpa/inet.h>

#include "pbft/metrics.hh"
#include "pbft/node.hh"

#include "salticidae/conn.h"
#include "salticidae/stream.h"

#include "spdlog/sinks/basic_file_sink.h"

using salticidae::_1;
using salticidae::_2;
using salticidae::MsgNetwork;
using salticidae::NetAddr;

#define NETADDR_STR(addr)                                                      \
  ([&] {                                                                       \
    char _ipbuf[INET_ADDRSTRLEN];                                              \
    uint32_t _ip = htonl((addr).ip);                                           \
    inet_ntop(AF_INET, &_ip, _ipbuf, sizeof(_ipbuf));                          \
    static thread_local char _outbuf[64];                                      \
    snprintf(_outbuf, sizeof(_outbuf), "%s:%u", _ipbuf,                        \
             (unsigned)((addr).port));                                         \
    return _outbuf;                                                            \
  }())

namespace pbft {

Node::Node(uint32_t replica_id, const NodeConfig &config,
           std::unique_ptr<ServiceInterface> service)
    : id_(replica_id), n_(config.num_replicas), vc_timeout_(config.vc_timeout),
      view_change_timer_(
          ec_, [this](salticidae::TimerEvent &) { start_view_change(); }),
      service_(std::move(service)) {

  f_ = (n_ - 1) / 3;
  K_ = config.checkpoint_interval;
  L_ = config.log_size;
  H_ = L_;

  // Set TLS in the network
  auto tls_config = config.tls_config;
  MsgNetwork<uint8_t>::Config network_config;
  if (tls_config.has_value()) {
    network_config.enable_tls(true)
        .tls_cert_file(tls_config->cert_file)
        .tls_key_file(tls_config->key_file);
  } else {
    network_config.enable_tls(false);
  }
  net_ = std::make_unique<MsgNetwork<uint8_t>>(ec_, network_config);

  service_->initialize();

  std::string metrics_addr = "0.0.0.0:" + std::to_string(9460 + id_);
  metrics_ = std::make_unique<Metrics>(metrics_addr);

  metrics_->set_view(view_);

  // Initialize logger
  logger_ =
      spdlog::basic_logger_mt(fmt::format("node-{}", id_), // logger name
                              fmt::format("logs/node-{}.log", id_) // file path
      );

  logger_->set_level(spdlog::level::info);
  logger_->set_pattern("[%Y-%m-%dT%H:%M:%S.%e] [%n] [%^%l%$] %v");

  register_handlers();
}

Node::~Node() {
  if (logger_) {
    logger_->flush();
    spdlog::drop(logger_->name());
  }
}

void Node::start(const NetAddr &listen_addr) {
  net_->start();
  net_->listen(listen_addr);
}

// Should always be executed after start
void Node::add_replica(uint32_t id, const NetAddr &addr) {
  if (id == id_)
    return;
  peers_[id] = net_->connect_sync(addr);
  logger_->info("CONNECTED REPLICA id={} addr={}", id, NETADDR_STR(addr));
}

// Should always be executed after start
void Node::add_client(uint32_t id, const NetAddr &addr) {
  clients_[id] = net_->connect_sync(addr);
  logger_->info("CONNECTED CLIENT id={} addr={}", id, NETADDR_STR(addr));
}

void Node::stop() {
  ec_.stop();
  if (net_) {
    for (const auto &kv : peers_) {
      net_->terminate(kv.second);
    }
    for (const auto &kv : clients_) {
      net_->terminate(kv.second);
    }
    net_->stop();
  }
}

void Node::run() {
  logger_->info("RUNNING");
  ec_.dispatch();
}

void Node::register_handlers() {
  net_->reg_handler(
      salticidae::generic_bind(&Node::on_handshake, this, _1, _2));
  net_->reg_handler(salticidae::generic_bind(&Node::on_request, this, _1, _2));
  net_->reg_handler(
      salticidae::generic_bind(&Node::on_preprepare, this, _1, _2));
  net_->reg_handler(salticidae::generic_bind(&Node::on_prepare, this, _1, _2));
  net_->reg_handler(salticidae::generic_bind(&Node::on_commit, this, _1, _2));
  net_->reg_handler(
      salticidae::generic_bind(&Node::on_checkpoint, this, _1, _2));
  net_->reg_handler(
      salticidae::generic_bind(&Node::on_viewchange, this, _1, _2));
  net_->reg_handler(salticidae::generic_bind(&Node::on_newview, this, _1, _2));

  net_->reg_conn_handler([this](const salticidae::ConnPool::conn_t &_conn,
                                bool connected) {
    auto conn =
        salticidae::static_pointer_cast<salticidae::MsgNetwork<uint8_t>::Conn>(
            _conn);
    if (connected) {
      net_->send_msg(HandshakeMsg(id_), conn);
    }
    return true;
  });
}

// Stablish which connections belong to who
void Node::on_handshake(HandshakeMsg &&m,
                        const salticidae::MsgNetwork<uint8_t>::conn_t &conn) {
  metrics_->inc_msg("handshake", m.serialized.size(), false);
  conn_to_peer_[conn.get()] = m.id;
}

// --------------------------------------------------------------------------
// REQUEST
// --------------------------------------------------------------------------
void Node::on_request(RequestMsg &&m, const MsgNetwork<uint8_t>::conn_t &) {
  metrics_->inc_msg("request", m.serialized.size(), false);
  logger_->info("MSG_RECV REQUEST op={} from={} ts={}", m.operation,
                m.client_id, m.timestamp);

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

  if (view_changing_)
    return;

  // Requests are only processed by primary
  if (this->is_primary()) {
    // Verify sequence within watermarks
    if (seq_num_ < h_ || seq_num_ >= H_)
      return;

    // Check if request is already in progress
    for (const auto &entry : reqlog_) {
      if (entry.second.has_preprepare &&
          entry.second.req.client_id == m.client_id &&
          entry.second.req.timestamp == m.timestamp) {
        return;
      }
    }

    seq_num_++;

    logger_->info("REQUEST ACCEPTED view={} op={} seq={}", view_, m.operation,
                  seq_num_);

    auto &request = reqlog_[seq_num_]; // Create log entry
    // Begin PRE_PREPARE phase
    request.view = view_;
    request.seq = seq_num_;
    request.req = m;
    request.digest = salticidae::get_hash(m);
    request.has_preprepare = true;
    request.stage = ReqStage::PRE_PREPARED;
    request.t_preprepare_start = std::chrono::steady_clock::now();

    // Broadcast preprepare message to replicas
    broadcast(PrePrepareMsg(view_, seq_num_, request.digest, m));

    request.prepares.insert(id_);
    metrics_->set_inflight(seq_num_ - last_exec_);
  } else {
    // Otherwise, send to primary
    start_timer_if_not_running();
    send_to_replica(primary(),
                    RequestMsg(m.operation, m.timestamp, m.client_id));
  }
}

// --------------------------------------------------------------------------
// PRE-PREPARE
// --------------------------------------------------------------------------
void Node::on_preprepare(PrePrepareMsg &&m,
                         const MsgNetwork<uint8_t>::conn_t &conn) {
  on_preprepare_impl(std::move(m), conn, false);
}
void Node::on_preprepare_impl(PrePrepareMsg &&m,
                              const MsgNetwork<uint8_t>::conn_t &conn,
                              bool from_self) {
  if (!from_self) {
    metrics_->inc_msg("preprepare", m.serialized.size(), false);
    logger_->info("MSG_RECV PREPREPARE view={} op={} seq={} hash={}", view_,
                  m.view, m.req.operation, m.seq_num,
                  m.req_digest.to_hex().substr(0, 5));
  }

  // Discard message criteria
  if (!from_self && !comes_from_primary(conn))
    return;
  if (view_changing_)
    return;
  if (m.view != view_)
    return;
  if (m.seq_num < h_ || m.seq_num > H_)
    return;

  auto &req_entry = reqlog_[m.seq_num];

  // Check for digest mismatch
  if (req_entry.has_preprepare && req_entry.digest != m.req_digest) {
    return;
  }
  logger_->info("PREPREPARE ACCEPTED h1={} h2={}",
                req_entry.digest.to_hex().substr(0, 5),
                m.req_digest.to_hex().substr(0, 5));

  // The request is valid, start the timer for view change
  start_timer_if_not_running();

  // Begin PRE_PREPARE phase
  req_entry.view = m.view;
  req_entry.seq = m.seq_num;
  req_entry.digest = m.req_digest;
  req_entry.req = m.req;
  req_entry.has_preprepare = true;
  req_entry.stage = ReqStage::PRE_PREPARED;
  req_entry.t_preprepare_start = std::chrono::steady_clock::now();

  // Broadcast prepare to all replicas
  broadcast(PrepareMsg(view_, m.seq_num, m.req_digest, id_));

  // Set the node itself as prepared in the set
  req_entry.prepares.insert(id_);

  try_prepare(req_entry);
}

// --------------------------------------------------------------------------
// PREPARE
// --------------------------------------------------------------------------
void Node::on_prepare(PrepareMsg &&m, const MsgNetwork<uint8_t>::conn_t &conn) {
  metrics_->inc_msg("prepare", m.serialized.size(), false);
  logger_->info("MSG_RECV PREPARE view={} seq={} from={} hash={}", m.view,
                m.seq_num, m.replica_id, m.req_digest.to_hex().substr(0, 5));

  // Discard message criteria
  if (comes_from_primary(conn))
    return;
  if (view_changing_)
    return;
  if (m.view != view_)
    return;
  if (m.seq_num < h_ || m.seq_num > H_)
    return;

  auto &req_entry = reqlog_[m.seq_num];
  // Buffer the prepare even if we haven't received PrePrepare yet (Out-of-Order
  // handling) But we only count it if digests match
  if (req_entry.has_preprepare && req_entry.digest != m.req_digest) {
    return;
  }
  logger_->info("PREPARE ACCEPTED h1={} h2={}",
                req_entry.digest.to_hex().substr(0, 5),
                m.req_digest.to_hex().substr(0, 5));

  req_entry.prepares.insert(m.replica_id);
  try_prepare(req_entry);
}

void Node::try_prepare(ReqLogEntry &req_entry) {
  // Total needed matching PrePrepare: 2f Prepares + 1 PrePrepared
  if (req_entry.stage == ReqStage::PRE_PREPARED && req_entry.has_preprepare &&
      req_entry.prepares.size() >= 2 * f_) {
    logger_->info("LOCALLY PREPARED view={} op={} seq={} hash={}",
                  req_entry.view, req_entry.req.operation, req_entry.seq,
                  req_entry.digest.to_hex().substr(0, 5));

    auto duration =
        std::chrono::duration_cast<std::chrono::duration<double>>(
            std::chrono::steady_clock::now() - req_entry.t_preprepare_start)
            .count();
    metrics_->observe_phase("prepared", duration);

    req_entry.stage = ReqStage::PREPARED;
    req_entry.t_commit_start = std::chrono::steady_clock::now();

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
  metrics_->inc_msg("commit", m.serialized.size(), false);
  logger_->info("MSG_RECV COMMIT view={} seq={} from={} hash={}", m.view,
                m.seq_num, m.replica_id, m.req_digest.to_hex().substr(0, 5));

  // Discard message criteria
  if (view_changing_)
    return;
  if (m.view != view_)
    return;
  if (m.seq_num < h_ || m.seq_num > H_)
    return;

  auto &req_entry = reqlog_[m.seq_num];
  // Check digest
  if (req_entry.digest != m.req_digest)
    return;

  req_entry.commits.insert(m.replica_id);
  try_commit(req_entry);
}

void Node::try_commit(ReqLogEntry &req_entry) {
  // Prepared AND 2f+1 commits (including self)
  if (req_entry.stage == ReqStage::PREPARED &&
      req_entry.commits.size() >= 2 * f_ + 1) {
    logger_->info("LOCALLY COMMITTED view={} op={} seq={} hash={}",
                  req_entry.view, req_entry.req.operation, req_entry.seq,
                  req_entry.digest.to_hex().substr(0, 5));

    auto duration =
        std::chrono::duration_cast<std::chrono::duration<double>>(
            std::chrono::steady_clock::now() - req_entry.t_commit_start)
            .count();
    metrics_->observe_phase("commit", duration);

    req_entry.stage = ReqStage::COMMITTED;
    try_execute();
  }
}

void Node::try_execute() {
  // Execute in order assigned by leader
  while (true) {
    auto it = reqlog_.find(last_exec_ + 1);
    if (it == reqlog_.end())
      break;
    auto &req_entry = it->second;

    // Never execute unless all lower sequence numbers are executed
    if (req_entry.stage != ReqStage::COMMITTED)
      break; // Ignore uncommited transactions and skip

    // Execute operation
    auto result = service_->execute(req_entry.req.operation);

    // Send reply to client
    auto reply = ReplyMsg(view_, req_entry.req.timestamp,
                          req_entry.req.client_id, id_, result);
    send_to_client(req_entry.req.client_id, reply);
    ClientReplyInfo cache_reply;

    cache_reply.replica_id = reply.replica_id;
    cache_reply.timestamp = reply.timestamp;
    cache_reply.result = reply.result;
    last_replies_[req_entry.req.client_id] = cache_reply;

    last_exec_++;
    logger_->info("LOCALLY EXECUTED view={} op={} seq={} hash={}",
                  req_entry.view, req_entry.req.operation, req_entry.seq,
                  req_entry.digest.to_hex().substr(0, 5));

    // Stop timer if no longer waiting, restart if still waiting for other
    // requests
    manage_timer();
    if (last_exec_ % K_ == 0)
      make_checkpoint();
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
  logger_->info("START CHECKPOINT state={}", digest.to_hex().substr(0, 5));
}

void Node::on_checkpoint(CheckpointMsg &&m,
                         const MsgNetwork<uint8_t>::conn_t &) {
  metrics_->inc_msg("checkpoint", m.serialized.size(), false);
  logger_->info("MSG_RECV CHECKPOINT view={} seq={} from={} state={}", view_,
                m.seq_num, m.replica_id, m.state_digest.to_hex().substr(0, 5));
  if (m.seq_num < h_)
    return; // Old checkpoint
  auto &cp_data = checkpoints_[m.seq_num];
  // Save vote for the checkpoint
  cp_data.votes[m.state_digest].insert(m.replica_id);

  // Check stability: 2f+1 votes
  if (!cp_data.stable && cp_data.votes[m.state_digest].size() >= 2 * f_ + 1) {
    logger_->info("CHECKPOINT AGREED view={} seq={} state={}", view_, m.seq_num,
                  m.state_digest.to_hex().substr(0, 5));
    cp_data.stable = true;
    last_stable_digest_ = m.state_digest;
    advance_watermarks(m.seq_num);
    // Update the sequence number accordingly
    seq_num_ = recompute_highest_sequence();
    garbage_collect();
  }
}

void Node::advance_watermarks(uint64_t stable_seq) {
  h_ = stable_seq;
  H_ = h_ + L_;
  metrics_->set_watermarks(h_, H_);
}

uint64_t Node::recompute_highest_sequence() const {
  uint64_t highest = std::max(last_exec_, h_);
  if (!reqlog_.empty()) {
    highest = std::max(highest, reqlog_.rbegin()->first);
  }
  return highest;
}

void Node::garbage_collect() {
  // Delete anything that is not in current window
  // Drop all pre-prepare/prepare/commit log entries <= stable_seq
  for (auto it = reqlog_.begin(); it != reqlog_.end();) {
    if (it->first <= h_)
      it = reqlog_.erase(it);
    else
      it++;
  }

  // Drop all checkpoint tracking strictly older than the stable one
  for (auto it = checkpoints_.begin(); it != checkpoints_.end();) {
    if (it->first < h_)
      it = checkpoints_.erase(it);
    else
      it++;
  }
}

// --------------------------------------------------------------------------
// VIEW CHANGE
// --------------------------------------------------------------------------
void Node::start_view_change() {
  view_changing_ = true;
  metrics_->inc_view_change("timeout");
  uint64_t next_view = view_ + 1;
  view_change_timeout_count_++;

  logger_->info("VIEW CHANGE new_view={}", next_view);

  // Stop the timer when starting view change
  stop_timer();

  uint32_t last_stable_checkpoint = h_;
  // Create C set (Valid Checkpoints)
  std::vector<CheckpointMsg> C;
  if (checkpoints_.count(last_stable_checkpoint)) {
    for (const auto &digest_votes :
         checkpoints_[last_stable_checkpoint].votes) {
      for (uint32_t rid : digest_votes.second) {
        C.emplace_back(last_stable_checkpoint, digest_votes.first, rid);
      }
      if (digest_votes.second.size() >= 2 * f_ + 1)
        break;
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
}

void Node::on_viewchange(ViewChangeMsg &&m,
                         const MsgNetwork<uint8_t>::conn_t &) {
  metrics_->inc_msg("viewchange", m.serialized.size(), false);
  logger_->info("MSG_RECV VIEW_CHANGE view={} new_view={} checkpoint={} C={} "
                "P={} from={}",
                view_, m.next_view, m.last_stable_checkpoint,
                m.checkpoint_proof.size(), m.prepared_proofs.size(),
                m.replica_id);
  // Ignore previous views
  if (m.next_view < view_)
    return;
  // If not the primary for v + 1, ignore
  if ((m.next_view % n_) != id_)
    return;

  view_change_store_[m.next_view][m.replica_id] = m;
  auto &vcs = view_change_store_[m.next_view];
  if (vcs.size() >= 2 * f_ + 1) {
    // Create V set (Valid View Change messages received)
    std::vector<ViewChangeMsg> V;
    for (auto &kv : vcs)
      V.push_back(kv.second);

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
          // The primary must choose the request from the most recent (highest)
          // view
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
    metrics_->set_view(view_);
    view_change_timeout_count_ = 0;
    // Update the sequence number accordingly
    seq_num_ = recompute_highest_sequence();

    // Stop timer and restart if we have pending requests
    manage_timer();
  }
}

void Node::on_newview(NewViewMsg &&m, const MsgNetwork<uint8_t>::conn_t &conn) {
  metrics_->inc_msg("newview", m.serialized.size(), false);
  logger_->info("MSG_RECV NEW_VIEW view={} new_view={} V={} O={}", view_,
                m.next_view, m.view_changes.size(), m.pre_prepares.size());

  // Only accept messages from primary for new view
  if (!comes_from_primary(conn, m.next_view))
    return;
  if (!view_changing_ || m.next_view <= view_) {
    return; // Ignore: either not waiting, or message is stale
  }
  // Validate V set
  if (m.view_changes.size() < 2 * f_ + 1)
    return;

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
  view_changing_ = false;
  view_ = m.next_view;
  metrics_->set_view(view_);
  view_change_timeout_count_ = 0;

  // Process PrePrepares in O set
  for (auto &pp : m.pre_prepares) {
    on_preprepare_impl(std::move(pp), std::move(conn), true);
  }

  // Update the sequence number accordingly
  seq_num_ = recompute_highest_sequence();

  // Stop if no pending requests
  manage_timer();
}

bool Node::is_waiting_for_request() {
  // We are waiting if there's any request received but not yet executed
  for (const auto &entry : reqlog_) {
    if (entry.second.has_preprepare && entry.first > last_exec_) {
      return true;
    }
  }
  return false;
}

void Node::start_timer_if_not_running() {
  if (!timer_running_ && !view_changing_) {
    double timeout = vc_timeout_ * (1u << view_change_timeout_count_);
    timeout = std::min(timeout, 300.0); // Cap at 5 minutes
    view_change_timer_.add(timeout);
    timer_running_ = true;
  }
}

void Node::stop_timer() {
  if (timer_running_) {
    view_change_timer_.del();
    timer_running_ = false;
  }
}

// Helper function to manage timer after execution
void Node::manage_timer() {
  if (!is_waiting_for_request()) {
    // Stop the timer when no longer waiting to execute any request
    stop_timer();
  } else {
    // Restart the timer if we are still waiting for other requests
    if (timer_running_) {
      stop_timer();
      start_timer_if_not_running();
    }
  }
}

} // namespace pbft
