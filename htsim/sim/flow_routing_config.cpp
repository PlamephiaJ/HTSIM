#include "flow_routing_config.h"

#include <limits>
#include <set>
#include <stdexcept>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

namespace {
uint64_t unsignedNumber(const std::string& value) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) {
        throw std::invalid_argument("Expected a nonnegative integer, got '" + value + "'");
    }
    return std::stoull(value);
}
uint64_t number(const boost::property_tree::ptree& tree, const std::string& key) {
    return unsignedNumber(tree.get<std::string>(key));
}
uint32_t field32(const boost::property_tree::ptree& tree, const std::string& key) {
    const uint64_t value = number(tree, key);
    if (value > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument("Field '" + key + "' exceeds uint32");
    }
    return static_cast<uint32_t>(value);
}
} // namespace

FlowRoutingConfig FlowRoutingConfig::loadFromFile(const std::string& filename, bool metadata) {
    try {
        boost::property_tree::ptree tree;
        boost::property_tree::read_json(filename, tree);
        if (number(tree, "schema_version") != 1) {
            throw std::invalid_argument("Unsupported routing schema_version");
        }
        FlowRoutingConfig config;
        if (metadata && tree.get_optional<std::string>("workload.dag_id"))
            config.dag_id = number(tree, "workload.dag_id");
        if (metadata) {
            if (const auto ranks = tree.get_child_optional("workload.rank_dags")) {
                if (ranks->empty() || config.dag_id)
                    throw std::invalid_argument("rank_dags requires a nonempty array in place of workload.dag_id");
                for (const auto& rank : *ranks) {
                    if (!rank.first.empty() || !rank.second.empty())
                        throw std::invalid_argument("rank_dags must contain DAG IDs in GOAL rank order");
                    config.rank_dags.push_back(unsignedNumber(rank.second.get_value<std::string>()));
                }
            }
        }
        std::set<uint64_t> ids;
        for (const auto& item : tree.get_child("flows")) {
            const auto& row = item.second;
            Entry entry{unsignedNumber(item.first), field32(row, "src"), field32(row, "dst"),
                        field32(row, "tag"), field32(row, metadata ? "send_task.task_id" : "send_task_id"),
                        number(row, "bytes"), metadata ? "minimum" : row.get<std::string>("label")};
            if (metadata && (field32(row, "send_task.rank") != entry.src ||
                             number(row, "flow_id") != entry.flow_id))
                throw std::invalid_argument("Inconsistent metadata flow identity " + item.first);
            if (metadata && row.get_child_optional("recv_task")) {
                if (field32(row, "recv_task.rank") != entry.dst)
                    throw std::invalid_argument("Inconsistent metadata receive rank " + item.first);
                entry.recv_task_id = field32(row, "recv_task.task_id");
            }
            if (metadata && row.get_optional<std::string>("collective_id"))
                entry.collective_id = number(row, "collective_id");
            if (metadata && row.get_optional<std::string>("comm_position"))
                entry.comm_position = number(row, "comm_position");
            if (!config.rank_dags.empty()) {
                const auto& ranks = config.rank_dags;
                if (entry.src >= ranks.size() || entry.dst >= ranks.size() || ranks[entry.src] != ranks[entry.dst] ||
                    (row.get_optional<std::string>("dag_id") && number(row, "dag_id") != ranks[entry.src]))
                    throw std::invalid_argument("Flow endpoints must belong to the same declared DAG: " + item.first);
            }
            if (entry.label != "minimum" && entry.label != "valiant") {
                throw std::invalid_argument("Unknown routing label '" + entry.label + "'");
            }
            if (!ids.insert(entry.flow_id).second) {
                throw std::invalid_argument("Duplicate flow ID " + item.first);
            }
            if (!config.sends.emplace(std::make_pair(entry.src, entry.send_task_id), entry).second) {
                throw std::invalid_argument("Duplicate GOAL send task for flow " + item.first);
            }
        }
        if (number(tree, metadata ? "workload.p2p_flows" : "flow_count") != config.sends.size()) {
            throw std::invalid_argument("flow_count does not match flows");
        }
        return config;
    } catch (const std::exception& error) {
        throw std::runtime_error("Invalid routing config '" + filename + "': " + error.what());
    }
}
