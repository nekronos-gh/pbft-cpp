#include <arpa/inet.h>
#include <future>

#include "pbft/client.hh"

using salticidae::_1;
using salticidae::_2;
using salticidae::MsgNetwork;
using salticidae::NetAddr;

namespace pbft {

Client::Client(uint32_t client_id, const ClientConfig &config)
    : id_(client_id), n_(config.num_replicas),
      timeout_(config.request_timeout_) {

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
  // TODO: Add client logger
  // TODO: Add client metrics
}

Client::~Client() {}

void Client::add_replica(uint32_t id, const NetAddr &addr) {
  if (id == id_)
    return;
  replicas_[id] = net_->connect_sync(addr);
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
  // Only accept replies for this client
  if (m.client_id != id_) {
    return;
  }

  // Update current knowledge on view
  if (m.view > current_view_) {
    current_view_ = m.view;
  }

  // Find the pending request by timestamp
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
  uint64_t ts = ++timestamp_;
  RequestMsg req(operation, ts, id_);

  std::promise<std::string> prom;
  auto fut = prom.get_future();
  inflight_requests_.emplace(ts, InflightRequest{ts, std::move(prom), {}, {}});

  if (replicas_.count(primary_hint())) {
    net_->send_msg(req, replicas_[primary_hint()]);
  } else {
    broadcast(req);
  }

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
  } else {
    broadcast(req);
  }

  auto current_timeout = std::chrono::milliseconds(timeout_);

  for (;;) {
    if (fut.wait_for(current_timeout) == std::future_status::ready) {
      return fut.get();
    }

    // Timeout, so broadcast to all replicas
    broadcast(req);
    current_timeout *= 2;
  }
}

} // namespace pbft
