"""Build/sign the restoration helper; embed only our own executable bytes."""
import hashlib
from pathlib import Path
import plistlib
import struct
import subprocess

def signed_xml_entitlements(data):
    if len(data) < 32 or data[:4] != b'\xcf\xfa\xed\xfe':
        raise RuntimeError('Expected a thin arm64 Mach-O helper')
    at = 32
    signature = None
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, size = struct.unpack_from('<II', data, at)
        if size < 8 or at + size > len(data): raise RuntimeError('Invalid Mach-O command')
        if command == 0x1d:
            offset, length = struct.unpack_from('<II', data, at + 8)
            if offset + length > len(data): raise RuntimeError('Invalid signature range')
            signature = data[offset:offset + length]
        at += size
    if signature is None or len(signature) < 12: raise RuntimeError('Missing code signature')
    magic, length, count = struct.unpack_from('>III', signature)
    if magic != 0xfade0cc0 or length > len(signature) or 12 + count * 8 > length:
        raise RuntimeError('Invalid signature superblob')
    for index in range(count):
        slot, offset = struct.unpack_from('>II', signature, 12 + index * 8)
        if slot != 5: continue
        if offset + 8 > length: raise RuntimeError('Invalid XML entitlement offset')
        blob_magic, blob_length = struct.unpack_from('>II', signature, offset)
        if blob_magic != 0xfade7171 or blob_length < 8 or offset + blob_length > length:
            raise RuntimeError('Invalid XML entitlement blob')
        return plistlib.loads(signature[offset + 8:offset + blob_length])
    raise RuntimeError('Kernel requires an XML entitlement slot; DER alone is unsupported')

def build(work, output, clang, common):
    source = Path(__file__).with_name('ui_restore_worker.m')
    binary = output / 'xlamiOS-restore'
    entitlements = {
        'platform-application': True,
        'com.apple.private.coreservices.canmaplsdatabase': True,
        'com.apple.lsapplicationworkspace.rebuildappdatabases': True,
        'com.apple.private.security.storage.AppBundles': True,
        'com.apple.private.security.storage-exempt.heritable': True,
        'com.apple.private.MobileInstallationHelperService.allowed': True,
        'com.apple.private.MobileInstallationHelperService.InstallDaemonOpsEnabled': True,
        'com.apple.springboard.launchapplications': True,
        'com.apple.backboardd.launchapplications': True,
        'com.apple.frontboard.launchapplications': True,
        'com.apple.multitasking.termination': True,
        'com.apple.private.mobileinstall.allowedSPI': [
            'InstallForLaunchServices',
            'Install',
            'UninstallForLaunchServices',
            'Uninstall',
            'UpdatePlaceholderMetadata',
        ],
        'com.apple.springboard.wallpaper-access': True,
        'com.apple.private.security.no-container': True,
    }
    ent = work / 'ui-restore.entitlements'
    ent.write_bytes(plistlib.dumps(entitlements))
    subprocess.run([clang, *common, '-O2', '-Wall', '-fobjc-arc', '-fblocks', str(source),
                    '-framework', 'Foundation', '-o', str(binary)], check=True)
    subprocess.run(['codesign', '-f', '-s', '-', '-i', 'com.xlamios.restore',
                    '--entitlements', str(ent), str(binary)], check=True)
    data = binary.read_bytes()
    signed = signed_xml_entitlements(data)
    if any(signed.get(key) != value for key, value in entitlements.items()):
        raise RuntimeError('Signed XML entitlements do not match the helper policy')
    digest = hashlib.sha256(data).hexdigest()
    (work / 'ui_restore_payload.h').write_text('#include <stddef.h>\n'
        'static const char ui_restore_filename[] = "xlamiOS-restore-' + digest[:16] + '";\n'
        'static const unsigned char ui_restore_payload[] = {' + ','.join(map(str, data)) + '};\n')
    return dict(version=185, source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                binary_sha256=digest, bytes=len(data), entitlements=signed, xml_entitlements_verified=True)
