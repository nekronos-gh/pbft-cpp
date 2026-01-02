#pragma once
#include "salticidae/stream.h"
#include <string>

namespace pbft {

// Deterministic service to be replicated by PBFT.
// The abstract ServiceInterface class defines the contract that any service
// must implement. Execute: process client requests Get/Set/Digest checkpoints:
// manage snapshots
class ServiceInterface {
public:
  virtual ~ServiceInterface() = default;

  // Service initialization
  virtual void initialize() = 0;

  // Deterministic operation execution.
  virtual std::string execute(const std::string &operation) = 0;

  // Checkpointing.
  virtual uint256_t get_checkpoint_digest() = 0;
};
} // namespace pbft
