#!/usr/bin/env python3
"""Generate the shim's vtables from every OpenVR SDK release.

Alyx reaches OpenVR through C++ interface pointers: it asks
VR_GetGenericInterface for a version string such as "IVRSystem_022" and calls
through the vtable it gets back. Which versions it asks for depends on the SDK
it was built against, which we cannot see until the engine boots. So instead of
guessing, this serves every interface version Valve ever shipped.

The source of truth is ValveSoftware/openvr's own history: each tagged release
carries headers/openvr_api.json, which lists every interface method in
declaration order (= vtable order, since the interfaces are pure virtual with
no destructor). The newest release is also cross-checked against openvr.h
itself, so a JSON that drifted from its header fails the build here rather
than as a wrong slot at runtime.

Output (checked in, so a build never needs the openvr history):
  src/generated/klvr_decls.h   one prototype per (method, signature)
  src/generated/klvr_vtables.inc  weak stubs, ABI thunks, vtables, registry

Usage: gen_vtables.py <path to a full clone of ValveSoftware/openvr>
"""
import json
import os
import re
import subprocess
import sys
from collections import OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'src', 'generated')


def git(repo, *args):
    return subprocess.run(['git', '-C', repo] + list(args), check=True,
                          capture_output=True, text=True).stdout


def tag_key(t):
    nums = [int(x) for x in re.findall(r'\d+', t)]
    return nums + [0] * (4 - len(nums))


def norm_type(t):
    t = t.replace('class ', '').replace('struct ', '').replace('enum ', '')
    t = re.sub(r'\s+', ' ', t).strip()
    t = t.replace(' *', '*').replace('* ', '*').replace(' &', '&')
    return t


def header_method_order(header_text):
    """Method names per interface, in declaration order, from openvr.h."""
    out = {}
    for m in re.finditer(r'\nclass (IVR\w+)\s*\{(.*?)\n\};', header_text, re.S):
        body = re.sub(r'//[^\n]*', '', m.group(2))
        names = re.findall(r'virtual\s[^;{]*?\b(\w+)\s*\(', body)
        out[m.group(1)] = names
    return out


def load_releases(repo):
    tags = [t for t in git(repo, 'tag').split() if t]
    releases = []
    for t in sorted(set(tags), key=tag_key):
        try:
            j = json.loads(git(repo, 'show', t + ':headers/openvr_api.json'))
        except (subprocess.CalledProcessError, json.JSONDecodeError):
            continue
        releases.append((t, j))
    return releases


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    repo = sys.argv[1]
    releases = load_releases(repo)
    if not releases:
        sys.exit('no releases with headers/openvr_api.json found in ' + repo)
    newest_tag, newest = releases[-1]

    # Types the newest header declares; anything else in an old signature is
    # lowered to an ABI-equivalent stand-in below.
    # (The JSON also lists driver-side types that openvr.h does not declare,
    # so each name is checked against the header text itself.)
    newest_hdr = git(repo, 'show', newest_tag + ':headers/openvr.h')
    known = set()
    for name in ([st['struct'] for st in newest['structs']] + [e['enumname'] for e in newest['enums']] +
                 [td['typedef'] for td in newest['typedefs']]):
        n = norm_type(name)
        bare = re.escape(n.split('::')[-1])
        if re.search(r'(?:struct|enum|class)\s+%s\b' % bare, newest_hdr) or \
           re.search(r'typedef[^;{}]*\b%s\s*;' % bare, newest_hdr):
            known.add(n)
    old_enums = set()
    for _, j in releases:
        for e in j['enums']:
            old_enums.add(norm_type(e['enumname']))

    # version string -> (class, [methods]) with the newest release winning.
    versions = OrderedDict()
    for tag, j in releases:
        cls_version = {}
        for c in j['consts']:
            m = re.match(r'(IVR\w+)_Version$', c['constname'])
            if m:
                cls_version['vr::' + m.group(1)] = c['constval']
        per_class = OrderedDict()
        for meth in j['methods']:
            per_class.setdefault(meth['classname'], []).append(meth)
        for cls, meths in per_class.items():
            # A virtual destructor is two Itanium vtable slots but one JSON
            # entry, so its layout cannot be derived here. Only the IPC
            # resource manager has one, and nothing in a game calls it.
            if any(m['methodname'].startswith('Destruct') for m in meths):
                continue
            ver = cls_version.get(cls)
            if not ver or not re.match(r'IVR\w+_\d{3}$', ver):
                continue
            versions[ver] = (cls.replace('vr::', ''), meths, tag)

    # Cross-check the newest JSON against the newest header.
    hdr = header_method_order(git(repo, 'show', newest_tag + ':headers/openvr.h'))
    per_class = OrderedDict()
    for meth in newest['methods']:
        per_class.setdefault(meth['classname'].replace('vr::', ''), []).append(meth['methodname'])
    for cls, names in per_class.items():
        if any(n.startswith('Destruct') for n in names):
            continue
        if cls in hdr and hdr[cls] != names:
            sys.exit('openvr_api.json and openvr.h disagree on %s slot order in %s'
                     % (cls, newest_tag))

    def lower(t):
        """A type the newest header can express, or an ABI-equal stand-in."""
        n = norm_type(t)
        base = n.replace('const ', '').rstrip('*&').strip()
        ptr = n.endswith('*') or n.endswith('&')
        if base in ('void', 'bool', 'float', 'double', 'char', 'int', 'uint32_t',
                    'int32_t', 'uint64_t', 'int64_t', 'uint8_t', 'uint16_t',
                    'int16_t', 'unsigned short', 'unsigned int', 'size_t',
                    'unsigned char', 'unsigned long long'):
            return n
        if base in known:
            return n
        if ptr:
            return 'void*' if 'const' not in n else 'const void*'
        if base in old_enums:
            return 'int32_t'
        return None   # a by-value struct the newest header no longer has

    # Parameters narrower than 32 bits. Apple's arm64 ABI has the CALLER extend
    # these to 32 bits and lets the callee rely on it; AAPCS64 (Linux) leaves
    # the upper bits unspecified. Alyx is the Linux caller, so each one arrives
    # through a thunk that takes it as uint32_t and truncates explicitly.
    NARROW = {'bool', 'char', 'uint8_t', 'int8_t', 'uint16_t', 'int16_t',
              'unsigned short', 'unsigned char', 'short'}

    def sig_of(meth):
        ret = lower(meth['returntype'])
        ps = [lower(p['paramtype']) for p in meth.get('params', [])]
        if ret is None or any(p is None for p in ps):
            return None
        return ret, ps

    # (class, method) -> OrderedDict(signature -> newest version using it)
    sigs = OrderedDict()
    for ver, (cls, meths, tag) in versions.items():
        for meth in meths:
            s = sig_of(meth)
            key = (cls, meth['methodname'])
            if s is None:
                continue
            sk = (s[0], tuple(s[1]))
            d = sigs.setdefault(key, OrderedDict())
            d[sk] = ver   # later (newer) releases overwrite: newest wins

    # The newest signature of a method gets the plain name; older ones a suffix.
    names = {}
    newest_ver_of = {}
    for ver, (cls, meths, tag) in versions.items():
        newest_ver_of[cls] = ver
    for (cls, mname), d in sigs.items():
        items = list(d.items())
        newest_sig = items[-1][0]
        for sk, ver in items:
            if sk == newest_sig:
                names[(cls, mname, sk)] = '%s__%s' % (cls, mname)
            else:
                names[(cls, mname, sk)] = '%s__%s__v%s' % (cls, mname, ver.rsplit('_', 1)[1])

    decls, stubs, thunks, tables, registry = [], [], [], [], []
    emitted = set()
    for (cls, mname, sk), fn in sorted(names.items(), key=lambda kv: kv[1]):
        ret, ps = sk
        params = ', '.join(['void *self'] + ['%s a%d' % (p, i) for i, p in enumerate(ps)])
        decls.append('%s %s(%s);' % (ret, fn, params))
        # An error-enum return gets a non-zero value (every OpenVR error enum
        # uses 0 for success), so an unimplemented call reads as a failure
        # rather than as success with its out-parameters never written.
        if ret == 'void':
            tail = 'return;'
        elif 'Error' in ret and '*' not in ret:
            tail = 'return (%s)1;' % ret
        else:
            tail = 'return {};'
        body = 'klvr_unimplemented("%s", "%s"); %s' % (cls, fn.split('__', 1)[1], tail)
        stubs.append('KLVR_WEAK %s %s(%s) { %s }' % (ret, fn, params, body))
        if any(norm_type(p) in NARROW for p in ps):
            tps = ', '.join(['void *self'] + [('uint32_t a%d' % i) if norm_type(p) in NARROW
                                              else '%s a%d' % (p, i) for i, p in enumerate(ps)])
            def narrow_arg(p, i):
                p = norm_type(p)
                if p == 'bool':
                    return '((a%d & 0xff) != 0)' % i
                if p in ('char', 'uint8_t', 'int8_t', 'unsigned char'):
                    return '(%s)(a%d & 0xff)' % (p, i)
                if p in NARROW:
                    return '(%s)(a%d & 0xffff)' % (p, i)
                return 'a%d' % i
            args = ', '.join(['self'] + [narrow_arg(p, i) for i, p in enumerate(ps)])
            thunks.append('static %s thunk_%s(%s) { return %s(%s); }' % (ret, fn, tps, fn, args))

    for ver, (cls, meths, tag) in versions.items():
        slots = []
        for meth in meths:
            s = sig_of(meth)
            if s is None:
                slots.append('(const void *)&klvr_unrepresentable_slot /* %s::%s */' % (ver, meth['methodname']))
                continue
            fn = names[(cls, meth['methodname'], (s[0], tuple(s[1])))]
            has_thunk = any(norm_type(p) in NARROW for p in s[1])
            slots.append('(const void *)&%s%s' % ('thunk_' if has_thunk else '', fn))
        tables.append('static const void *const vt_%s[] = {\n    %s\n};' % (ver, ',\n    '.join(slots)))
        registry.append('    { "%s", "%s", vt_%s, %d, "%s" },' % (ver, cls, ver, len(meths), tag))

    os.makedirs(OUT, exist_ok=True)
    hdr_txt = ('// GENERATED by tools/gen_vtables.py from ValveSoftware/openvr %s..%s. Do not edit.\n'
               '#pragma once\n#include "openvr.h"\n#include <stdint.h>\n'
               'namespace klvr { namespace impl {\nusing namespace vr;\n%s\n}}\n'
               % (releases[0][0], newest_tag, '\n'.join(decls)))
    inc_txt = ('// GENERATED by tools/gen_vtables.py from ValveSoftware/openvr %s..%s. Do not edit.\n'
               '// Included once, by klvr_registry.cpp.\n'
               'namespace klvr { namespace impl {\nusing namespace vr;\n'
               '// Weak defaults: a method nobody implemented logs its name and returns zero.\n%s\n\n'
               '// ABI thunks for parameters narrower than 32 bits (see gen_vtables.py).\n%s\n\n%s\n}}\n\n'
               'const klvr::InterfaceVersion klvr::kInterfaceVersions[] = {\n%s\n};\n'
               'const size_t klvr::kInterfaceVersionCount = sizeof(klvr::kInterfaceVersions) / sizeof(klvr::kInterfaceVersions[0]);\n'
               % (releases[0][0], newest_tag, '\n'.join(stubs), '\n'.join(thunks), '\n'.join(tables),
                  '\n'.join(r.replace('vt_', 'klvr::impl::vt_') for r in registry)))
    with open(os.path.join(OUT, 'klvr_decls.h'), 'w') as f:
        f.write(hdr_txt)
    with open(os.path.join(OUT, 'klvr_vtables.inc'), 'w') as f:
        f.write(inc_txt)
    print('%d releases (%s..%s), %d interface versions, %d prototypes, %d thunks'
          % (len(releases), releases[0][0], newest_tag, len(versions), len(decls), len(thunks)))


if __name__ == '__main__':
    main()
