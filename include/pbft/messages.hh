#pragma once
#include "serialization.hh"

#include <cstdint>
#include <set>
#include <string>

namespace pbft {

using salticidae::DataStream;

enum MsgType : uint8_t {
  REQUEST = 0x00,
  REPLY = 0x01,
  PRE_PREPARE = 0x02,
  PREPARE = 0x03,
  COMMIT = 0x04,
  CHECKPOINT = 0x05,
  VIEW_CHANGE = 0x06,
  NEW_VIEW = 0x07,
  HANDSHAKE = 0x08
};

// Client request (client -> any)
// <REQUEST, operation, timestamp, client>
struct RequestMsg {
  static constexpr uint8_t opcode = REQUEST;
  DataStream serialized;

  std::string operation;
  uint64_t timestamp; // Timestamp is used to ensure exactly-one semantics
  uint32_t client_id;

  // Needed for PRE-PREPARE piggybacking
  RequestMsg() = default;
  RequestMsg(const std::string &op, uint64_t ts, uint32_t cid)
      : operation(op), timestamp(ts), client_id(cid) {
    serialized << salticidae::htole((uint32_t)operation.size()) << operation
               << timestamp << client_id;
  }
  RequestMsg(DataStream &&s) { s >> operation >> timestamp >> client_id; }

  void serialize(DataStream &s) const {
    s << salticidae::htole((uint32_t)operation.size()) << operation << timestamp
      << client_id;
  }
  void unserialize(DataStream &s) { s >> operation >> timestamp >> client_id; }
};

// Reply (replica -> client)
// <REPLY, view, timestamp, client_id, replica_id, result>
struct ReplyMsg {
  static constexpr uint8_t opcode = REPLY;
  DataStream serialized;

  uint32_t view;
  uint64_t timestamp;
  uint32_t client_id;
  uint32_t replica_id;
  std::string result;

  ReplyMsg(uint32_t v, uint64_t ts, uint32_t cid, uint32_t rid,
           const std::string &res)
      : view(v), timestamp(ts), client_id(cid), replica_id(rid), result(res) {
    serialized << view << timestamp << client_id << replica_id
               << salticidae::htole((uint32_t)result.size()) << result;
  }
  ReplyMsg(DataStream &&s) {
    s >> view >> timestamp >> client_id >> replica_id >> result;
  }
};

// Pre-prepare (primary -> replicas).
// <<PRE-PREPARE, view, sequence_number, request_digest> REQUEST>
struct PrePrepareMsg {
  static constexpr uint8_t opcode = PRE_PREPARE;
  DataStream serialized;

  uint32_t view;
  uint64_t seq_num;
  uint256_t req_digest;

  RequestMsg req; // Piggybacked request

  // Empty constructor for serialization
  PrePrepareMsg() = default;
  PrePrepareMsg(uint32_t v, uint64_t n, const uint256_t &rd,
                const RequestMsg &rqm)
      : view(v), seq_num(n), req_digest(rd), req(rqm) {
    serialized << view << seq_num << req_digest << req;
  }
  PrePrepareMsg(DataStream &&s) { s >> view >> seq_num >> req_digest >> req; }

  void serialize(DataStream &s) const {
    s << view << seq_num << req_digest << req;
  }
  void unserialize(DataStream &s) { s >> view >> seq_num >> req_digest >> req; }
};

// Prepare (replica -> all)
// <PREPARE, view, sequence_number, req_digest, replica_id>
struct PrepareMsg {
  static constexpr uint8_t opcode = PREPARE;
  DataStream serialized;

  uint32_t view;
  uint64_t seq_num;
  uint256_t req_digest;
  uint32_t replica_id;

  PrepareMsg(uint32_t v, uint64_t n, const uint256_t &d, uint32_t rid)
      : view(v), seq_num(n), req_digest(d), replica_id(rid) {
    serialized << view << seq_num << req_digest << replica_id;
  }
  PrepareMsg(DataStream &&s) {
    s >> view >> seq_num >> req_digest >> replica_id;
  }

  void serialize(DataStream &s) const {
    s << view << seq_num << req_digest << replica_id;
  }
  void unserialize(DataStream &s) {
    s >> view >> seq_num >> req_digest >> replica_id;
  }
};

// Commit (replica -> all)
// <COMMIT, view, sequence_number, request_digest, replica_id>
struct CommitMsg {
  static constexpr uint8_t opcode = COMMIT;
  DataStream serialized;

  uint32_t view;
  uint64_t seq_num;
  uint256_t req_digest;
  uint32_t replica_id;

  CommitMsg(uint32_t v, uint64_t n, const uint256_t &rd, uint32_t rid)
      : view(v), seq_num(n), req_digest(rd), replica_id(rid) {
    serialized << view << seq_num << req_digest << replica_id;
  }
  CommitMsg(DataStream &&s) {
    s >> view >> seq_num >> req_digest >> replica_id;
  }
};

// Checkpoint (replica -> all)
// <CHECKPOINT, sequence_number, state_digest, replica_id>
struct CheckpointMsg {
  static constexpr uint8_t opcode = CHECKPOINT;
  DataStream serialized;

  uint64_t seq_num;
  uint256_t state_digest;
  uint32_t replica_id;

  // Empty constructor for serialization
  CheckpointMsg() = default;
  CheckpointMsg(uint64_t n, const uint256_t &sd, uint32_t rid)
      : seq_num(n), state_digest(sd), replica_id(rid) {
    serialized << seq_num << state_digest << replica_id;
  }
  CheckpointMsg(DataStream &&s) { s >> seq_num >> state_digest >> replica_id; }

  void serialize(DataStream &s) const {
    s << seq_num << state_digest << replica_id;
  }
  void unserialize(DataStream &s) {
    s >> seq_num >> state_digest >> replica_id;
  }
};

// Used for P set in ViewChange
// Represents a request that prepared at a specific replica
struct PrepareProof {
  uint32_t view;
  uint64_t seq_num;
  uint256_t digest;
  std::set<uint32_t> prepared_replicas;

  // Empty constructor for serialization
  PrepareProof() = default;
  PrepareProof(uint32_t v, uint64_t n, uint256_t d)
      : view(v), seq_num(n), digest(d) {}

  friend DataStream &operator<<(DataStream &s, const PrepareProof &p) {
    return s << p.view << p.seq_num << p.digest;
  }
  friend DataStream &operator>>(DataStream &s, PrepareProof &p) {
    return s >> p.view >> p.seq_num >> p.digest;
  }

  void serialize(DataStream &s) const { s << view << seq_num << digest; }
  void unserialize(DataStream &s) { s >> view >> seq_num >> digest; }
};

// View change (replica -> all)
// <VIEW-CHANGE, next_view, sequence_number_last_stable_checkpoint,
//    set_valid_checkpoints, set_of_requests, replica_id>
struct ViewChangeMsg {
  static constexpr uint8_t opcode = VIEW_CHANGE;
  DataStream serialized;

  uint64_t next_view;
  uint64_t last_stable_checkpoint;
  uint32_t replica_id;
  // C set: 2f+1 checkpoints
  std::vector<CheckpointMsg> checkpoint_proof;
  // P set: List of requests that have prepared locally at this replica
  std::vector<PrepareProof> prepared_proofs;

  // Empty constructor for serialization
  ViewChangeMsg() = default;
  ViewChangeMsg(uint32_t nv, uint64_t lsc, uint32_t rid,
                const std::vector<CheckpointMsg> &C,
                const std::vector<PrepareProof> &P)
      : next_view(nv), last_stable_checkpoint(lsc), replica_id(rid),
        checkpoint_proof(C), prepared_proofs(P) {
    serialized << next_view << last_stable_checkpoint << replica_id
               << checkpoint_proof << prepared_proofs;
  }
  ViewChangeMsg(DataStream &&s) {
    s >> next_view >> last_stable_checkpoint >> replica_id >>
        checkpoint_proof >> prepared_proofs;
  }

  void serialize(DataStream &s) const {
    s << next_view << last_stable_checkpoint << replica_id << checkpoint_proof
      << prepared_proofs;
  }
  void unserialize(DataStream &s) {
    s >> next_view >> last_stable_checkpoint >> replica_id >>
        checkpoint_proof >> prepared_proofs;
  }
};

// New view ( primary -> all)
// <NEW-VIEW, next_view, set_valid_view_change_messages,
//  set_pre_prepare_messages>
struct NewViewMsg {
  static constexpr uint8_t opcode = NEW_VIEW;
  DataStream serialized;

  uint64_t next_view;
  // V set: Valid ViewChange messages received
  std::vector<ViewChangeMsg> view_changes;
  // O set: PrePrepare messages for new view
  std::vector<PrePrepareMsg> pre_prepares;

  NewViewMsg(uint32_t nv, const std::vector<ViewChangeMsg> &V,
             const std::vector<PrePrepareMsg> &O)
      : next_view(nv), view_changes(V), pre_prepares(O) {
    serialized << next_view;

    serialized << (uint32_t)view_changes.size();
    for (auto &vc : view_changes)
      serialized << vc;

    serialized << (uint32_t)pre_prepares.size();
    for (auto &pp : pre_prepares)
      serialized << pp;
  }
  NewViewMsg(DataStream &&s) {
    s >> next_view;
    uint32_t count;

    s >> count;
    for (uint32_t i = 0; i < count; i++) {
      ViewChangeMsg tmp;
      s >> tmp;
      view_changes.emplace_back(std::move(tmp));
    }

    s >> count;
    for (uint32_t i = 0; i < count; i++) {
      PrePrepareMsg tmp;
      s >> tmp;
      pre_prepares.emplace_back(std::move(tmp));
    }
  }
};

struct HandshakeMsg {
  static constexpr uint8_t opcode = HANDSHAKE;
  DataStream serialized;
  uint32_t id;

  HandshakeMsg(uint32_t _id) : id(_id) { serialized << id; }

  HandshakeMsg(DataStream &&s) { s >> id; }
};

} // namespace pbft
