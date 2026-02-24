#pragma once
#include "pbft/node.hh"

using salticidae::_1;
using salticidae::_2;
using salticidae::MsgNetwork;
using salticidae::NetAddr;
namespace pbft::testing {
enum class FaultMode {
  NONE,
  DROP_ALL,
  EQUIVOCATE_PREPREPARE,
  DOUBLE_PREPARE,
  DOUBLE_COMMIT,
  CORRUPT_CHECKPOINT,
  FAKE_VIEWCHANGE,
  SELECTIVE_SILENCE
};

struct ByzantineNodeConfig {
  uint32_t id;
  FaultMode fault_mode;
};

class ByzantineNode : public Node {
public:
  ByzantineNode(uint32_t replica_id, const NodeConfig &config, FaultMode mode,
                std::unique_ptr<ServiceInterface> service)
      : Node(replica_id, config, std::move(service)), fault_mode_(mode) {
    overwrite_handlers();
  }

private:
  FaultMode fault_mode_;

  static uint256_t fake_digest(uint64_t salt) {
    std::string evil = "EVIL_" + std::to_string(salt);
    return salticidae::get_hash(evil);
  }

  void overwrite_handlers() {
    net_->reg_handler(
        salticidae::generic_bind(&ByzantineNode::on_request, this, _1, _2));
    net_->reg_handler(
        salticidae::generic_bind(&ByzantineNode::on_preprepare, this, _1, _2));
    net_->reg_handler(
        salticidae::generic_bind(&ByzantineNode::on_prepare, this, _1, _2));
    net_->reg_handler(
        salticidae::generic_bind(&ByzantineNode::on_commit, this, _1, _2));
    net_->reg_handler(
        salticidae::generic_bind(&ByzantineNode::on_checkpoint, this, _1, _2));
    net_->reg_handler(
        salticidae::generic_bind(&ByzantineNode::on_viewchange, this, _1, _2));
    net_->reg_handler(
        salticidae::generic_bind(&ByzantineNode::on_newview, this, _1, _2));
  }

  // ---------------------------------------------------------------------------
  // REQUEST
  // ---------------------------------------------------------------------------
  void on_request(RequestMsg &&m, const MsgNetwork<uint8_t>::conn_t &conn) {
    if (fault_mode_ == FaultMode::DROP_ALL)
      return;

    if (fault_mode_ == FaultMode::SELECTIVE_SILENCE && is_primary() &&
        ((seq_num_ + 1) % 2 == 0))
      return;

    Node::on_request(std::move(m), conn);
  }

  // ---------------------------------------------------------------------------
  // PREPREPARE
  // ---------------------------------------------------------------------------
  void on_preprepare(PrePrepareMsg &&m,
                     const MsgNetwork<uint8_t>::conn_t &conn) {

    if (fault_mode_ == FaultMode::DROP_ALL)
      return;

    if (fault_mode_ == FaultMode::DOUBLE_PREPARE) {
      // Let honest logic run first
      Node::on_preprepare(PrePrepareMsg(m), conn);
      // Now send conflicting prepare
      PrepareMsg evil(m.view, m.seq_num, fake_digest(m.seq_num), id_);
      broadcast(evil);
      return;
    }

    Node::on_preprepare(std::move(m), conn);
  }

  // ---------------------------------------------------------------------------
  // PREPARE
  // ---------------------------------------------------------------------------
  void on_prepare(PrepareMsg &&m, const MsgNetwork<uint8_t>::conn_t &conn) {

    if (fault_mode_ == FaultMode::DROP_ALL)
      return;

    if (fault_mode_ == FaultMode::DOUBLE_COMMIT) {
      // Run honest prepare logic
      Node::on_prepare(PrepareMsg(m), conn);
      // Emit conflicting commit
      CommitMsg evil(m.view, m.seq_num, fake_digest(m.seq_num), id_);
      broadcast(evil);
      return;
    }

    Node::on_prepare(std::move(m), conn);
  }

  // ---------------------------------------------------------------------------
  // COMMIT
  // ---------------------------------------------------------------------------
  void on_commit(CommitMsg &&m, const MsgNetwork<uint8_t>::conn_t &conn) {

    if (fault_mode_ == FaultMode::DROP_ALL)

      Node::on_commit(std::move(m), conn);
  }

  // ---------------------------------------------------------------------------
  // CHECKPOINT
  // ---------------------------------------------------------------------------
  void on_checkpoint(CheckpointMsg &&m,
                     const MsgNetwork<uint8_t>::conn_t &conn) {

    if (fault_mode_ == FaultMode::DROP_ALL)
      return;

    if (fault_mode_ == FaultMode::CORRUPT_CHECKPOINT && m.replica_id == id_) {

      CheckpointMsg evil(m.seq_num, fake_digest(m.seq_num), id_);
      broadcast(evil);
      return;
    }
    Node::on_checkpoint(std::move(m), conn);
  }

  // ---------------------------------------------------------------------------
  // VIEWCHANGE
  // ---------------------------------------------------------------------------
  void on_viewchange(ViewChangeMsg &&m,
                     const MsgNetwork<uint8_t>::conn_t &conn) {

    if (fault_mode_ == FaultMode::DROP_ALL)
      return;

    if (fault_mode_ == FaultMode::FAKE_VIEWCHANGE) {
      Node::on_viewchange(ViewChangeMsg(m), conn);
      ViewChangeMsg fake_vc(m.next_view + 1, h_, id_, {}, {});
      broadcast(fake_vc);
      return;
    }

    Node::on_viewchange(std::move(m), conn);
  }

  // ---------------------------------------------------------------------------
  // NEWVIEW
  // ---------------------------------------------------------------------------
  void on_newview(NewViewMsg &&m, const MsgNetwork<uint8_t>::conn_t &conn) {

    if (fault_mode_ == FaultMode::DROP_ALL)
      return;

    if (fault_mode_ == FaultMode::FAKE_VIEWCHANGE)
      return; // stall progress

    Node::on_newview(std::move(m), conn);
  }
};

} // namespace pbft::testing
