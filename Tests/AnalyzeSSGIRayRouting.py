"""Validate and summarize optional production-ray routing replays."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / 'tmp/vxgi-ray-routing'


def load(label):
    path = ROOT / f'tmp/VXGIStage75-{label}-640x400-report.json'
    report = json.loads(path.read_text(encoding='utf-8-sig'))
    assert report['completed'] and report['d3d12_errors'] == 0, path
    assert report['raw_gpu_bytes_unchanged_after_all_captures'], path
    assert report['samples_per_sequence'] == 64, path
    routing = report['ray_routing']
    assert routing['enabled'] and len(routing['frames']) == 64
    for frame in routing['frames']:
        assert frame['partition_checks_passed'] and frame['production_hit_alpha_matches']
    for group, totals in routing['totals'].items():
        for key, value in totals.items():
            assert sum(frame['groups'][group][key] for frame in routing['frames']) == value, (group, key)
        assert totals['launched_rays'] == totals['screen_hit'] + totals['fallback_entered'] + totals['fallback_disabled']
        assert totals['fallback_entered'] == totals['segment_invalid'] + sum(totals[k] for k in (
            'world_miss', 'world_hit', 'world_invalid', 'world_exhausted', 'world_stalled', 'world_unknown'))
    return path, report


def image_pair_check(report, previous_label, mode):
    previous = json.loads((ROOT / f'tmp/VXGIStage75-{previous_label}-640x400-report.json').read_text(encoding='utf-8-sig'))
    current = next(c for c in report['captures'] if c['mode'] == mode)
    reference = next(c for c in previous['captures'] if c['mode'] == mode)
    result = {}
    for key in ('linear_hdr', 'raster_depth'):
        a, b = (ROOT / current[key]).read_bytes(), (ROOT / reference[key]).read_bytes()
        assert a == b, f'Routing diagnostic changed {key}: {previous_label}'
        result[key + '_unchanged'] = True
        result[key + '_sha256'] = hashlib.sha256(a).hexdigest()
    assert report['raw_snapshot_sha256'].lower() == previous['raw_snapshot_sha256'].lower()
    return result


def group_summary(totals):
    rays = totals['launched_rays']
    return {'rays': rays, 'mean_rays_per_frame': rays / 64,
            'counts': totals,
            'percent_of_launched': {key: value * 100 / rays if rays else None for key, value in totals.items()
                                    if key not in ('dispatch_texels', 'sky_skipped', 'unlit_skipped', 'valid_receivers')}}


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    result = {'scope': '64 fixed-sampling frames, 640x400 scene, factor2, quality1 (4 rays/valid trace receiver). '
                       'Every logical ray first attempts SSGI; fallback is subsequent work. '
                       'Diagnostic replay timings are not a performance measurement.',
              'geometry_mask': 'Unbiased receiver world Y>.03 approximates car geometry, not object IDs.',
              'views': {}}
    full_exterior = None
    for view in ('exterior', 'front', 'cavity'):
        path, report = load('routing025-' + view)
        if view == 'exterior':
            full_exterior = report
        result['views'][view] = {'report': str(path), 'raw_sha256': report['raw_snapshot_sha256'],
                                'output_validation': image_pair_check(report, 'lum025-normal-' + view, 2),
                                'groups': {name: group_summary(totals) for name, totals in report['ray_routing']['totals'].items()}}
    path, control = load('routing025-screen-control')
    for frame, full in zip(control['ray_routing']['frames'], full_exterior['ray_routing']['frames']):
        assert frame['sample'] == full['sample']
        for group in ('all', 'above_ground'):
            c, f = frame['groups'][group], full['groups'][group]
            for key in ('dispatch_texels', 'sky_skipped', 'unlit_skipped', 'valid_receivers', 'launched_rays',
                        'screen_miss', 'screen_hit', 'screen_out_of_screen', 'screen_radius_limit',
                        'screen_iteration_limit', 'screen_invalid', 'screen_source_valid', 'screen_positive', 'screen_black'):
                assert c[key] == f[key], ('screen front end changed', frame['sample'], group, key)
            assert c['fallback_entered'] == 0
            assert c['fallback_disabled'] == f['fallback_entered']
    result['screen_only_control'] = {'report': str(path), 'same_per_frame_screen_frontend': True,
                                     'no_world_fallback': True,
                                     'output_validation': image_pair_check(control, 'lum025-normal-exterior', 0)}
    (OUTPUT / 'summary.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    labels = {'exterior': '整车外景', 'front': '前侧', 'cavity': '固定车内空腔'}
    lines = ['# SSGI / VXGI 射线分流实测', '',
             '640×400、追踪降采样2、quality1，每个有效追踪像素每帧4条射线，固定64帧。',
             '所有射线先尝试SSGI，之后才可能回退；比例的分母是实际发出的逻辑GI射线，天空/不参与GI材质不发射线。', '',
             '计数使用同一帧实际CBV、蓝噪声、纹理、几何、探针与生产函数进行只读诊断重放。',
             '逐帧以未统计生产Trace输出的alpha命中总数交叉校验，并与上一轮未开启统计的正常HDR/深度逐字节比较。',
             '全部守恒和输出校验通过。mode0对照与mode2的每帧屏幕阶段计数完全相同，所有世界回退计数为0。', '',
             '这是运行时逻辑分流统计，不是GPU时间基准。正常shader未定义诊断宏，不执行这些计数。', '']
    for group, title in (('all', '全部有效接收面'), ('above_ground', '车体近似区域（世界Y>0.03）')):
        lines += ['## ' + title, '', '| 机位 | 射线/帧 | SSGI命中 | 转入VXGI | 世界几何命中 | 获探针支持 | 世界非零光 |',
                  '|---|---:|---:|---:|---:|---:|---:|']
        for view, data in result['views'].items():
            g = data['groups'][group]
            p = g['percent_of_launched']
            cells = [f'{p[key]:.2f}%' for key in ('screen_hit', 'fallback_entered', 'world_hit', 'probe_support', 'world_positive')]
            lines.append(f'| {labels[view]} | {g["mean_rays_per_frame"]:,.0f} | ' + ' | '.join(cells) + ' |')
        lines.append('')
    lines += ['SSGI命中包含合法黑色光源；世界几何命中也可能没有可用探针或缓存照明为零。',
              '因此，转入VXGI不等于命中，不等于成功读取探针，更不等于产生可见补光。', '',
              '64帧计数、屏幕失败原因、世界失败状态和逐字节核验哈希见summary.json。']
    (OUTPUT / 'report.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print('\n'.join(lines))


if __name__ == '__main__':
    main()
