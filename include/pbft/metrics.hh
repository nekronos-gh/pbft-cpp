#pragma once
#include <memory>
#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <string>
#include <unordered_map>

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
  void set_watermarks(uint64_t low, uint64_t high);

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
  std::unordered_map<std::string, prometheus::Counter *> msg_;
  std::unordered_map<std::string, prometheus::Counter *> vc_;
  prometheus::Gauge *view_;
  prometheus::Gauge *inflight_;
  prometheus::Gauge *low_watermark_;
  prometheus::Gauge *high_watermark_;
  std::unordered_map<std::string, prometheus::Histogram *> phase_;
};

} // namespace pbft
