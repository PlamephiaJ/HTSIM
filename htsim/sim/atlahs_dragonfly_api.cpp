#include "atlahs_dragonfly_api.h"

#include <stdexcept>

#include "logsim-interface.h"
#include "datacenter/dragonfly_topology.h"

void AtlahsDragonflyApi::Send(const SendEvent& event, graph_node_properties elem) {
    if (!_df_topo) {
        throw std::logic_error("Dragonfly topology not set");
    }

    int to = event.getTo();
    int from = event.getFrom();
    int tag = event.getTag();
    int size = event.getSizeBytes();

    from = getHtsimNodeNumber(from, elem.nic);
    to = getHtsimNodeNumber(to, elem.nic);

    if (from == to) {
        throw std::logic_error("Send event source and destination map to the same HTSim node");
    }

    if (getLogSimInterface()->get_protocol() != UEC_PROTOCOL) {
        throw std::logic_error("AtlahsDragonflyApi only supports UEC");
    }

    TrafficLoggerSimple* traffic_logger = nullptr;
    auto per_flow_mp = createMultipath();

    UecSrc* uec_src = new UecSrc(traffic_logger,
                                 *getEventList(),
                                 std::move(per_flow_mp),
                                 *uec_nics.at(from),
                                 1);
    uec_src->setFlowsize(size);
    uec_src->initNscc(cwnd_b, _max_rtt);
    uec_src->setName("uec_" + std::to_string(from) + "_" + std::to_string(to));
    uec_src->from = from;
    uec_src->to = to;
    uec_src->tag = tag;
    uec_src->send_size = size;
    uec_src->_atlahs_api = this;

    UecSink* uec_sink = new UecSink(traffic_logger,
                                    linkspeed,
                                    1.1,
                                    UecBasePacket::unquantize(UecSink::_credit_per_pull),
                                    *getEventList(),
                                    *uec_nics.at(to),
                                    1);
    uec_sink->setName("uec_sink_" + std::to_string(from) + "_" + std::to_string(to));
    uec_sink->from_sink = from;
    uec_sink->to_sink = to;
    uec_sink->tag_sink = tag;

    uec_src->set_dst(to);
    uec_src->setSrc(from);
    uec_src->setDst(to);
    uec_sink->set_src(from);

    uint32_t src_switch = _df_topo->get_host_switch(from);
    Route* srctotor = new Route();
    srctotor->push_back(_df_topo->queues_host_switch[from][src_switch]);
    srctotor->push_back(_df_topo->pipes_host_switch[from][src_switch]);
    srctotor->push_back(
        _df_topo->queues_host_switch[from][src_switch]->getRemoteEndpoint());

    uint32_t dst_switch = _df_topo->get_host_switch(to);
    Route* dsttotor = new Route();
    dsttotor->push_back(_df_topo->queues_host_switch[to][dst_switch]);
    dsttotor->push_back(_df_topo->pipes_host_switch[to][dst_switch]);
    dsttotor->push_back(
        _df_topo->queues_host_switch[to][dst_switch]->getRemoteEndpoint());

    graph_node_properties* node_copy = new graph_node_properties(elem);
    uec_src->lgs_node = node_copy;

    uec_src->connectPort(0, *srctotor, *dsttotor, *uec_sink, getEventList()->now());

    _df_topo->switches[src_switch]->addHostPort(
        from, uec_sink->flowId(), uec_src->getPort(0));
    _df_topo->switches[dst_switch]->addHostPort(
        to, uec_src->flowId(), uec_sink->getPort(0));
}
