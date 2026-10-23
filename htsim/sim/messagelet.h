#ifndef MESSAGELET_H
#define MESSAGELET_H

#include <cstdint>
#include <tuple>

struct MessageletKey {
    uint64_t dag_id = 0;
    uint64_t flow_id = 0; // global flow ID in the workload DAG, not PacketFlow's ID
    uint64_t messagelet_idx = 0;

    bool operator<(const MessageletKey& other) const {
        return std::tie(dag_id, flow_id, messagelet_idx) <
               std::tie(other.dag_id, other.flow_id, other.messagelet_idx);
    }
    bool operator==(const MessageletKey& other) const {
        return std::tie(dag_id, flow_id, messagelet_idx) ==
               std::tie(other.dag_id, other.flow_id, other.messagelet_idx);
    }
};

struct MessageletMeta {
    uint64_t mlet_size = 0; // actual original payload bytes, including a short tail
    uint64_t total_mlet = 0;
    bool operator==(const MessageletMeta& other) const {
        return mlet_size == other.mlet_size && total_mlet == other.total_mlet;
    }
};

struct MessageletInfo {
    MessageletKey key;
    MessageletMeta meta;
};

#endif
