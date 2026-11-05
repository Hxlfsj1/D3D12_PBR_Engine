"""Analyze linear Cornell captures and validate sealed geometry with triangle rays."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
import numpy as np
from PIL import Image
from PrepareVXGICornellLeak import ROOT, OUT, read_glb, save_json


def accessor(doc, binary, index):
    a = doc['accessors'][index]
    v = doc['bufferViews'][a['bufferView']]
    components = {'SCALAR': 1, 'VEC3': 3}[a['type']]
    dtype = {5126: '<f4', 5123: '<u2', 5125: '<u4'}[a['componentType']]
    offset = v.get('byteOffset', 0) + a.get('byteOffset', 0)
    return np.frombuffer(binary, dtype=dtype, count=a['count'] * components, offset=offset).reshape(a['count'], components)


def model_triangles(path, shell_only=False):
    doc, binary = read_glb(path)
    triangles = []
    for node in doc['nodes']:
        if 'mesh' not in node:
            continue
        if shell_only and (node['name'].startswith('Block') or node['name'] == 'CeilingLight'):
            continue
        q = node.get('rotation', [0, 0, 0, 1])
        x, y, z, w = q
        rot = np.array([[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],
                        [2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],
                        [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]])
        for prim in doc['meshes'][node['mesh']]['primitives']:
            vertices = accessor(doc, binary, prim['attributes']['POSITION']).astype(float)
            vertices = (vertices * node.get('scale', [1, 1, 1])) @ rot.T + node.get('translation', [0, 0, 0])
            vertices[:, 2] *= -1
            vertices[:, 1] += .01
            indices = accessor(doc, binary, prim['indices']).reshape(-1, 3)
            triangles.extend(vertices[indices])
    return np.array(triangles)


def triangle_hits(origins, directions, triangles, tmin=1e-6, tmax=100):
    closest = np.full(len(origins), np.inf)
    for tri in triangles:
        e1, e2 = tri[1] - tri[0], tri[2] - tri[0]
        p = np.cross(directions, e2)
        determinant = p @ e1
        valid = np.abs(determinant) > 1e-10
        inverse = np.divide(1, determinant, out=np.zeros_like(determinant), where=valid)
        offset = origins - tri[0]
        u = np.einsum('ij,ij->i', offset, p) * inverse
        q = np.cross(offset, e1)
        v = np.einsum('ij,ij->i', directions, q) * inverse
        distance = (q @ e2) * inverse
        hit = valid & (u >= -1e-8) & (v >= -1e-8) & (u+v <= 1+1e-8) & (distance >= tmin) & (distance <= tmax)
        closest = np.where(hit, np.minimum(closest, distance), closest)
    return closest


def validate_geometry():
    rng = np.random.default_rng(20261102)
    origins = rng.uniform([-1.99, .02, -2.015], [1.99, 3.97, 2.015], (20000, 3))
    directions = rng.normal(size=origins.shape)
    directions /= np.linalg.norm(directions, axis=1)[:, None]
    tests = {}
    for case in ('B0', 'B1', 'B2'):
        tris = model_triangles(OUT / case / 'Models/Cornell_Box.glb', shell_only=True)
        distance = triangle_hits(origins, directions, tris)
        tests[case] = dict(shell_triangles=len(tris), rays=len(origins), escaping=int(np.isinf(distance).sum()))
    # Matched rays from inside to the donor rectangle must encounter the closed back wall first.
    source = rng.uniform([-1.5, .71, -2.4], [1.5, 3.31, -2.4], (20000, 3))
    origins = rng.uniform([-1.8, .21, -1.8], [1.8, 3.78, 1.8], (20000, 3))
    vector = source-origins
    length = np.linalg.norm(vector, axis=1)
    hits = triangle_hits(origins, vector/length[:, None], model_triangles(OUT/'B1/Models/Cornell_Box.glb'))
    tests['source_visibility'] = dict(rays=len(origins), unblocked=int((hits >= length-1e-5).sum()))
    save_json(OUT / 'geometry-validation.json', tests)
    assert tests['B0']['escaping'] == tests['B1']['escaping'] == tests['source_visibility']['unblocked'] == 0
    assert tests['B2']['escaping'] > 0
    return tests


def capture(runfile):
    run = json.loads(runfile.read_text(encoding='utf-8-sig'))
    meta_path = runfile.parent.parent / run['test_case'] / run['capture']
    meta = json.loads(meta_path.read_text())
    arrays = {}
    for output in meta['outputs']:
        if output['preview']:
            continue
        fmt = output['format_dxgi']
        dtype, channels = {10: ('<f2', 4), 39: ('<f4', 1), 28: ('u1', 4)}[fmt]
        name = output['file'].split('-', 1)[1].removesuffix('.raw')
        arrays[name] = np.fromfile(meta_path.parent/output['file'], dtype=dtype).reshape(output['height'], output['width'], channels).astype(np.float32)
    return run, meta, arrays, meta_path


def world_positions(meta, depth):
    c = meta['camera']; h, w = depth.shape[:2]
    eye = np.array(c['eye']); f = np.array(c['forward']); up = np.array(c['up'])
    right = np.cross(up, f); right /= np.linalg.norm(right); up = np.cross(f, right)
    yy, xx = np.mgrid[:h, :w]
    tangent = np.tan(np.deg2rad(c['vertical_fov_degrees'])/2)
    ray = f + ((xx+.5)/w*2-1)[...,None]*tangent*w/h*right + (1-(yy+.5)/h*2)[...,None]*tangent*up
    near, far = c['near_plane'], c['far_plane']
    z = near*far/(far-depth[...,0].astype(float)*(far-near))
    return eye + ray*z[...,None]


def stats(rgb, mask):
    v = rgb[mask]
    y = v @ np.array([.2126,.7152,.0722])
    return dict(pixels=len(y), mean=float(y.mean()) if len(y) else 0,
                p95=float(np.percentile(y,95)) if len(y) else 0,
                maximum=float(y.max()) if len(y) else 0,
                fraction_above_1e_minus5=float((y>1e-5).mean()) if len(y) else 0)


def largest_component(mask):
    pending=mask.copy();largest=0;h,w=pending.shape
    for yy,xx in zip(*np.nonzero(mask)):
        if not pending[yy,xx]:continue
        todo=[(yy,xx)];pending[yy,xx]=False;count=0
        while todo:
            y,x=todo.pop();count+=1
            for ny,nx in ((y-1,x),(y+1,x),(y,x-1),(y,x+1)):
                if 0<=ny<h and 0<=nx<w and pending[ny,nx]:
                    pending[ny,nx]=False;todo.append((ny,nx))
        largest=max(largest,count)
    return largest


def compare_visibility_fix():
    if not (ROOT/'Shaders/VXGIProbeVisibilityCommon.hlsli').exists():return
    baseline=ROOT/'tmp/vxgi-cornell-leak'
    if OUT==baseline or not (baseline/'analysis.json').exists():return
    from PIL import ImageDraw
    comparison=Image.new('RGB',(1280,920),'white');draw=ImageDraw.Draw(comparison)
    report={}
    for row,case in enumerate(('B1','B2')):
        pair=[]
        for column,folder in enumerate((baseline,OUT)):
            path=folder/'runs'/f'{case}-inside-default-gi-256.json'
            if not path.exists():return
            run,meta,arrays,mp=capture(path)
            rgb=arrays['gi-filtered'][...,:3]
            y=rgb@np.array([.2126,.7152,.0722])
            pair.append(dict(mean=float(y.mean()),maximum=float(y.max()),
                fraction_above_epsilon=float((y>1e-5).mean()),metadata=str(mp),
                depth_sha256=hashlib.sha256(arrays['depth'].tobytes()).hexdigest()))
            x0,y0=column*640,row*460
            comparison.paste(Image.fromarray(np.uint8(np.clip(y/.01,0,1)*255)).convert('RGB'),(x0,y0+55))
            label=f'{case} '+('BEFORE' if column==0 else 'AFTER')
            draw.text((x0+8,y0+8),label+' | black=0, white>=0.01 E/pi (linear)',fill='black')
            draw.text((x0+8,y0+26),f'mean={y.mean():.8f}; max={y.max():.8f}',fill='black')
        assert pair[0]['depth_sha256']==pair[1]['depth_sha256'],'Before/after geometry or camera changed'
        report[case]=dict(before=pair[0],after=pair[1])
    assert report['B1']['after']['maximum']==0,'Closed box still leaks'
    assert report['B2']['after']['mean']>1e-5,'Positive control went black'
    report['positive_control_mean_ratio']=report['B2']['after']['mean']/report['B2']['before']['mean']
    old_fixture=json.loads((baseline/'fixtures.json').read_text())
    fixture=json.loads((OUT/'fixtures.json').read_text())
    report['identical_fixture_model_hashes']=all(fixture['cases'][c]['models']==old_fixture['cases'][c]['models'] for c in ('A','B0','B1','B2'))
    assert report['identical_fixture_model_hashes'],'Fixture geometry/materials changed'
    # Distinguish final-source verification from earlier development captures.
    hashes={name:hashlib.sha256((ROOT/'Shaders'/name).read_bytes()).hexdigest().upper()
        for name in ('VXGIProbeRadianceCommon.hlsli','VXGIProbeVisibilityCommon.hlsli','SSGIHybridCommon.hlsli','Shaders_For_SSGI.hlsl')}
    required=['B0-inside-default-gi-256.json','B1-inside-default-gi-256.json','B2-inside-default-gi-256.json',
        'B1-inside-default-gi-256-Release.json','B2-inside-default-gi-256-Release.json']
    report['final_source_runs']={}
    for name in required:
        recorded=json.loads((OUT/'runs'/name).read_text(encoding='utf-8-sig')).get('shader_sha256',{})
        report['final_source_runs'][name]=all(recorded.get(shader)==sha for shader,sha in hashes.items())
    assert all(report['final_source_runs'].values()),'Required capture does not match final production shaders'
    comparison.save(OUT/'before-after.png')
    save_json(OUT/'before-after.json',report)


def analyze():
    result = dict(geometry=validate_geometry(), captures=[])
    heatmaps = []
    for path in sorted((OUT/'runs').glob('*.json')):
        run, meta, a, mp = capture(path)
        if run['test_case']=='B1Endpoints':
            owner=a['gi-full-world-owners']
            passed=bool(np.max(owner[...,0])==0 and np.max(owner[...,1])>0)
            save_json(OUT/'endpoint-validation.json',dict(passed=passed,assertions=6,
                red_max=float(np.max(owner[...,0])),green_max=float(np.max(owner[...,1])),metadata=str(mp)))
            assert passed,'Production endpoint/front/back visibility regression'
            continue
        if 'gi-filtered' not in a:
            continue
        gi = a['gi-filtered'][...,:3]
        for name, data in a.items():
            assert np.isfinite(data).all(), (path, name)
        positions = world_positions(meta,a['depth'])[1::2,1::2]
        visible = (a['depth'][1::2,1::2,0] < 1)
        back = visible & (abs(positions[...,2]+2.02536249)<.015)
        # Reserve the positive-control window in every case for a matched wall ROI.
        window = (abs(positions[...,0])<.7)&(positions[...,1]>1.06)&(positions[...,1]<2.96)
        main = back & (abs(positions[...,0])<1.85)&(positions[...,1]>.16)&(positions[...,1]<3.83)&~window
        entry = dict(run=path.name, metadata=str(mp.relative_to(ROOT)), probes=run['vxgi_probes'],
                     full=stats(gi,visible), back_wall=stats(gi,main), stages={})
        for name in ('gi-full-raw','gi-full-reconstructed','gi-full-world','gi-filtered'):
            if name in a:
                entry['stages'][name] = stats(a[name][...,:3],visible)
        entry['bounce_source_max']=float(a['bounce-source'][...,:3].max())
        if 'gi-full-world-owners' in a:
            owners=a['gi-full-world-owners'];half=owners.shape[1]//2
            entry['owner_atlas']={name: stats(data[...,:3],np.ones(data.shape[:2],dtype=bool))
                for name,data in [('mesh',owners[:,:half]),('global',owners[:,half:])]}
        result['captures'].append(entry)
        if run['test_case'] in ('B0','B1','B2') and run['test_source']=='default':
            heatmaps.append((path.stem,gi,main))
    save_json(OUT/'analysis.json',result)
    # A compact, consistently scaled data visualization for the report.
    from PIL import ImageDraw
    comparison=Image.new('RGB',(1280,920),'white');draw=ImageDraw.Draw(comparison)
    for i,case in enumerate(('B0','B1','B2','B1Visible')):
        path=OUT/'runs'/f'{case}-inside-default-gi-256.json'
        if not path.exists():continue
        run,meta,arrays,mp=capture(path);y=arrays['gi-filtered'][...,:3]@np.array([.2126,.7152,.0722])
        x0,y0=(i%2)*640,(i//2)*460
        panel=Image.fromarray(np.uint8(np.clip(y/.01,0,1)*255)).convert('RGB')
        comparison.paste(panel,(x0,y0+55))
        draw.text((x0+8,y0+8),case+' | linear GI luminance: black=0, white>=0.01 E/pi',fill='black')
        draw.text((x0+8,y0+26),f'mean={y.mean():.8f}; max={y.max():.8f}',fill='black')
        if case=='B1':
            result['leak_detection']=dict(epsilon=1e-5,above_threshold=int((y>1e-5).sum()),
                largest_connected_component=largest_component(y>1e-5),
                note='4-connected GI-grid pixels; closed view contains only interior geometry')
    comparison.save(OUT/'comparison.png')
    save_json(OUT/'analysis.json',result)
    manifest=json.loads((OUT/'fixtures.json').read_text())
    unchanged={p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest()==h for p,h in manifest['inputs'].items()}
    assert all(unchanged.values()),'Production test inputs changed'
    runs=[json.loads(p.read_text(encoding='utf-8-sig')) for p in (OUT/'runs').glob('*.json')]
    assert all(r['passed'] and r['exit_code']==0 and r['d3d12_errors']==r['d3d12_warnings']==0 for r in runs)
    assert all(r['d3d12_validation_available'] for r in runs if r.get('configuration','Debug')=='Debug')
    assert all(r['vxgi_probes']['unclassified']==0 and r['vxgi_probes']['valid']>0 and
        r['vxgi_probes']['total_updates']==r['vxgi_probes']['valid']*r['vxgi_probes']['target_updates'] for r in runs)
    depth_hashes={case:hashlib.sha256(capture(OUT/'runs'/f'{case}-inside-default-gi-256.json')[2]['depth'].tobytes()).hexdigest()
        for case in ('B0','B1','B1Visible','B1Interior') if (OUT/'runs'/f'{case}-inside-default-gi-256.json').exists()}
    assert len(set(depth_hashes.values()))==1,'Closed control depth changed'
    shader_hashes={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in OUT.glob('probe-*.dxil')}
    assert not shader_hashes or len(set(shader_hashes.values()))==1,'Probe update shader changed'
    source_unchanged=all(hashlib.sha256(p.read_bytes()).digest()==hashlib.sha256((OUT/'common/Shaders'/p.name).read_bytes()).digest()
        for p in (ROOT/'Shaders').iterdir() if p.is_file())
    assert source_unchanged,'Baseline shader copy differs from production'
    save_json(OUT/'validation.json',dict(runs=len(runs),
        debug_runs=sum(r.get('configuration','Debug')=='Debug' for r in runs),
        release_runs=sum(r.get('configuration','Debug')=='Release' for r in runs),
        all_runs_passed=True,all_runs_zero_d3d12_warnings_errors=True,
        all_debug_runs_validation_available=True,all_probe_fills_complete=True,
        protected_inputs_unchanged=unchanged,baseline_shaders_match_production=source_unchanged,
        closed_control_depth_hashes=depth_hashes,probe_update_dxil_hashes=shader_hashes,
        note='Optional independent O3 probe hashes use VXGI_MESH_SDF=1 and VXGI_PROBE_SPARSE=1; no raw SH buffer comparison'))
    if heatmaps:
        from PIL import ImageDraw
        sheet=Image.new('RGB',(1280,440*len(heatmaps)),'white')
        draw=ImageDraw.Draw(sheet)
        for row,(label,gi,mask) in enumerate(heatmaps):
            y=gi@np.array([.2126,.7152,.0722])
            panel=Image.fromarray(np.uint8(np.clip(y/.02,0,1)*255)).convert('RGB')
            sheet.paste(panel,(0,row*440+35))
            sheet.paste(Image.fromarray(np.uint8(mask)*255).convert('RGB'),(640,row*440+35))
            draw.text((10,row*440+10),label+' | linear luminance: black=0, white>=0.02 E/pi',fill='black')
            draw.text((650,row*440+10),'Matched back-wall ROI (white)',fill='black')
        sheet.save(OUT/'linear-heatmaps.png')
    compare_visibility_fix()
    print(json.dumps(result,indent=2))


if __name__=='__main__':
    analyze()
