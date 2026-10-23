#ifndef FLOW_ROUTING_CONFIG_H
#define FLOW_ROUTING_CONFIG_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>

// IDs belong to workload.meta.json, not PacketFlow's allocation order.
struct FlowRoutingConfig {
    struct Entry {
        uint64_t flow_id;
        uint32_t src, dst, tag, send_task_id;
        uint64_t bytes;
        std::string label;
        std::optional<uint32_t> recv_task_id = std::nullopt;
    };
    std::map<std::pair<uint32_t, uint32_t>, Entry> sends;
    // Metadata supplies identity only; routing labels remain a separate input.
    static FlowRoutingConfig loadFromFile(const std::string& filename, bool metadata = false);
};

#endif
