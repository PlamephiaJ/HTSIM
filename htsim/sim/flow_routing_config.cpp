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
