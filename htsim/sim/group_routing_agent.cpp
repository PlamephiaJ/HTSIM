#include "group_routing_agent.h"

#include <stdexcept>
#include <cmath>
#include "datacenter/dragonfly_topology.h"
#include "data_collector.h"
#include "dragonfly_source_route.h"
#include "uecpacket.h"

const GraStaticGraph& GroupRoutingAgent::staticGraph() const {
    if (!_static_graph) throw std::logic_error("GRA static graph has not been initialized");
    return *_static_graph;
}

const GraExecutionProgress& GroupRoutingAgent::executionProgress() const {
    if (!_execution_progress) throw std::logic_error("GRA static graph has not been initialized");
    return *_execution_progress;
}

GraExecutionProgress::Result GroupRoutingAgent::receiveProgress(const GraProgressReport& report) {
    executionProgress();
    return _execution_progress->receive(report);
}

GroupRoutingAgent::GroupRoutingAgent(DragonflyTopology& topology, uint32_t group)
    : _topology(topology), _group(group) {
    const auto a = topology.get_a();
    for (uint32_t src = group * a; src < (group + 1) * a; ++src) {
        for (uint32_t dst = 0; dst < a * topology.get_no_groups(); ++dst) {
            auto* queue = topology.queues_switch_switch[src][dst];
            if (!queue || dst / a == group) continue;
            Corridor corridor{src, dst};
            _queues.emplace(corridor, queue);
            _remaining.emplace(corridor, 0);
            queue->setAdmissionObserver([this, corridor](const Packet& packet) {
                admitted(corridor, packet);
            });
        }
    }
}

std::optional<GroupRoutingAgent::Corridor> GroupRoutingAgent::corridorForRoute(
    uint32_t from, uint32_t to, uint32_t path_id) const {
    const auto a = _topology.get_a();
    const auto src = _topology.get_host_switch(from);
    const auto dst = _topology.get_host_switch(to);
    if (src / a != _group) throw std::invalid_argument("Source is outside GRA group");
    if (path_id & DragonflySourceRoute::kWaypointFlag) {
        const auto waypoint = path_id & DragonflySourceRoute::kWaypointMask;
        if (waypoint >= a * _topology.get_no_groups()) throw std::invalid_argument("Invalid waypoint");
        const auto target_group = waypoint / a == _group ? dst / a : waypoint / a;
        if (target_group == _group) return std::nullopt;
        Corridor c{_topology.get_group_switch(_group, target_group),
                   _topology.get_group_switch(target_group, _group)};
        query(c);
        return c;
    }
    if (src / a == dst / a) return std::nullopt;
    const uint32_t first = path_id >> 16;
    const uint32_t second = path_id & 0xffff;
    Corridor c = first / a == _group ? Corridor{first, second} : Corridor{src, first};
    query(c);
    return c;
}

GroupRoutingAgent::CorridorState GroupRoutingAgent::query(Corridor c) const {
    return {c, static_cast<uint64_t>(_queues.at(c)->queuesize()), _remaining.at(c)};
}

uint64_t GroupRoutingAgent::remaining(Messagelet m) const {
    auto found = _commitments.find(m);
    return found == _commitments.end() ? 0 : found->second.remaining_bytes;
}

GroupRoutingAgent::Snapshot GroupRoutingAgent::snapshot() const {
    Snapshot result;
    result.version = _version;
    for (const auto& item : _queues) result.corridors.push_back(query(item.first));
    for (const auto& item : _commitments) {
        if (item.second.remaining_bytes) result.messagelets.insert(item);
    }
    return result;
}

void GroupRoutingAgent::commit(Messagelet m, Corridor c, uint64_t bytes,
                               std::optional<uint64_t> transport_flow_id) {
    query(c);
    auto inserted = _commitments.emplace(m, Commitment{c, bytes, bytes, transport_flow_id.value_or(m.flow_id)});
    if (!inserted.second) throw std::logic_error("Messagelet already committed");
    _remaining.at(c) += bytes;
    ++_version;
    log("commit", m, inserted.first->second, bytes);
    routeDecided(m); // refresh after R changes; never count both R and a claim
}

void GroupRoutingAgent::setDemandPolicy(std::unique_ptr<RoutingDemandPolicy> policy, double alpha) {
    if (!std::isfinite(alpha) || alpha < 0) throw std::invalid_argument("Invalid demand alpha");
    _demand_policy = std::move(policy);
    _demand_alpha = alpha;
}

void GroupRoutingAgent::messageletReady(Messagelet m, uint32_t from, uint32_t to, uint64_t bytes) {
    if (!_demand_policy || _commitments.count(m)) return; // late READY after commit
    const auto src = _topology.get_host_switch(from);
    const auto dst = _topology.get_host_switch(to);
    if (src / _topology.get_a() != _group) throw std::invalid_argument("READY outside GRA group");
    if (src / _topology.get_a() == dst / _topology.get_a()) return; // no global demand
    const auto priority = staticGraph().dTable().find({m.dag_id, m.flow_id});
    if (priority == staticGraph().dTable().end()) return; // unknown DAG identity: baseline
    if (_demand_policy->ready({m, from, to, bytes, priority->second})) {
        ++_version;
        refreshDemand();
        traceDemand("ready", m);
    }
}

void GroupRoutingAgent::refreshDemand() {
    if (!_demand_policy) return;
    _demand_policy->refresh([&](const RoutingDemandPolicy::Ready& demand) {
        const auto src = _topology.get_host_switch(demand.from);
        const auto dst = _topology.get_host_switch(demand.to);
        auto* source = static_cast<DragonflySwitch*>(_topology.switches.at(src));
        const auto decision = source->previewUgalDecision(src, dst, [&](uint32_t first, uint32_t second) {
            const auto corridor = corridorForRoute(demand.from, demand.to, (first << 16) | second);
            const auto state = query(*corridor);
            return state.q_bytes + _demand_alpha * state.r_bytes;
        });
        return corridorForRoute(demand.from, demand.to,
            (decision.next_switch << 16) | decision.second_switch);
    });
}

uint64_t GroupRoutingAgent::softClaimPressure(Messagelet requester, Corridor corridor) const {
    return _demand_policy ? _demand_policy->pressure(requester, corridor) : 0;
}

void GroupRoutingAgent::removeDemand(Messagelet m, const char* action) {
    if (!_demand_policy) return;
    if (_demand_policy->remove(m)) ++_version;
    // Even a missing-READY commit may change remaining demands' preferences.
    refreshDemand();
    traceDemand(action, m);
}

void GroupRoutingAgent::cancelReady(Messagelet m) { removeDemand(m, "cancel"); }
void GroupRoutingAgent::routeDecided(Messagelet m) { removeDemand(m, "routed"); }

void GroupRoutingAgent::traceDemand(const char* action, Messagelet m) const {
    if (!_trace || !_demand_policy) return;
    auto* metric = htsim::DataCollector::get_instance().RegisterCsvMetric("gra_soft_claims",
        {"time_ps", "group", "version", "action", "dag_id", "flow_id", "messagelet_index",
         "ready_payload_bytes", "ready_priority", "pending_count", "claim_dag_id", "claim_flow_id", "claim_messagelet_index", "priority",
         "payload_bytes", "egress_switch", "remote_switch"});
    const auto ready = _demand_policy->pending().find(m);
    std::vector<std::string> row{std::to_string(EventList::getTheEventList().now()),
        std::to_string(_group), std::to_string(_version), action, std::to_string(m.dag_id),
        std::to_string(m.flow_id), std::to_string(m.messagelet_idx),
        ready == _demand_policy->pending().end() ? "" : std::to_string(ready->second.payload_bytes),
        ready == _demand_policy->pending().end() ? "" : std::to_string(ready->second.priority),
        std::to_string(_demand_policy->pending().size())};
    if (_demand_policy->claims().empty()) {
        row.resize(17);
        metric->LogData(row);
    } else for (const auto& item : _demand_policy->claims()) {
        auto claim_row = row;
        const auto& owner = item.second.owner;
        const std::vector<std::string> claim{std::to_string(owner.key.dag_id), std::to_string(owner.key.flow_id),
            std::to_string(owner.key.messagelet_idx), std::to_string(owner.priority), std::to_string(owner.payload_bytes),
            std::to_string(item.first.first), std::to_string(item.first.second)};
        claim_row.insert(claim_row.end(), claim.begin(), claim.end());
        metric->LogData(claim_row);
    }
}

void GroupRoutingAgent::bindPacket(Messagelet m, uint64_t seqno, uint64_t bytes) {
    if (!_commitments.count(m)) throw std::logic_error("Packet has no commitment");
    if (!_packets.emplace(std::make_pair(_commitments.at(m).transport_flow_id, seqno), PacketState{m, bytes, false}).second)
        throw std::logic_error("Original packet already bound");
}

void GroupRoutingAgent::bindPacketOnce(Messagelet m, uint64_t seqno, uint64_t bytes) {
    const auto& commitment = _commitments.at(m);
    const auto found = _packets.find({commitment.transport_flow_id, seqno});
    if (found == _packets.end()) {
        bindPacket(m, seqno, bytes);
    } else if (!(found->second.messagelet == m) || found->second.bytes != bytes) {
        throw std::logic_error("Inconsistent retransmitted messagelet packet");
    }
}

void GroupRoutingAgent::admitted(Corridor c, const Packet& packet) {
    // A trimmed header carries no original data into the corridor. A packet
    // admitted intact and trimmed/dropped later has already entered it.
    if (packet.type() != UECDATA || packet.header_only()) return;
    const auto& data = static_cast<const UecDataPacket&>(packet);
    auto found = _packets.find({packet.flow_id(), data.epsn()});
    if (found == _packets.end()) return; // other group, transparent or native flow
    auto& p = found->second;
    auto& commitment = _commitments.at(p.messagelet);
    if (p.entered || commitment.corridor != c) return;
    if (p.bytes > commitment.remaining_bytes) throw std::logic_error("GRA accounting underflow");
    p.entered = true;
    commitment.remaining_bytes -= p.bytes;
    _remaining.at(c) -= p.bytes;
    ++_version;
    log("first_admission", p.messagelet, commitment, p.bytes, data.epsn());
}

void GroupRoutingAgent::log(const char* action, Messagelet m, const Commitment& state,
                            uint64_t bytes, std::optional<uint64_t> seqno) const {
    if (!_trace) return;
    auto* metric = htsim::DataCollector::get_instance().RegisterCsvMetric("gra_events",
        {"time_ps", "group", "version", "action", "transport_flow_id", "messagelet_index", "egress_switch",
         "remote_switch", "payload_bytes", "seqno", "remaining_messagelet_bytes", "remaining_corridor_bytes",
         "dag_id", "flow_id"});
    metric->LogData({std::to_string(EventList::getTheEventList().now()), std::to_string(_group), std::to_string(_version), action,
        std::to_string(state.transport_flow_id), std::to_string(m.messagelet_idx), std::to_string(state.corridor.first),
        std::to_string(state.corridor.second), std::to_string(bytes), seqno ? std::to_string(*seqno) : "",
        std::to_string(state.remaining_bytes), std::to_string(_remaining.at(state.corridor)),
        std::to_string(m.dag_id), std::to_string(m.flow_id)});
}
