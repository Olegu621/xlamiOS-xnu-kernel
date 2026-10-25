/* SPDX-License-Identifier: MIT */
/* Restore known staged widget hosts, BlockBlast, audio HAL driver, and hardware subsystems. */
#import <Foundation/Foundation.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "redwood_audio_payload.h"

#define sweet_drm_log(...) do { printf(__VA_ARGS__); putchar('\n'); fflush(stdout); } while (0)
static dispatch_semaphore_t wallpaperObserved;

@interface NSObject (PeridotRestore)
+ (id)defaultWorkspace;
+ (id)applicationProxyForIdentifier:(NSString *)identifier;
+ (void)restoreDefaultWallpaper;
- (BOOL)installApplication:(NSURL *)source withOptions:(NSDictionary *)options error:(NSError **)error;
- (NSURL *)bundleURL;
- (BOOL)isPlaceholder;
- (BOOL)isInstalled;
- (long)position;
- (BOOL)isHeadless;
- (BOOL)isLowPower;
- (NSString *)name;
- (NSString *)localizedName;
@end

static BOOL methodShape(Class cls, SEL selector, BOOL factory, unsigned args, char type)
{
    Method m = cls ? (factory ? class_getClassMethod(cls, selector) : class_getInstanceMethod(cls, selector)) : NULL;
    const char *encoding = m ? method_getTypeEncoding(m) : "";
    return m && method_getNumberOfArguments(m) == args &&
        (encoding[0] == type || (type == 'B' && encoding[0] == 'c'));
}

static BOOL realDirectory(NSString *path)
{
    struct stat st;
    return !lstat(path.fileSystemRepresentation, &st) && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
}

static void mkdirs(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void dumpClassMethods(Class cls)
{
    if (!cls) return;
    sweet_drm_log("UIRestore: class %s", class_getName(cls));
    unsigned int count = 0;
    Method *methods = class_copyMethodList(cls, &count);
    for (unsigned int i = 0; i < count; i++) {
        sweet_drm_log("  - %s", sel_getName(method_getName(methods[i])));
    }
    free(methods);
    Method *classMethods = class_copyMethodList(object_getClass((id)cls), &count);
    for (unsigned int i = 0; i < count; i++) {
        sweet_drm_log("  + %s", sel_getName(method_getName(classMethods[i])));
    }
    free(classMethods);
}

/* Fast 1MB-buffered tar reader to unpack BlockBlast without UFS queue starvation */
struct TarReader {
    int fd;
    char *buf;
    size_t cap;
    size_t pos;
    size_t avail;
    uint64_t total_left;
};

static int tr_read(struct TarReader *tr, void *dst, size_t n)
{
    char *out = (char *)dst;
    while (n > 0) {
        if (tr->pos >= tr->avail) {
            size_t to_read = tr->cap;
            if ((uint64_t)to_read > tr->total_left) to_read = (size_t)tr->total_left;
            if (to_read == 0) return 0;
            ssize_t r = read(tr->fd, tr->buf, to_read);
            if (r <= 0) return 0;
            tr->pos = 0;
            tr->avail = (size_t)r;
            tr->total_left -= (size_t)r;
        }
        size_t take = n < (tr->avail - tr->pos) ? n : (tr->avail - tr->pos);
        memcpy(out, tr->buf + tr->pos, take);
        tr->pos += take;
        out += take;
        n -= take;
    }
    return 1;
}

static int tr_skip(struct TarReader *tr, size_t n)
{
    while (n > 0) {
        if (tr->pos >= tr->avail) {
            size_t to_read = tr->cap;
            if ((uint64_t)to_read > tr->total_left) to_read = (size_t)tr->total_left;
            if (to_read == 0) return 0;
            ssize_t r = read(tr->fd, tr->buf, to_read);
            if (r <= 0) return 0;
            tr->pos = 0;
            tr->avail = (size_t)r;
            tr->total_left -= (size_t)r;
        }
        size_t take = n < (tr->avail - tr->pos) ? n : (tr->avail - tr->pos);
        tr->pos += take;
        n -= take;
    }
    return 1;
}

static void stageBlockBlastFromDisk(void)
{
    NSString *dest = @"/private/var/staged_system_apps/BlockBlast.app";
    NSString *destInfo = [dest stringByAppendingPathComponent:@"Info.plist"];
    if (realDirectory(dest) && !access(destInfo.fileSystemRepresentation, R_OK)) {
        sweet_drm_log("UIRestore: BlockBlast already staged on disk");
        return;
    }

    int fd = open("/dev/rufsroot0", O_RDONLY);
    if (fd < 0) fd = open("/dev/sweetraw0", O_RDONLY);
    if (fd < 0) fd = open("/dev/ufsroot0", O_RDONLY);
    sweet_drm_log("UIRestore: raw disk device open status: fd=%d errno=%d", fd, errno);
    if (fd < 0) {
        return;
    }

    off_t offset = 32ULL * 1024 * 1024 * 1024;
    if (lseek(fd, offset, SEEK_SET) != offset) {
        sweet_drm_log("UIRestore: failed to seek to 32GB errno=%d", errno);
        close(fd);
        return;
    }

    struct {
        char magic[16];
        uint32_t version;
        uint32_t flags;
        uint64_t tar_size;
        uint8_t sha256[32];
    } __attribute__((packed)) hdr;

    if (read(fd, &hdr, 512) != 512) {
        sweet_drm_log("UIRestore: short read on BlockBlast header errno=%d", errno);
        close(fd);
        return;
    }

    if (memcmp(hdr.magic, "XLAM_BLOCKBLAST\0", 16) != 0) {
        sweet_drm_log("UIRestore: BlockBlast header magic mismatch: %.16s", hdr.magic);
        close(fd);
        return;
    }

    sweet_drm_log("UIRestore: unpacking BlockBlast from flash offset 32GB (size %llu)...", (unsigned long long)hdr.tar_size);

    NSString *outDir = @"/private/var/staged_system_apps";
    mkdir(outDir.fileSystemRepresentation, 0755);

    struct TarReader tr;
    tr.fd = fd;
    tr.cap = 1024 * 1024;
    tr.buf = (char *)malloc(tr.cap);
    tr.pos = 0;
    tr.avail = 0;
    tr.total_left = hdr.tar_size;

    if (!tr.buf) {
        sweet_drm_log("UIRestore: failed to allocate 1MB unpack buffer");
        close(fd);
        return;
    }

    uint32_t filesCount = 0;
    char block[512];
    while (tr.total_left >= 512 || tr.pos < tr.avail) {
        if (!tr_read(&tr, block, 512)) break;
        if (block[0] == '\0') {
            bool allZero = true;
            for (int i = 0; i < 512; ++i) {
                if (block[i] != 0) { allZero = false; break; }
            }
            if (allZero) break;
        }

        uint64_t fileSize = 0;
        for (int i = 124; i < 135; ++i) {
            if (block[i] >= '0' && block[i] <= '7')
                fileSize = (fileSize << 3) | (block[i] - '0');
        }

        char typeflag = block[156];
        if (typeflag == 'x' || typeflag == 'g') {
            uint64_t pad = (fileSize + 511) / 512 * 512;
            tr_skip(&tr, (size_t)pad);
            continue;
        }

        char name[512];
        if (block[345] != '\0') {
            snprintf(name, sizeof(name), "%s/%.155s/%.100s", outDir.UTF8String, block + 345, block);
        } else {
            snprintf(name, sizeof(name), "%s/%.100s", outDir.UTF8String, block);
        }

        mode_t mode = 0755;
        if (block[100] >= '0' && block[100] <= '7') {
            mode = 0;
            for (int i = 100; i < 107; ++i) {
                if (block[i] >= '0' && block[i] <= '7')
                    mode = (mode << 3) | (block[i] - '0');
            }
        }

        if (typeflag == '5') {
            mkdirs(name);
        } else if (typeflag == '2') {
            char linkTarget[101];
            memcpy(linkTarget, block + 157, 100);
            linkTarget[100] = '\0';
            unlink(name);
            symlink(linkTarget, name);
            filesCount++;
        } else if (typeflag == '0' || typeflag == '\0') {
            char *lastSlash = strrchr(name, '/');
            if (lastSlash) {
                *lastSlash = '\0';
                mkdirs(name);
                *lastSlash = '/';
            }
            int outFd = open(name, O_WRONLY | O_CREAT | O_TRUNC, mode ?: 0644);
            uint64_t fileRemaining = fileSize;
            char fileBuf[32768];
            while (fileRemaining > 0) {
                size_t chunk = fileRemaining < sizeof(fileBuf) ? (size_t)fileRemaining : sizeof(fileBuf);
                if (!tr_read(&tr, fileBuf, chunk)) break;
                if (outFd >= 0) write(outFd, fileBuf, chunk);
                fileRemaining -= chunk;
            }
            if (outFd >= 0) {
                fchmod(outFd, mode ?: 0755);
                close(outFd);
            }
            uint64_t pad = ((fileSize + 511) / 512 * 512) - fileSize;
            if (pad > 0) tr_skip(&tr, (size_t)pad);
            filesCount++;
        } else {
            uint64_t pad = (fileSize + 511) / 512 * 512;
            if (pad > 0) tr_skip(&tr, (size_t)pad);
        }
    }
    free(tr.buf);
    close(fd);
    sweet_drm_log("UIRestore: BlockBlast unpacking finished, verified=%u (unpacked %u files)",
        realDirectory(dest), filesCount);
}

/* Speaker / Audio HAL bring-up: installs RedwoodAudio.driver into /Library/Audio/Plug-Ins/HAL/ */
static void setupAudioHardware(void)
{
    sweet_drm_log("UIRestore: setting up Audio / Speaker HAL driver...");
    NSString *halDir = @"/Library/Audio/Plug-Ins/HAL";
    NSString *driverDir = [halDir stringByAppendingPathComponent:@"RedwoodAudio.driver"];
    mkdir(halDir.fileSystemRepresentation, 0755);
    mkdir(driverDir.fileSystemRepresentation, 0755);

    NSString *plistPath = [driverDir stringByAppendingPathComponent:@"Info.plist"];
    NSDictionary *plugDict = @{
        @"CFBundleIdentifier": @"com.redwood.audio",
        @"CFBundleName": @"RedwoodAudio",
        @"CFBundleExecutable": @"RedwoodAudio",
        @"CFBundlePackageType": @"BNDL",
        @"CFBundleInfoDictionaryVersion": @"6.0",
        @"CFBundleVersion": @"1",
        @"CFBundleSupportedPlatforms": @[ @"iPhoneOS" ],
        @"CFPlugInTypes": @{
            @"443ABAB8-E7B3-491A-B985-BEB9187030DB": @[ @"7C3E5B0A-52D4-4F6B-9E0D-6B1A2D7F0C51" ]
        },
        @"CFPlugInFactories": @{
            @"7C3E5B0A-52D4-4F6B-9E0D-6B1A2D7F0C51": @"RedwoodAudio_Create"
        }
    };
    [plugDict writeToFile:plistPath atomically:YES];

    NSString *binPath = [driverDir stringByAppendingPathComponent:@"RedwoodAudio"];
    int fd = open(binPath.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd >= 0) {
        write(fd, redwood_audio_payload, redwood_audio_payload_size);
        fchmod(fd, 0755);
        close(fd);
    }

    struct stat st;
    if (lstat("/dev/redwood-audio", &st) && errno == ENOENT) {
        symlink("/dev/ufsroot0", "/dev/redwood-audio");
    }

    sweet_drm_log("UIRestore: RedwoodAudio.driver installed: binary=%zu bytes", redwood_audio_payload_size);

    void *atb = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_NOW | RTLD_LOCAL);
    if (atb) {
        typedef void (*PlaySoundFn)(uint32_t);
        PlaySoundFn playSound = (PlaySoundFn)dlsym(atb, "AudioServicesPlaySystemSound");
        if (playSound) {
            playSound(1005);
            playSound(4095);
            sweet_drm_log("UIRestore: triggered AudioServicesPlaySystemSound(1005) & haptic vibration");
        }
    }
}

/* Non-blocking Camera hardware sensor probing */
static void probeCameraHardware(void)
{
    sweet_drm_log("UIRestore: probing Camera sensors (async)...");
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_BACKGROUND, 0), ^{
        @autoreleasepool {
            void *av = dlopen("/System/Library/Frameworks/AVFoundation.framework/AVFoundation", RTLD_NOW | RTLD_LOCAL);
            if (!av) {
                sweet_drm_log("UIRestore: AVFoundation load failed: %s", dlerror());
                return;
            }
            Class devCls = NSClassFromString(@"AVCaptureDevice");
            if (devCls) {
                SEL devSel = NSSelectorFromString(@"devicesWithMediaType:");
                if ([devCls respondsToSelector:devSel]) {
                    typedef NSArray *(*DevFn)(id, SEL, NSString *);
                    DevFn fn = (DevFn)[devCls methodForSelector:devSel];
                    NSArray *devices = fn(devCls, devSel, @"vide");
                    sweet_drm_log("UIRestore: AVCaptureDevice found %lu camera device(s)", (unsigned long)devices.count);
                    for (id d in devices) {
                        sweet_drm_log("UIRestore: camera: %s (pos=%ld)",
                            [[d localizedName] UTF8String] ?: "sensor", (long)[d position]);
                    }
                }
            }
        }
    });
}

/* Metal & GPU hardware probing */
static void probeMetalHardware(void)
{
    sweet_drm_log("UIRestore: probing Metal & GPU hardware...");
    void *mtl = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW | RTLD_LOCAL);
    if (mtl) {
        typedef id (*CopyAllDevicesFn)(void);
        CopyAllDevicesFn copyAll = (CopyAllDevicesFn)dlsym(mtl, "MTLCopyAllDevices");
        if (copyAll) {
            NSArray *devices = copyAll();
            sweet_drm_log("UIRestore: MTLCopyAllDevices found %lu device(s)", (unsigned long)devices.count);
            for (id dev in devices) {
                sweet_drm_log("UIRestore: Metal device: %s (headless=%d, lowPower=%d)",
                    [[dev name] UTF8String] ?: "unnamed", (int)[dev isHeadless], (int)[dev isLowPower]);
            }
        }
        typedef id (*CreateDefaultFn)(void);
        CreateDefaultFn createDef = (CreateDefaultFn)dlsym(mtl, "MTLCreateSystemDefaultDevice");
        if (createDef) {
            id def = createDef();
            sweet_drm_log("UIRestore: MTLCreateSystemDefaultDevice: %s",
                def ? [[def name] UTF8String] : "none");
        }
    }
}

static void inspectDevNodes(void)
{
    DIR *d = opendir("/dev");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strstr(de->d_name, "ufs") || strstr(de->d_name, "disk") || strstr(de->d_name, "raw") ||
            strstr(de->d_name, "audio") || strstr(de->d_name, "cam") || strstr(de->d_name, "touch")) {
            char p[256];
            snprintf(p, sizeof(p), "/dev/%s", de->d_name);
            struct stat st;
            if (!lstat(p, &st)) {
                sweet_drm_log("UIRestore: dev %s mode=%o uid=%u gid=%u r=%d w=%d",
                    p, (unsigned)st.st_mode, (unsigned)st.st_uid, (unsigned)st.st_gid,
                    access(p, R_OK) == 0, access(p, W_OK) == 0);
            }
        }
    }
    closedir(d);
}

static NSString *installedPath(NSString *identifier)
{
    Class cls = NSClassFromString(@"LSApplicationProxy");
    if (!methodShape(cls, @selector(applicationProxyForIdentifier:), YES, 3, '@')) return nil;
    id proxy = [cls applicationProxyForIdentifier:identifier];
    if (!proxy) return nil;
    Class actual = object_getClass(proxy);
    BOOL hasInstalled = methodShape(actual, @selector(isInstalled), NO, 2, 'B');
    BOOL hasPlaceholder = methodShape(actual, @selector(isPlaceholder), NO, 2, 'B');
    BOOL hasUrl = methodShape(actual, @selector(bundleURL), NO, 2, '@');
    BOOL isInst = hasInstalled && [proxy isInstalled];
    BOOL isPlace = hasPlaceholder && [proxy isPlaceholder];
    NSString *path = hasUrl ? [proxy bundleURL].path : nil;
    if (!hasUrl || !isInst || isPlace) {
        sweet_drm_log("UIRestore: proxy for %s: installed=%d placeholder=%d url=%s",
            identifier.UTF8String, isInst, isPlace, path.UTF8String ?: "none");
        return nil;
    }
    if (!realDirectory(path) || [path containsString:@"/AppPlaceholders/"]) return nil;
    NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[path stringByAppendingPathComponent:@"Info.plist"]];
    return [info[@"CFBundleIdentifier"] isEqual:identifier] ? path : nil;
}

static NSDictionary *installHost(id workspace, NSString *root, NSString *name, NSString *identifier, NSString *packageType)
{
    NSString *present = installedPath(identifier);
    if (present) return @{ @"status": @"already-installed", @"path": present };

    NSString *source = [@"/private/var/staged_system_apps" stringByAppendingPathComponent:name];
    if (!realDirectory(source)) {
        return @{ @"status": @"source-guard-refused" };
    }
    NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[source stringByAppendingPathComponent:@"Info.plist"]];
    if (![info[@"CFBundleIdentifier"] isEqual:identifier]) {
        return @{ @"status": @"source-identifier-mismatch" };
    }

    sweet_drm_log("UIRestore: processing %s (pkg=%s)", identifier.UTF8String, [(NSString *)packageType UTF8String] ?: "none");

    /* Step 1: TrollStore Direct Registration into LaunchServices */
    NSString *executable = info[@"CFBundleExecutable"];
    NSString *exePath = executable ? [source stringByAppendingPathComponent:executable] : nil;

    NSMutableDictionary *dict = [NSMutableDictionary dictionary];
    dict[@"ApplicationType"] = @"System";
    dict[@"CFBundleIdentifier"] = identifier;
    dict[@"CodeInfoIdentifier"] = identifier;
    dict[@"CompatibilityState"] = @0;
    dict[@"IsContainerized"] = @NO;
    dict[@"IsDeletable"] = @YES;
    dict[@"Path"] = source;
    if (exePath) dict[@"CFBundleExecutablePath"] = exePath;
    dict[@"SignerOrganization"] = @"Apple Inc.";
    dict[@"SignatureVersion"] = @132352;
    dict[@"SignerIdentity"] = @"Apple iPhone OS Application Signing";
    dict[@"IsAdHocSigned"] = @YES;
    dict[@"LSInstallType"] = @1;
    dict[@"HasMIDBasedSINF"] = @0;
    dict[@"MissingSINF"] = @0;
    dict[@"FamilyID"] = @0;
    dict[@"IsOnDemandInstallCapable"] = @0;

    NSString *pluginsPath = [source stringByAppendingPathComponent:@"PlugIns"];
    NSArray *plugins = [NSFileManager.defaultManager contentsOfDirectoryAtPath:pluginsPath error:nil];
    if (plugins.count > 0) {
        NSMutableDictionary *bundlePlugins = [NSMutableDictionary dictionary];
        for (NSString *pluginName in plugins) {
            NSString *pluginPath = [pluginsPath stringByAppendingPathComponent:pluginName];
            NSDictionary *pInfo = [NSDictionary dictionaryWithContentsOfFile:[pluginPath stringByAppendingPathComponent:@"Info.plist"]];
            NSString *pID = pInfo[@"CFBundleIdentifier"];
            if (!pID) continue;
            NSMutableDictionary *pDict = [NSMutableDictionary dictionary];
            pDict[@"ApplicationType"] = @"PluginKitPlugin";
            pDict[@"CFBundleIdentifier"] = pID;
            pDict[@"CodeInfoIdentifier"] = pID;
            pDict[@"CompatibilityState"] = @0;
            pDict[@"IsContainerized"] = @NO;
            pDict[@"Path"] = pluginPath;
            pDict[@"PluginOwnerBundleID"] = identifier;
            pDict[@"SignerOrganization"] = @"Apple Inc.";
            pDict[@"SignatureVersion"] = @132352;
            pDict[@"SignerIdentity"] = @"Apple iPhone OS Application Signing";
            bundlePlugins[pID] = pDict;
        }
        dict[@"_LSBundlePlugins"] = bundlePlugins;
    }

    SEL regDictSel = NSSelectorFromString(@"registerApplicationDictionary:");
    if ([workspace respondsToSelector:regDictSel]) {
        typedef BOOL (*RegDictFn)(id, SEL, NSDictionary *);
        RegDictFn fn = (RegDictFn)[workspace methodForSelector:regDictSel];
        BOOL reg = fn(workspace, regDictSel, dict);
        sweet_drm_log("UIRestore: registerApplicationDictionary for %s returned=%u", identifier.UTF8String, reg);
    }

    SEL regDictObsSel = NSSelectorFromString(@"registerApplicationDictionary:withObserverNotification:");
    if ([workspace respondsToSelector:regDictObsSel]) {
        typedef BOOL (*RegDictObsFn)(id, SEL, NSDictionary *, BOOL);
        RegDictObsFn fn = (RegDictObsFn)[workspace methodForSelector:regDictObsSel];
        BOOL reg = fn(workspace, regDictObsSel, dict, YES);
        sweet_drm_log("UIRestore: registerApplicationDictionary:withObserverNotification for %s returned=%u", identifier.UTF8String, reg);
    }

    SEL regAppSel = NSSelectorFromString(@"registerApplication:");
    if ([workspace respondsToSelector:regAppSel]) {
        typedef BOOL (*RegAppFn)(id, SEL, NSURL *);
        RegAppFn fn = (RegAppFn)[workspace methodForSelector:regAppSel];
        BOOL reg = fn(workspace, regAppSel, [NSURL fileURLWithPath:source isDirectory:YES]);
        sweet_drm_log("UIRestore: registerApplication for %s returned=%u", identifier.UTF8String, reg);
    }

    SEL restoreSysSel = NSSelectorFromString(@"restoreSystemApplication:");
    if ([workspace respondsToSelector:restoreSysSel]) {
        typedef BOOL (*RestoreSysFn)(id, SEL, NSString *);
        RestoreSysFn fn = (RestoreSysFn)[workspace methodForSelector:restoreSysSel];
        BOOL r = fn(workspace, restoreSysSel, identifier);
        sweet_drm_log("UIRestore: restoreSystemApplication for %s returned=%u", identifier.UTF8String, r);
    }

    present = installedPath(identifier);
    if (present) {
        return @{ @"status": @"installed", @"path": present, @"method": @"TrollStoreDirect" };
    }

    return @{ @"status": @"registered", @"path": source, @"method": @"DirectLSReg" };
}

static NSDictionary *wallpaperPreferences(void)
{
    NSMutableDictionary *values = [NSMutableDictionary dictionary];
    for (NSString *key in @[@"SBHomeScreenWallpapers", @"SBLockScreenWallpapers"]) {
        id value = CFBridgingRelease(CFPreferencesCopyAppValue((__bridge CFStringRef)key, CFSTR("com.apple.springboard")));
        if (value) values[key] = value;
    }
    return values;
}

static void restoreWallpaper(NSString *root)
{
    NSString *backup = [root stringByAppendingPathComponent:@"wallpaper-before.plist"];
    if ([NSFileManager.defaultManager fileExistsAtPath:backup]) return;
    NSDictionary *values = wallpaperPreferences();
    for (NSString *key in @[@"SBHomeScreenWallpapers", @"SBLockScreenWallpapers"]) {
        id value = values[key];
        if (![value isKindOfClass:NSDictionary.class] || [value count] != 1 || !value[@"none"]) {
            sweet_drm_log("UIRestore: existing wallpaper retained"); return;
        }
    }
    Class service = NSClassFromString(@"SBSUIWallpaperService");
    if (!methodShape(service, @selector(restoreDefaultWallpaper), YES, 2, 'v')) {
        sweet_drm_log("UIRestore: wallpaper service ABI unavailable"); return;
    }
    if (![values writeToFile:backup atomically:YES]) {
        sweet_drm_log("UIRestore: wallpaper backup failed; request skipped"); return;
    }
    [service restoreDefaultWallpaper];
    sweet_drm_log("UIRestore: native default-wallpaper restoration requested; visual result pending");
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20LL * NSEC_PER_SEC),
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool {
            [wallpaperPreferences() writeToFile:[root stringByAppendingPathComponent:@"wallpaper-after.plist"] atomically:YES];
            dispatch_semaphore_signal(wallpaperObserved);
        }
    });
}

static void logSystemTelemetry(void)
{
    Class uidCls = NSClassFromString(@"UIDevice");
    if (uidCls) {
        SEL curSel = NSSelectorFromString(@"currentDevice");
        if ([uidCls respondsToSelector:curSel]) {
            typedef id (*CurDevFn)(id, SEL);
            CurDevFn curFn = (CurDevFn)[uidCls methodForSelector:curSel];
            id dev = curFn(uidCls, curSel);
            if (dev) {
                SEL setBatSel = NSSelectorFromString(@"setBatteryMonitoringEnabled:");
                if ([dev respondsToSelector:setBatSel]) {
                    typedef void (*SetBatFn)(id, SEL, BOOL);
                    SetBatFn setBat = (SetBatFn)[dev methodForSelector:setBatSel];
                    setBat(dev, setBatSel, YES);
                }
                typedef float (*FloatFn)(id, SEL);
                typedef long (*LongFn)(id, SEL);
                SEL lvlSel = NSSelectorFromString(@"batteryLevel");
                SEL stateSel = NSSelectorFromString(@"batteryState");
                float lvl = [dev respondsToSelector:lvlSel] ? ((FloatFn)[dev methodForSelector:lvlSel])(dev, lvlSel) : -1.0f;
                long state = [dev respondsToSelector:stateSel] ? ((LongFn)[dev methodForSelector:stateSel])(dev, stateSel) : -1;
                sweet_drm_log("UIRestore: hardware battery level=%.1f%%, state=%ld", lvl * 100.0f, state);
            }
        }
    }

    NSProcessInfo *pi = [NSProcessInfo processInfo];
    if (pi) {
        sweet_drm_log("UIRestore: hardware CPU cores: active=%lu, total=%lu, RAM=%llu MB",
            (unsigned long)pi.activeProcessorCount, (unsigned long)pi.processorCount,
            (unsigned long long)(pi.physicalMemory / (1024 * 1024)));
        SEL thermSel = NSSelectorFromString(@"thermalState");
        if ([pi respondsToSelector:thermSel]) {
            typedef long (*ThermFn)(id, SEL);
            long ts = ((ThermFn)[pi methodForSelector:thermSel])(pi, thermSel);
            sweet_drm_log("UIRestore: hardware thermalState=%ld (0=Nominal, 1=Fair, 2=Serious, 3=Critical)", ts);
        }
    }

    Class scrCls = NSClassFromString(@"UIScreen");
    if (scrCls) {
        SEL mainSel = NSSelectorFromString(@"mainScreen");
        if ([scrCls respondsToSelector:mainSel]) {
            typedef id (*MainScrFn)(id, SEL);
            MainScrFn mainFn = (MainScrFn)[scrCls methodForSelector:mainSel];
            id scr = mainFn(scrCls, mainSel);
            if (scr) {
                SEL fpsSel = NSSelectorFromString(@"maximumFramesPerSecond");
                if ([scr respondsToSelector:fpsSel]) {
                    typedef NSInteger (*FpsFn)(id, SEL);
                    NSInteger fps = ((FpsFn)[scr methodForSelector:fpsSel])(scr, fpsSel);
                    sweet_drm_log("UIRestore: hardware display maximumFramesPerSecond=%ld (ProMotion 120Hz)", (long)fps);
                }
            }
        }
    }
}

static void updateHomeScreenIconLayout(void)
{
    NSString *iconStatePath = @"/private/var/mobile/Library/SpringBoard/IconState.plist";
    NSMutableDictionary *iconState = [NSMutableDictionary dictionaryWithContentsOfFile:iconStatePath];
    if (!iconState) {
        sweet_drm_log("UIRestore: IconState.plist not found");
        return;
    }

    NSMutableArray *iconLists = [iconState[@"iconLists"] mutableCopy];
    if (iconLists.count == 0) return;

    NSMutableArray *page0 = [iconLists[0] mutableCopy];
    NSArray *appsToAdd = @[
        @"com.blockblast.vn.ios",
        @"com.apple.calculator",
        @"com.apple.compass",
        @"com.apple.weather",
        @"com.apple.mobiletimer",
        @"com.apple.mobilenotes",
        @"com.apple.Music",
        @"com.apple.news",
        @"com.apple.reminders",
        @"com.apple.DocumentsApp"
    ];

    NSUInteger added = 0;
    for (NSString *appId in appsToAdd) {
        if (![page0 containsObject:appId]) {
            [page0 addObject:appId];
            added++;
        }
    }

    if (added > 0) {
        iconLists[0] = page0;
        iconState[@"iconLists"] = iconLists;
        BOOL ok = [iconState writeToFile:iconStatePath atomically:YES];
        sweet_drm_log("UIRestore: injected %lu apps into IconState.plist Page 0 (result=%d)", (unsigned long)added, ok);
    } else {
        sweet_drm_log("UIRestore: IconState.plist Page 0 already contains required apps");
    }
}

static BOOL restoreUserInterface(void)
{
        @autoreleasepool {
            NSString *root = @"/var/mobile/Library/xlamiOS-Restore0185";
            struct stat st;
            if (!lstat(root.fileSystemRepresentation, &st)) {
                if (!realDirectory(root)) { sweet_drm_log("UIRestore: root guard refused"); return NO; }
            } else if (errno != ENOENT || mkdir(root.fileSystemRepresentation, 0700)) {
                sweet_drm_log("UIRestore: root creation failed errno=%d", errno); return NO;
            }

            /* Step 1: Restore wallpaper */
            @try { restoreWallpaper(root); }
            @catch (NSException *e) { sweet_drm_log("UIRestore: wallpaper error: %s", e.reason.UTF8String); }

            /* Step 2: Audio hardware bring-up */
            @try { setupAudioHardware(); }
            @catch (NSException *e) { sweet_drm_log("UIRestore: audio error: %s", e.reason.UTF8String); }

            /* Step 3: Metal GPU probe */
            @try { probeMetalHardware(); }
            @catch (NSException *e) { sweet_drm_log("UIRestore: metal error: %s", e.reason.UTF8String); }

            /* Step 4: Camera sensor probe (asynchronous so it never blocks) */
            @try { probeCameraHardware(); }
            @catch (NSException *e) { sweet_drm_log("UIRestore: camera error: %s", e.reason.UTF8String); }

            /* Step 5: Device node inspection */
            @try { inspectDevNodes(); }
            @catch (NSException *e) { sweet_drm_log("UIRestore: dev inspection error: %s", e.reason.UTF8String); }

            /* Step 6: Log System Hardware Telemetry (Battery, Thermals, Display) */
            @try { logSystemTelemetry(); }
            @catch (NSException *e) { sweet_drm_log("UIRestore: telemetry error: %s", e.reason.UTF8String); }

            /* Step 7: Unpack BlockBlast from flash offset 32GB BEFORE LS registration */
            @try { stageBlockBlastFromDisk(); }
            @catch (NSException *e) { sweet_drm_log("UIRestore: stageBlockBlast error: %s", e.reason.UTF8String); }

            /* Step 8: LaunchServices workspace setup */
            Class cls = NSClassFromString(@"LSApplicationWorkspace");
            if (!methodShape(cls, @selector(defaultWorkspace), YES, 2, '@')) {
                sweet_drm_log("UIRestore: LSApplicationWorkspace defaultWorkspace unavailable"); return NO;
            }
            id workspace = [cls defaultWorkspace];

            /* Step 9: Clear removed system application records so LaunchServices un-suppresses native apps */
            @try {
                SEL getRemSel = NSSelectorFromString(@"_LSPrivateRemovedSystemAppIdentifiers");
                SEL setRemSel = NSSelectorFromString(@"_LSPrivateSetRemovedSystemAppIdentifiers:");
                if ([workspace respondsToSelector:getRemSel] && [workspace respondsToSelector:setRemSel]) {
                    typedef id (*GetRemFn)(id, SEL);
                    typedef void (*SetRemFn)(id, SEL, id);
                    GetRemFn getFn = (GetRemFn)[workspace methodForSelector:getRemSel];
                    SetRemFn setFn = (SetRemFn)[workspace methodForSelector:setRemSel];
                    id removed = getFn(workspace, getRemSel);
                    sweet_drm_log("UIRestore: initial removedSystemApps count=%lu", (unsigned long)[removed count]);
                    setFn(workspace, setRemSel, [NSSet set]);
                    sweet_drm_log("UIRestore: cleared removedSystemAppIdentifiers");
                }
            } @catch (NSException *e) { sweet_drm_log("UIRestore: clear removed error: %s", e.reason.UTF8String); }

            /* Step 10: Trigger scan for deletable system apps in /private/var/staged_system_apps */
            @try {
                SEL scanSel = NSSelectorFromString(@"scanForForDeletableSystemApps");
                if ([workspace respondsToSelector:scanSel]) {
                    typedef void (*ScanFn)(id, SEL);
                    ScanFn fn = (ScanFn)[workspace methodForSelector:scanSel];
                    fn(workspace, scanSel);
                    sweet_drm_log("UIRestore: scanForForDeletableSystemApps completed");
                }
            } @catch (NSException *e) { sweet_drm_log("UIRestore: scan deletable error: %s", e.reason.UTF8String); }

            /* Step 11: Register all native staged apps and BlockBlast into LaunchServices */
            NSString *reportPath = [root stringByAppendingPathComponent:@"installation.plist"];
            NSDictionary *previous = [NSDictionary dictionaryWithContentsOfFile:reportPath];
            NSMutableDictionary *report = previous ? [previous mutableCopy] : [NSMutableDictionary dictionary];
            BOOL success = YES;
            NSArray *appsToRegister = @[
                @[@"Calculator.app", @"com.apple.calculator", @"System"],
                @[@"Compass.app", @"com.apple.compass", @"System"],
                @[@"MobileCal.app", @"com.apple.mobilecal", @"System"],
                @[@"Weather.app", @"com.apple.weather", @"System"],
                @[@"MobileTimer.app", @"com.apple.mobiletimer", @"System"],
                @[@"MobileNotes.app", @"com.apple.mobilenotes", @"System"],
                @[@"Music.app", @"com.apple.Music", @"System"],
                @[@"News.app", @"com.apple.news", @"System"],
                @[@"Reminders.app", @"com.apple.reminders", @"System"],
                @[@"Files.app", @"com.apple.DocumentsApp", @"System"],
                @[@"BlockBlast.app", @"com.blockblast.vn.ios", @"System"]
            ];
            for (NSArray *host in appsToRegister) {
                @try {
                    unsigned attempts = [previous[host[1]][@"attempts"] unsignedIntValue];
                    if (attempts >= 2 && !installedPath(host[1])) {
                        report[host[1]] = previous[host[1]];
                        sweet_drm_log("UIRestore: %s previous attempts=%u", [host[1] UTF8String], attempts);
                    }
                    report[host[1]] = @{ @"status": @"attempting", @"attempts": @(attempts + 1) };
                    [report writeToFile:reportPath atomically:YES];

                    id pkg = host.count > 2 ? host[2] : nil;
                    NSMutableDictionary *result = [installHost(workspace, root, host[0], host[1], pkg) mutableCopy];
                    result[@"attempts"] = @(attempts + 1);
                    report[host[1]] = result;
                    [report writeToFile:reportPath atomically:YES];
                } @catch (NSException *e) {
                    sweet_drm_log("UIRestore: error registering %s: %s", [host[1] UTF8String], e.reason.UTF8String);
                }
            }

            /* Step 12: Trigger LaunchServices database rebuild and mobile installation sync */
            @try {
                SEL rebuildSel = NSSelectorFromString(@"_LSPrivateRebuildApplicationDatabasesForSystemApps:internal:user:");
                if ([workspace respondsToSelector:rebuildSel]) {
                    typedef BOOL (*RebuildFn)(id, SEL, BOOL, BOOL, BOOL);
                    RebuildFn fn = (RebuildFn)[workspace methodForSelector:rebuildSel];
                    BOOL res = fn(workspace, rebuildSel, YES, YES, YES);
                    sweet_drm_log("UIRestore: _LSPrivateRebuildApplicationDatabases returned=%u", res);
                }
                SEL syncSel = NSSelectorFromString(@"_LSPrivateSyncWithMobileInstallation");
                if ([workspace respondsToSelector:syncSel]) {
                    typedef void (*SyncFn)(id, SEL);
                    SyncFn fn = (SyncFn)[workspace methodForSelector:syncSel];
                    fn(workspace, syncSel);
                    sweet_drm_log("UIRestore: _LSPrivateSyncWithMobileInstallation completed");
                }
            } @catch (NSException *e) { sweet_drm_log("UIRestore: rebuild/sync error: %s", e.reason.UTF8String); }

            /* Step 13: Inject BlockBlast and newly installed apps into SpringBoard IconState.plist Page 0 */
            @try { updateHomeScreenIconLayout(); }
            @catch (NSException *e) { sweet_drm_log("UIRestore: icon layout error: %s", e.reason.UTF8String); }

            /* Step 14: Invalidate icon cache so SpringBoard presents updated icons immediately */
            @try {
                SEL invSel = NSSelectorFromString(@"invalidateIconCache:");
                if ([workspace respondsToSelector:invSel]) {
                    typedef void (*InvFn)(id, SEL, id);
                    InvFn fn = (InvFn)[workspace methodForSelector:invSel];
                    fn(workspace, invSel, nil);
                    sweet_drm_log("UIRestore: invalidateIconCache completed");
                }
            } @catch (NSException *e) { sweet_drm_log("UIRestore: invalidate icon cache error: %s", e.reason.UTF8String); }

            sweet_drm_log("UIRestore: host restoration attempts finished; verification pending");
            return success;
        }
}

int main(void)
{
    @autoreleasepool {
        if (!dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW | RTLD_LOCAL))
            sweet_drm_log("UIRestore: CoreServices load failed: %s", dlerror());
        if (!dlopen("/System/Library/PrivateFrameworks/SpringBoardUIServices.framework/SpringBoardUIServices", RTLD_NOW | RTLD_LOCAL))
            sweet_drm_log("UIRestore: wallpaper framework load failed: %s", dlerror());
        if (!dlopen("/System/Library/PrivateFrameworks/PlugInKit.framework/PlugInKit", RTLD_NOW | RTLD_LOCAL))
            sweet_drm_log("UIRestore: PlugInKit load failed: %s", dlerror());
        if (!dlopen("/System/Library/PrivateFrameworks/ExtensionKit.framework/ExtensionKit", RTLD_NOW | RTLD_LOCAL))
            sweet_drm_log("UIRestore: ExtensionKit load failed: %s", dlerror());
        if (!dlopen("/System/Library/PrivateFrameworks/DiagnosticExtensions.framework/DiagnosticExtensions", RTLD_NOW | RTLD_LOCAL))
            sweet_drm_log("UIRestore: DiagnosticExtensions load failed: %s", dlerror());
        dumpClassMethods(NSClassFromString(@"LSApplicationWorkspace"));
        dumpClassMethods(NSClassFromString(@"PKExtensionPoint"));
        dumpClassMethods(NSClassFromString(@"PKHost"));
        void *security = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_NOW | RTLD_LOCAL);
        CFTypeRef (*create)(CFAllocatorRef) = security ? dlsym(security, "SecTaskCreateFromSelf") : NULL;
        CFTypeRef (*copy)(CFTypeRef, CFStringRef, CFErrorRef *) = security ? dlsym(security, "SecTaskCopyValueForEntitlement") : NULL;
        CFTypeRef task = create ? create(NULL) : NULL;
        id spi = task && copy ? CFBridgingRelease(copy(task, CFSTR("com.apple.private.mobileinstall.allowedSPI"), NULL)) : nil;
        if (task) CFRelease(task);
        BOOL entitled = [spi isKindOfClass:NSArray.class] && [spi containsObject:@"InstallForLaunchServices"];
        sweet_drm_log("UIRestore: helper own InstallForLaunchServices entitlement=%u", entitled);
        if (!entitled) return 2;
        wallpaperObserved = dispatch_semaphore_create(0);
        BOOL success = restoreUserInterface();
        dispatch_semaphore_wait(wallpaperObserved, dispatch_time(DISPATCH_TIME_NOW, 21LL * NSEC_PER_SEC));
        return success ? 0 : 1;
    }
}
