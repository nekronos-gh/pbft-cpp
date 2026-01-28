#include <arpa/inet.h>
#include <future>

#include "pbft/client.hh"
#include "spdlog/sinks/basic_file_sink.h"

using salticidae::_1;
using salticidae::_2;
using salticidae::MsgNetwork;
using salticidae::NetAddr;

#define NETADDR_STR(addr)                                                      \
  ([&] {                                                                       \
    char _ipbuf[INET_ADDRSTRLEN];                                              \
    uint32_t _ip = htonl((addr).ip);                                           \
    inet_ntop(AF_INET, &_ip, _ipbuf, sizeof(_ipbuf));                          \
    static thread_local char _outbuf[64];                                      \
    snprintf(_outbuf, sizeof(_outbuf), "%s:%u", _ipbuf,                        \
             (unsigned)((addr).port));                                         \
    return _outbuf;                                                            \
  }())

namespace pbft {

Client::Client(uint32_t client_id, const ClientConfig &config)
    : id_(client_id), n_(config.num_replicas),
      timeout_(config.request_timeout_), current_view_(1) {

  f_ = (n_ - 1) / 3;

  // Set TLS in the network
  auto tls_config = config.tls_config;
  MsgNetwork<uint8_t>::Config network_config;
  if (tls_config.has_value()) {
    network_config.enable_tls(true)
        .tls_cert_file(tls_config->cert_file)
        .tls_key_file(tls_config->key_file);
  } else {
    network_config.enable_tls(false);
  }
  net_ = std::make_unique<MsgNetwork<uint8_t>>(ec_, network_config);

  std::string metrics_addr = "0.0.0.0:" + std::to_string(9560 + id_);
  metrics_ = std::make_unique<Metrics>(metrics_addr);

  // Initialize logger
  logger_ = spdlog::basic_logger_mt(
      fmt::format("client-{}", id_),         // logger name
      fmt::format("logs/client-{}.log", id_) // file path
  );

  logger_->set_level(spdlog::level::info);
  logger_->set_pattern("[%Y-%m-%dT%H:%M:%S.%e] [%n] [%^%l%$] %v");

  register_handlers();
}

Client::~Client() {
  if (logger_) {
    logger_->flush();
    spdlog::drop(logger_->name());
  }
}

void Client::add_replica(uint32_t id, const NetAddr &addr) {
  if (id == id_)
    return;
  replicas_[id] = net_->connect_sync(addr);
  logger_->info("CONNECTED REPLICA id={} addr={}", id, NETADDR_STR(addr));
}

void Client::start(const salticidae::NetAddr &listen_addr) {
  net_->start();
  net_->listen(listen_addr);
}

void Client::stop() {
  ec_.stop();
  if (net_) {
    for (const auto &kv : replicas_) {
      net_->terminate(kv.second);
    }
    net_->stop();
  }
}

void Client::run() { ec_.dispatch(); }

void Client::register_handlers() {
  net_->reg_handler(salticidae::generic_bind(&Client::on_reply, this, _1, _2));
}

void Client::on_reply(ReplyMsg &&m, const MsgNetwork<uint8_t>::conn_t &) {
  metrics_->inc_msg("reply", m.serialized.size(), false);
  logger_->info("MSG_RECV REPLY view={} ts={} from={} result={}", m.view,
                m.timestamp, m.replica_id, m.result);

  // Only accept replies for this client
  if (m.client_id != id_) {
    return;
  }

  // Update current knowledge on view
  if (m.view > current_view_) {
    current_view_ = m.view;
  }

  // Find the pending request by timestamp
  std::lock_guard<std::mutex> lk(mu_);
  auto it = inflight_requests_.find(m.timestamp);
  if (it == inflight_requests_.end())
    return;

  auto &request = it->second;
  // Ignore multiple replies from the same replica
  if (!request.seen_replicas.insert(m.replica_id).second)
    return;

  // Count matching results.
  auto &cnt = request.result_counts[m.result];
  cnt++;

  // If f + 1 replicas agree, accept the reply
  if (cnt >= (f_ + 1)) {
    // Set the value and remove the request
    request.done.set_value(m.result);
    inflight_requests_.erase(m.timestamp);
  }
}

std::future<std::string> Client::invoke_async(const std::string &operation) {
  std::lock_guard<std::mutex> lk(mu_);
  uint64_t ts = ++timestamp_;
  RequestMsg req(operation, ts, id_);

  std::promise<std::string> prom;
  auto fut = prom.get_future();
  inflight_requests_.emplace(ts, InflightRequest{ts, std::move(prom), {}, {}});

  if (replicas_.count(primary_hint())) {
    net_->send_msg(req, replicas_[primary_hint()]);
    metrics_->inc_msg("request", req.serialized.size(), true);
  } else {
    broadcast_request(req);
  }

  logger_->info("REQUEST SENT op={} ts={}", operation, ts);

  return fut;
}

std::string Client::invoke(const std::string &operation) {
  uint64_t ts = ++timestamp_;
  RequestMsg req(operation, ts, id_);

  // Set as inflight
  std::promise<std::string> prom;
  auto fut = prom.get_future();
  inflight_requests_.emplace(ts, InflightRequest{ts, std::move(prom), {}, {}});

  if (replicas_.count(primary_hint())) {
    net_->send_msg(req, replicas_[primary_hint()]);
    metrics_->inc_msg("request", req.serialized.size(), true);
  } else {
    broadcast_request(req);
  }

  logger_->info("REQUEST SENT op={} ts={}", operation, ts);

  auto current_timeout = std::chrono::milliseconds(timeout_);

  for (;;) {
    if (fut.wait_for(current_timeout) == std::future_status::ready) {
      return fut.get();
    }

    // Timeout, so broadcast to all replicas
    broadcast_request(req);
    logger_->info("REQUEST RETRY op={} ts={}", operation, ts);
    current_timeout *= 2;
  }
}

} // namespace pbft
