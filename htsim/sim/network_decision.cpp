#include "network_decision.h"

#include <stdexcept>

#include "datacenter/dragonfly_topology.h"

NetworkRoute UgalLNetworkDecision::decide(const NetworkDecisionContext& context) {
    auto& topology = context.topology;
    if (context.from >= topology.get_no_hosts() || context.to >= topology.get_no_hosts()) {
        throw std::out_of_range("Network decision host is outside the Dragonfly topology");
    }
    if (uint64_t{topology.get_a()} * topology.get_no_groups() > 32768) {
        throw std::logic_error("SOURCE routing supports at most 32768 switches");
    }

    const uint32_t src_switch = topology.get_host_switch(context.from);
    const uint32_t dst_switch = topology.get_host_switch(context.to);
    NetworkRoute route;
    route.log_info.emplace("policy", "ugal_l");
    // HTSim delivers locally before consulting UGAL-L.
    if (src_switch == dst_switch) {
        return route;
    }

    auto* source = static_cast<DragonflySwitch*>(topology.switches.at(src_switch));
    const auto decision = source->ugalDecision(src_switch, dst_switch);
    route.path_id = (decision.next_switch << 16) | decision.second_switch;
    route.description = decision.use_valiant ? "valiant" : "minimum";
    route.log_info.emplace("hop_one_switch", std::to_string(decision.next_switch));
    route.log_info.emplace("hop_two_switch", std::to_string(decision.second_switch));
    route.log_info.emplace("next_min", std::to_string(decision.next_min));
    route.log_info.emplace("next_val", std::to_string(decision.next_val));
    route.log_info.emplace("compared", decision.compared ? "true" : "false");
    if (decision.compared) {
        route.log_info.emplace("q_min_bytes", std::to_string(decision.q_min));
        route.log_info.emplace("q_val_bytes", std::to_string(decision.q_val));
        route.log_info.emplace("h_min", std::to_string(decision.h_min));
        route.log_info.emplace("h_val", std::to_string(decision.h_val));
    }
    return route;
}

std::unique_ptr<NetworkDecision> makeNetworkDecision(const std::string& strategy) {
    if (strategy == "none") {
        return nullptr;
    }
    if (strategy == "ugal_l") {
        return std::make_unique<UgalLNetworkDecision>();
    }
    throw std::invalid_argument("Unsupported network decision strategy '" + strategy + "'");
}
