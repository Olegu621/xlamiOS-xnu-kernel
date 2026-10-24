/* SPDX-License-Identifier: MIT */
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <objc/runtime.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sweet_drm.h"
#include "probe_payload.h"

@interface NSObject (PeridotWorkspace)
+ (id)defaultWorkspace;
+ (id)applicationProxyForIdentifier:(NSString *)identifier;
- (BOOL)registerApplication:(NSURL *)url;
- (BOOL)openApplicationWithBundleID:(NSString *)identifier;
- (NSURL *)bundleURL;
@end

static BOOL boolMethod(id object, SEL selector)
{
    Method method = object ? class_getInstanceMethod(object_getClass(object), selector) : NULL;
    const char *encoding = method ? method_getTypeEncoding(method) : "";
    return method && method_getNumberOfArguments(method) == 3 && (encoding[0] == 'B' || encoding[0] == 'c');
}

static BOOL directoryOrMissing(NSString *path)
{
    struct stat info;
    if (lstat(path.fileSystemRepresentation, &info)) return errno == ENOENT;
    return S_ISDIR(info.st_mode) && !S_ISLNK(info.st_mode);
}

static BOOL writeApp(NSString *stage, NSError **error)
{
    NSFileManager *fm = NSFileManager.defaultManager;
    for (size_t i = 0; i < sizeof(probe_files)/sizeof(probe_files[0]); ++i) {
        const struct probe_file *file = &probe_files[i];
        NSString *relative = [NSString stringWithUTF8String:file->name];
        if (!relative.length || [relative hasPrefix:@"/"] || [relative.pathComponents containsObject:@".."]) return NO;
        NSString *target = [stage stringByAppendingPathComponent:relative];
        if (![fm createDirectoryAtPath:target.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:error] ||
            ![[NSData dataWithBytes:file->data length:file->size] writeToFile:target options:NSDataWritingAtomic error:error] ||
            chmod(target.fileSystemRepresentation, file->mode)) return NO;
    }
    return YES;
}

void peridot_probe_install(void)
{
    if (strcmp(getprogname(), "SpringBoard")) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 60LL * NSEC_PER_SEC),
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool {
            NSString *root = @"/var/mobile/Library/PeridotProbe";
            NSString *path = [root stringByAppendingPathComponent:@"GLTest.app"];
            NSFileManager *fm = NSFileManager.defaultManager;
            NSError *error = nil;
            if (!directoryOrMissing(root) || !directoryOrMissing(path)) {
                sweet_drm_log("GLTest: destination directory/symlink guard refused"); return;
            }
            NSDictionary *existing = [NSDictionary dictionaryWithContentsOfFile:[path stringByAppendingPathComponent:@"Info.plist"]];
            if ([fm fileExistsAtPath:path] && ![existing[@"CFBundleIdentifier"] isEqual:@"com.xlamios.es3probe"]) {
                sweet_drm_log("GLTest: refusing unrelated existing destination"); return;
            }
            BOOL first = !existing;
            if (existing && ![existing[@"CFBundleVersion"] isEqual:@"166"]) {
                sweet_drm_log("GLTest: existing version retained; explicit upgrade needed"); return;
            }
            if (first) {
                if (![fm createDirectoryAtPath:root withIntermediateDirectories:NO attributes:nil error:&error] &&
                    ![fm fileExistsAtPath:root]) {
                    sweet_drm_log("GLTest: create failed: %s", error.description.UTF8String); return;
                }
                NSString *stage = [root stringByAppendingPathComponent:
                    [NSString stringWithFormat:@"GLTest-stage-%@.app", NSUUID.UUID.UUIDString]];
                if (mkdir(stage.fileSystemRepresentation, 0755)) {
                    sweet_drm_log("GLTest: unique staging creation failed errno=%d", errno); return;
                }
                if (!writeApp(stage, &error) || ![fm moveItemAtPath:stage toPath:path error:&error]) {
                    sweet_drm_log("GLTest: installation failed: %s", error.description.UTF8String ?: "file permissions");
                    /* This directory was created exclusively above in our own root. */
                    if (directoryOrMissing(stage)) [fm removeItemAtPath:stage error:nil];
                    return;
                }
            }
            Class workspaceClass = NSClassFromString(@"LSApplicationWorkspace");
            Method factory = workspaceClass ? class_getClassMethod(workspaceClass, @selector(defaultWorkspace)) : NULL;
            if (!factory || method_getNumberOfArguments(factory) != 2 || method_getTypeEncoding(factory)[0] != '@') {
                sweet_drm_log("GLTest: workspace factory unavailable"); return;
            }
            id workspace = [workspaceClass defaultWorkspace];
            if (!boolMethod(workspace, @selector(registerApplication:))) { sweet_drm_log("GLTest: registration API guard refused"); return; }
            NSURL *url = [NSURL fileURLWithPath:path isDirectory:YES];
            BOOL ok = [workspace registerApplication:url];
            sweet_drm_log("GLTest: registerApplication=%u bundle=%s", ok, path.UTF8String);
            if (!ok) return;
            Class proxyClass = NSClassFromString(@"LSApplicationProxy");
            Method proxyFactory = proxyClass ? class_getClassMethod(proxyClass, @selector(applicationProxyForIdentifier:)) : NULL;
            if (!proxyFactory || method_getNumberOfArguments(proxyFactory) != 3 || method_getTypeEncoding(proxyFactory)[0] != '@') return;
            id proxy = [proxyClass applicationProxyForIdentifier:@"com.xlamios.es3probe"];
            Method getter = proxy ? class_getInstanceMethod(object_getClass(proxy), @selector(bundleURL)) : NULL;
            if (!getter || method_getNumberOfArguments(getter) != 2 || method_getTypeEncoding(getter)[0] != '@') return;
            NSString *actual = [[proxy bundleURL].path stringByStandardizingPath];
            BOOL verified = [actual isEqual:path] || [actual isEqual:[@"/private" stringByAppendingString:path]];
            sweet_drm_log("GLTest: registered path verified=%u actual=%s", verified, actual.UTF8String ?: "nil");
            BOOL launched = [fm fileExistsAtPath:[root stringByAppendingPathComponent:@"launch-confirmed.txt"]];
            if (!launched && verified && boolMethod(workspace, @selector(openApplicationWithBundleID:))) {
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20LL * NSEC_PER_SEC),
                    dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                    BOOL opened = [workspace openApplicationWithBundleID:@"com.xlamios.es3probe"];
                    sweet_drm_log("GLTest: first launch requested=%u", opened);
                });
            }
        }
    });
}
