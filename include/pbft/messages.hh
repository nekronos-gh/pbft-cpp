#pragma once
#include "salticidae/msg.h"
#include "salticidae/crypto.h"

#include "pbft/digest.hh"
#include <string>
#include <cstdint>

namespace pbft {

using salticidae::DataStream;

enum MsgType : uint8_t {
  REQUEST      = 0x00,
  REPLY        = 0x01,
  PRE_PREPARE  = 0x02,
  PREPARE      = 0x03,
  COMMIT       = 0x04,
  CHECKPOINT   = 0x05,
  VIEW_CHANGE  = 0x06,
  NEW_VIEW     = 0x07
};

// Client request (client -> any)
// <REQUEST, operation, timestamp, client>
struct RequestMsg {
  static const uint8_t opcode = REQUEST;
  DataStream serialized;

  std::string operation;
  uint64_t timestamp; // Timestamp is used to ensure exactly-one semantics
  uint32_t client_id;

  RequestMsg(const std::string &op, uint64_t ts, uint32_t cid)
  : operation(op), timestamp(ts), client_id(cid) {}
  RequestMsg(DataStream &&s) {
    s /*>> operation */>> timestamp >> client_id; // TODO: Find out how to unserialize a string
  }

  std::string digest() const {
    DataStream ds;
    ds << opcode << operation << timestamp << client_id;
    return compute_digest(ds.data(), ds.size());
  }
};

// Reply (replica -> client)
// <REPLY, view, timestamp, client_id, replica_id, result>
struct ReplyMsg {
  static const uint8_t opcode = REPLY;
  DataStream serialized;

  uint32_t view;
  uint64_t timestamp;
  uint32_t client_id;
  uint32_t replica_id;
  std::string result;

  ReplyMsg(uint32_t v, uint64_t ts, uint32_t cid, uint32_t rid, const std::string &res)
  : view(v), timestamp(ts), client_id(cid), replica_id(rid), result(res) {
    serialized << view << timestamp << client_id << replica_id << result;
  }
  ReplyMsg(DataStream &&s) {
    s >> view >> timestamp >> client_id >> replica_id /*>> result*/; 
  }
};


// Pre-prepare (primary -> replicas).
// <<PRE-PREPARE, view, sequence_number, request_digest> REQUEST>
struct PrePrepareMsg {
  static const uint8_t opcode = PRE_PREPARE;
  DataStream serialized;

  uint32_t view;
  uint64_t seq_num;
  std::string req_digest;

  // Piggybacked client request
  std::string operation; 
  uint64_t timestamp;
  uint32_t client_id;

  PrePrepareMsg(uint32_t v, uint64_t n, const std::string &rd,
                RequestMsg& rqm)
    : view(v), seq_num(n), req_digest(rd),
    operation(rqm.operation), timestamp(rqm.timestamp), client_id(rqm.client_id) {
    serialized << view << seq_num << req_digest
      << operation << timestamp << client_id;
  }
  PrePrepareMsg(DataStream &&s) {
    s >> view >> seq_num /*>> req_digest >> operation*/ >> timestamp >> client_id;
  }
};

// Prepare (replica -> all)
// <PREPARE, view, sequence_number, req_digest, replica_id>
struct PrepareMsg {
  static const uint8_t opcode = PREPARE;
  DataStream serialized;

  uint32_t view;
  uint64_t seq_num;
  std::string req_digest;
  uint32_t replica_id;

  PrepareMsg(uint32_t v, uint64_t n, const std::string &d, uint32_t rid)
  : view(v), seq_num(n), req_digest(d), replica_id(rid) {
    serialized << view << seq_num << req_digest << replica_id;
  }
  PrepareMsg(DataStream &&s) { s >> view >> seq_num /*>> req_digest*/ >> replica_id; }
};

// Commit (replica -> all)
// <COMMIT, view, sequence_number, request_digest, replica_id>
struct CommitMsg {
  static const uint8_t opcode = COMMIT;
  DataStream serialized;

  uint32_t view;
  uint64_t seq_num;
  std::string req_digest;
  uint32_t replica_id;

  CommitMsg(uint32_t v, uint64_t n, const std::string &rd, uint32_t rid)
  : view(v), seq_num(n), req_digest(rd), replica_id(rid) {
    serialized << view << seq_num << req_digest << replica_id;
  }
  CommitMsg(DataStream &&s) { s >> view >> seq_num /*>> req_digest */>> replica_id; }
};

// Checkpoint (replica -> all)
// <CHECKPOINT, sequence_number, state_digest, replica_id>
struct CheckpointMsg {
  static const uint8_t opcode = CHECKPOINT;
  DataStream serialized;

  uint64_t seq_num;
  std::string state_digest;
  uint32_t replica_id;

  CheckpointMsg(uint64_t n, const std::string &sd, uint32_t rid)
  : seq_num(n), state_digest(sd), replica_id(rid) {
    serialized << seq_num << state_digest << replica_id;
  }
  CheckpointMsg(DataStream &&s) { s >> seq_num /*>> state_digest */>> replica_id; }
};

// View change (replica -> all)
// <VIEW-CHANGE, next_view, sequence_number_last_stable_checkpoint, 
//    set_valid_checkpoints, set_of_requests, replica_id>
struct ViewChangeMsg {
  static const uint8_t opcode = VIEW_CHANGE;
  DataStream serialized;

  uint64_t next_view;
  uint64_t seq_num_last_checkpoint;
  uint32_t replica_id;
  // TODO: S set of 2f + 1 valid checkpoints
  //
  // TODO: P set of sets for each request prepared at i 
  // with seq_num higer than the sequence number of the last stable checkpoint

  ViewChangeMsg(uint64_t nv, uint64_t snlc, uint32_t rid)
  : next_view(nv), seq_num_last_checkpoint(snlc), replica_id(rid) {
    serialized << next_view << seq_num_last_checkpoint << replica_id;
  }
  ViewChangeMsg(DataStream &&s) { s >> next_view >> seq_num_last_checkpoint >> replica_id; }
};

// New view ( primary -> all)
// <NEW-VIEW, next_view, set_valid_view_change_messages,
//  set_pre_prepare_messages>
struct NewViewMsg {
  static const uint8_t opcode = VIEW_CHANGE;
  DataStream serialized;

  uint64_t next_view;
  // TODO: V set containing the valid view-change messages received 
  // by the primary plus the view-change message for 1 the primary 
  // sent (or would have sent)
  //
  // TODO: O set of pre-prepare messages (without the piggybacked request)

  NewViewMsg(uint64_t nv)
  : next_view(nv) {
    serialized << next_view;
  }
  NewViewMsg(DataStream &&s) { s >> next_view ; }
};

} // namespace pbft
