#include "network_decision.h"

#include <stdexcept>
#include <cmath>
#include <iomanip>
#include <sstream>

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
    route.log_info.emplace("policy", _commitment_aware ? "commitment_aware_ugal" : "ugal_l");
    auto number = [](double value) { std::ostringstream out; out << std::setprecision(17) << value; return out.str(); };
    route.log_info.emplace("alpha", number(_commitment_aware ? _alpha : 0.0));
    // HTSim delivers locally before consulting UGAL-L.
    if (src_switch == dst_switch) {
        return route;
    }

    auto* source = static_cast<DragonflySwitch*>(topology.switches.at(src_switch));
    auto& gra = topology.groupRoutingAgent(src_switch / topology.get_a());
    if (gra.demandPolicy()) route.log_info.emplace("beta", number(_beta));
    std::vector<GroupRoutingAgent::CorridorState> observations;
    std::vector<uint64_t> claims;
    std::vector<RoutingDemandPolicy::PressureObservation> claim_observations;
    DragonflySwitch::PressureQuery pressure;
    // Intra-group routes do not enter a global corridor; retain local UGAL.
    if (_commitment_aware && src_switch / topology.get_a() != dst_switch / topology.get_a()) {
        gra.refreshDemand("decision", context.messagelet); // current Q/R and pending preferences
        if (gra.demandPolicy() && gra.demandSnapshotId())
            route.log_info.emplace("soft_claim_snapshot_id", std::to_string(gra.demandSnapshotId()));
        pressure = [&](uint32_t first, uint32_t second) {
            const auto c = gra.corridorForRoute(context.from, context.to, (first << 16) | second);
            const auto state = gra.query(*c);
            observations.push_back(state);
            const auto observation = gra.observeSoftClaim(context.messagelet, *c);
            const uint64_t claim = observation.bytes;
            claims.push_back(claim);
            claim_observations.push_back(observation);
            return state.q_bytes + _alpha * state.r_bytes + _beta * claim;
        };
    }
    const auto decision = source->ugalDecision(src_switch, dst_switch, pressure);
    route.path_id = (decision.next_switch << 16) | decision.second_switch;
    route.description = decision.use_valiant ? "valiant" : "minimum";
    route.log_info.emplace("hop_one_switch", std::to_string(decision.next_switch));
    route.log_info.emplace("hop_two_switch", std::to_string(decision.second_switch));
    route.log_info.emplace("next_min", std::to_string(decision.next_min));
    route.log_info.emplace("next_val", std::to_string(decision.next_val));
    route.log_info.emplace("compared", decision.compared ? "true" : "false");
    if (gra.demandPolicy()) {
        route.log_info.emplace("soft_claim_evaluation", !_commitment_aware ? "not_commitment_aware" :
            src_switch / topology.get_a() == dst_switch / topology.get_a() ? "intra_group" :
            decision.compared ? "compared" : "not_compared");
        for (size_t i = 0; i < claim_observations.size(); ++i) {
            const auto& observation = claim_observations[i];
            const std::string prefix = i == 0 ? "soft_claim_min_" : "soft_claim_val_";
            route.log_info.emplace(prefix + "reason", observation.bytes && _beta == 0 ? "beta_zero" : observation.reason);
            route.log_info.emplace(prefix + "pressure", number(_beta * observation.bytes));
            route.log_info.emplace(prefix + "requester_priority", observation.requester_priority ?
                std::to_string(*observation.requester_priority) : "");
            if (observation.claim) {
                const auto& owner = observation.claim->owner;
                route.log_info.emplace(prefix + "owner_dag_id", std::to_string(owner.key.dag_id));
                route.log_info.emplace(prefix + "owner_flow_id", std::to_string(owner.key.flow_id));
                route.log_info.emplace(prefix + "owner_messagelet_index", std::to_string(owner.key.messagelet_idx));
                route.log_info.emplace(prefix + "owner_priority", std::to_string(owner.priority));
                route.log_info.emplace(prefix + "owner_payload_bytes", std::to_string(owner.payload_bytes));
            }
        }
    }
    if (decision.compared) {
        const uint64_t q_min = observations.empty() ? decision.q_min : observations.at(0).q_bytes;
        const uint64_t q_val = observations.empty() ? decision.q_val : observations.at(1).q_bytes;
        const uint64_t r_min = observations.empty() ? 0 : observations.at(0).r_bytes;
        const uint64_t r_val = observations.empty() ? 0 : observations.at(1).r_bytes;
        route.log_info.emplace("q_min_bytes", std::to_string(q_min));
        route.log_info.emplace("q_val_bytes", std::to_string(q_val));
        route.log_info.emplace("r_min_bytes", std::to_string(r_min));
        route.log_info.emplace("r_val_bytes", std::to_string(r_val));
        const uint64_t s_min = claims.empty() ? 0 : claims.at(0);
        const uint64_t s_val = claims.empty() ? 0 : claims.at(1);
        if (gra.demandPolicy()) {
            route.log_info.emplace("soft_claim_min_bytes", std::to_string(s_min));
            route.log_info.emplace("soft_claim_val_bytes", std::to_string(s_val));
        }
        route.log_info.emplace("pressure_min", number(q_min + (_commitment_aware ? _alpha * r_min : 0) + _beta * s_min));
        route.log_info.emplace("pressure_val", number(q_val + (_commitment_aware ? _alpha * r_val : 0) + _beta * s_val));
        for (size_t i = 0; i < observations.size(); ++i) {
            const std::string prefix = i == 0 ? "min_" : "val_";
            route.log_info.emplace(prefix + "egress_switch", std::to_string(observations[i].corridor.first));
            route.log_info.emplace(prefix + "remote_switch", std::to_string(observations[i].corridor.second));
        }
        route.log_info.emplace("h_min", std::to_string(decision.h_min));
        route.log_info.emplace("h_val", std::to_string(decision.h_val));
    }
    return route;
}

std::unique_ptr<NetworkDecision> makeNetworkDecision(const std::string& strategy, double alpha,
                                                    std::optional<double> beta) {
    if (!std::isfinite(alpha) || alpha < 0) throw std::invalid_argument("Invalid commitment alpha");
    if (beta && (!std::isfinite(*beta) || *beta < 0)) throw std::invalid_argument("Invalid soft claim beta");
    if (strategy == "none") {
        return nullptr;
    }
    if (strategy == "ugal_l") {
        return std::make_unique<UgalLNetworkDecision>();
    }
    if (strategy == "commitment_aware_ugal") {
        return std::make_unique<UgalLNetworkDecision>(true, alpha, beta);
    }
    throw std::invalid_argument("Unsupported network decision strategy '" + strategy + "'");
}
