# PBFT Implementation in C++

This repository contains a robust implementation of the **Practical Byzantine Fault Tolerance (PBFT)** consensus algorithm in C++. It is designed to be modular, allowing developers to plug in their own deterministic services (State Machine Replication) on top of the consensus layer.

## Table of Contents

- [Overview](#overview)
- [Prerequisites](#prerequisites)
- [Building the Project](#building-the-project)
- [Running the Key-Value Store Example](#running-the-key-value-store-example)
- [Testing](#testing)

## Overview

The goal of this project is to provide a clear, understandable, and functional implementation of PBFT as described in the seminal paper by Miguel Castro and Barbara Liskov. It handles the complexities of:

-   **Normal Case Operation**: Pre-prepare, Prepare, and Commit phases.
-   **View Changes**: Handling primary failures and leader election.
-   **Checkpointing**: Garbage collection of logs and state synchronization.
-   **State Transfer**: (Planned/Partial) Bringing lagging replicas up to date.

The implementation decouples the consensus logic (`pbft::Node`) from the application logic (`pbft::ServiceInterface`), enabling you to replicate any deterministic service (e.g., Key-Value Store, Blockchain, Distributed Ledger).

## Prerequisites

Ensure you have the following dependencies installed:

-   **C++17 Compiler** (GCC 9+ or Clang 10+)
-   **CMake** (3.16+)
-   **Salticidae**: A C++ network library.
-   **OpenSSL**: For cryptographic operations.
-   **Prometheus-cpp**: For metrics.
-   **spdlog**: Fast C++ logging library.
-   **libuv**: Asynchronous I/O.
-   **fmt**: Formatting library.
-   **PkgConfig**

On Ubuntu/Debian, you might need:
```bash
sudo apt-get install build-essential cmake libssl-dev libuv1-dev pkg-config
# Note: salticidae and prometheus-cpp usually need to be built from source or installed via a package manager that supports them.
```

## Building the Project

This project uses `CMake` and provides a `Makefile` wrapper for convenience.

### Debug Build (with Examples and Tests)
```bash
make build-debug
```

### Release Build (Optimized)
```bash
make build-release
```

### Clean
```bash
make clean
```

## 🚀 Running the Key-Value Store Example

The repository includes a `kv_store` example to demonstrate the usage.

1.  **Navigate to the build directory:**
    ```bash
    cd build/debug/examples/kv_store
    ```

2.  **Create a configuration file**
    A `config.json` is typically generated or provided in `examples/kv_store/config.json`. Ensure it looks like this:

    ```json
    {
      "node": {
        "num_replicas": 4,
        "log_size": 200,
        "checkpoint_interval": 10,
        "vc_timeout": 2.5
      },
      "replicas": {
        "0": "127.0.0.1:9500",
        "1": "127.0.0.1:9501",
        "2": "127.0.0.1:9502",
        "3": "127.0.0.1:9503"
      },
      "clients": {
        "0": "127.0.0.1:9600"
      }
    }
    ```

3.  **Start the 4 Replicas:**
    Open 4 separate terminal windows (or use `tmux`/`screen`) and run:

    ```bash
    # Terminal 1
    ./kv_store --id 0 --config config.json
    
    # Terminal 2
    ./kv_store --id 1 --config config.json
    
    # Terminal 3
    ./kv_store --id 2 --config config.json
    
    # Terminal 4
    ./kv_store --id 3 --config config.json
    ```

4.  **Start the Client:**
    In a new terminal:
    ```bash
    ./kv_client --id 0 --config config.json
    ```

5.  **Interact:**
    The client supports the following commands:
    -   `PUT <key> <value>` (e.g., `PUT user1 100`)
    -   `GET <key>` (e.g., `GET user1`)
    -   `DELETE <key>`
    -   `EXIT`

    ```text
    > PUT user1 100
    Result: OK
    > GET user1
    Result: 100
    ```


## Testing

The project includes unit tests and integration tests.

To run all tests:
```bash
make test
```

To run only unit tests:
```bash
make unit-test
```

To run integration tests:
```bash
make integration-test
```
