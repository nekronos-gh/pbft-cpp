#include "pbft/metrics.hh"

using namespace prometheus;

namespace pbft {

static Histogram::BucketBoundaries default_buckets() {
  return {0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0};
}

// Buckets for message sizes (in bytes)
// 64B, 128B, 256B, 512B, 1KB, 2KB, 4KB, 8KB, 16KB, 32KB, 64KB, 1MB, 10MB
static Histogram::BucketBoundaries size_buckets() {
  return {64,   128,   256,   512,   1024,    2048,    4096,
          8192, 16384, 32768, 65536, 1048576, 10485760};
}

Metrics::Metrics(const std::string &bind_addr) {
  exposer_ = std::make_unique<Exposer>(bind_addr);
  registry_ = std::make_shared<Registry>();
  exposer_->RegisterCollectable(registry_);

  msg_family_ = &BuildCounter()
                     .Name("pbft_messages_total")
                     .Help("PBFT messages by type")
                     .Register(*registry_);

  bytes_family_ = &BuildCounter()
                       .Name("pbft_network_bytes_total")
                       .Help("Total network bytes sent/received")
                       .Register(*registry_);

  vc_family_ = &BuildCounter()
                    .Name("pbft_view_changes_timeout")
                    .Help("View changes by timeout")
                    .Register(*registry_);

  retrans_family_ = &BuildCounter()
                         .Name("pbft_retransmissions_total")
                         .Help("Total retransmissions by type")
                         .Register(*registry_);

  gauge_family_ = &BuildGauge()
                       .Name("pbft_status")
                       .Help("PBFT status gauges")
                       .Register(*registry_);

  phase_family_ = &BuildHistogram()
                       .Name("pbft_phase_seconds")
                       .Help("Latency per PBFT phase")
                       .Register(*registry_);

  size_family_ = &BuildHistogram()
                      .Name("pbft_message_size_bytes")
                      .Help("Message size distribution")
                      .Register(*registry_);

  // Initialize common labels for counters to ensure they appear even if 0
  std::vector<std::string> types = {"request",    "reply",   "preprepare",
                                    "prepare",    "commit",  "checkpoint",
                                    "viewchange", "newview", "handshake"};

  for (const auto &t : types) {
    msg_[{t, true}] = &msg_family_->Add({{"type", t}, {"direction", "tx"}});
    msg_[{t, false}] = &msg_family_->Add({{"type", t}, {"direction", "rx"}});

    // Bytes (RX/TX)
    bytes_[{t, true}] = &bytes_family_->Add({{"type", t}, {"direction", "tx"}});
    bytes_[{t, false}] =
        &bytes_family_->Add({{"type", t}, {"direction", "rx"}});

    // Retransmissions
    retrans_[t] = &retrans_family_->Add({{"type", t}});

    // Sizes
    size_[t] = &size_family_->Add({{"type", t}}, size_buckets());
  }

  vc_["timeout"] = &vc_family_->Add({{"reason", "timeout"}});

  view_ = &gauge_family_->Add({{"name", "current_view"}});
  inflight_ = &gauge_family_->Add({{"name", "inflight_requests"}});
  low_watermark_ = &gauge_family_->Add({{"name", "low_watermark"}});
  high_watermark_ = &gauge_family_->Add({{"name", "high_watermark"}});

  phase_["prepare"] =
      &phase_family_->Add({{"phase", "prepare"}}, default_buckets());
  phase_["commit"] =
      &phase_family_->Add({{"phase", "commit"}}, default_buckets());
  phase_["preprepare"] =
      &phase_family_->Add({{"phase", "preprepare"}}, default_buckets());
}

void Metrics::inc_msg(const std::string &type, size_t size, bool is_tx) {
  auto it_msg = msg_.find({type, is_tx});
  if (it_msg != msg_.end())
    it_msg->second->Increment();

  inc_bytes(type, size, is_tx);
  observe_msg_size(type, size);
}

void Metrics::observe_msg_size(const std::string &type, size_t bytes) {
  auto it = size_.find(type);
  if (it != size_.end())
    it->second->Observe(static_cast<double>(bytes));
}

void Metrics::inc_bytes(const std::string &type, size_t bytes, bool is_tx) {
  auto it = bytes_.find({type, is_tx});
  if (it != bytes_.end())
    it->second->Increment(static_cast<double>(bytes));
}

void Metrics::inc_view_change(const std::string &reason) {
  auto it = vc_.find(reason);
  if (it != vc_.end())
    it->second->Increment();
}

void Metrics::set_view(uint32_t v) { view_->Set(v); }

void Metrics::set_inflight(uint64_t n) {
  inflight_->Set(static_cast<double>(n));
}

void Metrics::observe_phase(const std::string &phase, double seconds) {
  auto it = phase_.find(phase);
  if (it != phase_.end())
    it->second->Observe(seconds);
}

void Metrics::set_watermarks(uint64_t low, uint64_t high) {
  low_watermark_->Set(low);
  high_watermark_->Set(high);
}

} // namespace pbft
