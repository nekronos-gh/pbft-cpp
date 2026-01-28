#include <future>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#define PBFT_TESTING_ACCESS public
#include "pbft/client.hh"
#include "pbft/messages.hh"

using ::testing::Return;

namespace pbft {
namespace testing {

class ClientTest : public ::testing::Test {
protected:
  void SetUp() override {
    // n=4, f=1
    ClientConfig config = {4, 200, std::nullopt};
    client_ = std::make_unique<Client>(100, config);
  }

  std::unique_ptr<Client> client_;
  salticidae::MsgNetwork<uint8_t>::conn_t dummy_conn;
};

TEST_F(ClientTest, InitializationCorrect) {
  EXPECT_EQ(client_->id_, 100);
  EXPECT_EQ(client_->n_, 4);
  EXPECT_EQ(client_->f_, 1);
  EXPECT_EQ(client_->current_view_, 1);
  EXPECT_EQ(client_->timestamp_, 0);
  EXPECT_TRUE(client_->inflight_requests_.empty());
}

TEST_F(ClientTest, InvokeAsyncCreatesInflightRequest) {
  auto fut = client_->invoke_async("op1");

  EXPECT_EQ(client_->timestamp_, 1);
  ASSERT_EQ(client_->inflight_requests_.size(), 1);

  auto &req = client_->inflight_requests_[1];
  EXPECT_EQ(req.timestamp, 1);
  EXPECT_TRUE(req.seen_replicas.empty());
  EXPECT_TRUE(req.result_counts.empty());
}

TEST_F(ClientTest, OnReplyIgnoredIfWrongClient) {
  client_->invoke_async("op");

  // Reply for different client
  ReplyMsg reply(0, 1, 999, 0, "res");
  client_->on_reply(std::move(reply), dummy_conn);

  auto &req = client_->inflight_requests_[1];
  EXPECT_TRUE(req.result_counts.empty());
}

TEST_F(ClientTest, OnReplyIgnoredIfUnknownTimestamp) {
  // Reply for non-existent request
  ReplyMsg reply(0, 999, 100, 0, "res");
  client_->on_reply(std::move(reply), dummy_conn);

  EXPECT_TRUE(client_->inflight_requests_.empty());
}

TEST_F(ClientTest, OnReplyUpdatesView) {
  client_->invoke_async("op");

  // Reply with higher view
  ReplyMsg reply(5, 1, 100, 0, "res");
  client_->on_reply(std::move(reply), dummy_conn);

  EXPECT_EQ(client_->current_view_, 5);
}

TEST_F(ClientTest, OnReplyCountsVotes) {
  client_->invoke_async("op"); // timestamp 1

  // Replica 0 replies
  ReplyMsg r0(0, 1, 100, 0, "result");
  client_->on_reply(std::move(r0), dummy_conn);

  auto &req = client_->inflight_requests_[1];
  EXPECT_EQ(req.seen_replicas.size(), 1);
  EXPECT_TRUE(req.seen_replicas.count(0));
  EXPECT_EQ(req.result_counts["result"], 1);
}

TEST_F(ClientTest, OnReplyDuplicatesIgnored) {
  client_->invoke_async("op");

  // Replica 0 replies twice
  ReplyMsg r0(0, 1, 100, 0, "result");
  client_->on_reply(std::move(r0), dummy_conn);

  ReplyMsg r0_dup(0, 1, 100, 0, "result");
  client_->on_reply(std::move(r0_dup), dummy_conn);

  auto &req = client_->inflight_requests_[1];
  EXPECT_EQ(req.seen_replicas.size(), 1);
  EXPECT_EQ(req.result_counts["result"], 1);
}

TEST_F(ClientTest, OnReplyCompletesRequestOnQuorum) {
  auto fut = client_->invoke_async("op");

  // f=1, need f+1 = 2 replies

  // Replica 0
  ReplyMsg r0(0, 1, 100, 0, "result");
  client_->on_reply(std::move(r0), dummy_conn);

  // Check not ready yet
  EXPECT_EQ(fut.wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout);

  // Replica 1
  ReplyMsg r1(0, 1, 100, 1, "result");
  client_->on_reply(std::move(r1), dummy_conn);

  // Should be ready now
  ASSERT_EQ(fut.wait_for(std::chrono::milliseconds(0)),
            std::future_status::ready);
  EXPECT_EQ(fut.get(), "result");

  // Request should be removed from map
  EXPECT_TRUE(client_->inflight_requests_.empty());
}

TEST_F(ClientTest, OnReplyDifferentResultsDoNotFormQuorum) {
  auto fut = client_->invoke_async("op");

  // Replica 0 says "A"
  ReplyMsg r0(0, 1, 100, 0, "A");
  client_->on_reply(std::move(r0), dummy_conn);

  // Replica 1 says "B"
  ReplyMsg r1(0, 1, 100, 1, "B");
  client_->on_reply(std::move(r1), dummy_conn);

  // Total replies 2, but max votes for any result is 1. Quorum is 2.
  EXPECT_EQ(fut.wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout);

  auto &req = client_->inflight_requests_[1];
  EXPECT_EQ(req.result_counts["A"], 1);
  EXPECT_EQ(req.result_counts["B"], 1);
}

} // namespace testing
} // namespace pbft
