"""Generate isolated Cornell leakage fixtures; never edit production settings/assets."""
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct

ROOT = Path(__file__).resolve().parents[1]
OUT = (ROOT / os.environ.get('VXGI_CORNELL_OUTPUT', 'tmp/vxgi-cornell-leak')).resolve()
if not OUT.is_relative_to(ROOT / 'tmp'):
    raise ValueError('Cornell test output must remain inside workspace tmp')


def read_glb(path):
    data = path.read_bytes()
    length, kind = struct.unpack_from('<II', data, 12)
    document = json.loads(data[20:20 + length])
    size, kind = struct.unpack_from('<II', data, 20 + length)
    return document, bytearray(data[28 + length:28 + length + size])


def write_glb(path, doc, binary):
    doc['buffers'][0]['byteLength'] = len(binary)
    header = json.dumps(doc, separators=(',', ':')).encode()
    header += b' ' * (-len(header) % 4)
    binary += b'\0' * (-len(binary) % 4)
    path.write_bytes(struct.pack('<III', 0x46546c67, 2, 28 + len(header) + len(binary))
                     + struct.pack('<II', len(header), 0x4e4f534a) + header
                     + struct.pack('<II', len(binary), 0x004e4942) + binary)


def append_accessor(doc, binary, values, component, shape):
    binary += b'\0' * (-len(binary) % 4)
    offset = len(binary)
    flattened = [v for row in values for v in row] if shape == 'VEC3' else values
    binary += struct.pack('<' + ('f' if component == 5126 else 'H') * len(flattened), *flattened)
    view = len(doc['bufferViews'])
    doc['bufferViews'].append(dict(buffer=0, byteOffset=offset, byteLength=len(binary) - offset))
    accessor = dict(bufferView=view, componentType=component, count=len(values), type=shape)
    if shape == 'VEC3':
        accessor.update(min=[min(v[a] for v in values) for a in range(3)],
                        max=[max(v[a] for v in values) for a in range(3)])
    doc['accessors'].append(accessor)
    return len(doc['accessors']) - 1


def rectangle(doc, binary, xmin, xmax, ymin, ymax, z, normal_z, material):
    vertices = [(xmin, ymin, z), (xmax, ymin, z), (xmax, ymax, z), (xmin, ymax, z)]
    indices = [0, 1, 2, 0, 2, 3] if normal_z > 0 else [0, 2, 1, 0, 3, 2]
    return dict(attributes={
        'POSITION': append_accessor(doc, binary, vertices, 5126, 'VEC3'),
        'NORMAL': append_accessor(doc, binary, [(0, 0, normal_z)] * 4, 5126, 'VEC3')},
        indices=append_accessor(doc, binary, indices, 5123, 'SCALAR'), material=material)


def add_mesh(doc, name, primitives):
    doc['meshes'].append(dict(name=name, primitives=primitives))
    doc['nodes'].append(dict(name=name, mesh=len(doc['meshes']) - 1))
    doc['scenes'][doc.get('scene', 0)]['nodes'].append(len(doc['nodes']) - 1)


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def prepare():
    OUT.mkdir(parents=True, exist_ok=True)
    common = OUT / 'common'
    common.mkdir(exist_ok=True)
    for directory in ('Shaders', 'ThirdParty'):
        shutil.copytree(ROOT / directory, common / directory, dirs_exist_ok=True)
    cameras = {
        'overview': dict(eye=[0, 2, 7.2], target=[0, 1.9, 0]),
        'inside': dict(eye=[0, 2, 1.4], target=[0, 1.9, -2]),
        'corner': dict(eye=[1.2, 2, 1.3], target=[-1.5, 1, -1.5]),
        'outside': dict(eye=[0, 2, -3.7], target=[0, 2, -2.025]),
    }
    for camera in cameras.values():
        camera.update(up=[0, 1, 0], vertical_fov_degrees=60, near_plane=.1, far_plane=1000)
    original, original_binary = read_glb(ROOT / 'Models/Cornell_Box.glb')
    manifest = dict(cameras=cameras, cases={}, inputs={})
    for filename in ('Models/Cornell_Box.glb', 'Models/Plain.glb', 'Settings/Pipeline.json',
                     'Settings/Scene.json', 'Settings/Lighting.json'):
        manifest['inputs'][filename] = hashlib.sha256((ROOT / filename).read_bytes()).hexdigest()
    for case in ('A', 'B0', 'B1', 'B2'):
        stage = OUT / case
        (stage / 'Settings').mkdir(parents=True, exist_ok=True)
        (stage / 'Models').mkdir(exist_ok=True)
        # A directory junction is installed by the PowerShell runner for shared runtime assets.
        pipeline = json.loads((ROOT / 'Settings/Pipeline.json').read_text())
        pipeline['DLSS SR']['anti_aliasing'] = 'None'
        pipeline['hbao']['enabled'] = False
        scene = json.loads((ROOT / 'Settings/Scene.json').read_text())
        scene['skybox_path'] = os.path.relpath(ROOT / scene['skybox_path'], stage).replace('\\', '/')
        lighting = json.loads((ROOT / 'Settings/Lighting.json').read_text())
        if case == 'A':
            for name in ('Cornell_Box.glb', 'Plain.glb'):
                shutil.copy2(ROOT / 'Models' / name, stage / 'Models' / name)
        else:
            doc, binary = copy.deepcopy(original), bytearray(original_binary)
            for material in doc['materials']:
                material['emissiveFactor'] = [0, 0, 0]
                material.get('extensions', {}).pop('KHR_materials_emissive_strength', None)
            add_mesh(doc, 'SealedEntrance', [rectangle(doc, binary, -1.7028985023498535,
                1.7028985023498535, .2971014380455017, 3.6739132404327393,
                -2.025362491607666, 1, 0)])
            if case == 'B2':
                z = 2.025362491607666
                doc['meshes'][2]['primitives'] = [rectangle(doc, binary, *bounds, z, -1, 0)
                    for bounds in [(-2, -.55, 0, 3.9710144996643066),
                                   (.55, 2, 0, 3.9710144996643066),
                                   (-.55, .55, 0, 1.2), (-.55, .55, 2.8, 3.9710144996643066)]]
            write_glb(stage / 'Models/Cornell_Box.glb', doc, binary)
            # A separate two-sided source outside the back wall, 0.375 units away.
            donor = dict(asset={'version': '2.0'}, buffers=[{'byteLength': 0}], bufferViews=[],
                         accessors=[], meshes=[], nodes=[], scenes=[{'nodes': []}], scene=0,
                         materials=[dict(name='ExternalEmitter', doubleSided=True,
                            emissiveFactor=[2, 1, .5] if case != 'B0' else [0, 0, 0],
                            pbrMetallicRoughness=dict(baseColorFactor=[0, 0, 0, 1],
                                                     metallicFactor=0, roughnessFactor=1))])
            donor_binary = bytearray()
            add_mesh(donor, 'ExternalEmitter', [rectangle(donor, donor_binary, -1.5, 1.5,
                                                        .7, 3.3, 2.4, -1, 0)])
            write_glb(stage / 'Models/ExternalEmitter.glb', donor, donor_binary)
            scene['instances'] = [scene['instances'][0], dict(name='ExternalEmitter',
                model_path='Models/ExternalEmitter.glb', pos=[0, .01, 0], rot=[0, 0, 0],
                scale=[1, 1, 1], is_transparent=False, is_cutout=False)]
            lighting['direct light']['light_color'] = [0, 0, 0]
            lighting['environment']['intensity'] = 0
        save_json(stage / 'Settings/Pipeline.json', pipeline)
        save_json(stage / 'Settings/Scene.json', scene)
        save_json(stage / 'Settings/Lighting.json', lighting)
        save_json(stage / 'Settings/Window.json', dict(width=1280, height=800, fullscreen=False,
                                                     title='VXGI Cornell leak test'))
        for name, camera in cameras.items():
            save_json(stage / f'camera-{name}.json', camera)
        manifest['cases'][case] = dict(stage=str(stage), models={p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                                            for p in (stage / 'Models').glob('*.glb')})
    save_json(OUT / 'fixtures.json', manifest)
    print(OUT)


def visibility_control(interior_only=False):
    """Exact two-sided triangle visibility in the diagnostic shader only."""
    from AnalyzeVXGICornellLeak import model_triangles
    stage = OUT / ('B1Interior' if interior_only else 'B1Visible')
    for folder in ('Models', 'Settings'):
        shutil.copytree(OUT / 'B1' / folder, stage / folder, dirs_exist_ok=True)
    for path in (OUT/'B1').glob('camera-*.json'):
        shutil.copy2(path,stage/path.name)
    shutil.copytree(OUT/'common/Shaders',stage/'Shaders',dirs_exist_ok=True)
    triangles=model_triangles(OUT/'B1/Models/Cornell_Box.glb')
    vector=lambda v: 'float3('+','.join(format(float(x),'.9g') for x in v)+')'
    constants=',\n'.join(vector(v) for t in triangles for v in t)
    helper='''
// Test-only finite segment/triangle visibility. Same fixture as CPU reference.
static const float3 CornellTriangles[%d] = { %s };
bool CornellContributorVisible(float3 start, float3 end)
{
    float3 ray=end-start;
    [loop] for(uint i=0;i<%d;i++)
    {
        float3 a=CornellTriangles[i*3], e1=CornellTriangles[i*3+1]-a, e2=CornellTriangles[i*3+2]-a;
        float3 p=cross(ray,e2);float det=dot(e1,p);
        if(abs(det)<1e-9)continue;
        float inv=1.0/det;float3 s=start-a;float u=dot(s,p)*inv;
        if(u< -1e-6 || u>1.000001)continue;
        float3 q=cross(s,e1);float v=dot(ray,q)*inv;
        if(v< -1e-6 || u+v>1.000001)continue;
        float t=dot(e2,q)*inv;
        if(t>1e-5 && t<.99999)return false;
    }
    return true;
}
'''%(len(triangles)*3,constants,len(triangles))
    path=stage/'Shaders/VXGIProbeRadianceCommon.hlsli'
    source=path.read_text(encoding='utf-8-sig')
    needle='bool VXGIProbeQueryRecordValid('
    assert source.count(needle)==1
    source=source.replace(needle,helper+'\n'+needle)
    needle='if (!VXGIProbeQueryRecordValid(source)) continue;'
    assert source.count(needle)==1
    condition = '''
        float3 testCenter=(float3(base + corner) + 0.5) * voxelSize;
        if(any(testCenter<float3(-2,.01,-2.02536249)) || any(testCenter>float3(2,3.9810145,2.02536249))) continue;
''' if interior_only else '\n        if (!CornellContributorVisible((float3(base + corner) + 0.5) * voxelSize, worldPosition)) continue;'
    source=source.replace(needle,needle+condition)
    path.write_text(source,encoding='utf-8')
    save_json(stage/'visibility-control.json',dict(triangles=len(triangles),test_only=True,
        description='Only radiance-query contributors change; probe capture/classification/source shaders unchanged.',
        interior_only=interior_only,
        shader_sha256=hashlib.sha256(path.read_bytes()).hexdigest()))


def endpoint_control():
    raise RuntimeError('Exact contributor visibility was reverted for performance; endpoint captures are historical only.')


def sun_control():
    """Separate probe-source sunlight occlusion from external emissive visibility."""
    original_lighting=json.loads((ROOT/'Settings/Lighting.json').read_text())
    for case,base in (('B1Sun','B0'),('B2Sun','B2')):
        stage=OUT/case
        for folder in ('Models','Settings'):
            shutil.copytree(OUT/base/folder,stage/folder,dirs_exist_ok=True)
        # Retain the off-emitter asset for traceability; its instance is omitted below.
        shutil.copy2(OUT/'B0/Models/ExternalEmitter.glb',stage/'Models/ExternalEmitter.glb')
        for path in (OUT/base).glob('camera-*.json'):
            shutil.copy2(path,stage/path.name)
        lighting=json.loads((stage/'Settings/Lighting.json').read_text())
        lighting['direct light']=original_lighting['direct light']
        lighting['environment']['intensity']=0
        save_json(stage/'Settings/Lighting.json',lighting)
        # The opaque external emitter would shade the opening from this sun.
        # Remove it in both sunlight cases so opening the wall is a real positive control.
        scene=json.loads((stage/'Settings/Scene.json').read_text())
        scene['instances']=scene['instances'][:1]
        save_json(stage/'Settings/Scene.json',scene)


def bake_sun_witness():
    """Encode the rare interior sun-source hit and its small-tMin shadow trace in raw SH."""
    stage=OUT/'B1SunWitness'
    for folder in ('Models','Settings'):
        shutil.copytree(OUT/'B1Sun'/folder,stage/folder,dirs_exist_ok=True)
    for path in (OUT/'B1Sun').glob('camera-*.json'):
        shutil.copy2(path,stage/path.name)
    shutil.copytree(OUT/'common/Shaders',stage/'Shaders',dirs_exist_ok=True)
    path=stage/'Shaders/Shaders_For_VXGI_Probe_Update.hlsl'
    source=path.read_text(encoding='utf-8-sig')
    needle='radiance = source.lo0;'
    assert source.count(needle)==1
    replacement='''
    radiance=0;
    if (source.lo0.r>0 && all(center>float3(-2,.01,-2.02536249)) && all(center<float3(2,3.9810145,2.02536249)))
    {
        VXGISDFHit nearBlocker;
        bool nearFound=VXGISDFTrace(hit.position,normalize(probeParameters.sunDirectionTowardLight),
            1e-7,probeParameters.maxRayDistance,grid,sdfNodes,cellOffsets,triangleReferences,
            surfaceTriangles,surfaceMaterials,probeParameters.materialCount,materialSampler,
            probeParameters.materialMipLevel,nearBlocker);
        uint packed=(hit.triangleIndex+1) | ((nearFound?nearBlocker.triangleIndex+1:0)<<8) | (nearBlocker.traceStatus<<16);
        radiance=float3(float(packed),nearFound?nearBlocker.distance:-1.0,hit.position.z+2.02536249);
    }
'''
    path.write_text(source.replace(needle,replacement),encoding='utf-8')
    save_json(stage/'bake-witness.json',dict(test_only=True,
        description='Raw SH encodes packed primary/shadow triangle IDs and trace status, small-tMin shadow distance, back-wall z gap. Only original nonzero interior sun-source samples contribute.',
        shadow_test_min=1e-7,production_shadow_min=.0001))


if __name__ == '__main__':
    import sys
    if '--bake-sun-witness' in sys.argv:
        bake_sun_witness()
    elif '--sun-control' in sys.argv:
        sun_control()
    elif '--endpoint-control' in sys.argv:
        endpoint_control()
    elif '--visibility-control' in sys.argv or '--interior-control' in sys.argv:
        visibility_control('--interior-control' in sys.argv)
    else:
        prepare()
