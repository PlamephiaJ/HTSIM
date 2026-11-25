#include "online_fct_predictor.h"

#include <algorithm>
#include <stdexcept>
#include <string>

OnlineFctPredictor::OnlineFctPredictor(const GraStaticGraph& graph) {
    for (const auto& arc : graph.eventArcs()) {
        if (arc.source.node_index != arc.target.node_index ||
            arc.source.event != GraStaticGraph::Event::START ||
            arc.target.event != GraStaticGraph::Event::FINISH) continue;
        const auto& node = graph.dags().at(arc.source.dag_index).nodes.at(arc.source.node_index);
        if (node.type != GraStaticGraph::Type::COMM) continue;
        if (!node.collective_id)
            throw std::invalid_argument("global_slack_online requires collective_id metadata for DAG " +
                std::to_string(arc.source.dag_index) + " flow " + std::to_string(node.flow_id));
        _flows.emplace(Flow{arc.source.dag_index, node.flow_id},
                       FlowInfo{{arc.source.dag_index, *node.collective_id, node.comm_position}, arc.cost_ns});
    }
}

bool OnlineFctPredictor::observeCompletion(Flow flow, uint64_t start_ps, uint64_t end_ps, uint64_t now_ps) {
    const auto& info = _flows.at(flow);
    if (end_ps < start_ps || end_ps > now_ps)
        throw std::invalid_argument("Online FCT observation must be a completed network flow visible now");
    // Convert the duration, not separately rounded start/end timestamps.
    // Visibility uses the estimator's integer-ns clock; sub-ns FCT is retained.
    const Sample sample{now_ps / 1000, static_cast<long double>(end_ps - start_ps) / 1000.0L};
    if (!_completed.emplace(flow, sample).second) return false;
    auto& history = _history[info.window];
    history.recent.push_back(sample);
    if (history.recent.size() > window_size) history.recent.pop_front();
    history.lifetime.add(sample.fct_ns);
    history.latest_visible_ns = std::max(history.latest_visible_ns, sample.visible_ns);
    return true;
}

OnlineFctPredictor::Prediction OnlineFctPredictor::predict(Flow flow, uint64_t now_ns, Horizon horizon) const {
    const auto& info = _flows.at(flow);
    Totals samples;
    const auto found = _history.find(info.window);
    if (found != _history.end()) {
        const auto& history = found->second;
        if (horizon == Horizon::ShortTerm) {
            for (const auto& sample : history.recent)
                if (sample.visible_ns <= now_ns) samples.add(sample.fct_ns);
        } else if (history.latest_visible_ns <= now_ns) {
            samples = history.lifetime;
        } else {
            // Past-time queries can recover the visible lifetime samples from
            // the same completions retained for per-flow actual FCTs.
            for (const auto& item : _completed)
                if (item.second.visible_ns <= now_ns && _flows.at(item.first).window == info.window)
                    samples.add(item.second.fct_ns);
        }
    }
    const auto& [dag_id, collective_id, comm_position] = info.window;
    const auto completed = _completed.find(flow);
    if (completed != _completed.end() && completed->second.visible_ns <= now_ns)
        return {collective_id, comm_position, completed->second.fct_ns, samples.count, false, horizon};
    const long double fct_ns = samples.count ? samples.sum_ns / samples.count : info.nominal_ns;
    return {collective_id, comm_position, fct_ns, samples.count, samples.count == 0, horizon};
}
