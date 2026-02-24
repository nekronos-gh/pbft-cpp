#include "pbft/service_interface.hh"

namespace pbft::testing {

// Mock service for dummy application
// Manage a global counter and allow for increase, decrease, addition and set
class CounterService : public ServiceInterface {
private:
  std::vector<std::string> executed_ops_;
  int counter_;
  uint256_t state_digest_;

public:
  CounterService() : counter_(0) {}

  void initialize() override {
    executed_ops_.clear();
    counter_ = 0;
    update_digest();
  }

  std::string execute(const std::string &operation) override {
    std::string op_to_execute = operation;

    executed_ops_.push_back(op_to_execute);

    if (op_to_execute == "INC") {
      counter_++;
    } else if (op_to_execute == "DEC") {
      counter_--;
    } else if (op_to_execute.substr(0, 4) == "ADD:") {
      counter_ += std::stoi(op_to_execute.substr(4));
    } else if (op_to_execute.substr(0, 4) == "SET:") {
      counter_ = std::stoi(op_to_execute.substr(4));
    }

    update_digest();
    return "OK";
  }

  uint256_t get_checkpoint_digest() override { return state_digest_; }
  int get_counter() const { return counter_; }
  size_t get_op_count() const { return executed_ops_.size(); }

private:
  void update_digest() {
    std::string state =
        std::to_string(counter_) + ":" + std::to_string(executed_ops_.size());
    state_digest_ = salticidae::get_hash(state);
  }
};

} // namespace pbft::testing
