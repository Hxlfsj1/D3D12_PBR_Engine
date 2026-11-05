"""Inspect actual GPU-baked SH records, independently of GI cache interpolation."""
import hashlib
import json
from pathlib import Path
import numpy as np
from PrepareVXGICornellLeak import ROOT, OUT, read_glb, save_json

RECORD = np.dtype([
    ('sh', '<f4', (4, 4)), ('state', '<u4'), ('updates', '<u4'), ('samples', '<u4'),
    ('last_frame', '<u4'), ('backface_ratio', '<f4'), ('minimum_distance', '<f4'),
    ('classify_rays', '<u4'), ('reason', '<u4')])
assert RECORD.itemsize == 96


def inside_blocks(centers, model):
    doc, _ = read_glb(model)
    gltf = centers.astype(float).copy()
    gltf[:, 1] -= .01
    gltf[:, 2] *= -1
    mask = np.zeros(len(centers), dtype=bool)
    for node in doc['nodes']:
        if not node.get('name', '').startswith('Block'):
            continue
        x, y, z, w = node.get('rotation', [0, 0, 0, 1])
        rotation = np.array([[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
                             [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
                             [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]])
        local = ((gltf-node.get('translation', [0, 0, 0])) @ rotation) / node.get('scale', [1, 1, 1])
        mesh = doc['meshes'][node['mesh']]
        for primitive in mesh['primitives']:
            bounds = doc['accessors'][primitive['attributes']['POSITION']]
            mask |= np.all((local >= np.array(bounds['min'])-1e-6) &
                           (local <= np.array(bounds['max'])+1e-6), axis=1)
    return mask


def load(run_path):
    run = json.loads(run_path.read_text(encoding='utf-8-sig'))
    metadata_path = run_path.parent.parent / run['test_case'] / run['capture']
    meta = json.loads(metadata_path.read_text())
    output = next(b for b in meta['buffers'] if b['file'] == 'vxgi-probes.bin')
    assert output['stride'] == RECORD.itemsize
    data_path = metadata_path.parent/output['file']
    records = np.fromfile(data_path, dtype=RECORD)
    centers = np.array(meta['probe_snapshot']['centers'])
    assert len(records) == len(centers) == output['count'] == run['vxgi_probes']['count']
    return run, meta, metadata_path, data_path, records, centers


def summarize_region(records, mask):
    sh = records['sh'][mask, :, :3]
    absolute = np.max(np.abs(sh), axis=(1, 2)) if len(sh) else np.zeros(0)
    l0 = sh[:, 0] if len(sh) else np.zeros((0, 3))
    return dict(count=int(mask.sum()), nonzero_sh=int((absolute > 0).sum()),
                above_1e_minus8=int((absolute > 1e-8).sum()),
                max_abs_sh=float(absolute.max()) if len(absolute) else 0,
                max_l0=float(l0.max()) if len(l0) else 0,
                l0_rgb_sum=np.sum(l0.astype(float),axis=0).tolist(),
                l0_luminance_sum=float(np.sum(l0.astype(float)@np.array([.2126,.7152,.0722]))))


def analyze():
    report = dict(record_layout='96B: float4 SH[4], state/updates/samples/lastFrame, backfaceRatio/minDistance, classifyRays/reason',
                  interpretation='raw unconvolved incoming-radiance SH; no interpolation, visibility query, clamping or tone mapping',
                  runs=[])
    loaded = {}
    for path in sorted((OUT/'runs').glob('*.json')):
        run, meta, mp, raw, records, centers = load(path)
        assert run['passed'] and run['exit_code']==0 and run['d3d12_warnings']==run['d3d12_errors']==0
        if run.get('configuration','Debug')=='Debug':
            assert run['d3d12_validation_available']
        if run['test_case']=='B1SunWitness':
            # Diagnostic source values are payloads, not physical SH radiance.
            factor=4*np.pi/2048*.28209479177387814
            witnesses=[]
            for index in np.flatnonzero((records['state']==1)&(np.abs(records['sh'][:,0,0])>0)):
                payload=records['sh'][index,0,:3].astype(float)/factor
                packed=int(round(payload[0]))
                witnesses.append(dict(probe=int(index),center=centers[index].tolist(),
                    source_triangle=(packed&255)-1,small_min_shadow_triangle=((packed>>8)&255)-1,
                    small_min_shadow_status=packed>>16,small_min_shadow_distance=float(payload[1]),
                    source_back_wall_z_gap=float(payload[2]),
                    analytic_back_wall_distance=float(payload[2]/(1/np.sqrt(1.5)))))
            save_json(OUT/'sun-shadow-witnesses.json',dict(test_only=True,source_shadow_min=.0001,
                diagnostic_shadow_min=1e-7,witnesses=witnesses,metadata=str(mp),
                adjacent_blockers_below_production_min=bool(witnesses) and all(
                    w['source_triangle']!=w['small_min_shadow_triangle'] and w['small_min_shadow_status']==1 and
                    0<w['small_min_shadow_distance']<.0001 for w in witnesses)))
            continue
        valid = records['state'] == 1
        assert np.all(np.isfinite(records['sh'])) and np.all(np.isfinite(records['backface_ratio']))
        assert np.all(records['sh'][:, :, 3] == 0)
        assert not np.any(records['state'] == 0)
        assert np.all(records['updates'][valid] == 32) and np.all(records['samples'][valid] == 2048)
        assert not np.any(np.isin(records['reason'], [3, 4]))
        room = np.all((centers > [-2+1e-6, .01+1e-6, -2.02536249+1e-6]) &
                      (centers < [2-1e-6, 3.9810145-1e-6, 2.02536249-1e-6]), axis=1)
        blocks = inside_blocks(centers, OUT/run['test_case']/'Models/Cornell_Box.glb')
        free = room & ~blocks
        wall_distance = np.min(np.minimum(centers-[-2, .01, -2.02536249],
                                          [2, 3.9810145, 2.02536249]-centers), axis=1)
        entry = dict(run=path.name, metadata=str(mp), raw_sha256=hashlib.sha256(raw.read_bytes()).hexdigest(),
            all=summarize_region(records, valid), inside_room=summarize_region(records, valid & room),
            interior_free_space=summarize_region(records, valid & free),
            interior_near_wall=summarize_region(records, valid & free & (wall_distance < .5)),
            exterior=summarize_region(records, valid & ~room),
            disabled=int((records['state']==2).sum()), valid_inside_solid_blocks=int((valid & blocks).sum()),
            disabled_max_abs_sh=float(np.max(np.abs(records['sh'][~valid]))) if np.any(~valid) else 0,
            d3d12_warnings=run['d3d12_warnings'], d3d12_errors=run['d3d12_errors'])
        assert run['passed'] and run['exit_code']==0 and run['d3d12_warnings']==run['d3d12_errors']==0
        report['runs'].append(entry)
        loaded[path.name]=(records,centers,free,entry)
        # Per-probe evidence makes any nonzero interior sample directly inspectable.
        rows=[]
        for index in np.flatnonzero(valid & free):
            rows.append(dict(index=int(index),center=centers[index].tolist(),
                             sh=records['sh'][index,:,:3].tolist(),updates=int(records['updates'][index]),
                             samples=int(records['samples'][index])))
        save_json(OUT/(path.stem+'-interior-probes.json'),rows)
    off=loaded.get('B0-inside-default-gi-6.json')
    closed=loaded.get('B1-inside-default-gi-6.json')
    opened=loaded.get('B2-inside-default-gi-6.json')
    if off and closed:
        assert np.array_equal(off[1],closed[1]),'Closed on/off probe layout changed'
        common=(off[0]['state']==1)&(closed[0]['state']==1)&off[2]&closed[2]
        report['closed_on_off_interior_max_abs_delta']=float(np.max(np.abs(off[0]['sh'][common]-closed[0]['sh'][common])))
        report['closed_interior_leak_detected']=closed[3]['interior_free_space']['max_abs_sh']>1e-8
        report['external_source_observed']=closed[3]['exterior']['above_1e_minus8']>0
        assert report['external_source_observed'],'No valid exterior probe captured the source'
    if opened:
        report['opening_positive_control']=opened[3]['interior_free_space']['above_1e_minus8']>0
        assert report['opening_positive_control'],'Opening did not illuminate any interior probe'
    late=loaded.get('B1-inside-default-gi-160.json')
    if closed and late:
        report['closed_raw_frame6_frame160_identical']=closed[3]['raw_sha256']==late[3]['raw_sha256']
        assert report['closed_raw_frame6_frame160_identical'],'Runtime modified the baked cache'
    sun=loaded.get('B1Sun-inside-default-gi-6.json')
    sun_open=loaded.get('B2Sun-inside-default-gi-6.json')
    if sun:
        report['closed_sunlight_leak_detected']=sun[3]['interior_free_space']['max_abs_sh']>1e-8
        report['sunlight_exterior_observed']=sun[3]['exterior']['above_1e_minus8']>0
        assert report['sunlight_exterior_observed'],'No exterior sunlight source observed'
    if sun_open:
        report['sunlight_opening_positive_control']=sun_open[3]['interior_free_space']['above_1e_minus8']>0
        if sun:
            report['sunlight_opening_positive_control'] &= sun_open[3]['interior_free_space']['above_1e_minus8']>sun[3]['interior_free_space']['above_1e_minus8']
        assert report['sunlight_opening_positive_control'],'Sunlight opening positive control failed'
    manifest=json.loads((OUT/'fixtures.json').read_text())
    report['protected_inputs_unchanged']={name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest()==sha
                                          for name,sha in manifest['inputs'].items()}
    assert all(report['protected_inputs_unchanged'].values())
    save_json(OUT/'bake-analysis.json',report)
    baseline=ROOT/'tmp/vxgi-cornell-bake'
    if OUT.name=='vxgi-cornell-bake-fix' and baseline.exists():
        comparison={}
        for case in ('B1Sun','B2Sun','B1','B2'):
            old=baseline/'runs'/f'{case}-inside-default-gi-6.json'
            new=OUT/'runs'/f'{case}-inside-default-gi-6.json'
            if not old.exists() or not new.exists():continue
            _,_,_,old_raw,a,c=load(old)
            _,_,_,new_raw,b,d=load(new)
            assert np.array_equal(c,d) and np.array_equal(a['state'],b['state'])
            interior=(a['state']==1)&np.all((c>[-2,.01,-2.02536249])&(c<[2,3.9810145,2.02536249]),axis=1)
            interior &= ~inside_blocks(c,OUT/case/'Models/Cornell_Box.glb')
            before,after=summarize_region(a,interior),summarize_region(b,interior)
            comparison[case]=dict(before=before,after=after,same_positions_and_states=True,
                l0_sum_ratio=after['l0_luminance_sum']/before['l0_luminance_sum'] if before['l0_luminance_sum'] else None,
                changed_records=int(np.count_nonzero(np.any(a['sh'][interior]!=b['sh'][interior],axis=(1,2)))),
                full_raw_identical=old_raw.read_bytes()==new_raw.read_bytes())
            if case=='B1Sun':assert after['max_abs_sh']==0
            if case=='B2Sun':assert after['nonzero_sh']==before['nonzero_sh']>0
            if case in ('B1','B2'):assert comparison[case]['full_raw_identical']
        save_json(OUT/'sun-fix-comparison.json',comparison)
    print(json.dumps(report,indent=2))


if __name__=='__main__':
    analyze()
