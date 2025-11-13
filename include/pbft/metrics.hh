#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <prometheus/exposer.h>
#include <prometheus/registry.h>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>

namespace pbft {

// TODO: Improve metrics
class Metrics {
public:
  explicit Metrics(const std::string &bind_addr);

  // Counters
  void inc_msg(const std::string &type);
  void inc_view_change(const std::string &reason);

  // Gauges
  void set_view(uint32_t view);
  void set_inflight(uint64_t n);

  // Histograms
  void observe_phase(const std::string &phase, double seconds);

private:
  std::unique_ptr<prometheus::Exposer> exposer_;
  std::shared_ptr<prometheus::Registry> registry_;

  prometheus::Family<prometheus::Counter> *msg_family_;
  prometheus::Family<prometheus::Counter> *vc_family_;
  prometheus::Family<prometheus::Gauge> *gauge_family_;
  prometheus::Family<prometheus::Histogram> *phase_family_;

  // handles
  std::unordered_map<std::string, prometheus::Counter*> msg_;
  std::unordered_map<std::string, prometheus::Counter*> vc_;
  prometheus::Gauge *view_;
  prometheus::Gauge *inflight_;
  std::unordered_map<std::string, prometheus::Histogram*> phase_;
};

} // namespace pbft

