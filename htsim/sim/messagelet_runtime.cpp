#include "messagelet_runtime.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

#include "atlahs_dragonfly_api.h"
#include "logsim-interface.h"
#include "datacenter/dragonfly_topology.h"
#include "dragonfly_source_route.h"
#include "data_collector.h"
#include "gra_goal_graph.h"
#include "uec.h"

namespace {
uint64_t routingUnitSize(MessageletConfig::Granularity granularity,
                         uint64_t flow_size, uint64_t messagelet_size) {
    switch (granularity) {
        case MessageletConfig::Granularity::Flow:
            return std::max<uint64_t>(flow_size, 1);
        case MessageletConfig::Granularity::Messagelet:
            return messagelet_size;
        case MessageletConfig::Granularity::Packet:
            return Packet::data_packet_size();
    }
    throw std::invalid_argument("Unsupported runtime granularity");
}
} // namespace

MessageletRuntime::MessageletRuntime(AtlahsDragonflyApi& goal_runtime)
    : _goal_runtime(goal_runtime) {}

void MessageletRuntime::submit(const SendEvent& event, graph_node_properties node) {
    DragonflyTopology* topology = _goal_runtime.getDragonflyTopology();
    if (!topology) {
        throw std::logic_error("Dragonfly topology not set");
    }
    if (transparent() && DragonflySwitch::get_routing_strategy() == DragonflySwitch::SOURCE) {
        throw std::logic_error("Transparent runtime requires switch routing MINIMAL, VALIANT or UGAL_L");
    }

    if (entrySwitchRouting() != (DragonflySwitch::get_routing_strategy() == DragonflySwitch::ENTRY_SWITCH)) {
        throw std::logic_error("Host runtime.routing_mode and switch -routing must agree on ENTRY_SWITCH");
    }
    if (entrySwitchRouting()) {
        if (!_flow_metadata_loaded)
            throw std::logic_error("ENTRY_SWITCH requires workload flow metadata with DAG-global flow IDs");
        DragonflySwitch::configureEntryRouting(_config);
    }

    int to = event.getTo();
    int from = event.getFrom();
    int tag = event.getTag();
    int size = event.getSizeBytes();

    const FlowRoutingConfig::Entry* routing = nullptr;
    if (_routing_config_loaded || _flow_metadata_loaded || _static_graph_initialized) {
        const auto& identities = _static_graph_initialized ? _graph_identities :
            (_routing_config_loaded ? _routing_config : _flow_metadata);
        auto entry = identities.sends.find({static_cast<uint32_t>(from), node.offset});
        if (entry == identities.sends.end()) {
            throw std::runtime_error("Routing config has no label for GOAL send rank " +
                                     std::to_string(from) + " task " + std::to_string(node.offset));
        }
        routing = &entry->second;
        if (routing->dst != static_cast<uint32_t>(to) ||
            routing->tag != static_cast<uint32_t>(tag) ||
            routing->bytes != static_cast<uint64_t>(size)) {
            throw std::runtime_error("Routing config does not match GOAL flow " +
                                     std::to_string(routing->flow_id));
        }
    }

    from = _goal_runtime.getHtsimNodeNumber(from, node.nic);
    to = _goal_runtime.getHtsimNodeNumber(to, node.nic);

    if (from == to) {
        throw std::logic_error("Send event source and destination map to the same HTSim node");
    }

    if (_goal_runtime.getLogSimInterface()->get_protocol() != UEC_PROTOCOL) {
        throw std::logic_error("AtlahsDragonflyApi only supports UEC");
    }

    TrafficLoggerSimple* traffic_logger = nullptr;
    auto per_flow_mp = _goal_runtime.createMultipath();

    UecSrc* uec_src = new UecSrc(traffic_logger,
                                 *_goal_runtime.getEventList(),
                                 std::move(per_flow_mp),
                                 *_goal_runtime.uec_nics.at(from),
                                 1);
    uec_src->setFlowsize(size);
    uec_src->initNscc(_goal_runtime.cwnd_b, _goal_runtime.getMaxRtt());
    uec_src->setName("uec_" + std::to_string(from) + "_" + std::to_string(to));
    uec_src->from = from;
    uec_src->to = to;
    uec_src->tag = tag;
    uec_src->send_size = size;
    uec_src->_atlahs_api = &_goal_runtime;
    // A null routing hook selects UecSrc's original entropy, retransmission
    // and control-packet paths. GOAL completion still uses _atlahs_api.
    uec_src->_messagelet_runtime = transparent() ? nullptr : this;
    registerFlow(*uec_src, size, from, to, tag, routing);

    UecSink* uec_sink = new UecSink(traffic_logger,
                                    _goal_runtime.linkspeed,
                                    1.1,
                                    UecBasePacket::unquantize(UecSink::_credit_per_pull),
                                    *_goal_runtime.getEventList(),
                                    *_goal_runtime.uec_nics.at(to),
                                    1);
    uec_sink->setName("uec_sink_" + std::to_string(from) + "_" + std::to_string(to));
    uec_sink->from_sink = from;
    uec_sink->to_sink = to;
    uec_sink->tag_sink = tag;

    uec_src->set_dst(to);
    uec_src->setSrc(from);
    uec_src->setDst(to);
    uec_sink->set_src(from);

    uint32_t src_switch = topology->get_host_switch(from);
    Route* srctotor = new Route();
    srctotor->push_back(topology->queues_host_switch[from][src_switch]);
    srctotor->push_back(topology->pipes_host_switch[from][src_switch]);
    srctotor->push_back(
        topology->queues_host_switch[from][src_switch]->getRemoteEndpoint());

    uint32_t dst_switch = topology->get_host_switch(to);
    Route* dsttotor = new Route();
    dsttotor->push_back(topology->queues_host_switch[to][dst_switch]);
    dsttotor->push_back(topology->pipes_host_switch[to][dst_switch]);
    dsttotor->push_back(
        topology->queues_host_switch[to][dst_switch]->getRemoteEndpoint());

    graph_node_properties* node_copy = new graph_node_properties(node);
    uec_src->lgs_node = node_copy;

    uec_src->connectPort(
        0, *srctotor, *dsttotor, *uec_sink, _goal_runtime.getEventList()->now());
    if (!transparent() && !entrySwitchRouting()) {
        // GOAL SOURCE flows compute both directions without .lt files.
        // Transparent mode keeps the sink's native control-packet entropy.
        uec_sink->setReturnPath(minimumRoute(to, from).path_id);
    }

    topology->switches[src_switch]->addHostPort(
        from, uec_sink->flowId(), uec_src->getPort(0));
    topology->switches[dst_switch]->addHostPort(
        to, uec_src->flowId(), uec_sink->getPort(0));
}

void MessageletRuntime::eventFinished(const EventOver& event) {
    _goal_runtime.EventFinished(event);
}

void MessageletRuntime::reportTaskProgress(uint32_t rank, uint32_t task_offset, bool finished,
                                          uint64_t event_time_ns, uint8_t nic) {
    auto* topology = _goal_runtime.getDragonflyTopology();
    if (!topology) throw std::logic_error("Dragonfly topology not set");
    auto& version = _rank_progress_versions[rank];
    if (version == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("Host progress report version exhausted");
    GraProgressReport report{_config.runtime.dag_id, rank, task_offset,
        finished ? GraTaskEvent::FINISH : GraTaskEvent::START, ++version, event_time_ns};
    const auto host = _goal_runtime.getHtsimNodeNumber(rank, nic);
    topology->reportHostProgress(host, report);

    const char* trace = std::getenv("HTSIM_TRACE_GRA_PROGRESS");
    if (trace && std::string(trace) == "1") {
        const auto group = topology->get_host_switch(host) / topology->get_a();
        const auto& gra = topology->groupRoutingAgent(group);
        std::string frontier;
        for (const auto& key : gra.frontier(report.dag_index)) {
            const auto event = key.event == GraStaticGraph::Event::START ? "START" :
                key.event == GraStaticGraph::Event::RECEIVE_START ? "RECEIVE_START" : "FINISH";
            frontier += std::to_string(key.node_index) + ":" + event + ";";
        }
        auto* metric = htsim::DataCollector::get_instance().RegisterCsvMetric("gra_progress_reports",
            {"dag_index", "rank", "task_offset", "event", "version", "event_time_ns", "group",
             "confirmed_events", "frontier"});
        metric->LogData({std::to_string(report.dag_index), std::to_string(rank), std::to_string(task_offset),
            finished ? "FINISH" : "START", std::to_string(version), std::to_string(event_time_ns),
            std::to_string(group), std::to_string(gra.executionProgress().confirmedEvents().size()), frontier});
    }
}

void MessageletRuntime::loadConfig(const std::string& filename) {
    if (_static_graph_initialized) throw std::logic_error("GRA static graph is already initialized");
    if (!_flows.empty()) {
        throw std::logic_error("Messagelet config must be loaded before submitting flows");
    }
    auto config = MessageletConfig::loadFromFile(filename);
    if (config.runtime.mode == MessageletConfig::Mode::Transparent && _routing_config_loaded) {
        throw std::logic_error("Transparent runtime cannot be combined with a static routing config");
    }
    if (config.runtime.routing_mode == MessageletConfig::RoutingMode::ENTRY_SWITCH && _routing_config_loaded)
        throw std::logic_error("ENTRY_SWITCH cannot be combined with a static routing config");
    auto decision = config.runtime.routing_mode == MessageletConfig::RoutingMode::ENTRY_SWITCH ? nullptr :
        makeNetworkDecision(config.routing.network_decision, config.routing.commitment_alpha);
    if (decision && _routing_config_loaded) {
        throw std::logic_error("Network decision policy cannot be combined with a static routing config");
    }
    _config = std::move(config);
    _network_decision = std::move(decision);
}

void MessageletRuntime::loadRoutingConfig(const std::string& filename) {
    if (_static_graph_initialized) throw std::logic_error("GRA static graph is already initialized");
    if (!_flows.empty()) {
        throw std::logic_error("Routing config must be loaded before submitting flows");
    }
    if (entrySwitchRouting())
        throw std::logic_error("ENTRY_SWITCH cannot be combined with a static routing config");
    if (transparent()) {
        throw std::logic_error("Transparent runtime cannot be combined with a static routing config");
    }
    if (_network_decision) {
        throw std::logic_error("Static routing config cannot be combined with a network decision policy");
    }
    _routing_config = FlowRoutingConfig::loadFromFile(filename);
    _routing_config_loaded = true;
}

void MessageletRuntime::loadFlowMetadata(const std::string& filename) {
    if (_static_graph_initialized) throw std::logic_error("GRA static graph is already initialized");
    if (!_flows.empty()) throw std::logic_error("Flow metadata must be loaded before submitting flows");
    _flow_metadata = FlowRoutingConfig::loadFromFile(filename, true);
    _flow_metadata_loaded = true;
}

void MessageletRuntime::initializeGoalGraph(const Parser& parser) {
    if (_static_graph_initialized || !_flows.empty())
        throw std::logic_error("GRA static graph must be initialized once before submitting flows");
    auto* topology = _goal_runtime.getDragonflyTopology();
    if (!topology) throw std::logic_error("Dragonfly topology not set");
    if (entrySwitchRouting() && !_flow_metadata_loaded)
        throw std::logic_error("ENTRY_SWITCH requires workload flow metadata with DAG-global flow IDs");

    FlowRoutingConfig identities = _routing_config_loaded ? _routing_config : _flow_metadata;
    if (_routing_config_loaded && _flow_metadata_loaded) {
        if (identities.sends.size() != _flow_metadata.sends.size())
            throw std::invalid_argument("GRA routing and metadata flow identities differ");
        for (auto& item : identities.sends) {
            auto found = _flow_metadata.sends.find(item.first);
            if (found == _flow_metadata.sends.end())
                throw std::invalid_argument("GRA routing and metadata send tasks differ");
            const auto& meta = found->second;
            auto& route = item.second;
            if (std::tie(route.flow_id, route.src, route.dst, route.tag, route.bytes) !=
                std::tie(meta.flow_id, meta.src, meta.dst, meta.tag, meta.bytes))
                throw std::invalid_argument("GRA routing and metadata flow identities differ");
            route.recv_task_id = meta.recv_task_id;
        }
    }
    auto graph = buildGraGoalGraph(parser, _config.runtime.dag_id,
        (_routing_config_loaded || _flow_metadata_loaded) ? &identities : nullptr);
    topology->initializeGraStaticGraphs({std::move(graph.dag)});
    _graph_identities = std::move(graph.identities);
    _static_graph_initialized = true;
}

void MessageletRuntime::validateRoutingComplete() const {
    if (_flow_metadata_loaded && _flows.size() != _flow_metadata.sends.size())
        throw std::runtime_error("Flow metadata contains flows absent from the GOAL workload");
    if (_routing_config_loaded && _flows.size() != _routing_config.sends.size()) {
        throw std::runtime_error("Routing config contains flows absent from the GOAL workload");
    }
}

uint64_t MessageletRuntime::messageletSize(const UecSrc& source) const {
    return flowState(source).messagelet_size;
}

MessageletRuntime::Granularity MessageletRuntime::granularity(const UecSrc& source) const {
    return flowState(source).granularity;
}

uint32_t MessageletRuntime::pathIdForMessagelet(const UecSrc& source,
                                                uint64_t messagelet_index) {
    if (entrySwitchRouting())
        throw std::logic_error("ENTRY_SWITCH runtime does not assign path IDs");
    if (transparent()) {
        throw std::logic_error("Transparent runtime does not assign messagelet routes");
    }
    FlowState& state = mutableFlowState(source);
    if (state.finalized_routes.count(messagelet_index)) {
        return state.messagelet_routes.at(messagelet_index).path_id;
    }
    if (messagelet_index >= state.messagelet_routes.size()) {
        state.messagelet_routes.resize(messagelet_index + 1, state.default_route);
    }
    if (_network_decision && !state.route_overrides.count(messagelet_index)) {
        auto* topology = _goal_runtime.getDragonflyTopology();
        if (!topology) {
            throw std::logic_error("Dragonfly topology not set");
        }
        state.messagelet_routes[messagelet_index] = _network_decision->decide(
            {*topology, state.from, state.to, state.tag, state.flow_size,
             state.messagelet_size, messagelet_index});
    }
    state.finalized_routes.insert(messagelet_index);
    const RouteInfo& route = state.messagelet_routes[messagelet_index];
    auto& topology = *_goal_runtime.getDragonflyTopology();
    auto& gra = topology.groupRoutingAgent(topology.get_host_switch(state.from) / topology.get_a());
    gra.setTrace(_config.logging.trace_decisions);
    const auto corridor = DragonflySwitch::get_routing_strategy() == DragonflySwitch::SOURCE ?
        gra.corridorForRoute(state.from, state.to, route.path_id) : std::nullopt;
    const uint64_t mtu = Packet::data_packet_size();
    const uint64_t mss = mtu - UecSrc::_hdr_size;
    const uint64_t remaining = state.flow_size - state.original_payload_sent;
    const uint64_t payload = state.granularity == Granularity::Flow ? remaining :
        std::min(remaining, (state.messagelet_size / mtu + (state.messagelet_size % mtu != 0)) * mss);
    const auto decision_id = _decision_index++;
    if (_config.logging.trace_decisions) {
        // Capture all corridors and all pending R_m,c before this decision's
        // own reservation. The single-threaded simulator cannot mutate state
        // between the policy query above and this snapshot.
        const auto snapshot = gra.snapshot();
        auto& collector = htsim::DataCollector::get_instance();
        auto* decisions = collector.RegisterCsvMetric("runtime_decisions",
            {"decision_id", "time_ps", "group", "gra_version", "transport_flow_id", "send_task_id",
             "src_host", "dst_host", "tag", "messagelet_index", "granularity", "size_bytes",
             "payload_bytes", "path_id", "route", "egress_switch", "remote_switch", "policy_state"});
        std::string policy_state;
        for (const auto& item : route.log_info) policy_state += item.first + "=" + item.second + ";";
        decisions->LogData({std::to_string(decision_id), std::to_string(_goal_runtime.getEventList()->now()),
            std::to_string(gra.group()), std::to_string(snapshot.version), std::to_string(source.flowId()),
            source.lgs_node ? std::to_string(source.lgs_node->offset) : "", std::to_string(state.from),
            std::to_string(state.to), std::to_string(state.tag), std::to_string(messagelet_index),
            MessageletConfig::granularityName(state.granularity), std::to_string(state.messagelet_size),
            std::to_string(payload), std::to_string(route.path_id), route.description,
            corridor ? std::to_string(corridor->first) : "", corridor ? std::to_string(corridor->second) : "", policy_state});
        auto* corridors = collector.RegisterCsvMetric("gra_corridor_snapshots",
            {"decision_id", "egress_switch", "remote_switch", "q_wire_bytes", "r_payload_bytes"});
        for (const auto& c : snapshot.corridors) corridors->LogData({std::to_string(decision_id),
            std::to_string(c.corridor.first), std::to_string(c.corridor.second),
            std::to_string(c.q_bytes), std::to_string(c.r_bytes)});
        auto* messages = collector.RegisterCsvMetric("gra_messagelet_snapshots",
            {"decision_id", "transport_flow_id", "messagelet_index", "egress_switch", "remote_switch",
             "original_payload_bytes", "remaining_payload_bytes"});
        for (const auto& item : snapshot.messagelets) messages->LogData({std::to_string(decision_id),
            std::to_string(item.second.transport_flow_id), std::to_string(item.first.messagelet_idx),
            std::to_string(item.second.corridor.first), std::to_string(item.second.corridor.second),
            std::to_string(item.second.original_bytes), std::to_string(item.second.remaining_bytes)});
    }
    if (corridor) gra.commit(messageletInfo(source, messagelet_index).key, *corridor, payload, source.flowId());
    if (_config.logging.trace_decisions) {
        std::cout << "Messagelet route - Src " << state.from
                  << " - Dst " << state.to
                  << " - Tag " << state.tag
                  << " - FlowId " << (state.has_flow_id ? std::to_string(state.flow_id) : "unassigned")
                  << " - Index " << messagelet_index
                  << " - Granularity " << MessageletConfig::granularityName(state.granularity)
                  << " - Size " << state.messagelet_size
                  << " - PathId " << route.path_id
                  << " - Route " << route.description
                  << " - TransportFlowId " << source.flowId();
        if (source.lgs_node) {
            std::cout << " - SendTaskId " << source.lgs_node->offset;
        }
        for (const auto& item : route.log_info) {
            std::cout << " - " << item.first << " " << item.second;
        }
        std::cout << std::endl;
    }
    return route.path_id;
}

const MessageletInfo& MessageletRuntime::messageletInfo(const UecSrc& source, uint64_t index) {
    auto& state = mutableFlowState(source);
    auto found = state.messagelets.find(index);
    if (found != state.messagelets.end()) return found->second;
    const uint64_t mtu = Packet::data_packet_size();
    const uint64_t mss = mtu - UecSrc::_hdr_size;
    const uint64_t packets_per_mlet = state.granularity == Granularity::Flow ?
        std::max<uint64_t>(1, state.flow_size / mss + (state.flow_size % mss != 0)) :
        (state.messagelet_size / mtu + (state.messagelet_size % mtu != 0));
    const uint64_t packet_count = state.flow_size / mss + (state.flow_size % mss != 0);
    const uint64_t total = packet_count / packets_per_mlet + (packet_count % packets_per_mlet != 0);
    const uint64_t payload = std::min(state.flow_size - state.original_payload_sent, packets_per_mlet * mss);
    return state.messagelets.emplace(index, MessageletInfo{
        {_config.runtime.dag_id, state.flow_id, index}, {payload, total}}).first->second;
}

void MessageletRuntime::bindPacket(const UecSrc& source, uint64_t index, uint64_t seqno, uint64_t bytes) {
    const auto info = messageletInfo(source, index);
    auto& state = mutableFlowState(source);
    if (!entrySwitchRouting()) {
        auto& topology = *_goal_runtime.getDragonflyTopology();
        auto& gra = topology.groupRoutingAgent(topology.get_host_switch(state.from) / topology.get_a());
        const auto corridor = DragonflySwitch::get_routing_strategy() == DragonflySwitch::SOURCE ?
            gra.corridorForRoute(state.from, state.to, state.messagelet_routes.at(index).path_id) : std::nullopt;
        if (corridor) gra.bindPacket(info.key, seqno, bytes);
    }
    state.original_payload_sent += bytes;
}

void MessageletRuntime::setNetworkDecision(std::unique_ptr<NetworkDecision> decision) {
    if (decision && entrySwitchRouting())
        throw std::logic_error("Install ENTRY_SWITCH policies through routing.network_decision");
    if (decision && transparent()) {
        throw std::logic_error("Transparent runtime cannot install a network decision policy");
    }
    if (decision && _routing_config_loaded) {
        throw std::logic_error("Network decision policy cannot be combined with a static routing config");
    }
    _network_decision = std::move(decision);
}

void MessageletRuntime::setGranularity(const UecSrc& source,
                                      Granularity granularity,
                                      uint64_t size_bytes) {
    MessageletConfig settings;
    settings.runtime.granularity = granularity;
    settings.messagelet.size_bytes = size_bytes;
    settings.validate();
    auto& state = mutableFlowState(source);
    if (entrySwitchRouting() && !state.messagelets.empty())
        throw std::logic_error("Cannot change ENTRY_SWITCH segmentation after metadata has been issued");
    state.granularity = granularity;
    state.messagelet_size = routingUnitSize(granularity, state.flow_size, size_bytes);
}

void MessageletRuntime::setMessageletSize(const UecSrc& source, uint64_t size) {
    setGranularity(source, Granularity::Messagelet, size);
}

void MessageletRuntime::setMessageletRoute(const UecSrc& source,
                                           uint64_t messagelet_index,
                                           RouteInfo route_info) {
    if (entrySwitchRouting())
        throw std::logic_error("ENTRY_SWITCH runtime does not assign messagelet routes");
    if (transparent()) {
        throw std::logic_error("Transparent runtime does not assign messagelet routes");
    }
    FlowState& state = mutableFlowState(source);
    if (state.finalized_routes.count(messagelet_index)) {
        throw std::logic_error("Cannot change a messagelet route after it has been labelled");
    }
    if (messagelet_index >= state.messagelet_routes.size()) {
        state.messagelet_routes.resize(messagelet_index + 1, state.default_route);
    }
    state.messagelet_routes[messagelet_index] = std::move(route_info);
    state.route_overrides.insert(messagelet_index);
}

const MessageletRuntime::FlowState& MessageletRuntime::flowState(const UecSrc& source) const {
    auto flow = _flows.find(&source);
    if (flow == _flows.end()) {
        throw std::logic_error("UecSrc is not registered with the messagelet runtime");
    }
    return flow->second;
}

void MessageletRuntime::registerFlow(UecSrc& source,
                                     uint64_t flow_size,
                                     uint32_t from,
                                     uint32_t to,
                                     uint32_t tag,
                                     const FlowRoutingConfig::Entry* routing) {
    FlowState state;
    state.from = from;
    state.to = to;
    state.tag = tag;
    state.flow_size = flow_size;
    state.granularity = _config.runtime.granularity;
    state.messagelet_size = routingUnitSize(state.granularity, flow_size, _config.messagelet.size_bytes);
    state.flow_id = (uint64_t{from} << 32) | static_cast<uint32_t>(_flows.size());
    if (transparent() || entrySwitchRouting()) {
        state.default_route.description = entrySwitchRouting() ? "entry_switch" : "transparent";
    } else {
        state.default_route = minimumRoute(from, to);
    }
    if (routing) {
        state.flow_id = routing->flow_id;
        state.has_flow_id = true;
    }
    if (routing && _routing_config_loaded) {
        if (routing->label == "valiant") {
            state.default_route = valiantRoute(from, to, routing->flow_id);
        }
        auto* metric = htsim::DataCollector::get_instance().RegisterCsvMetric(
            "flowRouting", {"flow_id", "src_rank", "dst_rank", "send_task_id", "src_host",
                            "dst_host", "label", "path_id", "intermediate_group"});
        const auto intermediate = state.default_route.log_info.find("intermediate_group");
        metric->LogData({std::to_string(routing->flow_id), std::to_string(routing->src),
                         std::to_string(routing->dst), std::to_string(routing->send_task_id),
                         std::to_string(from), std::to_string(to), routing->label,
                         std::to_string(state.default_route.path_id),
                         intermediate == state.default_route.log_info.end() ? "" : intermediate->second});
    }

    auto inserted = _flows.emplace(&source, std::move(state));
    if (!inserted.second) {
        throw std::logic_error("UecSrc is already registered with the messagelet runtime");
    }
}

MessageletRuntime::RouteInfo MessageletRuntime::minimumRoute(uint32_t from, uint32_t to) const {
    DragonflyTopology* topology = _goal_runtime.getDragonflyTopology();
    if (!topology) {
        throw std::logic_error("Dragonfly topology not set");
    }

    const uint32_t src_switch = topology->get_host_switch(from);
    const uint32_t dst_switch = topology->get_host_switch(to);
    const uint32_t switches_per_group = topology->get_a();
    const uint32_t src_group = src_switch / switches_per_group;
    const uint32_t dst_group = dst_switch / switches_per_group;

    uint32_t hop_one_switch = 0;
    uint32_t hop_two_switch = 0;

    if (src_switch == dst_switch) {
        // Delivered locally before SOURCE routing consults pathid.
    } else if (src_group == dst_group) {
        hop_one_switch = dst_switch;
    } else {
        const uint32_t src_group_switch =
            topology->get_group_switch(src_group, dst_group);
        const uint32_t dst_group_switch =
            topology->get_group_switch(dst_group, src_group);

        if (src_switch == src_group_switch) {
            hop_one_switch = dst_group_switch;
        } else {
            hop_one_switch = src_group_switch;
            hop_two_switch = dst_group_switch;
        }
    }

    RouteInfo route;
    route.path_id = ((hop_one_switch & 0xFFFF) << 16) | (hop_two_switch & 0xFFFF);
    route.description = "minimum";
    route.log_info.emplace("hop_one_switch", std::to_string(hop_one_switch));
    route.log_info.emplace("hop_two_switch", std::to_string(hop_two_switch));
    return route;
}

MessageletRuntime::RouteInfo MessageletRuntime::valiantRoute(uint32_t from, uint32_t to,
                                                           uint64_t flow_id) const {
    DragonflyTopology* topology = _goal_runtime.getDragonflyTopology();
    const uint32_t a = topology->get_a();
    const uint32_t src_group = topology->get_host_switch(from) / a;
    const uint32_t dst_group = topology->get_host_switch(to) / a;
    std::vector<uint32_t> candidates;
    for (uint32_t group = 0; group < topology->get_no_groups(); ++group) {
        if (group != src_group && group != dst_group) {
            candidates.push_back(group);
        }
    }
    if (candidates.empty()) {
        throw std::runtime_error("No intermediate group for valiant flow " + std::to_string(flow_id));
    }
    const uint32_t intermediate_group = candidates[flow_id % candidates.size()];
    const uint32_t waypoint = topology->get_group_switch(intermediate_group, src_group);
    RouteInfo route;
    route.path_id = DragonflySourceRoute::kWaypointFlag | waypoint;
    route.description = "valiant";
    route.log_info.emplace("intermediate_group", std::to_string(intermediate_group));
    route.log_info.emplace("waypoint_switch", std::to_string(waypoint));
    return route;
}

MessageletRuntime::FlowState& MessageletRuntime::mutableFlowState(const UecSrc& source) {
    auto flow = _flows.find(&source);
    if (flow == _flows.end()) {
        throw std::logic_error("UecSrc is not registered with the messagelet runtime");
    }
    return flow->second;
}
