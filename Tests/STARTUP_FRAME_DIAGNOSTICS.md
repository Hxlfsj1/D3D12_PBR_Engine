# Startup / Pass round-trip diagnostics

Run from the repository root with the matching application already built:

```powershell
D:\Python\python.exe Tests\RunStartupFrameDiagnostics.py --label startup-control --sequence control --seconds 32 --configuration Debug
D:\Python\python.exe Tests\RunStartupFrameDiagnostics.py --label startup-toggle --sequence capture-toggle --seconds 32 --configuration Debug
D:\Python\python.exe Tests\RunStartupFrameDiagnostics.py --label debug-pass-check --sequence debug-pass --seconds 1 --configuration Debug
D:\Python\python.exe Tests\AnalyzeStartupFrameDiagnostics.py tmp/VXGI-full-startup-control-frames.json tmp/VXGI-full-startup-toggle-frames.json --output-prefix tmp/startup-comparison
```

The actual application runs with a hidden window. It executes initialization,
Update, the production render graph, DLSS, editor rendering and Present. The
diagnostic process changes the selected Debug > Pass view at 8 and 10 seconds;
there is no keyboard injection or desktop interaction. Legacy CLI names such as
`toggle` and `capture-toggle` are preserved for existing capture scripts. Camera
navigation is disabled only in the diagnostic process.

Frame timestamps use a three-slot GPU query/readback ring. Collection follows the
application's existing frame fences; ordinary capture adds no GPU waits. JSON is
written once after the finite run. `wall_ms` describes the preceding frame's CPU
interval, so compare settled phase windows rather than the transition frame.
Instrumentation and hidden presentation make these diagnostic FPS different
from a Visual Studio session. Compare GPU pass times and matched phases.

`capture-toggle` also copies RawGI once after 6 seconds and once after 12 seconds,
matching the combined STBN64 / DLSS18 sampling period of 576 frames for the current
1707-to-2560 configuration. Copies restore the UAV state and preserve its pixels.
The report compares every RGBA16F byte and reports numerical error. These two
copy frames should be excluded from timing distributions. This fixed sampling
period must be revised if jitter phase count or blue-noise depth changes.

Outputs use `tmp/VXGI-full-<label>` followed by `-frames.json`, `-gpu.csv`,
`-acceptance.json`, and, when enabled, `-rawgi-report.json` plus two RGBA16F files.
The runner requires successful exit, the requested actual-mode sequence, zero
static-scene probe work and completed GPU samples. It records the DLSS DLL hash.
For `toggle` and `capture-toggle`, it also requires at least 30 completed GPU
frames spanning two seconds in each of the pre `[2,8)` and post `[10.25,end)`
windows. `capture-toggle` requires its first RawGI snapshot before the actual
screen-only Pass transition and its second after restoring the GI Pass. A successful mode sequence alone
does not establish a usable before/after baseline. Keep application runs and
their resource initialization strictly sequential; wait for the runner to exit
before starting another run.
`--dlss-runtime dev|rel` temporarily selects a bundled runtime, restores the
original file in `finally`, and verifies its hash.

`debug-pass` exercises the four temporary viewport choices in order: Default,
VXGIOff, SSGIOnly, GIOnly, then Default again. Each phase uses at least 12 frames;
the sequence returns to Default before it finishes. Its compressed view sequence
must be `0,3,1,2,0`, and its world-mode sequence must be `2,0,2`. Only SSGIOnly and
GIOnly display pure GI. VXGIOff retains normal composition with screen GI and
world mode zero. Every frame must
agree on the selected view, pure-GI composition and effective world mode. On view
changes, final color history must be reset; when DLSS runs, its actual reset
argument must also be true. World-mode changes must reset the GI signal history.
The VXGIOff-to-SSGIOnly transition may retain GI signal history because its source
is unchanged, while its final color history must still reset.
This frame-count acceptance checks rendering state rather than the long settled
timing windows used by `toggle` and `capture-toggle`.

## Full-application GPU-based validation

With a matching Debug executable already built, run these one at a time:

```powershell
D:\Python\python.exe Tests\RunStartupFrameDiagnostics.py --label gbv-cbv-check --sequence validation-toggle --gpu-validation --mode-read cbv --configuration Debug
D:\Python\python.exe Tests\RunStartupFrameDiagnostics.py --label gbv-srv-check --sequence validation-toggle --gpu-validation --mode-read srv --configuration Debug
```

`--mode-read` selects the explicit CBV control or the production SRV mode read
without changing rendering source. The default is `srv`. The runner forwards
`--profile-mode-read=cbv|srv`; `--gpu-validation` also forwards
`--profile-gpu-validation`. GPU validation requires a profiling label and enables
the Debug Layer and GBV before device creation.

`validation-toggle` requires GBV. It exercises 12 frames in mode 2, 12 in mode 0,
then 12 in mode 2. These switches use frame counts rather than elapsed seconds,
and the sequence ends after 36 attempted frames. Runtime shader patching can make
the first frame take longer than the nominal `--seconds` value. The runner still
requires at least 12 distinct, completed frames per stage, with all three physical
frame slots represented at least three times in each stage. Actual and effective
modes must both match 2/0/2. Ordinary GBV `control` runs instead require at least
two completed submissions through every slot, and therefore at least six frames.

InfoQueue collection attaches as soon as device creation returns, before the
application creates queues, command lists or resources. Coverage then uses the
actual application's initialization GPU commands, production
SSGI graph, DLSS evaluation, editor rendering, Present, final fence drain, and
DLSS/ImGui cleanup. Each validated frame must include successful submission and
DLSS metadata, successful Present, real editor draw data, and completed timestamps
for Frame, SSGI.Trace, Editor.Draw and DLSS. This is separate from the small numeric
GPU fixtures. Final member/device COM releases happen after the last InfoQueue
collection, so this is not a live-object or leak check. GBV also does not replace
the independent RawGI image comparison.

Messages denied by the runtime's initial storage filter before this attachment
cannot be recovered: the device/InfoQueue interface does not exist for the
application before device creation returns. Their count remains explicit as
`pre_attach_denied_messages`, alongside the complete initial allow/deny filter
snapshot. This snapshot cannot establish the severities of messages already
denied. For example, a baseline of 64 is reported as 64 unavailable internal
messages, never as zero. Application work after attachment requires zero newly
denied messages. The absolute denied count must equal the pre-attachment baseline
plus the attached-session count. This boundary does not exclude application
object creation, initialization GPU work, the 36 production frames or GPU-idle
DLSS/ImGui cleanup.

GBV patches shaders and adds synchronization and diagnostic work. **Its wall-clock
and GPU timings must not be compared as performance measurements**, including
CBV-versus-SRV timings. `validation-toggle` does not use the wall-clock pre/post
windows of `toggle` and `capture-toggle`; those earlier acceptance gates remain
unchanged for normal performance captures.

GBV adds two original application artifacts:

- `-validation.jsonl` retains every InfoQueue message, including its original
  severity, ID, category and description. Context identifies when a message was
  polled; asynchronous GBV messages may originate from an earlier GPU operation.
- `-validation.json` records enabling, attachment, device status, counters,
  discarded/filtered messages and finalization. Any available D3D12 error or
  corruption, diagnostic failure, discarded message, newly denied message after
  attachment, raw-log mismatch, or missing frame coverage rejects the capture.
  The discarded-message counter must still be absolutely zero. Warnings are
  retained and reported, with severity and warning counts checked against the raw
  log; known IDs are never filtered. Acceptance also requires the runtime's GBV
  enabled confirmation (message ID 1016, severity 4), in addition to enable-call
  metadata.

An application exit code of zero alone does not establish acceptance. The runner
writes `-acceptance.json` with `passed: true` only after every gate passes. Rejected
runs write `passed: false`, the exception and traceback, application exit code when
available, original report paths, executable hash, and source hashes before/after
the run. Missing files, initialization failures, source changes and coverage
failures also produce this artifact. Original reports and JSONL are preserved.
Labels with existing artifacts are rejected to preserve history; exclusive file
creation also prevents overwriting an earlier successful acceptance report.

Other sequences are explicit isolation experiments, never default rendering
settings. In particular, `serial` intentionally waits for all frame slots.
Driver background-processing controls require a supporting Device6 interface;
the disabling modes also require Windows developer mode. They apply only to the
diagnostic device and are restored before shutdown. No OS or driver profile
settings are changed. Do not use disabled driver optimization as a shipping fix.
