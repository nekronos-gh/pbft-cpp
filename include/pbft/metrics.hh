#pragma once
#include <map>
#include <memory>
#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <string>
#include <unordered_map>

namespace pbft {

class Metrics {
public:
  explicit Metrics(const std::string &bind_addr);

  // Counters
  void inc_msg(const std::string &type, size_t size, bool is_tx);
  void inc_view_change(const std::string &reason);

  // Gauges
  void set_view(uint32_t view);
  void set_inflight(uint64_t n);
  void set_watermarks(uint64_t low, uint64_t high);

  // Histograms
  void observe_phase(const std::string &phase, double seconds);

private:
  void inc_bytes(const std::string &type, size_t bytes, bool is_tx);
  void observe_msg_size(const std::string &type, size_t bytes);

  std::unique_ptr<prometheus::Exposer> exposer_;
  std::shared_ptr<prometheus::Registry> registry_;

  prometheus::Family<prometheus::Counter> *msg_family_;
  prometheus::Family<prometheus::Counter> *vc_family_;
  prometheus::Family<prometheus::Counter> *bytes_family_;
  prometheus::Family<prometheus::Counter> *retrans_family_;
  prometheus::Family<prometheus::Gauge> *gauge_family_;
  prometheus::Family<prometheus::Histogram> *phase_family_;
  prometheus::Family<prometheus::Histogram> *size_family_;

  // handles
  std::map<std::pair<std::string, bool>, prometheus::Counter *> msg_;
  std::unordered_map<std::string, prometheus::Counter *> vc_;
  // Cache for bytes counters: key = type + direction
  std::map<std::pair<std::string, bool>, prometheus::Counter *> bytes_;
  std::unordered_map<std::string, prometheus::Counter *> retrans_;

  prometheus::Gauge *view_;
  prometheus::Gauge *inflight_;
  prometheus::Gauge *low_watermark_;
  prometheus::Gauge *high_watermark_;
  std::unordered_map<std::string, prometheus::Histogram *> phase_;
  std::unordered_map<std::string, prometheus::Histogram *> size_;
};

} // namespace pbft
