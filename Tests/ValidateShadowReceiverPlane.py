"""Verify actual GPU helper results and the fixed-pose shadow regression."""
import json
from pathlib import Path
import numpy as np

stage = Path(__file__).resolve().parents[1] / 'tmp/shadow-receiver-plane/runtime'
runs = json.loads((stage / 'runs.json').read_text(encoding='utf-8-sig'))
images = {}
for run in runs:
    metadata_path = stage / run['capture']
    metadata = json.loads(metadata_path.read_text(encoding='utf-8-sig'))
    width, height = metadata['render_size']
    pixels = np.fromfile(metadata_path.parent / '0-result.raw', dtype='<f2').astype(np.float32).reshape(height, width, 4)
    assert np.isfinite(pixels).all(), run['name']
    assert pixels[:, :, :3].std() > .001, run['name']
    assert run['debug_available'] and run['warnings'] == run['errors'] == 0
    images[run['name']] = pixels

analytic = images['analytic']
errors = []
for index in range(32):
    expected = np.zeros(2)
    if index < 24:
        slope = np.array([(.25, -.5), (2, -3), (100, 30)][index // 8])
        # Analytic plane z = a*x+b*y; the existing grazing cap is unchanged.
        expected = slope / max(1, np.sqrt(1 + np.dot(slope, slope)) * .0872665)
    elif index == 29:
        expected = np.array([-1 / .0872665, 0])
    error = np.abs(analytic[:, index::32, :2] - expected).max()
    assert error < .008, (index, float(error), expected.tolist())  # Native output is FP16.
    assert (analytic[:, index::32, 2:] == 1).all(), index
    errors.append(float(error))

# Known exterior pixels above the real silhouette: legacy falsely darkens the
# receiver itself. Dark interior pixels ensure the fix does not erase shadows.
legacy, fixed = images['legacy'], images['fixed']
points = [(600, 250), (600, 350), (600, 380), (1000, 300)]
samples = []
for x, y in points:
    gain = float((fixed[y, x, :3] - legacy[y, x, :3]).mean())
    assert gain > .05, (x, y, gain)
    samples.append(dict(pixel=[x, y], mean_rgb_gain=gain))
assert np.abs(fixed[600:750, 700:1100] - legacy[600:750, 700:1100]).max() < .002
# A few tiny legacy acne spots in the exterior also become brighter. Require
# no new darkening there, rather than preserving those erroneous shadows.
assert (fixed[:120, 600:1100] - legacy[:120, 600:1100]).min() > -.002
for x, y in [(600, 100), (700, 50), (1000, 100), (800, 30)]:
    assert np.abs(fixed[y, x] - legacy[y, x]).max() < .002
report = dict(passed=True, runs=len(runs), analytic_cases=32, max_fp16_gradient_error=max(errors),
              regression_samples=samples, debug_warnings=0, debug_errors=0)
(stage / 'validation.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps(report, indent=2))
