#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

#define PBFT_TESTING_ACCESS public
#include "pbft/messages.hh"
#include "pbft/node.hh"
#include "pbft/service_interface.hh"

using ::testing::Return;

namespace pbft {
namespace testing {

class MockService : public ServiceInterface {
public:
  MOCK_METHOD(void, initialize, (), (override));
  MOCK_METHOD(std::string, execute, (const std::string &), (override));
  MOCK_METHOD(uint256_t, get_checkpoint_digest, (), (override));
};

class NodeTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto service_ptr = std::make_unique<MockService>();
    service = service_ptr.get(); // Only for mock
    // Setup mock expectations
    EXPECT_CALL(*service, initialize()).Times(1).WillOnce(Return());
    // Create 4 replicas (f=1)
    NodeConfig config = {NUM_REPLICAS, 200, 100, 2.0, {}};
    node_ = std::make_unique<Node>(0, config, std::move(service_ptr));
    // Send always from primary by default
    node_->conn_to_peer_[dummy_conn.get()] = 0;
  }

  std::unique_ptr<Node> node_;
  static constexpr uint32_t MAX_FAULTY = 1;
  static constexpr uint32_t NUM_REPLICAS = 3 * MAX_FAULTY + 1;
  static constexpr uint64_t L = 200; // Watermark window
  static constexpr uint64_t K = 100; // Checkpoint interval

  salticidae::MsgNetwork<uint8_t>::conn_t dummy_conn;
  MockService *service;
};

// ============================================================================
// BASIC INITIALIZATION AND SETUP TESTS
// ============================================================================

TEST_F(NodeTest, InitializationCorrect) {
  EXPECT_EQ(node_->id_, 0);
  EXPECT_EQ(node_->n_, NUM_REPLICAS);
  EXPECT_EQ(node_->f_, MAX_FAULTY);
  EXPECT_EQ(node_->view_, 0);
  EXPECT_EQ(node_->seq_num_, 0);
  EXPECT_EQ(node_->h_, 0);
  EXPECT_EQ(node_->H_, L);
  EXPECT_FALSE(node_->view_changing_);
  EXPECT_EQ(node_->last_exec_, 0);
}

TEST_F(NodeTest, IsPrimary) {
  EXPECT_TRUE(node_->is_primary());
  EXPECT_EQ(node_->primary(), node_->id_);

  // Force view change
  node_->view_ = 1;
  EXPECT_FALSE(node_->is_primary());
  EXPECT_NE(node_->primary(), node_->id_);
}

// ============================================================================
// HANDSHAKE TESTS
// ============================================================================

TEST_F(NodeTest, PopulateConnToPeer) {
  HandshakeMsg handshake(1);
  node_->on_handshake(std::move(handshake), dummy_conn);

  // No changes to state
  EXPECT_TRUE(!node_->conn_to_peer_.empty());
  EXPECT_EQ(node_->conn_to_peer_[dummy_conn.get()], 1);
}

// ============================================================================
// REQUEST PROCESSING TESTS
// ============================================================================

TEST_F(NodeTest, RequestIgnoredIfViewChanging) {
  // Trigger test while on view change
  node_->view_changing_ = true;
  RequestMsg req("op", 1000, 0);
  node_->on_request(std::move(req), dummy_conn);

  // No changes to state
  EXPECT_TRUE(node_->reqlog_.empty());
  EXPECT_EQ(node_->seq_num_, 0);
}

TEST_F(NodeTest, RequestIgnoredIfIsNotPrimary) {
  // Trigger for non_primary
  node_->id_ = 1;
  RequestMsg req("op", 1000, 0);
  node_->on_request(std::move(req), dummy_conn);

  // No changes to state
  EXPECT_TRUE(node_->reqlog_.empty());
  EXPECT_EQ(node_->seq_num_, 0);
}

TEST_F(NodeTest, RequestIgnoredIfIsSeqOutOfWatermarks) {
  RequestMsg req("op", 1000, 0);
  node_->seq_num_ = node_->H_;
  node_->on_request(std::move(req), dummy_conn);

  // No changes to state
  EXPECT_TRUE(node_->reqlog_.empty());
}

TEST_F(NodeTest, RequestDuplicateUsesCache) {
  // Inject previous response
  Node::ClientReplyInfo cached;
  cached.timestamp = 2000;
  cached.replica_id = 0;
  cached.result = "executed_result";
  node_->last_replies_[0] = cached;

  uint64_t initial_seq = node_->seq_num_;
  RequestMsg dup("op1", 2000, 0);
  node_->on_request(std::move(dup), dummy_conn);

  // No changes have been done
  EXPECT_EQ(node_->seq_num_, initial_seq);
  EXPECT_TRUE(node_->reqlog_.empty());
  EXPECT_TRUE(node_->last_replies_.count(0));
  EXPECT_EQ(node_->last_replies_[0].result, "executed_result");
}

TEST_F(NodeTest, RequestPrimaryCreatesLogEntry) {
  RequestMsg req("op", 1000, 0);
  node_->on_request(std::move(req), dummy_conn);

  // Validate sequence and log
  EXPECT_EQ(node_->seq_num_, 1);
  ASSERT_EQ(node_->reqlog_.size(), 1);

  // Validate log entry
  auto &entry = node_->reqlog_[1];
  EXPECT_EQ(entry.view, 0);
  EXPECT_EQ(entry.seq, 1);
  EXPECT_EQ(entry.req.timestamp, 1000);
  EXPECT_EQ(entry.req.client_id, 0);
  EXPECT_EQ(entry.req.operation, "op");
  EXPECT_EQ(entry.stage, Node::ReqStage::PRE_PREPARED);
  EXPECT_TRUE(entry.has_preprepare);
  EXPECT_TRUE(entry.prepares.count(node_->id_));
}

TEST_F(NodeTest, PrimarySequenceNumberValidation) {
  // Sequence number outside the window

  // Lower bound
  node_->h_ = 5;
  node_->seq_num_ = 4; // Next is going to be 5
  RequestMsg req("op", 1000, 1);
  node_->on_request(std::move(req), salticidae::MsgNetwork<uint8_t>::conn_t{});
  EXPECT_EQ(node_->seq_num_, 4); // Should not increment

  // Upper bound
  node_->h_ = 0;
  node_->H_ = 5;
  node_->seq_num_ = 5;
  node_->on_request(std::move(req), salticidae::MsgNetwork<uint8_t>::conn_t{});
  EXPECT_EQ(node_->seq_num_, 5);
}

// ============================================================================
// PRE_PREPARE PROCESSING TESTS
// ============================================================================

TEST_F(NodeTest, PrePrepareIgnoredIfNotFromPrimary) {
  // Does not come from primary
  node_->conn_to_peer_[dummy_conn.get()] = 1;
  PrePrepareMsg m(0, 0, salticidae::get_hash("op"), RequestMsg("op", 0, 0));
  node_->on_preprepare(std::move(m), dummy_conn);

  EXPECT_TRUE(node_->reqlog_.empty());
}

TEST_F(NodeTest, PrePrepareIgnoredIfViewChanging) {
  node_->view_changing_ = true;
  PrePrepareMsg m(0, 0, salticidae::get_hash("op"), RequestMsg("op", 0, 0));
  node_->on_preprepare(std::move(m), dummy_conn);

  EXPECT_TRUE(node_->reqlog_.empty());
}

TEST_F(NodeTest, PrePrepareIgnoredIfViewMismatch) {
  node_->view_ = 1;
  PrePrepareMsg m(0, 0, salticidae::get_hash("op"), RequestMsg("op", 0, 0));
  node_->on_preprepare(std::move(m), dummy_conn);

  EXPECT_TRUE(node_->reqlog_.empty());
}

TEST_F(NodeTest, PrePrepareIgnoredIfSeqOutOfWatermarks) {
  node_->h_ = 10;
  node_->H_ = 20;
  // seq_num below h_
  PrePrepareMsg m1(node_->view_, 5, salticidae::get_hash("a"),
                   RequestMsg("a", 0, 0));
  node_->on_preprepare(std::move(m1), dummy_conn);
  // seq_num above H_
  PrePrepareMsg m2(node_->view_, 25, salticidae::get_hash("b"),
                   RequestMsg("b", 0, 0));
  node_->on_preprepare(std::move(m2), dummy_conn);

  EXPECT_TRUE(node_->reqlog_.empty());
}

TEST_F(NodeTest, PrePrepareIgnoredIfDigestConflicts) {
  // First, one pre-prepare with some digest
  PrePrepareMsg ok(node_->view_, 10, salticidae::get_hash("a"),
                   RequestMsg("a", 0, 0));
  node_->on_preprepare(std::move(ok), dummy_conn);

  // Now, conflicting digest for same seq
  PrePrepareMsg conflict(node_->view_, 10, salticidae::get_hash("b"),
                         RequestMsg("b", 0, 0));
  node_->on_preprepare(std::move(conflict), dummy_conn);

  // Entry should keep first digest
  auto &entry = node_->reqlog_[10];
  EXPECT_EQ(entry.digest, salticidae::get_hash("a"));
  EXPECT_EQ(entry.req.operation, "a");
}

TEST_F(NodeTest, PrePrepareCreatesLogEntryAndSetsPrePrepared) {
  uint64_t seq = 1;
  PrePrepareMsg m(node_->view_, seq, salticidae::get_hash("op"),
                  RequestMsg("op", 123, 42));
  node_->on_preprepare(std::move(m), dummy_conn);

  // Log entry is created and filled
  ASSERT_TRUE(node_->reqlog_.count(seq));
  auto &entry = node_->reqlog_[seq];
  EXPECT_EQ(entry.seq, seq);
  EXPECT_EQ(entry.view, node_->view_);
  EXPECT_TRUE(entry.has_preprepare);
  EXPECT_EQ(entry.stage, Node::ReqStage::PRE_PREPARED);
  EXPECT_EQ(entry.digest, salticidae::get_hash("op"));
  EXPECT_EQ(entry.req.operation, "op");
  EXPECT_EQ(entry.req.timestamp, 123);
  EXPECT_EQ(entry.req.client_id, 42);

  // Self is in prepares
  EXPECT_TRUE(entry.prepares.count(node_->id_));
}

// ============================================================================
// PREPARE PROCESSING TESTS
// ============================================================================

TEST_F(NodeTest, PrepareIgnoredIfComesFromPrimary) {
  // Send from primary
  node_->conn_to_peer_[dummy_conn.get()] = 0;
  PrepareMsg m(node_->view_, 0, salticidae::get_hash("op"), 1);
  node_->on_prepare(std::move(m), dummy_conn);

  EXPECT_TRUE(node_->reqlog_.empty());
}

TEST_F(NodeTest, PrepareIgnoredIfViewChanging) {
  // Do not send from primary (Node 1)
  node_->conn_to_peer_[dummy_conn.get()] = 1;
  node_->view_changing_ = true;
  PrepareMsg m(node_->view_, 0, salticidae::get_hash("op"), 1);
  node_->on_prepare(std::move(m), dummy_conn);

  EXPECT_TRUE(node_->reqlog_.empty());
}

TEST_F(NodeTest, PrepareIgnoredIfViewMismatch) {
  // Do not send from primary (Node 1)
  node_->conn_to_peer_[dummy_conn.get()] = 1;
  node_->view_ = 2;
  PrepareMsg m(1, 1, salticidae::get_hash("op"), 1); // wrong view
  node_->on_prepare(std::move(m), dummy_conn);

  EXPECT_TRUE(node_->reqlog_.empty());
}

TEST_F(NodeTest, PrepareIgnoredIfSeqOutOfWatermarks) {
  // Do not send from primary (Node 1)
  node_->conn_to_peer_[dummy_conn.get()] = 1;
  node_->h_ = 10;
  node_->H_ = 20;

  // Too low
  PrepareMsg low(node_->view_, 5, salticidae::get_hash("op"), 1);
  node_->on_prepare(std::move(low), dummy_conn);
  // Too high
  PrepareMsg high(node_->view_, 25, salticidae::get_hash("op"), 1);
  node_->on_prepare(std::move(high), dummy_conn);

  EXPECT_TRUE(node_->reqlog_.empty());
}

TEST_F(NodeTest, PrepareBufferedWithoutPrePrepare) {
  // Do not send from primary (Node 1)
  node_->conn_to_peer_[dummy_conn.get()] = 1;
  uint64_t seq = 1;
  PrepareMsg m(node_->view_, seq, salticidae::get_hash("op"), 1);
  // Do not send from primary (Node 1)
  node_->conn_to_peer_[dummy_conn.get()] = 1;
  node_->on_prepare(std::move(m), dummy_conn);

  ASSERT_TRUE(node_->reqlog_.count(seq));
  auto &entry = node_->reqlog_[seq];
  EXPECT_FALSE(entry.has_preprepare);
  EXPECT_EQ(entry.stage, Node::ReqStage::NONE);
  EXPECT_TRUE(entry.prepares.count(1));
}

TEST_F(NodeTest, PrepareDigestConflictPrePrepare) {
  uint64_t seq = 1;
  // First set up a pre-prepare with digest1
  RequestMsg req1("op1", 100, 1);
  uint256_t digest1 = salticidae::get_hash(req1.operation);
  PrePrepareMsg pp(node_->view_, seq, digest1, req1);
  node_->on_preprepare(std::move(pp), dummy_conn);

  auto &entry = node_->reqlog_[seq];
  ASSERT_TRUE(entry.has_preprepare);
  ASSERT_EQ(entry.digest, digest1);

  // Now send prepare with conflicting digest
  // Do not send from primary (Node 1)
  node_->conn_to_peer_[dummy_conn.get()] = 1;
  uint256_t digest2 = salticidae::get_hash("op2");
  PrepareMsg m(node_->view_, seq, digest2, 2);
  node_->on_prepare(std::move(m), dummy_conn);

  // Prepare from replica 2 should NOT be counted
  EXPECT_FALSE(entry.prepares.count(2));
  EXPECT_EQ(entry.digest, digest1);
}

TEST_F(NodeTest, TryPrepareReachesPreparedWhenQuorumMet) {
  uint64_t seq = 1;
  // Pre-prepare already done
  RequestMsg req("op", 100, 1);
  uint256_t digest = salticidae::get_hash(req.operation);
  PrePrepareMsg pp(node_->view_, seq, digest, req);
  node_->on_preprepare(std::move(pp), dummy_conn);

  auto &entry = node_->reqlog_[seq];
  ASSERT_EQ(entry.stage, Node::ReqStage::PRE_PREPARED);
  ASSERT_TRUE(entry.has_preprepare);
  // on_preprepare already inserted self into prepares
  ASSERT_TRUE(entry.prepares.count(node_->id_));

  // Reach 2f + 1
  // Do not send from primary (Node 1)
  node_->conn_to_peer_[dummy_conn.get()] = 1;
  for (uint64_t i = 0; i < 2 * MAX_FAULTY + 1; i++) {
    PrepareMsg p(node_->view_, seq, digest, i);
    // Do not send from primary (Node 1)
    node_->conn_to_peer_[dummy_conn.get()] = 1;
    node_->on_prepare(std::move(p), dummy_conn);
  }

  // Now prepares.size() should be >= 2f+1 = 3, and stage PREPARED
  EXPECT_GE(entry.prepares.size(), 2 * node_->f_ + 1);
  EXPECT_EQ(entry.stage, Node::ReqStage::PREPARED);
  // try_prepare should have inserted self-commit
  EXPECT_TRUE(entry.commits.count(node_->id_));
}

TEST_F(NodeTest, TryPrepareDoesNotTriggerIfNotPrePrepared) {
  auto &entry = node_->reqlog_[0];
  // Do not send from primary (Node 1)
  node_->conn_to_peer_[dummy_conn.get()] = 1;

  entry.has_preprepare = false;
  entry.stage = Node::ReqStage::NONE;
  // Reach 2f + 1
  for (uint64_t i = 0; i < 2 * MAX_FAULTY + 1; i++) {
    entry.prepares.insert(i);
  }
  node_->try_prepare(entry);

  // Even with enough prepares, without pre-prepare it must not move
  EXPECT_EQ(entry.stage, Node::ReqStage::NONE);
  EXPECT_TRUE(entry.commits.empty());
}

TEST_F(NodeTest, TryPrepareDoesNotTriggerIfNoQuorum) {
  auto &entry = node_->reqlog_[0];
  // Do not send from primary (Node 1)
  node_->conn_to_peer_[dummy_conn.get()] = 1;

  entry.has_preprepare = false;
  entry.stage = Node::ReqStage::NONE;
  // Just one entry
  entry.prepares.insert(0);
  node_->try_prepare(entry);

  // Even with enough prepares, without pre-prepare it must not move
  EXPECT_EQ(entry.stage, Node::ReqStage::NONE);
  EXPECT_TRUE(entry.commits.empty());
}

TEST_F(NodeTest, OutOfOrderPrePrepareTriggersTryPrepare) {
  uint64_t seq = 1;
  // Do not send from primary (Node 1)
  node_->conn_to_peer_[dummy_conn.get()] = 1;
  RequestMsg req("op", 123, 42);
  uint256_t digest = salticidae::get_hash(req.operation);

  // Send 2f
  for (uint64_t i = 1; i < 2 * MAX_FAULTY + 1; i++) {
    PrepareMsg m(node_->view_, seq, digest, i);
    // Do not send from primary (Node 1)
    node_->conn_to_peer_[dummy_conn.get()] = 1;
    node_->on_prepare(std::move(m), dummy_conn);
  }

  // Out of order PrePreare
  // Now send from primary
  node_->conn_to_peer_[dummy_conn.get()] = 0;
  PrePrepareMsg m(node_->view_, seq, digest, req);
  node_->on_preprepare(std::move(m), dummy_conn);

  // Check that try prepare executes
  auto &entry = node_->reqlog_[seq];
  EXPECT_EQ(entry.prepares.size(), 2 * MAX_FAULTY + 1);
  EXPECT_EQ(entry.stage, Node::ReqStage::PREPARED);
}

// ============================================================================
// COMMIT PROCESSING TESTS
// ============================================================================

TEST_F(NodeTest, CommitIgnoredIfViewChanging) {
  node_->view_changing_ = true;
  CommitMsg m(node_->view_, 1, salticidae::get_hash("op"), 1);
  node_->on_commit(std::move(m), dummy_conn);
  EXPECT_TRUE(node_->reqlog_.empty());
}

TEST_F(NodeTest, CommitIgnoredIfViewMismatch) {
  node_->view_ = 1;
  CommitMsg m(42, 1, salticidae::get_hash("op"), 0);
  node_->on_commit(std::move(m), dummy_conn);
  EXPECT_TRUE(node_->reqlog_.empty());
}

TEST_F(NodeTest, CommitIgnoredIfSeqOutOfWatermarks) {
  node_->h_ = 10;
  node_->H_ = 20;
  // Too low
  CommitMsg m1(node_->view_, 5, salticidae::get_hash("op"), 0);
  node_->on_commit(std::move(m1), dummy_conn);
  // Too high
  CommitMsg m2(node_->view_, 21, salticidae::get_hash("op"), 0);
  node_->on_commit(std::move(m2), dummy_conn);
  EXPECT_TRUE(node_->reqlog_.empty());
}

TEST_F(NodeTest, CommitIgnoredIfDigestMismatch) {
  uint64_t seq = 1;
  auto &entry = node_->reqlog_[seq];
  entry.digest = salticidae::get_hash("op1");
  CommitMsg wrong(node_->view_, seq, salticidae::get_hash("op2"), 2);
  node_->on_commit(std::move(wrong), dummy_conn);
  EXPECT_TRUE(entry.commits.empty());
}

TEST_F(NodeTest, CommitStores) {
  uint64_t seq = 1;
  auto &entry = node_->reqlog_[seq];
  entry.stage = Node::ReqStage::PREPARED;
  entry.digest = salticidae::get_hash("op");

  CommitMsg c1(node_->view_, seq, entry.digest, 0);
  node_->on_commit(std::move(c1), dummy_conn);

  EXPECT_TRUE(entry.commits.count(0));
}

TEST_F(NodeTest, TryCommitDoesNotTriggerIfNotPrepared) {
  auto &entry = node_->reqlog_[0];
  entry.stage = Node::ReqStage::PRE_PREPARED; // Not PREPARED yet
  entry.digest = salticidae::get_hash("op");

  // Insert enough commits
  // Reach 2f + 1
  for (uint64_t i = 0; i < 2 * MAX_FAULTY + 1; i++) {
    entry.commits.insert(i);
  }
  node_->try_commit(entry);

  EXPECT_EQ(entry.stage, Node::ReqStage::PRE_PREPARED); // No transition!
}

TEST_F(NodeTest, TryCommitDoesNotTriggerIfNoQuorum) {
  auto &entry = node_->reqlog_[0];
  entry.stage = Node::ReqStage::PREPARED;
  entry.digest = salticidae::get_hash("op");
  entry.commits.insert(0); // self only

  node_->try_commit(entry);

  EXPECT_EQ(entry.stage, Node::ReqStage::PREPARED); // Not enough commits
}

TEST_F(NodeTest, CommitWithQuorumTriggersTryCommit) {
  auto &entry = node_->reqlog_[0];
  entry.stage = Node::ReqStage::PREPARED;
  entry.digest = salticidae::get_hash("op");

  // Insert enough commits
  // Reach 2f + 1
  for (uint64_t i = 0; i < 2 * MAX_FAULTY + 1; i++) {
    entry.commits.insert(i);
  }

  node_->try_commit(entry);

  EXPECT_EQ(entry.stage, Node::ReqStage::COMMITTED);
}

TEST_F(NodeTest, CommitWithQuorumTriggersExecution) {
  uint64_t seq = 1;
  auto &entry = node_->reqlog_[seq];
  entry.stage = Node::ReqStage::PREPARED;
  entry.digest = salticidae::get_hash("op");
  entry.req = RequestMsg("op", 123, 42);

  // Set up execute mock
  EXPECT_CALL(*service, execute("op")).Times(1).WillOnce(Return("executed_do"));

  // Simulate commits to reach quorum and PREPARED state
  // 2f + 1
  for (uint64_t i = 0; i < 2 * MAX_FAULTY + 1; i++) {
    CommitMsg commit(node_->view_, seq, entry.digest, i);
    node_->on_commit(std::move(commit), dummy_conn);
  }

  EXPECT_EQ(entry.stage, Node::ReqStage::COMMITTED);
  EXPECT_EQ(node_->last_exec_, seq);
  EXPECT_TRUE(node_->last_replies_.count(entry.req.client_id));
  EXPECT_EQ(node_->last_replies_[entry.req.client_id].result, "executed_do");
}

// ============================================================================
// STATE TRANSITION TESTS
// ============================================================================

TEST_F(NodeTest, RequestStateTransitionsPipeline) {

  // Simulate the entire pipeline
  uint64_t seq = 1;
  auto &entry = node_->reqlog_[seq];

  // Initial state
  EXPECT_EQ(entry.stage, Node::ReqStage::NONE);

  // Simulate pre-prepare
  entry.has_preprepare = true;
  entry.stage = Node::ReqStage::PRE_PREPARED;
  EXPECT_EQ(entry.stage, Node::ReqStage::PRE_PREPARED);

  // Add prepares to reach quorum
  // Reach 2f + 1
  for (uint64_t i = 0; i < 2 * MAX_FAULTY + 1; i++) {
    entry.prepares.insert(i);
  }
  node_->try_prepare(entry);
  EXPECT_EQ(entry.stage, Node::ReqStage::PREPARED);

  // Add commits to reach quorum
  // Reach 2f + 1
  for (uint64_t i = 0; i < 2 * MAX_FAULTY + 1; i++) {
    entry.commits.insert(i);
  }
  node_->try_commit(entry);
  EXPECT_EQ(entry.stage, Node::ReqStage::COMMITTED);

  // Execution should advance
  node_->try_execute();
  EXPECT_EQ(node_->last_exec_, seq);
}

// ============================================================================
// CHECKPOINT TESTS
// ============================================================================

TEST_F(NodeTest, MakeCheckpointTrigger) {
  node_->last_exec_ = 50;
  uint256_t digest = salticidae::get_hash("digest50");

  // Setup get_checkpoint_digest
  EXPECT_CALL(*service, get_checkpoint_digest())
      .Times(1)
      .WillOnce(Return(digest));

  node_->make_checkpoint();

  ASSERT_TRUE(node_->checkpoints_.count(50));
  auto &cp = node_->checkpoints_[50];
  ASSERT_FALSE(cp.votes.empty());
  EXPECT_TRUE(cp.votes[digest].count(node_->id_));
}

TEST_F(NodeTest, CheckpointIgnoredIfOutsideWindow) {
  node_->h_ = 100;
  CheckpointMsg m(90, salticidae::get_hash("op"), 1);
  node_->on_checkpoint(std::move(m), dummy_conn);

  // 90 < 100
  EXPECT_FALSE(node_->checkpoints_.count(90));
}

TEST_F(NodeTest, CheckpointAddsVote) {
  uint64_t seq = 150;
  uint256_t digest = salticidae::get_hash("state150");

  CheckpointMsg m(seq, digest, 1);
  node_->on_checkpoint(std::move(m), dummy_conn);

  ASSERT_TRUE(node_->checkpoints_.count(seq));
  auto &cp = node_->checkpoints_[seq];
  EXPECT_TRUE(cp.votes[digest].count(1));
  EXPECT_FALSE(cp.stable);
}

TEST_F(NodeTest, CheckpointTriggersStability) {
  node_->f_ = 1; // n=4, quorum=3
  uint64_t seq = 200;
  uint256_t digest = salticidae::get_hash("stable_state");

  // Pre-fill some old logs to test GC
  node_->reqlog_[100].seq = 100;
  node_->reqlog_[250].seq = 250; // Should survive

  // Add 2f existing votes
  for (uint64_t i = 0; i < 2 * MAX_FAULTY; i++) {
    node_->checkpoints_[seq].votes[digest].insert(i);
  }

  // Sending last vote (from replica 2)
  CheckpointMsg m(seq, digest, 2);
  node_->on_checkpoint(std::move(m), dummy_conn);

  // Verify Stability
  auto &cp = node_->checkpoints_[seq];
  EXPECT_TRUE(cp.stable);
  EXPECT_EQ(node_->last_stable_digest_, digest);

  // Verify Watermarks Advanced
  EXPECT_EQ(node_->h_, seq);
  EXPECT_EQ(node_->H_, seq + L);

  // Verify GC
  EXPECT_FALSE(node_->reqlog_.count(100));
  EXPECT_TRUE(node_->reqlog_.count(250));
}

TEST_F(NodeTest, AdvanceWatermarks) {
  EXPECT_EQ(node_->h_, 0);
  EXPECT_EQ(node_->H_, L);

  node_->advance_watermarks(100);
  EXPECT_EQ(node_->h_, 100);
  EXPECT_EQ(node_->H_, 100 + L);
}

TEST_F(NodeTest, GarbageCollectRemovesOldState) {
  node_->h_ = 50;

  // Logs to be removed
  node_->reqlog_[10];
  node_->reqlog_[50];
  // Logs to keep
  node_->reqlog_[51];

  // Checkpoints to be removed
  node_->checkpoints_[10];
  node_->checkpoints_[49];
  // Checkpoints to keep (>= h_)
  node_->checkpoints_[50];
  node_->checkpoints_[60];

  node_->garbage_collect();

  // Verify Logs
  EXPECT_FALSE(node_->reqlog_.count(10));
  EXPECT_FALSE(node_->reqlog_.count(50));
  EXPECT_TRUE(node_->reqlog_.count(51));

  // Verify Checkpoints
  EXPECT_FALSE(node_->checkpoints_.count(10));
  EXPECT_FALSE(node_->checkpoints_.count(49));
  EXPECT_TRUE(node_->checkpoints_.count(50));
  EXPECT_TRUE(node_->checkpoints_.count(60));
}

// ============================================================================
// VIEW CHANGE TESTS
// ============================================================================

TEST_F(NodeTest, StartViewChangeUpdatesState) {
  node_->view_ = 0;

  // Setup a prepared request to be included in P set
  uint64_t seq = 1;
  auto &entry = node_->reqlog_[seq];
  entry.stage = Node::ReqStage::PREPARED;
  entry.view = 0;
  entry.seq = seq;
  entry.digest = salticidae::get_hash("op");

  node_->start_view_change();

  // 1. View should not change inmediatly
  EXPECT_EQ(node_->view_, 0);
  // 2. State should be view_changing
  EXPECT_TRUE(node_->view_changing_);
  // 3. Timeout count should increment
  EXPECT_EQ(node_->view_change_timeout_count_, 1);
}

TEST_F(NodeTest, ViewChangeIgnoredIfSmallerNextView) {
  // Node 0 is NOT primary for View 5 (5 % 4 = 1)
  node_->view_ = NUM_REPLICAS;

  ViewChangeMsg vc(NUM_REPLICAS - 1, 0, 2, {}, {});
  node_->on_viewchange(std::move(vc), dummy_conn);

  // Should not store it
  EXPECT_TRUE(node_->view_change_store_[NUM_REPLICAS].empty());
  EXPECT_EQ(node_->view_, NUM_REPLICAS);
}

TEST_F(NodeTest, ViewChangeIgnoredIfIsDiffPrimary) {
  // Node 0 is NOT primary for View 5 (5 % 4 = 1)
  node_->view_ = 5;

  ViewChangeMsg vc(5, 0, 2, {}, {});
  node_->on_viewchange(std::move(vc), dummy_conn);

  // Should not store it
  EXPECT_TRUE(node_->view_change_store_[5].empty());
  EXPECT_EQ(node_->view_, 5);
}

TEST_F(NodeTest, ViewChangeIgnoredIfNotQuorum) {
  // We simulate being the primary for View 4 (Our goal)
  node_->id_ = 0;
  node_->view_ = 3;

  // Construct 3 ViewChange messages for View 4
  // We want to test O-set calculation.
  // Scenario:
  // - Last stable checkpoint: 50
  // - Replica 1 has Prepare for Seq 55 (View 2)
  // - Replica 2 has Prepare for Seq 55 (View 3) -> Higher view should win
  // - Replica 3 has no prepares
  // - Gap at Seq 51-54 should be filled with NULL

  uint64_t target_view = 4;
  uint64_t stable_ckpt = 50;

  // VC 1 (From self/Node 0)
  node_->start_view_change();

  // VC 2 (From Node 1) - Has prepare at 55, view 2
  PrepareProof p1(2, 55, salticidae::get_hash("op"));
  ViewChangeMsg vc1(target_view, stable_ckpt, 1, {}, {p1});
  // Inject Only one message
  node_->on_viewchange(std::move(vc1), dummy_conn);

  // Should not store it
  EXPECT_EQ(node_->view_change_store_[target_view].size(), 2);
  EXPECT_TRUE(node_->view_changing_);
  EXPECT_EQ(node_->view_, 3);
  EXPECT_NE(node_->view_change_timeout_count_, 0);
}

TEST_F(NodeTest, ViewChangeAggregatesAndCreatesNewView) {
  // We simulate being the primary for View 4 (Our goal)
  node_->id_ = 0;
  node_->view_ = 3;

  // Construct 3 ViewChange messages for View 4
  // We want to test O-set calculation.
  // Scenario:
  // - Last stable checkpoint: 50
  // - Replica 1 has Prepare for Seq 55 (View 2)
  // - Replica 2 has Prepare for Seq 55 (View 3) -> Higher view should win
  // - Replica 3 has no prepares
  // - Gap at Seq 51-54 should be filled with NULL

  uint64_t target_view = 4;
  uint64_t stable_ckpt = 50;

  // VC 1 (From self/Node 0)
  node_->start_view_change();

  // VC 2 (From Node 1) - Has prepare at 55, view 2
  PrepareProof p1(2, 55, salticidae::get_hash("op"));
  ViewChangeMsg vc1(target_view, stable_ckpt, 1, {}, {p1});

  // VC 3 (From Node 2) - Has prepare at 55, view 3
  PrepareProof p2(3, 55, salticidae::get_hash("op"));
  ViewChangeMsg vc2(target_view, stable_ckpt, 2, {}, {p2});

  // Inject messages
  node_->on_viewchange(std::move(vc1), dummy_conn);

  // Not enough yet (size 2)
  EXPECT_TRUE(node_->view_changing_);

  // Inject 3rd message -> Should trigger NewView creation
  node_->on_viewchange(std::move(vc2), dummy_conn);

  EXPECT_FALSE(node_->view_changing_);
  EXPECT_EQ(node_->view_, target_view);

  // Verify internal state was reset
  EXPECT_EQ(node_->view_change_timeout_count_, 0);
}

TEST_F(NodeTest, NewViewIgnoredIfNotFromPrimary) {
  node_->view_ = 3;
  uint64_t new_view = 4;
  node_->start_view_change();
  // Create Valid V set (3 VCs)
  std::vector<ViewChangeMsg> V;
  for (uint64_t i = 0; i < 2 * MAX_FAULTY + 1; i++) {
    V.emplace_back(new_view, 0, i, std::vector<CheckpointMsg>{},
                   std::vector<PrepareProof>{});
  }

  // Add one proof
  V[0].prepared_proofs.push_back(
      PrepareProof(2, 1, salticidae::get_hash("op")));

  // Now max_s=1, min_s=0. O set must have size 1.
  std::vector<PrePrepareMsg> O;
  RequestMsg req("op", 0, 0);
  O.emplace_back(new_view, 1, salticidae::get_hash("op"), req);

  NewViewMsg nv(new_view, V, O);

  // Does not come from primary
  node_->conn_to_peer_[dummy_conn.get()] = 1;
  node_->on_newview(std::move(nv), dummy_conn);

  // State does not update updated
  EXPECT_EQ(node_->view_, 3);
  EXPECT_TRUE(node_->view_changing_);
  EXPECT_NE(node_->view_change_timeout_count_, 0);

  // PrePrepare not processed
  ASSERT_FALSE(node_->reqlog_.count(0));
}

TEST_F(NodeTest, NewViewIgnoredIfNotViewChanging) {
  node_->view_ = 3;
  uint64_t new_view = 4;
  node_->view_changing_ = false;
  // Create Valid V set (3 VCs)
  std::vector<ViewChangeMsg> V;
  for (uint64_t i = 0; i < 2 * MAX_FAULTY + 1; i++) {
    V.emplace_back(new_view, 0, i, std::vector<CheckpointMsg>{},
                   std::vector<PrepareProof>{});
  }

  // Add one proof
  V[0].prepared_proofs.push_back(
      PrepareProof(2, 1, salticidae::get_hash("op")));

  // Now max_s=1, min_s=0. O set must have size 1.
  std::vector<PrePrepareMsg> O;
  RequestMsg req("op", 0, 0);
  O.emplace_back(new_view, 1, salticidae::get_hash("op"), req);

  NewViewMsg nv(new_view, V, O);

  // Comes from primary
  node_->conn_to_peer_[dummy_conn.get()] = 0;
  node_->on_newview(std::move(nv), dummy_conn);

  // State does not update
  EXPECT_EQ(node_->view_, 3);
  EXPECT_FALSE(node_->view_changing_);
  EXPECT_EQ(node_->view_change_timeout_count_, 0);

  // PrePrepare not processed
  ASSERT_FALSE(node_->reqlog_.count(0));
}

TEST_F(NodeTest, NewViewIgnoredIfSmallerNextView) {
  node_->view_ = 3;
  uint64_t new_view = 2;
  node_->start_view_change();
  // Create Valid V set (3 VCs)
  std::vector<ViewChangeMsg> V;
  for (uint64_t i = 0; i < 2 * MAX_FAULTY + 1; i++) {
    V.emplace_back(new_view, 0, i, std::vector<CheckpointMsg>{},
                   std::vector<PrepareProof>{});
  }

  // Add one proof
  V[0].prepared_proofs.push_back(
      PrepareProof(2, 1, salticidae::get_hash("op")));

  // Now max_s=1, min_s=0. O set must have size 1.
  std::vector<PrePrepareMsg> O;
  RequestMsg req("op", 0, 0);
  O.emplace_back(new_view, 1, salticidae::get_hash("op"), req);

  NewViewMsg nv(new_view, V, O);

  // Comes from primary
  node_->conn_to_peer_[dummy_conn.get()] = 0;
  node_->on_newview(std::move(nv), dummy_conn);

  // State does not update
  EXPECT_EQ(node_->view_, 3);
  EXPECT_TRUE(node_->view_changing_);
  EXPECT_NE(node_->view_change_timeout_count_, 0);

  // PrePrepare not processed
  ASSERT_FALSE(node_->reqlog_.count(0));
}

TEST_F(NodeTest, NewViewIgnoredIfInvalidVSet) {
  node_->view_ = 3;
  uint64_t new_view = 4;
  node_->start_view_change();
  // Create invalid Valid V set (3 VCs)
  std::vector<ViewChangeMsg> V;
  V.emplace_back(new_view, 0, 0, std::vector<CheckpointMsg>{},
                 std::vector<PrepareProof>{});

  // Add one proof
  V[0].prepared_proofs.push_back(
      PrepareProof(2, 1, salticidae::get_hash("op")));

  // Now max_s=1, min_s=0. O set must have size 1.
  std::vector<PrePrepareMsg> O;
  RequestMsg req("op", 0, 0);
  O.emplace_back(new_view, 1, salticidae::get_hash("op"), req);

  NewViewMsg nv(new_view, V, O);

  // Comes from primary
  node_->conn_to_peer_[dummy_conn.get()] = 0;
  node_->on_newview(std::move(nv), dummy_conn);

  // State does not update
  EXPECT_EQ(node_->view_, 3);
  EXPECT_TRUE(node_->view_changing_);
  EXPECT_NE(node_->view_change_timeout_count_, 0);

  // PrePrepare not processed
  ASSERT_FALSE(node_->reqlog_.count(0));
}

TEST_F(NodeTest, NewViewIgnoredIfInvalidOset) {
  node_->view_ = 3;
  node_->start_view_change();

  // V set implies range [0, 5]
  std::vector<ViewChangeMsg> V;
  ViewChangeMsg vc(4, 0, 1, {}, {});
  vc.prepared_proofs.push_back(
      PrepareProof(2, 5, salticidae::get_hash("a"))); // max_s=5
  V.push_back(vc);
  // Pad quorum
  V.push_back(ViewChangeMsg(4, 0, 2, {}, {}));
  V.push_back(ViewChangeMsg(4, 0, 3, {}, {}));

  // O set is empty (Invalid, should have 5 entries)
  std::vector<PrePrepareMsg> O;

  NewViewMsg nv(4, V, O);

  node_->on_newview(std::move(nv), dummy_conn);

  // State does not update
  EXPECT_EQ(node_->view_, 3);
  EXPECT_TRUE(node_->view_changing_);
  EXPECT_NE(node_->view_change_timeout_count_, 0);

  // PrePrepare not processed
  ASSERT_FALSE(node_->reqlog_.count(0));
}

TEST_F(NodeTest, NewViewUpdatesStateAndProcessesOset) {
  // Setup: Node is stuck in View 3, changing to View 4
  node_->view_ = 3;
  uint64_t new_view = 4;
  node_->view_changing_ = true;

  // Create Valid V set (3 VCs)
  std::vector<ViewChangeMsg> V;
  for (uint64_t i = 0; i < 2 * MAX_FAULTY + 1; i++) {
    V.emplace_back(new_view, 0, i, std::vector<CheckpointMsg>{},
                   std::vector<PrepareProof>{});
  }

  // Add one proof
  V[0].prepared_proofs.push_back(
      PrepareProof(2, 1, salticidae::get_hash("op")));

  // Now max_s=1, min_s=0. O set must have size 1.
  std::vector<PrePrepareMsg> O;
  RequestMsg req("op", 0, 0);
  O.emplace_back(new_view, 1, salticidae::get_hash("op"), req);

  NewViewMsg nv(new_view, V, O);

  // Send from new primary
  node_->conn_to_peer_[dummy_conn.get()] = 0;
  node_->on_newview(std::move(nv), dummy_conn);

  // State Updated
  EXPECT_EQ(node_->view_, new_view);
  EXPECT_FALSE(node_->view_changing_);
  EXPECT_EQ(node_->view_change_timeout_count_, 0);

  // PrePrepare processed (Log entry created)
  ASSERT_TRUE(node_->reqlog_.count(1));
  EXPECT_EQ(node_->reqlog_[1].view, new_view);
  EXPECT_EQ(node_->reqlog_[1].stage, Node::ReqStage::PRE_PREPARED);
}

// ============================================================================
// ERROR AND EDGE CASE TESTS
// ============================================================================

TEST_F(NodeTest, ViewChangeDuringNormalOperation) {
  // Start normal operation
  RequestMsg req("op1", 1000, 1);
  node_->on_request(std::move(req), salticidae::MsgNetwork<uint8_t>::conn_t{});

  // Trigger view change mid-operation
  node_->start_view_change();

  // New requests should be ignored
  RequestMsg new_req("op2", 1001, 1);
  node_->on_request(std::move(new_req),
                    salticidae::MsgNetwork<uint8_t>::conn_t{});
  EXPECT_EQ(node_->reqlog_.size(), 1); // Only original request
}

TEST_F(NodeTest, LogOverflowProtection) {
  node_->H_ = 10; // Small window for testing

  // Fill log to capacity
  for (uint64_t i = 1; i <= 10; i++) {
    RequestMsg req("op" + std::to_string(i), 1000 + i, 1);
    if (node_->is_primary()) {
      node_->on_request(std::move(req),
                        salticidae::MsgNetwork<uint8_t>::conn_t{});
    }
  }

  // Additional requests should be dropped
  uint64_t initial_size = node_->reqlog_.size();
  RequestMsg overflow_req("overflow", 2000, 1);
  node_->on_request(std::move(overflow_req),
                    salticidae::MsgNetwork<uint8_t>::conn_t{});
  EXPECT_EQ(node_->reqlog_.size(), initial_size);
}

// ============================================================================
// Helpers
// ============================================================================

TEST_F(NodeTest, ComesFromPrimary) {
  node_->conn_to_peer_[dummy_conn.get()] = 0;
  ASSERT_TRUE(node_->comes_from_primary(dummy_conn));
  node_->conn_to_peer_[dummy_conn.get()] = 1;
  ASSERT_FALSE(node_->comes_from_primary(dummy_conn));
}

TEST_F(NodeTest, IsWaitingForRequest) {
  ASSERT_FALSE(node_->is_waiting_for_request());
  RequestMsg req("op", 1000, 0);
  node_->on_request(std::move(req), dummy_conn);
  ASSERT_TRUE(node_->is_waiting_for_request());
}

TEST_F(NodeTest, StartTimerIfNotRunning) {
  ASSERT_FALSE(node_->timer_running_);
  node_->view_changing_ = true;
  node_->start_timer_if_not_running();
  ASSERT_FALSE(node_->timer_running_);
  node_->view_changing_ = false;
  node_->start_timer_if_not_running();
  ASSERT_TRUE(node_->timer_running_);
}
TEST_F(NodeTest, StopTimer) {
  node_->start_timer_if_not_running();
  ASSERT_TRUE(node_->timer_running_);
  node_->stop_timer();
  ASSERT_FALSE(node_->timer_running_);
}

TEST_F(NodeTest, ManageTimer) {
  node_->start_timer_if_not_running();
  node_->manage_timer();
  ASSERT_FALSE(node_->timer_running_);
}
} // namespace testing
} // namespace pbft

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
