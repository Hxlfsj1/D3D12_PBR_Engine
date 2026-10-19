"""Run the actual application hidden, with optional Pass changes and GPU telemetry."""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import traceback


def validate_toggle_evidence(rows, capture=None):
    """Require measured steady frames on both sides of the actual Pass round trip."""
    def finite_number(value):
        return (isinstance(value, (int, float)) and not isinstance(value, bool)
            and math.isfinite(value))

    timed_rows = []
    for row in rows:
        elapsed = row.get('metadata', {}).get('time_s')
        if finite_number(elapsed):
            timed_rows.append((float(elapsed), row))
    timed_rows.sort(key=lambda item: (item[0], item[1].get('frame', 0)))
    off_times = [elapsed for elapsed, row in timed_rows
        if row.get('metadata', {}).get('actual_mode') == 0]
    if not off_times:
        raise RuntimeError('Pass acceptance failed: the off transition has no finite time_s evidence')
    first_off = off_times[0]
    on_times = [elapsed for elapsed, row in timed_rows if elapsed > first_off
        and row.get('metadata', {}).get('actual_mode') == 2]
    if not on_times:
        raise RuntimeError('Pass acceptance failed: the on transition has no finite time_s evidence')
    first_on = on_times[0]

    if capture is not None:
        snapshots = capture.get('captures', [])
        if len(snapshots) != 2:
            raise RuntimeError('RawGI Pass acceptance failed: two completed capture records are required')
        first_time = snapshots[0].get('time_s')
        second_time = snapshots[1].get('time_s')
        if not finite_number(first_time) or not 0 <= first_time < min(8.0, first_off):
            raise RuntimeError('RawGI Pass acceptance failed: the first snapshot must precede the first screen-only Pass '
                f'(time_s < 8); got {first_time!r}, first screen-only Pass {first_off:.6f}s')
        if not finite_number(second_time) or second_time < max(10.0, first_on):
            raise RuntimeError('RawGI Pass acceptance failed: the second snapshot must follow restoration of the GI Pass '
                f'(time_s >= {max(10.0, first_on):.6f}); got {second_time!r}')
        if any(snapshot.get('world_mode') != 2 for snapshot in snapshots):
            raise RuntimeError('RawGI Pass acceptance failed: both snapshots must use world mode 2')

    windows = {}
    for name, minimum, maximum in (('pre', 2.0, 8.0), ('post', 10.25, math.inf)):
        samples = []
        for elapsed, row in timed_rows:
            metadata = row.get('metadata', {})
            gpu = row.get('gpu_ms', {})
            if not minimum <= elapsed < maximum or metadata.get('actual_mode') != 2:
                continue
            if any(metadata.get(flag) for flag in ('diagnostics_gpu_pending',
                    'diagnostics_gpu_not_submitted', 'diagnostics_readback_failed')):
                continue
            if not all(finite_number(gpu.get(mark)) and gpu[mark] > 0
                    for mark in ('Frame', 'SSGI.Trace')):
                continue
            samples.append((elapsed, row))
        frame_ids = {row.get('frame') for _, row in samples}
        duration = samples[-1][0] - samples[0][0] if samples else 0.0
        if len(frame_ids) < 30 or duration < 2.0:
            interval = f'[{minimum}, {maximum})s' if math.isfinite(maximum) else f'[{minimum}, end)s'
            raise RuntimeError(f'Pass acceptance failed: insufficient steady {name} GPU evidence in {interval}; '
                f'got {len(frame_ids)} frames spanning {duration:.6f}s, require at least 30 frames '
                'spanning 2 seconds with completed Frame and SSGI.Trace timestamps. '
                'Startup/transient frames cannot substitute for the pre-switch baseline.')
        windows[name] = {'frame_count': len(frame_ids), 'first_time_s': samples[0][0],
            'last_time_s': samples[-1][0], 'span_seconds': duration}
    return {'first_off_time_s': first_off, 'first_on_time_s': first_on, 'steady_windows': windows}


def validate_debug_pass_evidence(rows):
    """Validate Default -> VXGIOff -> SSGIOnly -> GIOnly -> Default."""
    ordered = sorted(rows, key=lambda row: row.get('frame', -1))
    views, stages, frame_ids = [], [], set()
    previous_mode = None
    for row in ordered:
        frame = row.get('frame')
        if type(frame) is not int or frame < 0 or frame in frame_ids:
            raise RuntimeError(f'Pass acceptance failed: invalid or duplicate frame {frame!r}')
        frame_ids.add(frame)
        meta = row.get('metadata', {})
        view = meta.get('debug_pass')
        if type(view) is not int or view not in (0, 1, 2, 3):
            raise RuntimeError(f'Pass acceptance failed: invalid debug_pass {view!r}')
        if meta.get('gi_only') is not (view in (1, 2)):
            raise RuntimeError(f'Pass acceptance failed: view {view} has incorrect gi_only output')
        mode = 0 if view in (1, 3) else 2
        if meta.get('actual_mode') != mode or meta.get('effective_mode') != mode:
            raise RuntimeError(f'Pass acceptance failed: view {view} did not execute world mode {mode}')
        if meta.get('submitted') is not True or any(meta.get(flag) for flag in
                ('diagnostics_gpu_pending', 'diagnostics_gpu_not_submitted', 'diagnostics_readback_failed')):
            raise RuntimeError('Pass acceptance failed: incomplete or unsubmitted frame')
        if not views or views[-1] != view:
            if views and meta.get('view_history_reset') is not True:
                raise RuntimeError(f'Pass acceptance failed: view {view} retained final color history')
            if meta.get('dlss_evaluated') is True and meta.get('dlss_reset') is not True:
                raise RuntimeError(f'Pass acceptance failed: first frame of view {view} did not reset DLSS')
            if previous_mode is not None and previous_mode != mode and meta.get('history_reset') is not True:
                raise RuntimeError(f'Pass acceptance failed: world mode change at frame {frame} retained GI history')
            views.append(view)
            stages.append({'debug_pass': view, 'world_mode': mode, 'first_frame': frame,
                'frames': 0, 'view_history_reset': meta.get('view_history_reset'),
                'dlss_evaluated': meta.get('dlss_evaluated'), 'dlss_reset': meta.get('dlss_reset'),
                'gi_history_reset': meta.get('history_reset')})
        stages[-1]['frames'] += 1
        previous_mode = mode
    if views != [0, 3, 1, 2, 0]:
        raise RuntimeError(f'Pass acceptance failed: view sequence {views} != [0, 3, 1, 2, 0]')
    return {'view_transitions': views, 'stages': stages, 'completed_frames': len(ordered),
        'world_mode_matches_view': True, 'gi_only_matches_view': True,
        'output_history_reset_on_view_change': True, 'gi_history_reset_on_world_change': True}


def validate_gpu_validation(rows, report, prefix, sequence, mode_read):
    """Check full-program API correctness evidence, never performance."""
    required = ('enabled', 'enable_succeeded', 'debug_layer_enabled',
        'gpu_based_validation_enabled', 'attached', 'info_queue_available',
        'finished', 'passed', 'device_removed_queried')
    if not all(report.get(field) is True for field in required):
        raise RuntimeError('GBV acceptance failed: enable/attach/finish/correctness evidence failed; inspect '
            + str(prefix) + '-validation.json')
    for field in ('errors', 'corruption_errors', 'operation_failures', 'discarded_messages',
            'messages_denied_after_attach', 'device_removed_hresult_value'):
        if report.get(field) != 0:
            raise RuntimeError(f'GBV acceptance failed: {field}={report.get(field)!r}; raw messages retained')
    pre_attach_denied = report.get('pre_attach_denied_messages')
    denied_after_attach = report.get('messages_denied_after_attach')
    absolute_denied = report.get('messages_denied_by_storage_filter')
    if any(type(value) is not int or value < 0 for value in
            (pre_attach_denied, denied_after_attach, absolute_denied)):
        raise RuntimeError('GBV acceptance failed: missing or invalid InfoQueue denied-message baseline')
    if absolute_denied != pre_attach_denied + denied_after_attach:
        raise RuntimeError('GBV acceptance failed: absolute denied count does not match baseline plus attached-session count')
    if report.get('attach_info_queue_baseline', {}).get('messages_denied_by_storage_filter') != pre_attach_denied:
        raise RuntimeError('GBV acceptance failed: denied-message baseline does not match the attachment snapshot')
    if report.get('initial_storage_filter_captured') is not True or not isinstance(report.get('initial_storage_filter'), dict):
        raise RuntimeError('GBV acceptance failed: initial storage-filter snapshot is missing')
    log_path = prefix.with_name(prefix.name + '-validation.jsonl')
    messages = [json.loads(line) for line in log_path.read_text(encoding='utf-8').splitlines() if line.strip()]
    if any(message.get('record_type') != 'message' for message in messages):
        raise RuntimeError('GBV acceptance failed: diagnostic failures exist in the raw message log')
    if len(messages) != report.get('messages_total'):
        raise RuntimeError('GBV acceptance failed: raw message count does not match summary')
    if any(type(message.get('severity')) is not int or message['severity'] not in range(5)
            for message in messages):
        raise RuntimeError('GBV acceptance failed: invalid raw D3D12 severity')
    severities = Counter(str(message['severity']) for message in messages)
    if dict(severities) != report.get('messages_by_severity'):
        raise RuntimeError('GBV acceptance failed: raw severity counts do not match summary')
    if severities['2'] != report.get('warnings'):
        raise RuntimeError('GBV acceptance failed: raw warning count does not match summary')
    if dict(Counter(str(message['message_id']) for message in messages)) != report.get('messages_by_message_id'):
        raise RuntimeError('GBV acceptance failed: raw message IDs do not match summary')
    if any(message['severity'] <= 1 for message in messages):
        raise RuntimeError('GBV acceptance failed: raw D3D12 errors exist')
    enabled_confirmations = sum(message['message_id'] == 1016 and message['severity'] == 4 for message in messages)
    if not enabled_confirmations:
        raise RuntimeError('GBV acceptance failed: the runtime GPU-validation-enabled message (ID 1016) is missing')
    completed = []
    frame_ids = set()
    for row in rows:
        frame = row.get('frame')
        if type(frame) is not int or frame < 0 or frame in frame_ids:
            raise RuntimeError(f'GBV acceptance failed: invalid or duplicate logical frame {frame!r}')
        frame_ids.add(frame)
        meta, gpu = row.get('metadata', {}), row.get('gpu_ms', {})
        if any(meta.get(flag) for flag in ('diagnostics_gpu_pending',
                'diagnostics_gpu_not_submitted', 'diagnostics_readback_failed')):
            raise RuntimeError('GBV acceptance failed: incomplete GPU frame evidence')
        if meta.get('submitted') is not True or meta.get('dlss_evaluated') is not True:
            raise RuntimeError('GBV acceptance failed: frame submission or actual DLSS evaluation missing')
        if meta.get('gpu_validation_requested') is not True or meta.get('mode_read') != mode_read:
            raise RuntimeError('GBV acceptance failed: wrong validation or shader read path')
        present = meta.get('present_hr')
        if not isinstance(present, int) or present & 0x80000000:
            raise RuntimeError('GBV acceptance failed: Present failed or has no evidence')
        counts = meta.get('imgui_counts', [])
        if len(counts) != 2 or min(counts) <= 0:
            raise RuntimeError('GBV acceptance failed: real editor draw data missing')
        if not all(isinstance(gpu.get(mark), (int, float)) and math.isfinite(gpu[mark]) and gpu[mark] > 0
                for mark in ('Frame', 'SSGI.Trace', 'Editor.Draw', 'DLSS')):
            raise RuntimeError('GBV acceptance failed: full production GPU passes missing')
        if meta.get('slot') not in (0, 1, 2):
            raise RuntimeError('GBV acceptance failed: invalid physical frame slot')
        completed.append(row)
    slots = Counter(row['metadata']['slot'] for row in completed)
    if len(completed) < 6 or any(slots[slot] < 2 for slot in range(3)):
        raise RuntimeError('GBV acceptance failed: require at least two completed submissions through every slot')
    stages = {}
    if sequence == 'validation-toggle':
        for stage, mode in enumerate((2, 0, 2)):
            stage_rows = [row for row in completed if row['metadata'].get('validation_stage') == stage]
            stage_slots = Counter(row['metadata']['slot'] for row in stage_rows)
            if len(stage_rows) < 12 or any(stage_slots[slot] < 3 for slot in range(3)):
                raise RuntimeError(f'GBV acceptance failed: stage {stage} lacks 12 complete frames/all slots')
            if any(row['metadata'].get('actual_mode') != mode or row['metadata'].get('effective_mode') != mode
                    for row in stage_rows):
                raise RuntimeError(f'GBV acceptance failed: stage {stage} did not execute mode {mode}')
            stages[str(stage)] = {'mode': mode, 'frames': len(stage_rows), 'slots': dict(stage_slots)}
    return {'scope': 'application_work_after_device_return_through_gpu_idle_cleanup',
        'coverage_contract': 'InfoQueue attaches immediately after device creation returns, before application queues, '
            'resources and initialization GPU work. Device-internal messages denied before attachment cannot be '
            'recovered and their severities are unknown; their absolute count is retained, not declared zero.',
        'pre_attach_denied_messages': pre_attach_denied,
        'messages_denied_after_attach': denied_after_attach,
        'messages_denied_by_storage_filter': absolute_denied,
        'pre_attach_denied_messages_recoverable': False,
        'initial_storage_filter': report['initial_storage_filter'],
        'gbv_enabled_confirmation_count': enabled_confirmations,
        'timings_are_performance_comparable': False, 'completed_frames': len(completed),
        'frame_slots': dict(slots), 'stages': stages, 'errors': report['errors'],
        'warnings': report['warnings'], 'messages_total': len(messages),
        'message_log_sha256': hashlib.sha256(log_path.read_bytes()).hexdigest()}


def write_acceptance(path, document):
    """Never replace evidence from a previous successful or failed run."""
    with path.open('x', encoding='utf-8') as output:
        json.dump(document, output, indent=2)
        output.write('\n')


def record_failed_acceptance(context, error):
    """Keep the rejection separate from all original application reports."""
    root, prefix = context['root'], context['prefix']
    after_hashes, hash_errors = {}, {}
    for path in context.get('source_paths', []):
        key = path.relative_to(root).as_posix()
        try:
            after_hashes[key] = hashlib.sha256(path.read_bytes()).hexdigest()
        except Exception as hash_error:
            hash_errors[key] = str(hash_error)
    before_hashes = context.get('source_sha256_before')
    changed = None if before_hashes is None else sorted(key for key in before_hashes.keys() | after_hashes.keys()
        if before_hashes.get(key) != after_hashes.get(key))
    args = context['args']
    document = {'passed': False, 'sequence': args.sequence, 'configuration': args.configuration,
        'gpu_validation_requested': args.gpu_validation, 'mode_read': args.mode_read,
        'label': args.label, 'exception_type': type(error).__name__, 'exception': str(error),
        'exception_traceback': traceback.format_exc(), 'process_exit': context.get('process_exit'),
        'command': context.get('command'), 'executable_path': str(context['executable']),
        'executable_sha256': context.get('executable_sha256'),
        'source_sha256': before_hashes, 'source_sha256_before': before_hashes,
        'source_sha256_after': after_hashes, 'source_hash_errors': hash_errors,
        'source_changed_paths': changed,
        'rendering_source_unchanged': None if before_hashes is None else not changed and not hash_errors,
        'process_wall_seconds': time.perf_counter() - context['start'],
        'original_report_paths': {name: str(prefix.with_name(prefix.name + suffix)) for name, suffix in (
            ('frames', '-frames.json'), ('validation', '-validation.json'),
            ('validation_messages', '-validation.jsonl'), ('rawgi', '-rawgi-report.json'),
            ('gpu_telemetry', '-gpu.csv'))},
        'contract': 'Rejected capture. Original application reports and raw messages are preserved. '
            'GBV timestamps do not establish performance.'}
    # A failure before executable hashing still records the hash when readable.
    if document['executable_sha256'] is None:
        try:
            document['executable_sha256'] = hashlib.sha256(context['executable'].read_bytes()).hexdigest()
        except Exception as hash_error:
            document['executable_hash_error'] = str(hash_error)
    path = prefix.with_name(prefix.name + '-acceptance.json')
    try:
        write_acceptance(path, document)
    except FileExistsError:
        path = prefix.with_name(prefix.name + f'-failure-{time.time_ns()}-acceptance.json')
        write_acceptance(path, document)
    return path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--label', required=True)
    parser.add_argument('--sequence', choices=('control', 'toggle', 'capture-toggle', 'capture-fixed', 'immediate', 'reset', 'pulse', 'alias', 'one', 'no-pgo', 'no-background', 'pgo', 'pgo-discard', 'prime', 'no-dlss', 'skip-dlss', 'pause-dlss', 'no-jitter', 'clearstate', 'barrier', 'serial', 'validation-toggle', 'debug-pass'), default='control')
    parser.add_argument('--seconds', type=float, default=32)
    parser.add_argument('--configuration', choices=('Debug', 'Release'), default='Debug')
    parser.add_argument('--gpu-validation', action='store_true', help='Enable full-application GBV and collect all InfoQueue diagnostics; timings are not comparable.')
    parser.add_argument('--mode-read', choices=('cbv', 'srv'), default='srv', help='Production SRV path or explicit CBV control; no source mutation.')
    parser.add_argument('--dlss-runtime', choices=('dev', 'rel'), help='Temporarily select the bundled NGX runtime; restore the original DLL after exit.')
    args = parser.parse_args()
    if args.sequence == 'validation-toggle' and not args.gpu_validation:
        parser.error('validation-toggle requires --gpu-validation')
    if not args.label or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for c in args.label):
        parser.error('label must contain letters, digits, hyphens or underscores')
    root = Path(__file__).resolve().parent.parent
    prefix = root / 'tmp' / ('VXGI-full-' + args.label)
    prefix.parent.mkdir(exist_ok=True)
    if any(prefix.parent.glob(prefix.name + '-*')):
        parser.error('capture artifacts already exist; choose a fresh label to preserve evidence')
    context = {'args': args, 'root': root, 'prefix': prefix, 'start': time.perf_counter(),
        'executable': root / 'x64' / args.configuration / 'LearnDirectX.exe'}
    try:
        run_capture(args, root, prefix, context)
    except Exception as error:
        try:
            path = record_failed_acceptance(context, error)
            print('Capture rejected; failure evidence: ' + str(path), flush=True)
        except Exception as artifact_error:
            print('Unable to save failure evidence: ' + str(artifact_error), flush=True)
        raise


def run_capture(args, root, prefix, context):
    report_path = prefix.with_name(prefix.name + '-frames.json')
    source_paths = [root / p for p in ('PBR_Render.cpp', 'Passes/SSGIPass.h', 'Passes/ScalarTemporalFilterPass.h',
        'Graphics/PipelineManager.h', 'Graphics/PBR_Shader.h', 'Graphics/RenderDevice.h', 'Graphics/DLSSManager.h',
        'Core/D3D12App.h', 'Core/FrameDiagnostics.h', 'Core/TraceOutputDiagnostics.h', 'Core/D3D12ValidationDiagnostics.h',
        'Resources/ResourceManager.h', 'Resources/RenderDebugPass.h', 'Editor/EditorUI.h',
        'Editor/EditorUICenter.h', 'Settings/Pipeline.json')]
    source_paths.extend(sorted((root / 'Shaders').glob('*.hlsl')))
    source_paths.extend(sorted((root / 'Shaders').glob('*.hlsli')))
    context['source_paths'] = source_paths
    def source_hashes():
        return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in source_paths}
    before_hashes = source_hashes()
    context['source_sha256_before'] = before_hashes
    executable_hash = hashlib.sha256((root / 'x64' / args.configuration / 'LearnDirectX.exe').read_bytes()).hexdigest()
    context['executable_sha256'] = executable_hash
    telemetry = None
    telemetry_file = prefix.with_name(prefix.name + '-gpu.csv').open('w', encoding='utf-8')
    smi = Path(r'C:\Windows\System32\nvidia-smi.exe')
    start = time.perf_counter()
    runtime = root / 'x64' / args.configuration / 'nvngx_dlss.dll'
    original_hash = hashlib.sha256(runtime.read_bytes()).hexdigest()
    runtime_hash = original_hash
    runtime_backup = None
    try:
        if smi.exists():
            telemetry = subprocess.Popen([str(smi), '--query-gpu=timestamp,utilization.gpu,clocks.gr,clocks.mem,pstate,power.draw,temperature.gpu,memory.used,memory.free',
                '--format=csv', '--loop-ms=500'], cwd=root, stdout=telemetry_file,
                stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        if args.dlss_runtime:
            with tempfile.NamedTemporaryFile(prefix='VXGI-dlss-original-', suffix='.dll', dir=prefix.parent, delete=False) as backup:
                backup_path = Path(backup.name)
            try:
                shutil.copy2(runtime, backup_path)
            except Exception:
                backup_path.unlink(missing_ok=True)
                raise
            runtime_backup = backup_path
            shutil.copy2(root / 'ThirdParty/NVIDIA/DLSS/lib/Windows_x86_64' / args.dlss_runtime / 'nvngx_dlss.dll', runtime)
            runtime_hash = hashlib.sha256(runtime.read_bytes()).hexdigest()
        command = [str(root / 'x64' / args.configuration / 'LearnDirectX.exe'),
            '--profile-startup=' + args.label, '--profile-sequence=' + args.sequence,
            '--profile-seconds=' + str(args.seconds), '--profile-mode-read=' + args.mode_read]
        if args.gpu_validation:
            command.append('--profile-gpu-validation')
        context['command'] = command
        result = subprocess.run(command, cwd=root, creationflags=subprocess.CREATE_NO_WINDOW)
        context['process_exit'] = result.returncode
        if result.returncode:
            raise RuntimeError(f'application exit {result.returncode}')
    finally:
        try:
            if runtime_backup:
                shutil.copy2(runtime_backup, runtime)
                if hashlib.sha256(runtime.read_bytes()).hexdigest() != original_hash:
                    raise RuntimeError('original NGX runtime was not restored; backup retained at ' + str(runtime_backup))
                runtime_backup.unlink()
        finally:
            if telemetry:
                telemetry.terminate()
                telemetry.wait(timeout=10)
            telemetry_file.close()
    after_hashes = source_hashes()
    context['source_sha256_after'] = after_hashes
    if after_hashes != before_hashes:
        raise RuntimeError('rendering source changed during capture; results are not accepted')
    report = json.loads(report_path.read_text(encoding='utf-8'))
    rows = report['frames']
    modes = []
    for row in rows:
        mode = row['metadata']['actual_mode']
        if not modes or modes[-1] != mode:
            modes.append(mode)
    expected_modes = [2, 0, 2] if args.sequence in ('toggle', 'immediate', 'capture-toggle', 'validation-toggle', 'debug-pass') else [2]
    if modes != expected_modes:
        raise RuntimeError(f'Pass sequence did not execute as requested: {modes} != {expected_modes}')
    if any(row['metadata']['probe_work'] != 0 for row in rows):
        raise RuntimeError('unexpected probe generation during initialized static-scene profiling')
    if any(row['metadata'].get('diagnostics_gpu_pending') for row in rows):
        raise RuntimeError('GPU samples were not collected at shutdown')
    capture = None
    if args.sequence in ('capture-toggle', 'capture-fixed', 'alias'):
        capture = json.loads(prefix.with_name(prefix.name + '-rawgi-report.json').read_text(encoding='utf-8'))
        if not capture['passed'] or capture['capture_count'] != 2:
            raise RuntimeError('RawGI capture failed: ' + capture.get('error', 'missing captures'))
    toggle_evidence = None
    if args.sequence in ('toggle', 'capture-toggle'):
        toggle_evidence = validate_toggle_evidence(rows, capture if args.sequence == 'capture-toggle' else None)
    debug_pass_evidence = validate_debug_pass_evidence(rows) if args.sequence == 'debug-pass' else None
    validation_evidence = None
    if args.gpu_validation:
        validation = json.loads(prefix.with_name(prefix.name + '-validation.json').read_text(encoding='utf-8'))
        validation_evidence = validate_gpu_validation(rows, validation, prefix, args.sequence, args.mode_read)
    summary = {'passed': True, 'sequence': args.sequence, 'configuration': args.configuration, 'process_exit': 0,
        'gpu_validation_requested': args.gpu_validation, 'mode_read': args.mode_read,
        'dlss_runtime_override': args.dlss_runtime, 'dlss_runtime_sha256': runtime_hash,
        'dlss_original_sha256': original_hash, 'dlss_original_restored': hashlib.sha256(runtime.read_bytes()).hexdigest() == original_hash,
        'executable_sha256': executable_hash, 'source_sha256': before_hashes,
        'source_sha256_after': after_hashes, 'rendering_source_unchanged': True,
        'actual_mode_transitions': modes, 'frame_count': len(rows), 'process_wall_seconds': time.perf_counter() - start,
        'probe_work_always_zero': True, 'statistics': report['statistics']}
    if toggle_evidence is not None:
        summary['toggle_evidence'] = toggle_evidence
    if debug_pass_evidence is not None:
        summary['debug_pass_evidence'] = debug_pass_evidence
    if validation_evidence is not None:
        summary['validation_evidence'] = validation_evidence
    write_acceptance(prefix.with_name(prefix.name + '-acceptance.json'), summary)
    print(json.dumps({key: value for key, value in summary.items()
        if key not in ('source_sha256', 'source_sha256_after')}), flush=True)


if __name__ == '__main__':
    main()
