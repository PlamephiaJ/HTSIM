#include "messagelet_runtime.h"

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
