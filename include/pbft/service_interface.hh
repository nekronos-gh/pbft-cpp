#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pbft {

// Deterministic service to be replicated by PBFT.
// The abstract ServiceInterface class defines the contract that any service must implement.
// Execute: process client requests
// Get/Set/Digest checkpoints: manage snapshots
class ServiceInterface {
public:
  virtual ~ServiceInterface() = default;

  // Service initialization
  virtual void initialize() = 0;

  // Deterministic operation execution.
  virtual std::string execute(const std::string &operation) = 0;

  // Checkpointing.
  virtual std::vector<uint8_t> get_checkpoint() = 0;
  virtual void set_checkpoint(const std::vector<uint8_t> &checkpoint) = 0;
  virtual std::string get_checkpoint_digest() = 0;
};
} 
