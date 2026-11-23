#!/usr/bin/env python3
"""Generate Exp7.2 from the exact single-iteration Exp7.1 workload."""

import json
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory


ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
BASE = ROOT.parent / 'exp2s/exp2a1_collective_internal_slack/workload.meta.json'
ITERATIONS = 10


def save(path, data):
    path.write_text(json.dumps(data, indent=2) + '\n')


def main():
    base = json.loads(BASE.read_text())
    source = base['source_graph']
    predictor_ids = {row['node_id']: row['collective_id'] for row in base['collectives']}
    nodes, instances = [], {}
    for iteration in range(1, ITERATIONS + 1):
        prefix = f'iter{iteration:02d}_'
        for original in source['nodes']:
            name = original['id']
            node = {**original, 'id': prefix + name}
            if 'depends_on' in node:
                node['depends_on'] = [prefix + pred for pred in original['depends_on']]
            if name == 'start' and iteration > 1:
                node['depends_on'] = [f'iter{iteration - 1:02d}_sink']
            if name in predictor_ids:
                instances[node['id']] = (iteration, predictor_ids[name])
            nodes.append(node)

    template = {'schema_version': 1, 'kind': 'collective', 'name': 'exp7.2',
                'world_size': base['workload']['world_size'], 'defaults': source['defaults'],
                'nodes': nodes}
    with TemporaryDirectory(prefix='htsim-exp7.2-') as tmp:
        path = Path(tmp) / 'exp7.2.collective.json'
        save(path, template)
        subprocess.run([sys.executable, '-B', str(REPO / 'workload_gen/generate.py'),
                        str(path), '--output-root', str(ROOT)], check=True)

    output = ROOT / template['name']
    metadata_path = output / 'workload.meta.json'
    metadata = json.loads(metadata_path.read_text())
    metadata['workload'].update(iterations=ITERATIONS, base_workload=base['workload']['name'])
    for instance in metadata['collectives']:
        instance['iteration'], instance['predictor_collective_id'] = instances[instance['node_id']]

    # The predictor keys its history by collective_id. Keep the generated,
    # unique instance ID separately and share only the prediction identity.
    def set_prediction_identity(flow):
        flow['collective_instance_id'] = flow['collective_id']
        flow['iteration'], flow['collective_id'] = instances[flow['node_id']]

    for flow in metadata['flows'].values():
        set_prediction_identity(flow)
    save(metadata_path, metadata)
    flow_path = output / 'exp7.2.flow.json'
    flow_template = json.loads(flow_path.read_text())
    for node in flow_template['nodes']:
        if node['op'] == 'flow':
            set_prediction_identity(node['metadata'])
    save(flow_path, flow_template)


if __name__ == '__main__':
    main()
