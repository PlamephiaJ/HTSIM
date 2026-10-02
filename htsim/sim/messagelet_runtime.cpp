#include "messagelet_runtime.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "atlahs_dragonfly_api.h"
#include "logsim-interface.h"
#include "datacenter/dragonfly_topology.h"
#include "uec.h"

MessageletRuntime::MessageletRuntime(AtlahsDragonflyApi& goal_runtime)
    : _goal_runtime(goal_runtime) {}

void MessageletRuntime::submit(const SendEvent& event, graph_node_properties node) {
    DragonflyTopology* topology = _goal_runtime.getDragonflyTopology();
    if (!topology) {
        throw std::logic_error("Dragonfly topology not set");
    }

    int to = event.getTo();
    int from = event.getFrom();
    int tag = event.getTag();
    int size = event.getSizeBytes();

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
    uec_src->_messagelet_runtime = this;
    registerFlow(*uec_src, size, from, to, tag);

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

    topology->switches[src_switch]->addHostPort(
        from, uec_sink->flowId(), uec_src->getPort(0));
    topology->switches[dst_switch]->addHostPort(
        to, uec_src->flowId(), uec_sink->getPort(0));
}

void MessageletRuntime::eventFinished(const EventOver& event) {
    _goal_runtime.EventFinished(event);
}

void MessageletRuntime::loadConfig(const std::string& filename) {
    if (!_flows.empty()) {
        throw std::logic_error("Messagelet config must be loaded before submitting flows");
    }
    _config = MessageletConfig::loadFromFile(filename);
}

uint64_t MessageletRuntime::messageletSize(const UecSrc& source) const {
    return flowState(source).messagelet_size;
}

uint32_t MessageletRuntime::pathIdForMessagelet(const UecSrc& source,
                                                uint64_t messagelet_index) {
    FlowState& state = mutableFlowState(source);
    if (messagelet_index >= state.messagelet_routes.size()) {
        state.messagelet_routes.resize(messagelet_index + 1, state.default_route);
    }
    const RouteInfo& route = state.messagelet_routes[messagelet_index];
    if (_config.logging.trace_decisions) {
        std::cout << "Messagelet route - Src " << state.from
                  << " - Dst " << state.to
                  << " - Tag " << state.tag
                  << " - Index " << messagelet_index
                  << " - Size " << state.messagelet_size
                  << " - PathId " << route.path_id
                  << " - Route " << route.description << std::endl;
    }
    return route.path_id;
}

void MessageletRuntime::setMessageletSize(const UecSrc& source, uint64_t size) {
    if (size == 0) {
        throw std::invalid_argument("Messagelet size must be greater than zero");
    }
    mutableFlowState(source).messagelet_size = size;
}

void MessageletRuntime::setMessageletRoute(const UecSrc& source,
                                           uint64_t messagelet_index,
                                           RouteInfo route_info) {
    FlowState& state = mutableFlowState(source);
    if (messagelet_index >= state.messagelet_routes.size()) {
        state.messagelet_routes.resize(messagelet_index + 1, state.default_route);
    }
    state.messagelet_routes[messagelet_index] = std::move(route_info);
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
                                     uint32_t tag) {
    FlowState state;
    state.from = from;
    state.to = to;
    state.tag = tag;
    state.flow_size = flow_size;
    state.messagelet_size = _config.messagelet.size_bytes == 0
                                ? std::max<uint64_t>(flow_size, 1)
                                : _config.messagelet.size_bytes;
    state.default_route = minimumRoute(from, to);

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

MessageletRuntime::FlowState& MessageletRuntime::mutableFlowState(const UecSrc& source) {
    auto flow = _flows.find(&source);
    if (flow == _flows.end()) {
        throw std::logic_error("UecSrc is not registered with the messagelet runtime");
    }
    return flow->second;
}
