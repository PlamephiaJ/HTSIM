#ifndef GRA_GOAL_GRAPH_H
#define GRA_GOAL_GRAPH_H

#include "flow_routing_config.h"
#include "gra_static_graph.h"

class Parser;

struct GraGoalGraph {
    GraStaticGraph::Dag dag;
    FlowRoutingConfig identities;
};

// Merge each GOAL SEND/RECV pair into one COMM, preserving dependencies from
// both ranks. When identities are absent, (src_rank << 32) | send_task_id is
// the stable DAG flow ID and is also used by the messagelet runtime.
GraGoalGraph buildGraGoalGraph(const Parser& parser, uint64_t dag_index,
                              const FlowRoutingConfig* identities = nullptr);

#endif
