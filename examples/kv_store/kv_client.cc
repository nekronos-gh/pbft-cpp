#include "pbft/client.hh"
#include <iostream>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#include <thread>

using json = nlohmann::json;

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " --id <client-id> --config <config-file>" << std::endl;
}

int main(int argc, char** argv) {
    uint32_t client_id = 0xFFFFFFFF;
    std::string config_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--id" && i + 1 < argc) {
            client_id = std::stoi(argv[++i]);
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    if (client_id == 0xFFFFFFFF || config_path.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    std::ifstream f(config_path);
    if (!f.is_open()) {
        std::cerr << "Could not open config file: " << config_path << std::endl;
        return 1;
    }

    json data = json::parse(f);
    pbft::ClientConfig cfg;
    
    // Read from "node" or root
    json node_cfg = data.contains("node") ? data["node"] : data;
    cfg.num_replicas = node_cfg.value("num_replicas", 4);
    cfg.request_timeout_ = node_cfg.value("request_timeout", 5000); // 5 seconds default

    auto client = std::make_unique<pbft::Client>(client_id, cfg);

    salticidae::NetAddr client_addr;
    if (data.contains("clients") && data["clients"].contains(std::to_string(client_id))) {
        client_addr = salticidae::NetAddr(data["clients"][std::to_string(client_id)].get<std::string>());
    } else {
        // Default or error
        std::cerr << "Client ID " << client_id << " not found in config clients list" << std::endl;
        return 1;
    }

    client->start(client_addr);

    if (data.contains("replicas")) {
        for (auto& [key, val] : data["replicas"].items()) {
            uint32_t id = std::stoi(key);
            salticidae::NetAddr addr(val.get<std::string>());
            client->add_replica(id, addr);
        }
    }

    // Run client event loop in a separate thread
    std::thread client_thread([&client]() {
        client->run();
    });

    std::cout << "KV Client " << client_id << " started." << std::endl;
    std::cout << "Commands: PUT <key> <value>, GET <key>, DELETE <key>, EXIT" << std::endl;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "EXIT" || line == "exit") {
            break;
        }
        if (line.empty()) continue;

        try {
            std::string result = client->invoke(line);
            std::cout << "Result: " << result << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }
        std::cout << "> " << std::flush;
    }

    client->stop();
    if (client_thread.joinable()) {
        client_thread.join();
    }

    return 0;
}
