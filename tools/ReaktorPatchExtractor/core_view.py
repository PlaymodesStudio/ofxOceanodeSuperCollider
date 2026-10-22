#!/usr/bin/env python3
"""Readable views of a decoded Reaktor Core cell from an extractor JSON report.

Macro ports are named through property 9 of the inner pin modules (the port
index: not the module order). Primitive pins keep their raw numbers: inputs
are 1, 2... (1000, 1001... on variadic modules such as Add/Multiply/Merge),
outputs 1 (Router: 1 = true, 0 = false). qbusN are a structure's QuickBuses,
SR the sample-rate clock, #value a quick constant.

  core_view.py report.json --list                      # Core cells in the report
  core_view.py report.json --cell 29 --tree [--depth N]
  core_view.py report.json --cell 29 --net root/m27/m11 # one structure as a netlist
"""
import argparse, json, sys

def struct_name(m):
    s = m.get('inner_structure')
    if s and s.get('properties', {}).get('name'):
        return s['properties']['name']
    return m.get('name') or m.get('properties', {}).get('name') or ''

def label(m):
    n = struct_name(m)
    t = m['type_name']
    return f"{t} '{n}'" if n else t

def count(s):
    return sum(1 + (count(m['inner_structure']) if m.get('inner_structure') else 0) for m in s['modules'])

def find(s, path):
    if s['path'] == path:
        return s
    for m in s['modules']:
        if m.get('inner_structure'):
            if m['path'] == path:
                return m['inner_structure']
            r = find(m['inner_structure'], path)
            if r: return r
    return None

def tree(s, depth, maxd, out):
    for m in s['modules']:
        if m.get('inner_structure'):
            out.append('  ' * depth + f"{struct_name(m) or '?'}  [{m['path']}] {count(m['inner_structure'])}")
            if depth < maxd:
                tree(m['inner_structure'], depth + 1, maxd, out)

# Known property ids per Core module type (others print as pN=value).
PROPERTY_NAMES = {
    'Exp Approx': {1: 'order', 2: 'base'},   # output = base^x, polynomial order
    'Log Approx': {1: 'order', 2: 'base'},
    'Array': {1: 'size'},
}

def props(m):
    names = PROPERTY_NAMES.get(m['type_name'], {})
    vals = []
    for p in m.get('properties', {}).get('properties', []):
        pid = p['property_id']
        if pid in names or p.get('value_type') == 'float64' or (p.get('value_type') == 'integer' and pid not in (2, 4, 5, 9)):
            vals.append(f"{names.get(pid, f'p{pid}')}={p.get('value')}")
    return ' '.join(vals)

def prop(m, pid):
    for p in m.get('properties', {}).get('properties', []):
        if p['property_id'] == pid:
            return p.get('value')
    return None

def macro_pin(m, pin, side):
    """Name of a macro's port. Port order is property 9 of the inner pin module,
    not the module order or the flavour index."""
    inner = m.get('inner_structure')
    if not inner:
        return str(pin)
    want = 'input' if side == 'target' else 'output'
    for pm in inner['modules']:
        if pm.get('flavor') == want and prop(pm, 9) == pin:
            return prop(pm, 3) or pm.get('properties', {}).get('name') or f'#{pin}'
    return f'?{pin}'

def mname(m, i):
    if m['type_name'].startswith(('Macro Input Pin', 'Macro Output Pin', 'R5X Module')):
        return f"<{m.get('properties', {}).get('name') or 'pin'}{prop(m, 9) if prop(m, 9) is not None else ''}>"
    n = struct_name(m)
    return f"m{i}" + (f"'{n}'" if n else f"({m['type_name']})")

def endpoint(e, s, side='source'):
    k = e['kind']
    if k == 'module_pin':
        i = e['module_index']; m = s['modules'][i]
        if m['type_name'].startswith(('Macro Input Pin', 'Macro Output Pin', 'R5X Module')):
            return mname(m, i)
        return f"{mname(m, i)}.{macro_pin(m, e['pin_index'], side)}"
    if k == 'quick_constant':
        return f"#{e['value']:.9g}"
    if k == 'legacy_or_builtin_bus':
        w = e['raw_words']
        if w[:2] == [1, 1]: return f"qbus{w[2]}"
        if len(w) > 3 and w[3] == 1381171456: return "SR"
        return f"bus{w}"
    return f"{k}{e.get('raw_words')}"

def net(s):
    out = [f"== {s['properties'].get('name','')} [{s['path']}]"]
    for i, m in enumerate(s['modules']):
        line = f"  m{i:<3} {label(m)}"
        if m.get('inner_structure'):
            line += f"  ({count(m['inner_structure'])} modules)"
        pv = props(m)
        if pv: line += '  ' + pv
        out.append(line)
    out.append('  -- wires (src -> dst)')
    for c in s['connections']:
        out.append(f"  {endpoint(c['source'], s, 'source')} -> {endpoint(c['target'], s, 'target')}")
    return '\n'.join(out)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('report'); ap.add_argument('--cell', type=int)
    ap.add_argument('--list', action='store_true'); ap.add_argument('--tree', action='store_true')
    ap.add_argument('--depth', type=int, default=99); ap.add_argument('--net')
    a = ap.parse_args()
    r = json.load(open(a.report))
    cells = {c['object_id']: c for c in r['core_cells']}
    if a.list:
        for oid, c in cells.items():
            s = c['core_document'].get('structure')
            print(oid, c['hierarchy_path'], s['properties'].get('name') if s else '-', count(s) if s else 0)
        return
    root = cells[a.cell]['core_document']['structure']
    if a.tree:
        out = []; tree(root, 0, a.depth, out); print('\n'.join(out))
    if a.net:
        s = find(root, a.net)
        if not s: sys.exit(f'no structure {a.net}')
        print(net(s))

if __name__ == '__main__':
    main()
