#!/usr/bin/env python3
"""Cross-build the candidate FD735/GLES frontend on a Mac GitHub runner.

Does not install anything on a device. Downloaded third-party sources and SDK
headers stay in the build directory; only the library and provenance are outputs.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shlex
import subprocess
import tarfile
import urllib.request

MESA = '25.0.7'
MESA_SHA = '592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6'
FRONTEND = 'd7a74344828c87d37c64f87f237525cb65b7e5f5'
FILES = ['sweetgl.c', 'sweetgl.h', 'sweet_drm.c', 'sweet_drm.h', 'eagl.c',
         'eagl.h', 'layerdump.c', 'gen-dispatch.py', 'ios-dispatch.txt', 'include/xf86drm.h']


def run(args, cwd=None, capture=False):
    print('+', shlex.join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), cwd=cwd, check=True,
                          text=True, capture_output=capture)


def fetch(url, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(url, timeout=120) as response:
        destination.write_bytes(response.read())
    return hashlib.sha256(destination.read_bytes()).hexdigest()


def edit(path, old, new):
    text = path.read_text()
    if text.count(old) != 1:
        raise RuntimeError(f'exact source anchor changed: {path}: {old!r}')
    path.write_text(text.replace(old, new))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('work', type=Path)
    ap.add_argument('output', type=Path)
    a = ap.parse_args()
    work, output = a.work.resolve(), a.output.resolve()
    work.mkdir(parents=True, exist_ok=False)
    output.mkdir(parents=True, exist_ok=True)
    archive = work / f'mesa-{MESA}.tar.xz'
    digest = fetch(f'https://archive.mesa3d.org/mesa-{MESA}.tar.xz', archive)
    if digest != MESA_SHA:
        raise RuntimeError('Mesa release archive SHA256 mismatch')
    with tarfile.open(archive) as source:
        for member in source.getmembers():
            if member.name != f'mesa-{MESA}' and not member.name.startswith(f'mesa-{MESA}/'):
                raise RuntimeError('unexpected Mesa archive prefix')
        source.extractall(work, filter='data')
    mesa, build, front = work / f'mesa-{MESA}', work / 'build', work / 'frontend'
    provenance = {'mesa_version': MESA, 'mesa_archive_sha256': digest,
                  'frontend_commit': FRONTEND, 'frontend_sources': {},
                  'device_deployment': False, 'hardware_rendering_verified': False}
    for name in FILES:
        url = f'https://raw.githubusercontent.com/joranv1/ios-sailfish/{FRONTEND}/tools/sweetgl/{name}'
        provenance['frontend_sources'][name] = fetch(url, front / name)
    # Use the actual peridot identity. No A530 import-pitch workaround is added.
    edit(front / 'sweet_drm.c', 'p->value = 530;', 'p->value = 735;')
    edit(front / 'sweet_drm.c', 'IOServiceMatching("SweetGPU")', 'IOServiceMatching("PeridotGPU")')
    # Mesa 25 calls get_param unconditionally after creating its state tracker.
    # The surfaceless frontend owns FBO attachments explicitly and needs no
    # legacy window-system invalidation on glViewport.
    edit(front / 'sweetgl.c', 'bool\nsweetgl_init(void)\n',
         'static int\n'
         'sweetgl_frontend_param(struct pipe_frontend_screen *screen,\n'
         '    enum st_manager_param param)\n'
         '{\n'
         '\t(void)screen;\n'
         '\tswitch (param) {\n'
         '\tcase ST_MANAGER_BROKEN_INVALIDATE:\n'
         '\t\treturn 0;\n'
         '\t}\n'
         '\treturn 0;\n'
         '}\n\n'
         'bool\nsweetgl_init(void)\n')
    edit(front / 'sweetgl.c', '\t\t\t\tfscreen->screen = screen;',
         '\t\t\t\tif (fscreen == NULL) {\n'
         '\t\t\t\t\tscreen->destroy(screen);\n'
         '\t\t\t\t\tclose(fd);\n'
         '\t\t\t\t\tpthread_mutex_unlock(&init_lock);\n'
         '\t\t\t\t\treturn false;\n'
         '\t\t\t\t}\n'
         '\t\t\t\tfscreen->screen = screen;\n'
         '\t\t\t\tfscreen->get_param = sweetgl_frontend_param;')
    edit(front / 'sweetgl.c', '\tc->st = st;',
         '\tif (c == NULL) {\n'
         '\t\tst_destroy_context(st);\n'
         '\t\treturn NULL;\n'
         '\t}\n'
         '\tsweet_drm_log("sweetgl: GLES %d context created", attribs.major);\n'
         '\tc->st = st;')
    # The actual20H392 GLESContext constructor probes the legacy OES float
    # extension strings to select its extended-color image path. The
    # IOSurface frontend supports single-plane SDR storage; withhold these
    # optional extension aliases until two-plane RGB10/alpha interop exists.
    edit(front / 'eagl.c',
         '\t\tstatic char *ext;\n'
         '\t\tif (ext == NULL && asprintf(&ext, "%s GL_EXT_shader_framebuffer_fetch", s) < 0) {\n'
         '\t\t\text = NULL;\n'
         '\t\t}\n'
         '\t\treturn ext != NULL ? (const GLubyte *)ext : s;',
         '\t\tstatic char *ext;\n'
         '\t\tstatic pthread_mutex_t ext_lock = PTHREAD_MUTEX_INITIALIZER;\n'
         '\t\tpthread_mutex_lock(&ext_lock);\n'
         '\t\tif (ext == NULL) {\n'
         '\t\t\tif (asprintf(&ext, "%s GL_EXT_shader_framebuffer_fetch", s) < 0) ext = NULL;\n'
         '\t\t\tif (ext != NULL) {\n'
         '\t\t\tstatic const char *const deferred[] = {\n'
         '\t\t\t    "GL_OES_texture_half_float", "GL_OES_texture_half_float_linear",\n'
         '\t\t\t    "GL_OES_texture_float", "GL_OES_texture_float_linear"};\n'
         '\t\t\tfor (unsigned i = 0; i < sizeof(deferred) / sizeof(deferred[0]); ++i) {\n'
         '\t\t\t\tsize_t len = strlen(deferred[i]);\n'
         '\t\t\t\tchar *at = ext;\n'
         '\t\t\t\twhile ((at = strstr(at, deferred[i])) != NULL) {\n'
         '\t\t\t\t\tif ((at == ext || at[-1] == \' \') && (at[len] == \' \' || at[len] == 0))\n'
         '\t\t\t\t\t\tmemset(at, \' \', len);\n'
         '\t\t\t\t\tat += len;\n'
         '\t\t\t\t}\n'
         '\t\t\t}\n'
         '\t\t\tsweet_drm_log("sweetgl: withholding legacy float aliases for single-plane effect surfaces");\n'
         '\t\t\t}\n'
         '\t\t}\n'
         '\t\tconst GLubyte *result = ext != NULL ? (const GLubyte *)ext : s;\n'
         '\t\tpthread_mutex_unlock(&ext_lock);\n'
         '\t\treturn result;')
    caps = Path(__file__).resolve().parent / 'peridot-mesa/surface_caps.c'
    caps_source = caps.read_bytes()
    (front / 'peridot_surface_caps.c').write_bytes(caps_source)
    provenance['surface_capabilities_sha256'] = hashlib.sha256(caps_source).hexdigest()
    texture = Path(__file__).resolve().parent / 'peridot-mesa/texture_functions.c'
    texture_source = texture.read_bytes()
    (front / 'peridot_texture_functions.c').write_bytes(texture_source)
    provenance['texture_functions_sha256'] = hashlib.sha256(texture_source).hexdigest()
    formats = Path(__file__).resolve().parent / 'peridot-mesa/surface_formats.c'
    formats_source = formats.read_bytes()
    (front / 'peridot_surface_formats.c').write_bytes(formats_source)
    provenance['surface_formats_sha256'] = hashlib.sha256(formats_source).hexdigest()
    capture = Path(__file__).resolve().parent / 'peridot-mesa/surface_capture.c'
    capture_source = capture.read_bytes()
    (front / 'peridot_surface_capture.c').write_bytes(capture_source)
    provenance['surface_capture_sha256'] = hashlib.sha256(capture_source).hexdigest()
    # SpringBoard runs the capability policy before its existing layer-filter
    # adapter. Its constructor returns without making any Mesa/GPU context.
    edit(front / 'sweetgl.c', 'void sweetgl_vibrant_fill_install(void);',
         'void sweetgl_vibrant_fill_install(void);\nvoid peridot_surface_caps_install(void);\n'
         'void peridot_surface_formats_install(void);\nvoid peridot_surface_capture_install(void);')
    edit(front / 'sweetgl.c', 'sweetgl_load(void)\n{',
         'sweetgl_load(void)\n{\n\tperidot_surface_caps_install();\n'
         '\tperidot_surface_formats_install();\n\tperidot_surface_capture_install();')
    display_probe = Path(__file__).resolve().parent / 'peridot-mesa/display_probe.c'
    display_probe_source = display_probe.read_bytes()
    (front / 'peridot_display_probe.c').write_bytes(display_probe_source)
    provenance['display_probe_sha256'] = hashlib.sha256(display_probe_source).hexdigest()
    edit(front / 'sweetgl.c', '\tconst char *t = getenv("SWEETGL_TEST");',
         '\tif (!strcmp(getprogname(), "backboardd")) setenv("SWEETGL_TEST", "1", 1);\n'
         '\tconst char *t = getenv("SWEETGL_TEST");')
    edit(front / 'layerdump.c', '#include \"sweet_drm.h\"', '#include \"sweet_drm.h\"\nvoid peridot_display_probe(void);')
    edit(front / 'layerdump.c', '\tstatic int round;\n', '\tstatic int round;\n\tif (round == 0) peridot_display_probe();\n')
    provenance['layerdump_probe_sha256'] = hashlib.sha256((front / 'layerdump.c').read_bytes()).hexdigest()
    provenance['selftest_enabled_for'] = 'backboardd'
    for component, extension in [('probe_api', 'c'), ('probe_install', 'm'), ('ui_restore', 'm')]:
        local = Path(__file__).resolve().parent / ('peridot-mesa/' + component + '.' + extension)
        data = local.read_bytes()
        (front / ('peridot_' + component + '.' + extension)).write_bytes(data)
        provenance[component + '_sha256'] = hashlib.sha256(data).hexdigest()
    edit(front / 'sweetgl.c', 'void peridot_surface_capture_install(void);',
         'void peridot_surface_capture_install(void);\nvoid peridot_ui_restore(void);')
    edit(front / 'sweetgl.c', '\tperidot_surface_capture_install();',
         '\tperidot_surface_capture_install();\n\tperidot_ui_restore();')
    edit(front / 'eagl.c', '#include "eagl.h"',
         '#include "eagl.h"\nvoid peridot_texture_functions_install(void);')
    # Replace the old restore's table addresses with our exact20H392 guarded
    # expression/uniform data adapter. Diagnostic instruction hooks stay off.
    edit(front / 'eagl.c', '\tsgl_install_texfns();',
         '\tperidot_texture_functions_install();')
    edit(front / 'eagl.c', '\tif (trace_left > 0) {\n\t\tsgl_install_fmt_hook();\n\t}\n', '')
    edit(mesa / 'src/freedreno/meson.build', "if system_has_kms_drm\n  subdir('drm')\n",
         "if system_has_kms_drm or host_machine.system() == 'darwin'\n  subdir('drm')\n")
    # This build selects only MSM, with calls provided by our IOKit facade.
    # The virtual DRM library is neither available nor used on Darwin.
    edit(mesa / 'src/freedreno/drm/meson.build', 'link_with : [libvdrm],', 'link_with : [],')
    edit(mesa / 'src/freedreno/ir3/ir3_disk_cache.c',
         '   const struct build_id_note *note =\n'
         '      build_id_find_nhdr_for_addr(ir3_disk_cache_init);\n'
         '   assert(note && build_id_length(note) == 20); /* sha1 */\n\n'
         '   const uint8_t *id_sha1 = build_id_data(note);\n'
         '   assert(id_sha1);\n',
         '   struct mesa_sha1 build_id;\n'
         '   uint8_t id_sha1[20];\n'
         '   _mesa_sha1_init(&build_id);\n'
         '   if (!disk_cache_get_function_identifier(ir3_disk_cache_init, &build_id))\n'
         '      return;\n'
         '   _mesa_sha1_final(&build_id, id_sha1);\n')
    edit(mesa / 'src/gallium/drivers/freedreno/freedreno_screen.c',
         '#include <sys/sysinfo.h>', '#include "util/os_misc.h"')
    edit(mesa / 'src/gallium/drivers/freedreno/freedreno_screen.c',
         '   struct sysinfo si;\n   sysinfo(&si);\n   screen->ram_size = si.totalram;',
         '   uint64_t system_memory;\n'
         '   if (!os_get_total_physical_memory(&system_memory))\n      goto fail;\n'
         '   screen->ram_size = system_memory;')
    edit(mesa / 'src/freedreno/drm/freedreno_bo.c', '#include "freedreno_priv.h"\n',
         '#include "freedreno_priv.h"\n'
         '#ifdef __APPLE__\nvoid *sweet_drm_bo_map(int fd, uint64_t offset);\n'
         '#undef os_mmap\n#define os_mmap(addr, length, prot, flags, fd, offset) sweet_drm_bo_map(fd, offset)\n'
         '#define os_munmap(addr, length) 0\n#endif\n')
    sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], capture=True).stdout.strip()
    macsdk = run(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], capture=True).stdout.strip()
    clang = run(['xcrun', '--find', 'clang'], capture=True).stdout.strip()
    clangpp = run(['xcrun', '--find', 'clang++'], capture=True).stdout.strip()
    ar = run(['xcrun', '--find', 'ar'], capture=True).stdout.strip()
    strip = run(['xcrun', '--find', 'strip'], capture=True).stdout.strip()
    # iOS ships the IOKit linker interface, but some SDK releases omit its
    # public C declarations. Reuse only IOKit's headers, without exposing the
    # Mac frameworks directory to the iOS CoreFoundation/CoreGraphics search.
    iokit = Path(macsdk) / 'System/Library/Frameworks/IOKit.framework/Headers'
    if not (Path(sdk) / 'System/Library/Frameworks/IOKit.framework/Headers/IOKitLib.h').exists():
        (front / 'include/IOKit').symlink_to(iokit, target_is_directory=True)
    common = ['-arch', 'arm64', '-isysroot', sdk, '-miphoneos-version-min=16.0']
    cross = work / 'ios-cross.txt'
    cross.write_text('[binaries]\n' + '\n'.join(
        f'{key} = {value!r}' for key, value in {
            'c': [clang] + common, 'cpp': [clangpp] + common,
            'objc': [clang] + common, 'ar': ar, 'strip': strip, 'pkg-config': 'false'}.items()) +
        '\n[built-in options]\nc_args = ' + repr(['-I' + str(front / 'include')]) +
        '\ncpp_args = ' + repr(['-I' + str(front / 'include')]) +
        "\n[host_machine]\nsystem = 'darwin'\nsubsystem = 'ios'\ncpu_family = 'aarch64'\ncpu = 'arm64'\nendian = 'little'\n")
    options_file = mesa / 'meson_options.txt'
    names = set(re.findall(r"option\(\s*['\"]([^'\"]+)", options_file.read_text()))
    options = {'platforms': '', 'egl': 'disabled', 'glx': 'disabled', 'gbm': 'disabled',
               'gallium-drivers': 'freedreno', 'vulkan-drivers': '', 'freedreno-kmds': 'msm',
               'llvm': 'disabled', 'gles1': 'disabled', 'gles2': 'enabled', 'opengl': 'true',
               'zstd': 'disabled', 'zlib': 'disabled', 'expat': 'disabled', 'xmlconfig': 'disabled',
               'valgrind': 'disabled', 'libunwind': 'disabled', 'lmsensors': 'disabled',
               'build-tests': 'false', 'shader-cache': 'disabled', 'perfetto': 'false',
               'gallium-va': 'disabled', 'gallium-rusticl': 'false', 'tools': '',
               'html-docs': 'disabled', 'display-info': 'disabled', 'teflon': 'false'}
    required = {'gallium-drivers', 'vulkan-drivers', 'build-tests', 'freedreno-kmds'}
    if not required <= names:
        raise RuntimeError('Mesa build-option contract changed')
    run(['meson', 'setup', build, mesa, '--cross-file', cross, '--buildtype=release',
         '-Db_ndebug=true', '-Ddefault_library=static'] +
        [f'-D{k}={v}' for k, v in options.items() if k in names])
    # Build the needed archives directly, without a system GLES shared target.
    libs = ['src/mesa/libmesa.a', 'src/gallium/auxiliary/libgallium.a',
            'src/gallium/drivers/freedreno/libfreedreno.a',
            'src/gallium/winsys/freedreno/drm/libfreedrenowinsys.a',
            'src/freedreno/drm/libfreedreno_drm.a', 'src/freedreno/ir3/libfreedreno_ir3.a',
            'src/freedreno/ir2/libfreedreno_ir2.a', 'src/freedreno/fdl/libfreedreno_layout.a',
            'src/freedreno/common/libfreedreno_common.a', 'src/freedreno/isa/libir3encode.a',
            'src/freedreno/isa/libir3decode.a', 'src/compiler/isaspec/libisaspec.a',
            'src/freedreno/perfcntrs/libfreedreno_perfcntrs.a',
            'src/compiler/glsl/libglsl.a', 'src/compiler/glsl/glcpp/libglcpp.a',
            'src/compiler/nir/libnir.a', 'src/compiler/spirv/libvtn.a', 'src/compiler/libcompiler.a',
            'src/mapi/glapi/libglapi_bridge.a', 'src/mapi/shared-glapi/libglapi.a',
            'src/util/libxmlconfig.a', 'src/util/libmesa_util.a', 'src/util/libmesa_util_sse41.a',
            'src/util/libmesa_util_simd.a', 'src/util/blake3/libblake3.a',
            'src/c11/impl/libmesa_util_c11.a']
    # meson target metadata supplies the actual target paths for this pinned
    # release, including optional utility archives that changed names later.
    targets = json.loads(run(['meson', 'introspect', build, '--targets'], capture=True).stdout)
    actual = {Path(f).name: Path(f) for t in targets for f in t.get('filename', []) if f.endswith('.a')}
    selected = []
    optional = {'libmesa_util_simd.a', 'libblake3.a'}
    for lib in libs:
        name = Path(lib).name
        if name not in actual:
            if name in optional:
                continue
            raise RuntimeError(f'Mesa static target missing: {name}')
        selected.append(actual[name])
    run(['ninja', '-C', build] + [str(p.relative_to(build)) for p in selected])
    commands = json.loads((build / 'compile_commands.json').read_text())
    tracker = next(c for c in commands if c['file'].endswith('state_tracker/st_manager.c'))
    definitions = [arg for arg in shlex.split(tracker['command']) if arg.startswith('-D')]
    nm = work / 'bridge.nm'
    nm.write_text(run(['nm', '-g', actual['libglapi_bridge.a']], capture=True).stdout)
    run(['python3', front / 'gen-dispatch.py', front / 'ios-dispatch.txt',
         mesa / 'include', nm, work / 'dispatch.c'])
    includes = [front / 'include', mesa / 'include', mesa / 'src', mesa / 'src/mesa', mesa / 'src/mapi',
                mesa / 'src/gallium/include', mesa / 'src/gallium/auxiliary',
                mesa / 'src/gallium/drivers', mesa / 'src/gallium/winsys',
                build / 'src', build / 'src/mesa', build / 'src/mapi', front]
    probe_script = Path(__file__).resolve().parent / 'peridot-probe/build.py'
    spec = importlib.util.spec_from_file_location('peridot_probe_builder', probe_script)
    probe_builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe_builder)
    provenance['probe_app'] = probe_builder.build(work, output, clang, common)
    provenance['probe_builder_sha256'] = hashlib.sha256(probe_script.read_bytes()).hexdigest()
    includes.append(work)
    objects = []
    for name in ['sweetgl', 'sweet_drm', 'eagl', 'layerdump', 'dispatch',
                 'peridot_surface_caps', 'peridot_texture_functions', 'peridot_surface_formats',
                 'peridot_surface_capture', 'peridot_display_probe', 'peridot_probe_api', 'peridot_probe_install',
                 'peridot_ui_restore']:
        extension = 'm' if name in ('peridot_probe_install', 'peridot_ui_restore') else 'c'
        source = (work if name == 'dispatch' else front) / f'{name}.{extension}'
        obj = work / f'{name}.o'
        objc = ['-fobjc-arc', '-fblocks'] if extension == 'm' else []
        run([clang] + common + objc + ['-O2', '-Wall', '-fvisibility=hidden'] + definitions +
            ['-I' + str(p) for p in includes] + ['-c', source, '-o', obj])
        objects.append(obj)
    exports = work / 'exports.txt'
    exports.write_text('_sweetgl_init\n_sweetgl_create\n_sweetgl_make_current\n_sweetgl_destroy\n_sweetgl_selftest\n_sweetgl_get_proc_address\n')
    library = output / 'peridot-sweetgl.dylib'
    run([clangpp] + common + ['-dynamiclib', '-dead_strip', '-install_name', '/usr/lib/peridot-sweetgl.dylib',
        '-exported_symbols_list', exports, '-o', library] + objects + selected +
        ['-framework', 'IOKit', '-framework', 'CoreFoundation', '-framework', 'IOSurface',
         '-framework', 'OpenGLES', '-framework', 'CoreGraphics', '-framework', 'Foundation', '-lobjc', '-lc++', '-lz'])
    run(['codesign', '-f', '-s', '-', library])
    (output / 'symbols.txt').write_text(run(['nm', '-n', library], capture=True).stdout)
    provenance['frontend_revision'] = 'peridot-0167-native-widget-host-restore'
    provenance['library_sha256'] = hashlib.sha256(library.read_bytes()).hexdigest()
    provenance['compiler'] = run([clang, '--version'], capture=True).stdout
    provenance['build_dependencies'] = run(['python3', '-m', 'pip', 'freeze'], capture=True).stdout
    (output / 'build.json').write_text(json.dumps(provenance, indent=2) + '\n')
    print('Built candidate library; no device installation or hardware-rendering claim.', flush=True)


if __name__ == '__main__':
    main()
