#include "online_fct_predictor.h"

#include <algorithm>
#include <stdexcept>
#include <string>

OnlineFctPredictor::OnlineFctPredictor(const GraStaticGraph& graph) : _graph(graph), _components(graph.componentCount()) {
    std::map<WindowKey, size_t> windows;
    for (const auto& arc : graph.eventArcs()) {
        if (arc.source.node_index != arc.target.node_index ||
            arc.source.event != GraStaticGraph::Event::START ||
            arc.target.event != GraStaticGraph::Event::FINISH) continue;
        const auto& node = graph.dags().at(arc.source.dag_index).nodes.at(arc.source.node_index);
        if (node.type != GraStaticGraph::Type::COMM) continue;
        if (!node.collective_id)
            throw std::invalid_argument("global_slack_online requires collective_id metadata for DAG " +
                std::to_string(arc.source.dag_index) + " flow " + std::to_string(node.flow_id));
        const WindowKey key{arc.source.dag_index, *node.collective_id, node.comm_position};
        const auto [window, inserted] = windows.emplace(key, _history.size());
        if (inserted) _history.emplace_back(key);
        _flows.emplace(Flow{arc.source.dag_index, node.flow_id}, _flow_info.size());
        _flow_info.push_back({window->second, arc.cost_ns});
        _history[window->second].components.push_back(graph.eventComponent(graph.eventIndex(arc.source)));
    }
    // Historical queries previously summed the visible samples in FlowKey
    // order. Keep that order even though ordinary queries use dense indices.
    for (const auto& entry : _flows) _history[_flow_info[entry.second].window].flows.push_back(entry.second);
    for (auto& history : _history) {
        auto& components = history.components;
        std::sort(components.begin(), components.end());
        components.erase(std::unique(components.begin(), components.end()), components.end());
    }
    _completed.resize(_flow_info.size());
}

bool OnlineFctPredictor::observeCompletion(Flow flow, uint64_t start_ps, uint64_t end_ps, uint64_t now_ps) {
    const auto index = flowIndex(flow);
    if (end_ps < start_ps || end_ps > now_ps)
        throw std::invalid_argument("Online FCT observation must be a completed network flow visible now");
    // Convert the duration, not separately rounded start/end timestamps.
    // Visibility uses the estimator's integer-ns clock; sub-ns FCT is retained.
    const Sample sample{now_ps / 1000, static_cast<long double>(end_ps - start_ps) / 1000.0L};
    if (_completed[index]) return false;
    _completed[index] = sample;
    auto& history = _history[_flow_info[index].window];
    history.recent.push_back(sample);
    if (history.recent.size() > window_size) history.recent.pop_front();
    history.lifetime.add(sample.fct_ns);
    history.latest_visible_ns = std::max(history.latest_visible_ns, sample.visible_ns);
    for (const auto component : history.components) {
        auto& state = _components[component];
        ++state.revision;
        state.latest_visible_ns = std::max(state.latest_visible_ns, sample.visible_ns);
    }
    return true;
}

OnlineFctPredictor::Prediction OnlineFctPredictor::predict(Flow flow, uint64_t now_ns, Horizon horizon) const {
    return predictIndexed(flowIndex(flow), now_ns, horizon);
}

OnlineFctPredictor::Prediction OnlineFctPredictor::predictIndexed(size_t index, uint64_t now_ns, Horizon horizon) const {
    const auto& info = _flow_info[index];
    const auto& history = _history[info.window];
    Totals samples;
    if (horizon == Horizon::ShortTerm) {
        for (const auto& sample : history.recent)
            if (sample.visible_ns <= now_ns) samples.add(sample.fct_ns);
    } else if (history.latest_visible_ns <= now_ns) {
        samples = history.lifetime;
    } else {
        // Past-time queries recover only this window's visible lifetime samples.
        for (const auto member : history.flows) {
            const auto& sample = _completed[member];
            if (sample && sample->visible_ns <= now_ns) samples.add(sample->fct_ns);
        }
    }
    const auto& [dag_id, collective_id, comm_position] = history.key;
    const auto& completed = _completed[index];
    if (completed && completed->visible_ns <= now_ns)
        return {collective_id, comm_position, completed->fct_ns, samples.count, false, horizon};
    const long double fct_ns = samples.count ? samples.sum_ns / samples.count : info.nominal_ns;
    return {collective_id, comm_position, fct_ns, samples.count, samples.count == 0, horizon};
}
