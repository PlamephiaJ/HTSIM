#include "rank_placement.h"

#include <limits>
#include <stdexcept>
#include <unordered_set>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

namespace {
uint32_t readEndpointField(const boost::property_tree::ptree& endpoint,
                           const std::string& field,
                           const std::string& filename) {
    try {
        return endpoint.get<uint32_t>(field);
    } catch (const boost::property_tree::ptree_error&) {
        throw std::runtime_error("Missing or invalid '" + field +
                                 "' in rank placement config '" + filename + "'");
    }
}
} // namespace

RankPlacement RankPlacement::loadFromFile(const std::string& filename) {
    boost::property_tree::ptree tree;
    try {
        boost::property_tree::read_json(filename, tree);
    } catch (const boost::property_tree::json_parser_error& error) {
        throw std::runtime_error("Failed to parse rank placement config '" + filename +
                                 "': " + error.message());
    }

    uint32_t schema_version;
    try {
        schema_version = tree.get<uint32_t>("schema_version");
    } catch (const boost::property_tree::ptree_error&) {
        throw std::runtime_error("Missing or invalid 'schema_version' in rank placement config '" +
                                 filename + "'");
    }
    if (schema_version != kSchemaVersion) {
        throw std::runtime_error("Unsupported rank placement schema_version " +
                                 std::to_string(schema_version));
    }

    auto endpoints = tree.get_child_optional("placements");
    if (!endpoints) {
        throw std::runtime_error("Missing 'placements' array in rank placement config '" +
                                 filename + "'");
    }

    RankPlacement placement;
    for (const auto& item : *endpoints) {
        if (!item.first.empty() || item.second.empty()) {
            throw std::runtime_error("'placements' must be an array of endpoint objects in rank "
                                     "placement config '" + filename + "'");
        }
        placement._endpoints.push_back(
            {readEndpointField(item.second, "rank", filename),
             readEndpointField(item.second, "group", filename),
             readEndpointField(item.second, "switch", filename),
             readEndpointField(item.second, "host", filename)});
    }

    if (placement._endpoints.empty()) {
        throw std::runtime_error("'placements' must not be empty in rank placement config '" +
                                 filename + "'");
    }
    return placement;
}

void RankPlacement::resolveAndValidate(uint32_t rank_count,
                                       uint32_t hosts_per_switch,
                                       uint32_t switches_per_group,
                                       uint32_t group_count) {
    if (_endpoints.size() != rank_count) {
        throw std::runtime_error("Rank placement has " + std::to_string(_endpoints.size()) +
                                 " entries, but the GOAL workload requires " +
                                 std::to_string(rank_count));
    }

    _rank_to_host.assign(rank_count, std::numeric_limits<uint32_t>::max());
    std::unordered_set<uint32_t> used_hosts;
    for (const Endpoint& endpoint : _endpoints) {
        if (endpoint.rank >= rank_count) {
            throw std::runtime_error("Placement contains rank " +
                                     std::to_string(endpoint.rank) +
                                     ", but the GOAL workload only has " +
                                     std::to_string(rank_count) + " ranks");
        }
        if (_rank_to_host[endpoint.rank] != std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("GOAL rank " + std::to_string(endpoint.rank) +
                                     " appears more than once in rank placement");
        }
        if (endpoint.group >= group_count) {
            throw std::runtime_error("Rank " + std::to_string(endpoint.rank) + " uses group " +
                                     std::to_string(endpoint.group) + ", but the topology has " +
                                     std::to_string(group_count) + " groups");
        }
        if (endpoint.switch_in_group >= switches_per_group) {
            throw std::runtime_error("Rank " + std::to_string(endpoint.rank) + " uses switch " +
                                     std::to_string(endpoint.switch_in_group) +
                                     " within a group, but the topology only has " +
                                     std::to_string(switches_per_group) +
                                     " switches per group");
        }
        if (endpoint.host_in_switch >= hosts_per_switch) {
            throw std::runtime_error("Rank " + std::to_string(endpoint.rank) + " uses host " +
                                     std::to_string(endpoint.host_in_switch) +
                                     " within a switch, but the topology only has " +
                                     std::to_string(hosts_per_switch) +
                                     " hosts per switch");
        }

        const uint64_t global_switch =
            static_cast<uint64_t>(endpoint.group) * switches_per_group +
            endpoint.switch_in_group;
        const uint64_t global_host = global_switch * hosts_per_switch +
                                     endpoint.host_in_switch;
        if (global_host > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("Resolved HTSIM host ID exceeds placement limits");
        }
        const uint32_t host = static_cast<uint32_t>(global_host);
        if (!used_hosts.insert(host).second) {
            throw std::runtime_error("Dragonfly endpoint group " +
                                     std::to_string(endpoint.group) + ", switch " +
                                     std::to_string(endpoint.switch_in_group) + ", host " +
                                     std::to_string(endpoint.host_in_switch) +
                                     " is assigned to more than one rank");
        }
        _rank_to_host[endpoint.rank] = host;
    }
}

uint32_t RankPlacement::hostForRank(uint32_t rank) const {
    if (rank >= _rank_to_host.size()) {
        throw std::out_of_range("No HTSIM host placement for rank " + std::to_string(rank));
    }
    return _rank_to_host[rank];
}
