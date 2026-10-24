/* SPDX-License-Identifier: MIT */
/* Restore known staged widget hosts using iOS's installer, never raw disk writes. */
#import <Foundation/Foundation.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <objc/runtime.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sweet_drm.h"

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

static NSDictionary *installHost(id workspace, NSString *root, NSString *name, NSString *identifier)
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
    sweet_drm_log("UIRestore: native install begin %s; original staged bundle retained", identifier.UTF8String);
    BOOL ok = NO;
    @try {
        ok = [workspace installApplication:[NSURL fileURLWithPath:stage isDirectory:YES] withOptions:@{} error:&error];
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
        }
    });
}

void peridot_ui_restore(void)
{
    if (strcmp(getprogname(), "SpringBoard")) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 35LL * NSEC_PER_SEC),
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool {
            NSString *root = @"/var/mobile/Library/xlamiOS-Restore0167";
            struct stat st;
            if (!lstat(root.fileSystemRepresentation, &st)) {
                if (!realDirectory(root)) { sweet_drm_log("UIRestore: root guard refused"); return; }
            } else if (errno != ENOENT || mkdir(root.fileSystemRepresentation, 0700)) {
                sweet_drm_log("UIRestore: root creation failed errno=%d", errno); return;
            }
            restoreWallpaper(root);
            Class cls = NSClassFromString(@"LSApplicationWorkspace");
            if (!methodShape(cls, @selector(defaultWorkspace), YES, 2, '@') ||
                !methodShape(cls, @selector(installApplication:withOptions:error:), NO, 5, 'B')) {
                sweet_drm_log("UIRestore: installation service ABI unavailable"); return;
            }
            id workspace = [cls defaultWorkspace];
            NSString *reportPath = [root stringByAppendingPathComponent:@"installation.plist"];
            NSDictionary *previous = [NSDictionary dictionaryWithContentsOfFile:reportPath];
            NSMutableDictionary *report = previous ? [previous mutableCopy] : [NSMutableDictionary dictionary];
            for (NSArray *host in @[@[@"MobileCal.app", @"com.apple.mobilecal"],
                                    @[@"Weather.app", @"com.apple.weather"]]) {
                unsigned attempts = [previous[host[1]][@"attempts"] unsignedIntValue];
                if (attempts >= 2 && !installedPath(host[1])) {
                    report[host[1]] = previous[host[1]];
                    sweet_drm_log("UIRestore: %s retry bound reached; failed inputs retained", [host[1] UTF8String]);
                    continue;
                }
                /* Persist before IPC so a daemon crash cannot create endless copies. */
                report[host[1]] = @{ @"status": @"attempting", @"attempts": @(attempts + 1) };
                if (![report writeToFile:reportPath atomically:YES]) {
                    sweet_drm_log("UIRestore: attempt journal failed; native install skipped"); continue;
                }
                NSMutableDictionary *result = [installHost(workspace, root, host[0], host[1]) mutableCopy];
                result[@"attempts"] = @(attempts + 1);
                report[host[1]] = result;
                [report writeToFile:reportPath atomically:YES];
            }
            sweet_drm_log("UIRestore: host restoration attempts finished; widget timelines require separate verification");
        }
    });
}
