#include "pbft/node.hh"
#include "pbft/service_interface.hh"

#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

class KeyValueStore : public pbft::ServiceInterface {
public:
  // Service initialization
  void initialize() override { _store.clear(); }

  // Deterministic operation execution
  // Expected format: "PUT key value", "GET key", "DELETE key"
  virtual std::string execute(const std::string &operation) override {
    std::istringstream iss(operation);
    std::string cmd;
    iss >> cmd;

    if (cmd == "PUT") {
      std::string key;
      uint32_t value;
      if (iss >> key >> value) {
        _store[key] = value;
        return "OK";
      }
      return "ERROR: Invalid PUT format";
    } else if (cmd == "GET") {
      std::string key;
      if (iss >> key) {
        if (_store.find(key) != _store.end()) {
          return std::to_string(_store[key]);
        }
        return "NOT_FOUND";
      }
      return "ERROR: Invalid GET format";
    } else if (cmd == "DELETE") {
      std::string key;
      if (iss >> key) {
        _store.erase(key);
        return "OK";
      }
      return "ERROR: Invalid DELETE format";
    }

    return "ERROR: Unknown command";
  }

  // Checkpointing: Returns a deterministic hash of the state
  uint256_t get_checkpoint_digest() override {
    std::string state_accum;

    // Iterate sorted map
    for (const auto &[key, val] : _store) {
      state_accum += key;
      state_accum += ':';
      state_accum += std::to_string(val);
      state_accum += ';';
    }

    return salticidae::get_hash(state_accum);
  }

private:
  // Using std::map (Red-Black Tree)
  // This sorts keys automatically, making checkpointing O(N) and deterministic
  std::map<std::string, uint32_t> _store;
};

using json = nlohmann::json;
std::unique_ptr<pbft::Node> create_node(const std::string &filename,
                                        uint32_t node_id) {
  std::ifstream f(filename);
  if (!f.is_open()) {
    throw std::runtime_error("Could not open config file: " + filename);
  }

  json data = json::parse(f);
  pbft::NodeConfig cfg;
  cfg.num_replicas = data.value("num_replicas", 4);
  cfg.log_size = data.value("log_size", 100);
  cfg.checkpoint_interval = data.value("checkpoint_interval", 50);
  cfg.vc_timeout = data.value("vc_timeout", 2.0);

  // Configure the node
  auto service = std::make_unique<KeyValueStore>();
  auto node = std::make_unique<pbft::Node>(node_id, cfg, std::move(service));

  salticidae::NetAddr node_addr;
  if (data.contains("replicas")) {
    for (auto &[key, val] : data["replicas"].items()) {
      if (std::stoi(key) == (int)node_id) {
        node_addr = salticidae::NetAddr(val.get<std::string>());
        break;
      }
    }
  }

  std::cout << "Node " << node_id << " starting at " << std::string(node_addr)
            << std::endl;
  node->start(node_addr);

  // Add Replicas
  if (data.contains("replicas")) {
    for (auto &[key, val] : data["replicas"].items()) {
      uint32_t id = std::stoi(key);
      salticidae::NetAddr addr(val.get<std::string>());
      if (id != node_id) {
        node->add_replica(id, addr);
      }
    }
  }

  // Add Clients
  if (data.contains("clients")) {
    for (auto &[key, val] : data["clients"].items()) {
      uint32_t id = std::stoi(key);
      salticidae::NetAddr addr(val.get<std::string>());
      node->add_client(id, addr);
    }
  }

  return node;
}

int main(int argc, char **argv) {
  int replica_id = -1;
  std::string config = "";

  // Parse arguments
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--id" && i + 1 < argc) {
      replica_id = std::stoi(argv[++i]);
    }
    if (arg == "--config" && i + 1 < argc) {
      config = argv[++i];
    }
  }

  if (replica_id == -1 || config.size() == 0) {
    std::cerr << "Usage: " << argv[0]
              << " --id <replica-id> --config <config-file>\n";
    return 1;
  }

  try {
    // Create and run the node
    auto kv_service_node = create_node(config, replica_id);
    kv_service_node->run();

  } catch (const std::exception &e) {
    std::cerr << "Fatal Error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
