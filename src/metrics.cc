#include "pbft/metrics.hh"

using namespace prometheus;

namespace pbft {

static Histogram::BucketBoundaries default_buckets() {
  return {0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0};
}

Metrics::Metrics(const std::string &bind_addr) {
  exposer_ = std::make_unique<Exposer>(bind_addr);
  registry_ = std::make_shared<Registry>();
  exposer_->RegisterCollectable(registry_);

  msg_family_ = &BuildCounter()
                     .Name("pbft_messages_total")
                     .Help("PBFT messages by type")
                     .Register(*registry_);

  vc_family_ = &BuildCounter()
                    .Name("pbft_view_changes_total")
                    .Help("View changes by reason")
                    .Register(*registry_);

  gauge_family_ = &BuildGauge()
                       .Name("pbft_status")
                       .Help("PBFT status gauges")
                       .Register(*registry_);

  phase_family_ = &BuildHistogram()
                       .Name("pbft_phase_seconds")
                       .Help("Latency per PBFT phase")
                       .Register(*registry_);

  // Common labels
  msg_["request"] = &msg_family_->Add({{"type", "request"}});
  msg_["reply"] = &msg_family_->Add({{"type", "reply"}});
  msg_["preprepare"] = &msg_family_->Add({{"type", "preprepare"}});
  msg_["prepare"] = &msg_family_->Add({{"type", "prepare"}});
  msg_["commit"] = &msg_family_->Add({{"type", "commit"}});
  msg_["checkpoint"] = &msg_family_->Add({{"type", "checkpoint"}});
  msg_["viewchange"] = &msg_family_->Add({{"type", "viewchange"}});
  msg_["newview"] = &msg_family_->Add({{"type", "newview"}});

  vc_["timeout"] = &vc_family_->Add({{"reason", "timeout"}});
  vc_["primary_fault"] = &vc_family_->Add({{"reason", "primary_fault"}});

  view_ = &gauge_family_->Add({{"name", "current_view"}});
  inflight_ = &gauge_family_->Add({{"name", "inflight_requests"}});
  low_watermark_ = &gauge_family_->Add({{"name", "low_watermark"}});
  high_watermark_ = &gauge_family_->Add({{"name", "high_watermark"}});

  phase_["preprepare"] =
      &phase_family_->Add({{"phase", "preprepare"}}, default_buckets());
  phase_["prepare"] =
      &phase_family_->Add({{"phase", "prepare"}}, default_buckets());
  phase_["commit"] =
      &phase_family_->Add({{"phase", "commit"}}, default_buckets());
}

void Metrics::inc_msg(const std::string &type) {
  auto it = msg_.find(type);
  if (it != msg_.end())
    it->second->Increment();
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
