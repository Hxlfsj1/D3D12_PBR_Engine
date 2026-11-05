"""Evaluate the isolated probe-distance experiment, including negative results."""
import hashlib
import json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
from AnalyzeVXGICornellLeak import capture
from AnalyzeVXGICornellBake import load, inside_blocks, summarize_region

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'tmp/vxgi-probe-depth'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def metrics(rgb):
    y = rgb[..., :3] @ np.array([.2126, .7152, .0722])
    assert np.isfinite(y).all()
    return dict(mean=float(y.mean()), maximum=float(y.max()),
                pixels_above_1e_minus5=int((y > 1e-5).sum()))


def run_path(mode, case, frame=256, configuration='Debug', camera='inside', render_pass='gi'):
    suffix = '' if configuration == 'Debug' else '-' + configuration
    return OUT / mode / 'runs' / f'{case}-{camera}-default-{render_pass}-{frame}{suffix}.json'


def main():
    report = dict(experimental=True, default_enabled=False, accepted_for_production=False,
                  gpu_timing='complete frame command list; first 32 and capture frame excluded; no present wait')
    report['runs'] = {}
    loaded = {}
    for mode in ('final-off', 'final-on'):
        for path in sorted((OUT / mode / 'runs').glob('*.json')):
            run = json.loads(path.read_text(encoding='utf-8-sig'))
            assert run['passed'] and run['exit_code'] == 0
            assert run['d3d12_errors'] == run['d3d12_warnings'] == 0
            if run['configuration'] == 'Debug':
                assert run['d3d12_validation_available']
            assert run['vxgi_probes']['total_updates'] == run['vxgi_probes']['valid'] * 32
            assert run['vxgi_probes']['unclassified'] == 0
            assert all(digest(OUT / mode / 'common/Shaders' / name) == sha
                       for name, sha in run['shader_sha256'].items())
            assert digest(OUT / mode / run['test_case'] / 'Settings/Pipeline.json') == run['pipeline_sha256']
            item = dict(probes=run['vxgi_probes'], distance_bytes=run['probe_distance_bytes'],
                        metadata=run['capture'], warnings=0, errors=0)
            times = np.array(run['gpu_frame_ms'])
            if len(times):
                item['gpu_ms'] = dict(samples=len(times), median=float(np.median(times)),
                                      p95=float(np.percentile(times, 95)))
            if '-gi-' in path.name:
                _, meta, arrays, mp = capture(path)
                item['stages'] = {name: metrics(array) for name, array in arrays.items() if name.startswith('gi-')}
                _, _, _, raw_path, records, centers = load(path)
                valid = records['state'] == 1
                interior = valid & np.all((centers > [-2, .01, -2.02536249]) &
                    (centers < [2, 3.9810145, 2.02536249]), axis=1)
                interior &= ~inside_blocks(centers, OUT / mode / run['test_case'] / 'Models/Cornell_Box.glb')
                item['baked_interior'] = summarize_region(records, interior)
                item['probe_records_sha256'] = digest(raw_path)
                if mode == 'final-on':
                    bins = np.fromfile(mp.parent / 'vxgi-probe-distance.bin', dtype='<u4').reshape(-1, 64, 2)
                    assert len(bins) == len(records)
                    item['unknown_valid_direction_bins'] = int((bins[valid, :, 0] == 0).sum())
                    radial = (bins[..., 0] & 65535).astype('<u2').view('<f2').astype(float)
                    plane = (bins[..., 0] >> 16).astype('<u2').view('<f2').astype(float)
                    assert np.isfinite(radial).all() and np.isfinite(plane).all()
                    assert np.all((radial >= 0) & (radial <= 1)) and np.all((plane >= 0) & (plane <= 1))
                loaded[(mode, run['test_case'])] = (item, arrays, meta, raw_path)
            report['runs'][f'{mode}/{path.name}'] = item

    comparisons = {}
    canvas = Image.new('RGB', (1280, 920), 'white')
    draw = ImageDraw.Draw(canvas)
    for row, case in enumerate(('B1', 'B2')):
        before, after = loaded[('final-off', case)], loaded[('final-on', case)]
        assert np.array_equal(before[1]['depth'], after[1]['depth'])
        assert before[3].read_bytes() == after[3].read_bytes(), 'Distance capture changed baked SH'
        b = before[0]['stages']['gi-filtered']; a = after[0]['stages']['gi-filtered']
        comparisons[case] = dict(before=b, after=a, depth_identical=True, raw_probe_records_identical=True,
                                mean_ratio=a['mean'] / b['mean'])
        scale = .001 if case == 'B1' else .04
        for column, data in enumerate((before, after)):
            y = data[1]['gi-filtered'][..., :3] @ [.2126, .7152, .0722]
            canvas.paste(Image.fromarray(np.uint8(np.clip(y / scale, 0, 1) * 255)).convert('RGB'),
                         (column * 640, row * 460 + 55))
            draw.text((column * 640 + 8, row * 460 + 8),
                      f'{case}: cache {"OFF" if column == 0 else "ON"}; white >= {scale} linear E/pi', fill='black')
            draw.text((column * 640 + 8, row * 460 + 27),
                      f'mean={y.mean():.9f}; maximum={y.max():.7f}', fill='black')
    canvas.save(OUT / 'comparison.png')
    report['comparisons'] = comparisons
    report['closed_visibility_passed'] = comparisons['B1']['after']['maximum'] == 0
    report['closed_mean_reduction'] = 1 - comparisons['B1']['mean_ratio']
    assert loaded[('final-on', 'B0')][0]['stages']['gi-filtered']['maximum'] == 0
    assert loaded[('final-on', 'B1Sun')][0]['baked_interior']['nonzero_sh'] == 0
    assert loaded[('final-on', 'B2Sun')][0]['baked_interior']['nonzero_sh'] > 0
    report['sun_probe_records_identical_to_retained_bake_fix'] = {}
    for case in ('B1Sun', 'B2Sun'):
        old = ROOT / 'tmp/vxgi-cornell-bake-fix/runs' / f'{case}-inside-default-gi-6.json'
        _, _, _, raw, _, _ = load(old)
        same = raw.read_bytes() == loaded[('final-on', case)][3].read_bytes()
        report['sun_probe_records_identical_to_retained_bake_fix'][case] = same
        assert same
    perf_key = 'A-corner-default-default-256-Release.json'
    off = report['runs'].get('final-off/' + perf_key)
    on = report['runs'].get('final-on/' + perf_key)
    if off and on:
        report['performance'] = dict(off=off['gpu_ms'], on=on['gpu_ms'],
            median_delta_ms=on['gpu_ms']['median'] - off['gpu_ms']['median'],
            median_ratio=on['gpu_ms']['median'] / off['gpu_ms']['median'],
            additional_bytes=on['distance_bytes'] - off['distance_bytes'])
    # A later compatibility guard also treats old executables' zero padding as
    # disabled. Preserve the frozen timing inputs, and verify the final shader
    # against both the unchanged main executable and the cache-enabled candidate.
    report['compatibility'] = {}
    for case in ('B1', 'B2'):
        path = run_path('compatible', case)
        r, _, a, _ = capture(path)
        assert r['passed'] and r['exit_code'] == 0
        assert r['d3d12_warnings'] == r['d3d12_errors'] == 0 and r['d3d12_validation_available']
        assert all(digest(ROOT / 'Shaders' / name) == sha for name, sha in r['shader_sha256'].items())
        reference = loaded[('final-on', case)]
        for name in ('depth', 'normal', 'bounce-source', 'gi-full-world', 'gi-full-world-owners'):
            if name in a:
                assert np.array_equal(a[name], reference[1][name]), name
        differences = {}
        for name in ('gi-filtered', 'gi-full-raw', 'gi-full-reconstructed'):
            difference = np.abs(a[name][..., :3] - reference[1][name][..., :3])
            differences[name] = dict(maximum=float(difference.max()), mean=float(difference.mean()))
            assert difference.max() <= .0005 and difference.mean() <= .000001
        _, _, _, raw, _, _ = load(path)
        assert raw.read_bytes() == reference[3].read_bytes()
        _, _, _, mp = capture(path)
        _, _, _, frozen_mp = capture(run_path('final-on', case))
        assert (mp.parent / 'vxgi-probe-distance.bin').read_bytes() == (frozen_mp.parent / 'vxgi-probe-distance.bin').read_bytes()
        report['compatibility'][case] = dict(
            exact='raw SH, distance cache, depth/normal/bounce source, world and owner atlas',
            screen_and_filtered_rerun_differences=differences)
    legacy_path = run_path('compatible', 'B1Sun', frame=6)
    r, _, _, raw, _, _ = load(legacy_path)
    assert r['passed'] and r['exit_code'] == 0 and r['d3d12_validation_available']
    assert r['d3d12_warnings'] == r['d3d12_errors'] == 0
    assert digest(ROOT / 'x64/Debug/LearnDirectX.exe') == r['executable_sha256']
    assert raw.read_bytes() == loaded[('final-on', 'B1Sun')][3].read_bytes()
    report['compatibility']['unchanged_main_executable'] = 'zero-padding disabled path: completed and raw SH identical'
    (OUT / 'analysis.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({k: v for k, v in report.items() if k != 'runs'}, indent=2))


if __name__ == '__main__':
    main()
