"""Analyze screenshot-camera controls without changing renderer settings or code."""
import hashlib
import json
from collections import defaultdict
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
from AnalyzeVXGICornellLeak import capture, model_triangles, world_positions
from AnalyzeVXGICornellBake import load
from PrepareVXGICornellLeak import read_glb

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'tmp/vxgi-mesh-edge'
LUMA = np.array([.2126, .7152, .0722])


def sharp_edges():
    triangles = model_triangles(ROOT / 'Models/Cornell_Box.glb')
    document, _ = read_glb(ROOT / 'Models/Cornell_Box.glb')
    skip = set()
    first = 0
    for node in document['nodes']:
        if 'mesh' not in node:
            continue
        count = sum(document['accessors'][p['indices']]['count'] // 3
                    for p in document['meshes'][node['mesh']]['primitives'])
        if node['name'] == 'CeilingLight':
            skip.update(range(first, first + count))
        first += count
    adjacency = defaultdict(list)
    endpoints = {}
    for index, triangle in enumerate(triangles):
        if index in skip:
            continue
        normal = np.cross(triangle[1] - triangle[0], triangle[2] - triangle[0])
        normal /= np.linalg.norm(normal)
        for a, b in ((0, 1), (1, 2), (2, 0)):
            key = tuple(sorted((tuple(np.round(triangle[a], 5)), tuple(np.round(triangle[b], 5)))))
            adjacency[key].append(normal)
            endpoints[key] = triangle[[a, b]]
    return np.array([endpoints[key] for key, normals in adjacency.items()
                     if len(normals) == 1 or any(abs(n @ normals[0]) < .999 for n in normals[1:])])


def edge_mask(meta, arrays, edges):
    # Production GI receivers use pixel (2*x,2*y), not the other member of the 2x2.
    position = world_positions(meta, arrays['depth'])[::2, ::2]
    minimum = np.full(position.shape[:2], np.inf)
    for a, b in edges:
        delta = b - a
        t = np.clip(np.sum((position - a) * delta, axis=-1) / (delta @ delta), 0, 1)
        minimum = np.minimum(minimum, np.linalg.norm(position - a - t[..., None] * delta, axis=-1))
    return (minimum < .2) & (arrays['depth'][::2, ::2, 0] < 1), minimum


def stats(rgb, mask):
    y = rgb[..., :3] @ LUMA
    assert np.isfinite(y).all() and mask.any()
    values = y[mask]
    return dict(mean=float(values.mean()), maximum=float(values.max()), pixels=int(mask.sum()),
                pixels_above_005=int((values > .05).sum()))


def main():
    fixture = json.loads((OUT / 'fixture.json').read_text())
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    assert all(sha(ROOT / path) == value for path, value in fixture['inputs'].items())
    baseline_shader = OUT / 'baseline-v1/common/Shaders'
    assert all(sha(ROOT / 'Shaders' / path.name) == sha(path)
               for path in baseline_shader.iterdir() if path.is_file())
    edges = sharp_edges()
    report = dict(root_settings_models_shaders_unchanged=True,
                  source_stage='selected filtered preview: SSGI plus near Mesh SDF, Global disabled; raw/full exports belong to the simultaneous full Mesh+Global pipeline',
                  controlled_settings=fixture['setup'], edge_segments=len(edges),
                  roi='visible pixels within 0.2 world units of Cornell sharp/boundary edges, excluding lamp and quad diagonals',
                  runs={}, comparisons={})
    records_reference = None
    loaded = {}
    for directory in sorted(OUT.iterdir()):
        if not (directory / 'runs').is_dir():
            continue
        for path in sorted((directory / 'runs').glob('*.json')):
            run, meta, arrays, mp = capture(path)
            assert run['passed'] and run['exit_code'] == 0 and run['d3d12_validation_available']
            assert run['d3d12_warnings'] == run['d3d12_errors'] == 0
            assert run['vxgi_probes']['valid'] == 56939 and run['vxgi_probes']['unclassified'] == 0
            assert run['vxgi_probes']['total_updates'] == 56939 * 32
            _, _, _, raw, _, _ = load(path)
            if records_reference is None:
                records_reference = raw.read_bytes()
            assert raw.read_bytes() == records_reference
            mask, distance = edge_mask(meta, arrays, edges)
            owner = arrays['gi-full-world-owners']
            split = owner.shape[1] // 2
            item = dict(metadata=str(mp.relative_to(ROOT)), raw_SH_identical=True,
                        warnings=0, errors=0, probes=run['vxgi_probes'],
                        filtered_edge=stats(arrays['gi-filtered'], mask),
                        mesh_owner_slots=stats(owner[:, :split], np.ones(owner.shape[:2], bool)[:, :split]),
                        global_owner_max=float(np.max(owner[:, split:, :3])))
            label = directory.name + '/' + run['test_source']
            loaded[label] = (run, meta, arrays, mp, mask, distance)
            report['runs'][label] = item

    canvas = Image.new('RGB', (1280, 3 * 438), 'white')
    draw = ImageDraw.Draw(canvas)
    for view in range(1, 4):
        before = loaded[f'baseline-v{view}/mesh-sdf']
        after = loaded[f'exact-hit-v{view}/mesh-sdf']
        assert np.array_equal(before[2]['depth'], after[2]['depth'])
        for column, data in enumerate((before, after)):
            # Linear buffer display, identical scale in every panel. No AA/exposure comparison.
            y = data[2]['gi-filtered'][..., :3] @ LUMA
            panel = Image.fromarray(np.uint8(np.clip(y / .1, 0, 1) * 255)).convert('RGB')
            canvas.paste(panel, (column * 640, (view - 1) * 438 + 34))
            draw.text((column * 640 + 8, (view - 1) * 438 + 8),
                      f'View {view}: ' + ('current threshold hit' if column == 0 else 'triangle hit control') +
                      '; white >= 0.1 linear E/pi', fill='black')
        a = report['runs'][f'baseline-v{view}/mesh-sdf']['filtered_edge']['mean']
        b = report['runs'][f'exact-hit-v{view}/mesh-sdf']['filtered_edge']['mean']
        report['comparisons'][f'view{view}'] = dict(edge_mean_before=a, edge_mean_triangle_control=b,
                                                 reduction=1 - b / a, depth_identical=True)
    canvas.save(OUT / 'three-view-comparison.png')
    report['cpu_witness'] = json.loads((OUT / 'cpu-witness.json').read_text())
    assert report['cpu_witness']['rays'][0]['threshold_status'] == 1
    assert report['cpu_witness']['rays'][0]['threshold_distance'] == 0
    assert not report['cpu_witness']['rays'][0]['exact_hit']
    (OUT / 'analysis.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(dict(runs=len(report['runs']), comparisons=report['comparisons'],
                         view1_controls={k: v['filtered_edge'] for k, v in report['runs'].items()
                                         if '-v1/' in k}), indent=2))


if __name__ == '__main__':
    main()
