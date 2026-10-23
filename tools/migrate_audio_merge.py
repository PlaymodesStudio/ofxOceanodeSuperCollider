#!/usr/bin/env python3
"""Replace ScBus fan-in with AudioMerge in a preset/macro library.

Dry run by default; --apply backs up each changed JSON outside the active trees.
    python3 tools/migrate_audio_merge.py <bin/data> [--apply]
"""
import argparse
import collections
import datetime
import json
from pathlib import Path
import shutil


def migrate(folder, write):
    connections_path = folder / 'connections.json'
    modules_path = folder / 'modules.json'
    if not modules_path.exists():
        return 0
    conns = json.loads(connections_path.read_text()) or {}
    modules = json.loads(modules_path.read_text()) or {}
    nodes = {}
    for kind, positions in modules.items():
        for index in positions:
            key = kind.replace(' ', '_') + '_' + str(int(index))
            path = folder / (key + '.json')
            if path.exists(): nodes[key] = json.loads(path.read_text())
    incoming = collections.defaultdict(list)
    for src, outs in conns.items():
        for out, dests in (outs or {}).items():
            for dst, params in (dests or {}).items():
                for param in params: incoming[dst, param].append((src, out))

    def channels(key, port, seen=frozenset()):
        if key in seen: raise ValueError('Channel inference cycle: ' + key)
        data = nodes.get(key, {})
        if key.startswith('SC_'): return int(data['N_Chan'])
        if key.startswith('Macro_'):
            inner = folder / key
            for p in inner.glob('Router_ScBus_*.json'):
                if json.loads(p.read_text()).get('Name') == port:
                    # Resolve the output router against the nested patch.
                    return infer_router(inner, p.stem, seen | {key})
        if key.startswith('Router_ScBus_'):
            values = [channels(s, o, seen | {key}) for s, o in incoming.get((key, 'Value'), [])]
            if values: return max(values)
        raise ValueError(f'Cannot infer audio channels: {folder}/{key}:{port}')

    def infer_router(inner, router, seen):
        c = json.loads((inner / 'connections.json').read_text()) or {}
        values = []
        for src, outs in c.items():
            for out, dests in (outs or {}).items():
                if router in (dests or {}):
                    data = json.loads((inner / (src + '.json')).read_text())
                    if src.startswith('SC_'): values.append(int(data['N_Chan']))
                    else: raise ValueError(f'Unsupported nested router source: {inner}/{src}')
        if not values: raise ValueError(f'Unconnected audio output: {inner}/{router}')
        return max(values)

    count = 0
    for (dst, param), sources in incoming.items():
        if len(sources) < 2: continue
        # Only audio sources. A numeric fan-in is not an audio sum.
        if not all(s.startswith(('SC_', 'Macro_', 'Router_ScBus_')) for s, _ in sources):
            raise ValueError(f'Non-audio fan-in needs review: {folder}/{dst}:{param}')
        n = max(channels(src, out) for src, out in sources)
        if len(sources) > 32: raise ValueError('AudioMerge supports at most 32 inputs')
        positions = modules.setdefault('SC AudioMerge*', {})
        index = max([int(i) for i in positions] + [0]) + 1
        key = 'SC_AudioMerge*_' + str(index)
        dst_type, dst_index = dst.rsplit('_', 1)
        xy = modules.get(dst_type.replace('_', ' '), {}).get(f'{int(dst_index):02d}', [0, 0])
        positions[f'{index:02d}'] = [xy[0] - 260, xy[1] + 100 * count]
        for i, (src, out) in enumerate(sources, 1):
            del conns[src][out][dst][param]
            if not conns[src][out][dst]: del conns[src][out][dst]
            conns[src][out].setdefault(key, {})['In' + str(i)] = None
        conns[key] = {'Out': {dst: {param: None}}}
        write(folder / (key + '.json'), {'N_Chan': str(n), 'nInputs': str(len(sources)), 'On': True, 'expanded': True})
        count += 1
    if count:
        write(connections_path, conns)
        write(modules_path, modules)
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('data', type=Path)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    data = args.data.resolve()
    pending = {}
    def stage(path, value): pending[path] = json.dumps(value, indent=4) + '\n'
    count = 0
    for tree in ('Presets', 'Macros'):
        for path in sorted((data / tree).rglob('connections.json')):
            count += migrate(path.parent, stage)
    print(f'{count} AudioMerge nodes; {len(pending)} JSON files')
    if args.apply and pending:
        backup = data / 'Supercollider/_backups' / ('audio-merge-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
        for path, text in pending.items():
            if path.exists():
                saved = backup / path.relative_to(data)
                saved.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, saved)
            path.write_text(text)
        print('Backup:', backup)

if __name__ == '__main__': main()
