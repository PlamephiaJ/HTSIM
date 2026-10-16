#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <unistd.h>

#include "network_decision.h"
#include "atlahs_dragonfly_api.h"
#include "logsim-interface.h"
#include "datacenter/dragonfly_topology.h"
#include "messagelet_runtime.h"
#include "uec.h"
#include "uec_mp.h"

namespace {
class SizedQueue : public Queue {
public:
    SizedQueue(EventList& events, mem_b bytes)
        : Queue(speedFromGbps(200), 1000000, events, nullptr), bytes(bytes) {}
    mem_b queuesize() const override { return bytes; }
    mem_b bytes;
};

void checkUgalCosts() {
    EventList events;
    DragonflyTopology topology(COMPOSITE, 1000000, nullptr, &events);
    topology.construct_topology(2, 4, 2);
    // Independent switch objects restart HTSim's seed-33 candidate sequence.
    // Src switch 0 reaches dst switch 12 through local switch 1 in three
    // hops. Its first random Valiant choice is global switch 11.
    SizedQueue min_queue(events, 0);
    SizedQueue val_queue(events, 0);
    topology.queues_switch_switch[0][1] = &min_queue;
    topology.queues_switch_switch[0][11] = &val_queue;
    auto decide = [&]() {
        DragonflySwitch source(events, "test", DragonflySwitch::GENERAL, 0, 0, &topology);
        return source.ugalDecision(0, 12);
    };
    const auto idle = decide();
    assert(idle.compared && idle.next_min == 1 && idle.next_val == 11);
    assert(idle.h_min == 3 && idle.h_val > idle.h_min);
    assert(!idle.use_valiant && idle.next_switch == 1);

    min_queue.bytes = 1000;
    const auto congested = decide();
    assert(congested.use_valiant && congested.next_switch == 11);
    assert(topology.queues_switch_switch[11][congested.second_switch] != nullptr);

    // A smaller queue alone does not justify the longer path. Exercise both
    // sides of the hop-weighted boundary and the nonzero equal-cost case.
    min_queue.bytes = idle.h_val * 100;
    val_queue.bytes = idle.h_min * 100;
    assert(!decide().use_valiant);
    min_queue.bytes++;
    assert(decide().use_valiant);
    min_queue.bytes -= 2;
    assert(!decide().use_valiant);

    // Policy encoding freezes the very same candidate used for hop counting.
    min_queue.bytes = 1000;
    val_queue.bytes = 0;
    UgalLNetworkDecision policy;
    const auto route = policy.decide({topology, 0, 24, 7, 100000, 8192, 0});
    assert(route.description == "valiant");
    assert(route.path_id == (congested.next_switch << 16 | congested.second_switch));
    assert(route.log_info.at("q_min_bytes") == "1000");
    assert(policy.decide({topology, 0, 1, 7, 100000, 8192, 1}).path_id == 0);

    // Walk a seeded sequence in lockstep with a separate HTSim switch. This
    // covers sampling a second/global hop after a local Valiant first hop.
    DragonflySwitch reference(events, "reference", DragonflySwitch::GENERAL, 0, 0, &topology);
    DragonflySwitch actual(events, "actual", DragonflySwitch::GENERAL, 0, 0, &topology);
    auto* original_source = topology.switches[0];
    topology.switches[0] = &actual;
    bool saw_local_valiant = false;
    for (unsigned index = 0; index < 64; ++index) {
        const auto expected = reference.ugalDecision(0, 12);
        const auto selected = policy.decide({topology, 0, 24, 7, 100000, 8192, index});
        assert(selected.path_id == (expected.next_switch << 16 | expected.second_switch));
        if (expected.use_valiant && expected.next_switch < topology.get_a()) {
            saw_local_valiant = true;
            assert(expected.second_switch >= topology.get_a());
            assert(topology.queues_switch_switch[expected.next_switch][expected.second_switch]);
        }
    }
    assert(saw_local_valiant);
    topology.switches[0] = original_source;

    // Exercise the no-comparison fast path with a == 1, h == 1.
    DragonflyTopology tiny(COMPOSITE, 1000000, nullptr, &events);
    tiny.construct_topology(2, 1, 1);
    auto* source = static_cast<DragonflySwitch*>(tiny.switches[0]);
    const auto same_candidate = source->ugalDecision(0, 1);
    assert(!same_candidate.compared && !same_candidate.use_valiant);
    assert(same_candidate.next_min == same_candidate.next_val);
    assert(policy.decide({tiny, 0, 2, 7, 4096, 4096, 0}).path_id == (1u << 16));

    // Two-switch same-group traffic must terminate, including nonzero groups.
    DragonflyTopology pair(COMPOSITE, 1000000, nullptr, &events);
    pair.construct_topology(2, 2, 1);
    assert(policy.decide({pair, 4, 6, 7, 4096, 4096, 0}).path_id == (3u << 16));
}

struct PacketRecord {
    uint64_t seqno;
    uint32_t path_id;
    bool retransmitted;
};

class CaptureQueue : public Queue {
public:
    CaptureQueue(EventList& events, Queue& downstream)
        : Queue(speedFromGbps(200), 1000000, events, nullptr), downstream(downstream) {
        setRemoteEndpoint(downstream.getRemoteEndpoint());
    }
    void receivePacket(Packet& packet) override {
        assert(packet.type() == UECDATA);
        auto& data = static_cast<UecDataPacket&>(packet);
        records.push_back({data.epsn(), data.pathid(), data.retransmitted()});
        // Force a real retransmission after later messagelets have been sent.
        if (records.size() == 1) {
            packet.free();
            return;
        }
        downstream.receivePacket(packet);
    }
    Queue& downstream;
    std::vector<PacketRecord> records;
};

class AlternatingPolicy : public NetworkDecision {
public:
    explicit AlternatingPolicy(std::vector<uint64_t>& calls) : calls(calls) {}
    NetworkRoute decide(const NetworkDecisionContext& context) override {
        calls.push_back(context.messagelet_index);
        // Src 0 -> dst 8: either minimal via 11 or a detour via 1 -> 14.
        NetworkRoute route;
        route.path_id = context.messagelet_index % 2 == 0 ?
                            (11u << 16) : (1u << 16 | 14u);
        return route;
    }
    std::vector<uint64_t>& calls;
};

void checkRuntimeFreezingAndRetransmission() {
    auto& events = EventList::getTheEventList();
    events.setEndtime(timeFromMs(10));
    DragonflySwitch::set_config(DragonflySwitch::SOURCE, false, 64);
    DragonflyTopology topology(COMPOSITE, 1000000, nullptr, &events);
    topology.construct_topology(2, 4, 2);
    CaptureQueue capture(events, *topology.queues_host_switch[0][0]);
    topology.queues_host_switch[0][0] = &capture;

    AtlahsDragonflyApi api;
    api.setDragonflyTopology(&topology);
    api.setEventList(&events);
    api.linkspeed = topology.get_linkspeed();
    api.total_nodes = topology.get_no_hosts();
    const auto rtt = topology.get_max_rtt(DragonflySwitch::SOURCE, UecSrc::_mtu, 64);
    api.setMaxRtt(rtt);
    UecSrc::_sender_based_cc = true;
    UecSrc::_receiver_based_cc = false;
    UecSrc::initNsccParams(rtt, api.linkspeed, 0, -1, true);
    UecSrc::_global_node_count = api.total_nodes;
    LogSimInterface lgs(nullptr, nullptr, events, nullptr, nullptr);
    lgs.set_protocol(UEC_PROTOCOL);
    api.setLogSimInterface(&lgs);
    api.setMultipathFactory([]() { return std::make_unique<UecMpOblivious>(64, false); });
    api.Setup();

    auto& runtime = api.getMessageletRuntime();
    char config_path[] = "/tmp/htsim-network-decision-XXXXXX";
    const int config_fd = mkstemp(config_path);
    assert(config_fd >= 0);
    close(config_fd);
    {
        std::ofstream config(config_path);
        config << R"({"messagelet":{"size_bytes":8192}})";
    }
    runtime.loadConfig(config_path);
    std::remove(config_path);
    std::vector<uint64_t> calls;
    runtime.setNetworkDecision(std::make_unique<AlternatingPolicy>(calls));
    runtime.submit(SendEvent(0, 16, 1000000, 7, 0), graph_node_properties{});
    assert(events.doNextEvent());
    assert(!api.uec_nics[0]->_active_srcs.empty());
    auto& source = *api.uec_nics[0]->_active_srcs.front();
    assert(calls == std::vector<uint64_t>{0});
    const uint32_t original = runtime.pathIdForMessagelet(source, 0);
    assert(calls.size() == 1);
    bool rejected = false;
    try {
        runtime.setMessageletRoute(source, 0, {});
    } catch (const std::logic_error&) {
        rejected = true;
    }
    assert(rejected);
    // Check future overrides without creating artificial decisions for gaps.
    NetworkRoute override;
    override.path_id = 11u << 16;
    runtime.setMessageletRoute(source, 5, override);
    assert(runtime.pathIdForMessagelet(source, 5) == override.path_id);
    assert(calls.size() == 1);
    for (unsigned step = 0; calls.size() < 4 && step < 10000; ++step) {
        assert(events.doNextEvent());
    }
    assert((calls == std::vector<uint64_t>{0, 1, 2, 3}));
    assert(capture.records.size() >= 7);
    for (size_t packet = 0; packet < 7; ++packet) {
        assert(!capture.records[packet].retransmitted);
        const uint32_t expected = (packet / 2) % 2 == 0 ?
                                      (11u << 16) : (1u << 16 | 14u);
        assert(capture.records[packet].path_id == expected);
    }

    // Replace the policy while old packets are in flight. The first flow's
    // messagelet and retransmission must retain the original path label.
    runtime.setNetworkDecision(std::make_unique<UgalLNetworkDecision>());
    assert(runtime.pathIdForMessagelet(source, 0) == original);
    bool saw_retransmission = false;
    for (unsigned step = 0; step < 200000 && events.doNextEvent(); ++step) {
        for (const auto& record : capture.records) {
            if (record.seqno == 0 && record.retransmitted) {
                assert(record.path_id == original);
                saw_retransmission = true;
            }
        }
        if (saw_retransmission)
            break;
    }
    assert(saw_retransmission);
}
} // namespace

int main() {
    Packet::set_packet_size(4160);
    assert(!makeNetworkDecision("none"));
    assert(makeNetworkDecision("ugal_l"));
    bool rejected = false;
    try {
        makeNetworkDecision("unknown");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
    checkUgalCosts();
    checkRuntimeFreezingAndRetransmission();
    std::cout << "Network decision tests passed\n";
}
