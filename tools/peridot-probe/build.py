#!/usr/bin/env python3
"""Build a pinned MIT ES3-book sample and generate an embedded app manifest."""
import hashlib
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import urllib.request
import zipfile

BOOK = '423d936e31f5240fb893fcc8ae5549c70efaa2bf'
SOURCES = ['Common/Include/esUtil.h', 'Common/Include/esUtil_win.h',
    'Common/Source/esUtil.c', 'Common/Source/esShader.c',
    'Common/Source/iOS/FileWrapper.h', 'Common/Source/iOS/FileWrapper.m',
    'Chapter_14/ParticleSystem/ParticleSystem.c', 'Chapter_14/ParticleSystem/smoke.tga', 'LICENSE']

def build(work, output, clang, common):
    source = work / 'probe-source'; source.mkdir()
    app = output / 'GLTest.app'; app.mkdir()
    provenance = {'repository': 'https://github.com/danginsburg/opengles3-book', 'commit': BOOK, 'files': {}}
    for name in SOURCES:
        data = urllib.request.urlopen('https://raw.githubusercontent.com/danginsburg/opengles3-book/' + BOOK + '/' + name, timeout=60).read()
        (source / Path(name).name).write_bytes(data)
        provenance['files'][name] = hashlib.sha256(data).hexdigest()
    main = Path(__file__).with_name('main.m')
    shutil.copyfile(main, source / 'main.m')
    provenance['harness_sha256'] = hashlib.sha256(main.read_bytes()).hexdigest()
    # Preserve the book's particle algorithm; GLES3 uses VBO-backed attributes.
    p = source / 'ParticleSystem.c'; s = p.read_text()
    anchor = '   // Load the vertex attributes\n'
    assert s.count(anchor) == 1
    s = s.replace(anchor, '''   static GLuint probeVBO;
   if (!probeVBO) {
      glGenBuffers(1, &probeVBO);
      glBindBuffer(GL_ARRAY_BUFFER, probeVBO);
      glBufferData(GL_ARRAY_BUFFER, sizeof(userData->particleData), userData->particleData, GL_STATIC_DRAW);
   }
   glBindBuffer(GL_ARRAY_BUFFER, probeVBO);
''' + anchor)
    for old, new in [('                           userData->particleData );', '                           (void *)0 );'),
                     ('                           &userData->particleData[1] );', '                           (void *)(sizeof(GLfloat)) );'),
                     ('                           &userData->particleData[4] );', '                           (void *)(4 * sizeof(GLfloat)) );')]:
        assert s.count(old) == 1, old
        s = s.replace(old, new)
    p.write_text(s)
    util = source / 'esUtil.c'
    s = util.read_text(); assert s.count('vsprintf ( buf, formatStr, params );') == 1
    util.write_text(s.replace('vsprintf ( buf, formatStr, params );', 'vsnprintf ( buf, sizeof(buf), formatStr, params );'))
    names = sorted(set(re.findall(r'\b(gl[A-Z]\w*)\s*\(', '\n'.join(
        (source / name).read_text() for name in ['main.m', 'ParticleSystem.c', 'esUtil.c', 'esShader.c']))))
    header = '#pragma once\n#include <OpenGLES/ES3/gl.h>\n#include <stdbool.h>\n'
    header += '\n'.join('extern __typeof__(&%s) pg_%s;' % (n, n) for n in names) + '\n'
    header += '\n'.join('#define %s pg_%s' % (n, n) for n in names) + '\n'
    header += 'bool peridotGLLoad(void *(*lookup)(const char *));\n'
    (source / 'PeridotGL.h').write_text(header)
    loader = '#include "PeridotGL.h"\n'
    loader += '\n'.join('__typeof__(pg_%s) pg_%s;' % (n, n) for n in names) + '\n'
    loader += 'bool peridotGLLoad(void *(*lookup)(const char *)) {\n'
    loader += '\n'.join('pg_%s = (__typeof__(pg_%s))lookup("%s"); if (!pg_%s) return false;' % (n,n,n,n) for n in names)
    loader += '\nreturn true; }\n'
    (source / 'PeridotGL.c').write_text(loader)
    objects = []
    for name in ['main.m', 'esUtil.c', 'esShader.c', 'FileWrapper.m', 'ParticleSystem.c', 'PeridotGL.c']:
        obj = source / (name + '.o')
        flags = ['-fobjc-arc', '-fblocks'] if name.endswith('.m') else []
        subprocess.run([clang, *common, '-O2', '-Wall', *flags, '-I' + str(source),
            '-include', str(source / 'PeridotGL.h'), '-c', str(source / name), '-o', str(obj)], check=True)
        objects.append(obj)
    subprocess.run([clang, *common, '-o', str(app / 'GLProbe'), *map(str, objects),
        '-framework', 'UIKit', '-framework', 'Foundation', '-framework', 'QuartzCore', '-framework', 'CoreGraphics'], check=True)
    info = dict(CFBundleIdentifier='com.xlamios.es3probe', CFBundleExecutable='GLProbe',
        CFBundleName='GLTest', CFBundleDisplayName='GL Test', CFBundlePackageType='APPL',
        CFBundleVersion='166', CFBundleShortVersionString='1.0', MinimumOSVersion='16.0',
        CFBundleSupportedPlatforms=['iPhoneOS'], LSRequiresIPhoneOS=True, UILaunchScreen={},
        UISupportedInterfaceOrientations=['UIInterfaceOrientationPortrait'], UIRequiredDeviceCapabilities=['arm64'])
    (app / 'Info.plist').write_bytes(plistlib.dumps(info, fmt=plistlib.FMT_BINARY))
    for name in ['smoke.tga', 'LICENSE']: shutil.copyfile(source / name, app / name)
    ent = source / 'probe.entitlements'
    ent.write_bytes(plistlib.dumps({'com.apple.private.security.no-container': True}))
    subprocess.run(['codesign', '-f', '-s', '-', '--entitlements', str(ent), str(app)], check=True)
    files = sorted(p for p in app.rglob('*') if p.is_file())
    provenance['app_file_sha256'] = {p.relative_to(app).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    (output / 'probe-build.json').write_text(json.dumps(provenance, indent=2) + '\n')
    with zipfile.ZipFile(output / 'GLTest.ipa', 'w', zipfile.ZIP_DEFLATED) as archive:
        for p in files: archive.write(p, 'Payload/GLTest.app/' + p.relative_to(app).as_posix())
    manifest = '#include <stddef.h>\nstruct probe_file { const char *name; const unsigned char *data; size_t size; unsigned mode; };\n'
    entries = []
    for index, p in enumerate(files):
        data = p.read_bytes(); symbol = 'probe_file_%d' % index
        manifest += 'static const unsigned char %s[] = {%s};\n' % (symbol, ','.join(map(str, data)))
        entries.append('{"%s",%s,sizeof(%s),%s}' % (p.relative_to(app).as_posix(), symbol, symbol, '0755' if p.name == 'GLProbe' else '0644'))
    manifest += 'static const struct probe_file probe_files[] = {' + ','.join(entries) + '};\n'
    (work / 'probe_payload.h').write_text(manifest)
    return provenance
