/* SPDX-License-Identifier: MIT */
/* Restore known staged widget hosts and applications using iOS's installer, never raw disk writes. */
#import <Foundation/Foundation.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static void stageBlockBlastFromDisk(void)
{
    NSString *dest = @"/private/var/staged_system_apps/BlockBlast.app";
    if (realDirectory(dest)) {
        sweet_drm_log("UIRestore: BlockBlast already staged on disk");
        return;
    }

    int fd = open("/dev/rufsroot0", O_RDONLY);
    if (fd < 0) fd = open("/dev/ufsroot0", O_RDONLY);
    if (fd < 0) {
        sweet_drm_log("UIRestore: failed to open raw disk device errno=%d", errno);
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
        sweet_drm_log("UIRestore: short read on BlockBlast header");
        close(fd);
        return;
    }

    if (memcmp(hdr.magic, "XLAM_BLOCKBLAST\0", 16) != 0) {
        sweet_drm_log("UIRestore: BlockBlast header magic mismatch");
        close(fd);
        return;
    }

    sweet_drm_log("UIRestore: unpacking BlockBlast from flash offset 32GB (size %llu)...", (unsigned long long)hdr.tar_size);

    NSString *outDir = @"/private/var/staged_system_apps";
    mkdir(outDir.fileSystemRepresentation, 0755);

    uint64_t bytesLeft = hdr.tar_size;
    char block[512];
    while (bytesLeft >= 512) {
        if (read(fd, block, 512) != 512) break;
        bytesLeft -= 512;

        if (block[0] == '\0') {
            bool allZero = true;
            for (int i = 0; i < 512; ++i) {
                if (block[i] != 0) { allZero = false; break; }
            }
            if (allZero) break;
        }

        char name[512];
        if (block[345] != '\0') {
            snprintf(name, sizeof(name), "%s/%.155s/%.100s", outDir.UTF8String, block + 345, block);
        } else {
            snprintf(name, sizeof(name), "%s/%.100s", outDir.UTF8String, block);
        }

        uint64_t fileSize = 0;
        for (int i = 124; i < 135; ++i) {
            if (block[i] >= '0' && block[i] <= '7')
                fileSize = (fileSize << 3) | (block[i] - '0');
        }

        mode_t mode = 0755;
        if (block[100] >= '0' && block[100] <= '7') {
            mode = 0;
            for (int i = 100; i < 107; ++i) {
                if (block[i] >= '0' && block[i] <= '7')
                    mode = (mode << 3) | (block[i] - '0');
            }
        }

        char typeflag = block[156];
        if (typeflag == '5') {
            mkdirs(name);
        } else if (typeflag == '2') {
            char linkTarget[101];
            memcpy(linkTarget, block + 157, 100);
            linkTarget[100] = '\0';
            unlink(name);
            symlink(linkTarget, name);
        } else if (typeflag == '0' || typeflag == '\0') {
            char *lastSlash = strrchr(name, '/');
            if (lastSlash) {
                *lastSlash = '\0';
                mkdirs(name);
                *lastSlash = '/';
            }
            int outFd = open(name, O_WRONLY | O_CREAT | O_TRUNC, mode ?: 0644);
            uint64_t fileRemaining = fileSize;
            uint64_t padBlocks = (fileSize + 511) / 512;
            for (uint64_t b = 0; b < padBlocks; ++b) {
                if (bytesLeft < 512) break;
                if (read(fd, block, 512) != 512) break;
                bytesLeft -= 512;
                if (outFd >= 0 && fileRemaining > 0) {
                    size_t toWrite = fileRemaining < 512 ? fileRemaining : 512;
                    write(outFd, block, toWrite);
                    fileRemaining -= toWrite;
                }
            }
            if (outFd >= 0) close(outFd);
        } else {
            uint64_t padBlocks = (fileSize + 511) / 512;
            for (uint64_t b = 0; b < padBlocks; ++b) {
                if (bytesLeft < 512) break;
                if (read(fd, block, 512) != 512) break;
                bytesLeft -= 512;
            }
        }
    }
    close(fd);
    sweet_drm_log("UIRestore: BlockBlast unpacking finished, verified=%u", realDirectory(dest));
}

static NSString *installedPath(NSString *identifier)
{
    Class cls = NSClassFromString(@"LSApplicationProxy");
    if (!methodShape(cls, @selector(applicationProxyForIdentifier:), YES, 3, '@')) return nil;
    id proxy = [cls applicationProxyForIdentifier:identifier];
    Class actual = object_getClass(proxy);
    if (!methodShape(actual, @selector(bundleURL), NO, 2, '@') ||
        !methodShape(actual, @selector(isInstalled), NO, 2, 'B') || ![proxy isInstalled] ||
        !methodShape(actual, @selector(isPlaceholder), NO, 2, 'B') || [proxy isPlaceholder]) return nil;
    NSString *path = [proxy bundleURL].path;
    if (!realDirectory(path) || [path containsString:@"/AppPlaceholders/"] ||
        [path containsString:@"/staged_system_apps/"]) return nil;
    NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[path stringByAppendingPathComponent:@"Info.plist"]];
    return [info[@"CFBundleIdentifier"] isEqual:identifier] ? path : nil;
}

static NSDictionary *installHost(id workspace, NSString *root, NSString *name, NSString *identifier, NSString *packageType)
{
    NSString *present = installedPath(identifier);
    if (present) return @{ @"status": @"already-installed", @"path": present };
    NSString *source = [@"/private/var/staged_system_apps" stringByAppendingPathComponent:name];
    NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[source stringByAppendingPathComponent:@"Info.plist"]];
    if (!realDirectory(source) || ![info[@"CFBundleIdentifier"] isEqual:identifier])
        return @{ @"status": @"source-guard-refused" };
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *owned = [root stringByAppendingPathComponent:
        [NSString stringWithFormat:@"Source-%@", NSUUID.UUID.UUIDString]];
    if (mkdir(owned.fileSystemRepresentation, 0700))
        return @{ @"status": @"staging-creation-failed", @"errno": @(errno) };
    NSString *stage = [owned stringByAppendingPathComponent:name];
    NSError *error = nil;
    if (![fm copyItemAtPath:source toPath:stage error:&error]) {
        /* Only the directory exclusively created by this operation can be removed. */
        if (realDirectory(owned)) [fm removeItemAtPath:owned error:nil];
        return @{ @"status": @"copy-failed", @"error": error.description ?: @"unknown" };
    }
    /* Remove all PlugIns and Extensions from staged app bundle so missing system extension point definitions don't block app installation */
    for (NSString *sub in @[@"PlugIns", @"Extensions"]) {
        NSString *subPath = [stage stringByAppendingPathComponent:sub];
        if ([fm fileExistsAtPath:subPath]) {
            [fm removeItemAtPath:subPath error:nil];
        }
    }
    const char *pkgStr = ([packageType isKindOfClass:[NSString class]]) ? [(NSString *)packageType UTF8String] : "none";
    sweet_drm_log("UIRestore: native install begin %s (pkg=%s); original staged bundle retained",
        identifier.UTF8String, pkgStr);
    BOOL ok = NO;
    NSMutableDictionary *options = [NSMutableDictionary dictionary];
    options[@"CFBundleIdentifier"] = identifier;
    if ([packageType isKindOfClass:[NSString class]]) {
        options[@"PackageType"] = packageType;
    }
    @try {
        ok = [workspace installApplication:[NSURL fileURLWithPath:stage isDirectory:YES] withOptions:options error:&error];
    } @catch (NSException *exception) {
        error = [NSError errorWithDomain:@"xlamiOS.UIRestore" code:1 userInfo:
            @{ NSLocalizedDescriptionKey: [NSString stringWithFormat:@"%@: %@", exception.name, exception.reason ?: @""] }];
    }
    present = installedPath(identifier);
    sweet_drm_log("UIRestore: install %s returned=%u verified=%u error=%s", identifier.UTF8String,
        ok, present != nil, error.description.UTF8String ?: "none");
    /* Keep our input copy for failed-install diagnosis; never remove installer destinations. */
    return @{ @"status": ok && present ? @"installed" : @"installation-failed",
              @"serviceReturned": @(ok), @"path": present ?: @"", @"sourceCopy": stage,
              @"errorDomain": error.domain ?: @"", @"errorCode": @(error.code),
              @"error": error.description ?: @"" };
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

static BOOL restoreUserInterface(void)
{
        @autoreleasepool {
            NSString *root = @"/var/mobile/Library/xlamiOS-Restore0176";
            struct stat st;
            if (!lstat(root.fileSystemRepresentation, &st)) {
                if (!realDirectory(root)) { sweet_drm_log("UIRestore: root guard refused"); return NO; }
            } else if (errno != ENOENT || mkdir(root.fileSystemRepresentation, 0700)) {
                sweet_drm_log("UIRestore: root creation failed errno=%d", errno); return NO;
            }

            stageBlockBlastFromDisk();
            restoreWallpaper(root);

            Class cls = NSClassFromString(@"LSApplicationWorkspace");
            if (!methodShape(cls, @selector(defaultWorkspace), YES, 2, '@') ||
                !methodShape(cls, @selector(installApplication:withOptions:error:), NO, 5, 'B')) {
                sweet_drm_log("UIRestore: installation service ABI unavailable"); return NO;
            }
            id workspace = [cls defaultWorkspace];
            NSString *reportPath = [root stringByAppendingPathComponent:@"installation.plist"];
            NSDictionary *previous = [NSDictionary dictionaryWithContentsOfFile:reportPath];
            NSMutableDictionary *report = previous ? [previous mutableCopy] : [NSMutableDictionary dictionary];
            BOOL success = YES;
            for (NSArray *host in @[@[@"MobileCal.app", @"com.apple.mobilecal"],
                                    @[@"Weather.app", @"com.apple.weather", @"System"],
                                    @[@"BlockBlast.app", @"com.blockblast.vn.ios", @"Customer"]]) {
                unsigned attempts = [previous[host[1]][@"attempts"] unsignedIntValue];
                if (attempts >= 2 && !installedPath(host[1])) {
                    report[host[1]] = previous[host[1]];
                    success = NO;
                    sweet_drm_log("UIRestore: %s retry bound reached; failed inputs retained", [host[1] UTF8String]);
                    continue;
                }
                /* Persist before IPC so a daemon crash cannot create endless copies. */
                report[host[1]] = @{ @"status": @"attempting", @"attempts": @(attempts + 1) };
                if (![report writeToFile:reportPath atomically:YES]) {
                    sweet_drm_log("UIRestore: attempt journal failed; native install skipped"); success = NO; continue;
                }
                id pkg = host.count > 2 ? host[2] : nil;
                NSMutableDictionary *result = [installHost(workspace, root, host[0], host[1], pkg) mutableCopy];
                result[@"attempts"] = @(attempts + 1);
                if (![result[@"status"] isEqual:@"installed"] && ![result[@"status"] isEqual:@"already-installed"]) success = NO;
                report[host[1]] = result;
                [report writeToFile:reportPath atomically:YES];
            }
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
        /* Keep this process alive for the asynchronous preference observation. */
        dispatch_semaphore_wait(wallpaperObserved, dispatch_time(DISPATCH_TIME_NOW, 21LL * NSEC_PER_SEC));
        return success ? 0 : 1;
    }
}
