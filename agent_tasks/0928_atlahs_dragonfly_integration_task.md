# Task: Add ATLAHS/GOAL support to HTSim Dragonfly

## Goal

Enable `htsim_uec_df` to run ATLAHS/GOAL workloads directly on the existing HTSim Dragonfly topology and UEC transport.

The first working target is:

```text
GOAL workload
  -> LogSimInterface
  -> AtlahsDragonflyApi
  -> UecSrc / UecSink
  -> Dragonfly MINIMAL or UGAL_L routing
  -> flow completion
  -> ATLAHS EventFinished()
  -> GOAL dependency progression
```

The implementation should reuse the existing Dragonfly routing and UEC behavior.

---

## 1. Extend `AtlahsHtsimApi` with a protected multipath helper

File:

```text
htsim/sim/atlahs_htsim_api.h
```

`mp_factory` is currently private. Add a protected helper that creates one per-flow `UecMultipath` instance.

Target shape:

```cpp
protected:
    std::unique_ptr<UecMultipath> createMultipath() {
        if (!mp_factory) {
            throw std::logic_error("Multipath factory not set");
        }
        return mp_factory();
    }
```

Keep the existing `setMultipathFactory(...)` interface.

---

## 2. Add `AtlahsDragonflyApi`

Add:

```text
htsim/sim/atlahs_dragonfly_api.h
htsim/sim/atlahs_dragonfly_api.cpp
```

Define:

```cpp
class AtlahsDragonflyApi : public AtlahsHtsimApi
```

The class should:

- override `Send(const SendEvent&, graph_node_properties)`
- store a `DragonflyTopology*`
- store the Dragonfly `max_rtt`
- provide setters for both

Suggested state:

```cpp
DragonflyTopology* _df_topo = nullptr;
simtime_picosec _max_rtt = 0;
```

The existing inherited implementations of `Setup()`, `Recv()`, `Calc()`, and `EventFinished()` should continue to be used.

---

## 3. Implement Dragonfly-specific `Send()`

Use the current implementation of:

```text
htsim/sim/atlahs_htsim_api.cpp
AtlahsHtsimApi::Send()
```

as the base.

Preserve the ATLAHS/UEC flow setup:

```cpp
int to = event.getTo();
int from = event.getFrom();
int tag = event.getTag();
int size = event.getSizeBytes();

from = getHtsimNodeNumber(from, elem.nic);
to = getHtsimNodeNumber(to, elem.nic);

auto per_flow_mp = createMultipath();

UecSrc* uecSrc = new UecSrc(
    traffic_logger,
    *getEventList(),
    std::move(per_flow_mp),
    *uec_nics.at(from),
    1
);

uecSrc->setFlowsize(size);
uecSrc->initNscc(cwnd_b, _max_rtt);

uecSrc->setName(...);
uecSrc->from = from;
uecSrc->to = to;
uecSrc->tag = tag;
uecSrc->send_size = size;
uecSrc->_atlahs_api = this;

UecSink* uecSink = new UecSink(
    traffic_logger,
    linkspeed,
    1.1,
    UecBasePacket::unquantize(UecSink::_credit_per_pull),
    *getEventList(),
    *uec_nics.at(to),
    1
);

uecSrc->set_dst(to);
uecSrc->setSrc(from);
uecSrc->setDst(to);
uecSink->set_src(from);

graph_node_properties* node_copy = new graph_node_properties(elem);
uecSrc->lgs_node = node_copy;
```

Use Dragonfly topology attachment instead of the FatTree attachment.

Source-side attachment:

```cpp
uint32_t src_switch = _df_topo->get_host_switch(from);

Route* srctotor = new Route();
srctotor->push_back(
    _df_topo->queues_host_switch[from][src_switch]);
srctotor->push_back(
    _df_topo->pipes_host_switch[from][src_switch]);
srctotor->push_back(
    _df_topo->queues_host_switch[from][src_switch]
        ->getRemoteEndpoint());
```

Destination-side attachment:

```cpp
uint32_t dst_switch = _df_topo->get_host_switch(to);

Route* dsttotor = new Route();
dsttotor->push_back(
    _df_topo->queues_host_switch[to][dst_switch]);
dsttotor->push_back(
    _df_topo->pipes_host_switch[to][dst_switch]);
dsttotor->push_back(
    _df_topo->queues_host_switch[to][dst_switch]
        ->getRemoteEndpoint());
```

Connect the flow:

```cpp
uecSrc->connectPort(
    0,
    *srctotor,
    *dsttotor,
    *uecSink,
    getEventList()->now());
```

Register host ports:

```cpp
_df_topo->switches[src_switch]->addHostPort(
    from,
    uecSink->flowId(),
    uecSrc->getPort(0));

_df_topo->switches[dst_switch]->addHostPort(
    to,
    uecSrc->flowId(),
    uecSink->getPort(0));
```

Use the Dragonfly topology-level RTT already computed by:

```cpp
topo->get_max_rtt(
    routing_strategy,
    Packet::data_packet_size(),
    UecBasePacket::get_ack_size()
);
```

for:

```cpp
uecSrc->initNscc(cwnd_b, _max_rtt);
```

---

## 4. Add `-goal` support to `main_uec_df.cpp`

File:

```text
htsim/sim/datacenter/main_uec_df.cpp
```

Add:

```cpp
std::string goal_filename = "";
```

Add CLI parsing:

```cpp
} else if (!strcmp(argv[i], "-goal")) {
    goal_filename = argv[i + 1];
    i++;
```

---

## 5. Reorder Dragonfly initialization so GOAL mode does not require `-tm`

Current Dragonfly flow loads `ConnectionMatrix` immediately after topology creation.

Refactor the initialization order to:

```text
load Dragonfly topology
-> determine no_hosts
-> set UecSrc::_global_node_count
-> compute max_rtt
-> get linkspeed
-> initialize global NSCC params
-> if GOAL mode: initialize ATLAHS and run start_lgs()
-> otherwise: load ConnectionMatrix and run normal TM mode
```

`ConnectionMatrix` creation/loading must therefore happen only in the normal TM path.

---

## 6. Add the GOAL/ATLAHS branch to `main_uec_df.cpp`

After Dragonfly topology, `no_hosts`, `max_rtt`, `linkspeed`, and global NSCC initialization are available, add a GOAL branch.

Target structure:

```cpp
if (!goal_filename.empty()) {
    AtlahsDragonflyApi* api = new AtlahsDragonflyApi();

    api->setDragonflyTopology(topo);
    api->setMaxRtt(max_rtt);

    api->cwnd_b = cwnd_b;
    api->setEventList(&eventlist);
    api->setComputeEvent(new ComputeEvent(eventlist));
    api->setNullEvent(new NullEvent(eventlist));

    LogSimInterface* lgs = new LogSimInterface(
        nullptr,
        traffic_logger,
        eventlist,
        nullptr,
        nullptr
    );

    lgs->htsim_api = api;
    api->setLogSimInterface(lgs);

    lgs->set_protocol(UEC_PROTOCOL);

    api->linkspeed = linkspeed;
    api->total_nodes = no_hosts;
    api->print_stats_flows = LogSimInterface::print_stats_flows;

    // set per-flow multipath factory here

    double linkSpeedBytesPerSec =
        (linkspeed / 1000000000 * 1e9) / 8.0;

    api->htsim_G = 1e9 / linkSpeedBytesPerSec;

    api->Setup();

    start_lgs(goal_filename, *lgs);

    return 0;
}
```

Use the Dragonfly `cwnd_b` directly:

```cpp
api->cwnd_b = cwnd_b;
```

---

## 7. Configure per-flow multipath factory in GOAL mode

Mirror the existing multipath factory setup from `main_uec.cpp`, using the algorithms supported by `main_uec_df.cpp`.

For example:

```cpp
case BITMAP:
    api->setMultipathFactory([path_entropy_size]() {
        return std::make_unique<UecMpBitmap>(
            path_entropy_size,
            UecSrc::_debug);
    });
    break;
```

Likewise support the existing Dragonfly-compatible:

```text
BITMAP
REPS
REPS_LEGACY
OBLIVIOUS
MIXED
ECMP
```

The immediate validation target is:

```text
MINIMAL
UGAL_L
```

with the existing Dragonfly routing machinery.

---

## 8. Update the build

Add:

```text
atlahs_dragonfly_api.cpp
```

to the same HTSim library/target that currently compiles:

```text
atlahs_htsim_api.cpp
```

Make sure `htsim_uec_df` links the new implementation.

---

## 9. Validation

### Build

Rebuild `htsim_uec_df`.

### Minimal GOAL workload

Use a very small GOAL workload with one communication dependency:

```text
rank 0 -> rank 1
1 MiB send/recv
```

Run first with:

```text
-routing MINIMAL
```

Then run the exact same GOAL workload with:

```text
-routing UGAL_L
```

Verify:

1. GOAL parses successfully.
2. `AtlahsDragonflyApi::Send()` is called.
3. `UecSrc/UecSink` are created.
4. Packets enter the Dragonfly network.
5. Flow completion reaches `AtlahsHtsimApi::EventFinished()`.
6. `LogSimInterface::flow_over()` advances the GOAL dependency.
7. `start_lgs()` terminates normally.

### Second validation

Use a small dependency chain:

```text
compute
-> send
-> compute
-> send
```

Verify that later operations are released only after the expected compute/communication dependencies complete.

---

## Expected result

After this change, the following should be possible:

```bash
./htsim_uec_df   -basepath topologies/dragonfly/p3a6h3   -goal <workload.goal>   -routing MINIMAL   -q 88
```

and:

```bash
./htsim_uec_df   -basepath topologies/dragonfly/p3a6h3   -goal <workload.goal>   -routing UGAL_L   -q 88
```

using the existing HTSim Dragonfly routing and UEC transport while letting ATLAHS/GOAL drive application-level compute and communication dependencies.
