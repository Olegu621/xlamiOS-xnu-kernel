/* SPDX-License-Identifier: MIT */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#include <dlfcn.h>
#include <math.h>
#include <stdatomic.h>
#include "esUtil.h"
#include "PeridotGL.h"

extern int esMain(ESContext *);
static NSString *const resultPath = @"/var/mobile/Library/PeridotProbe/result.json";

@interface ProbeView : UIViewController {
    UIImageView *_preview;
    UILabel *_status;
    atomic_bool _active, _pending, _resetTiming;
}
@end

@implementation ProbeView
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor colorWithRed:.025 green:.045 blue:.08 alpha:1];
    _status = [[UILabel alloc] initWithFrame:CGRectZero];
    _status.textColor = UIColor.whiteColor; _status.numberOfLines = 0;
    _status.font = [UIFont monospacedSystemFontOfSize:15 weight:UIFontWeightRegular];
    _status.text = @"GL Test\nПодготовка драйвера…";
    _preview = [[UIImageView alloc] initWithFrame:CGRectZero];
    _preview.contentMode = UIViewContentModeScaleAspectFit;
    [self.view addSubview:_status]; [self.view addSubview:_preview];
    atomic_init(&_active, true); atomic_init(&_pending, false); atomic_init(&_resetTiming, false);
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(background:)
        name:UIApplicationDidEnterBackgroundNotification object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(foreground:)
        name:UIApplicationWillEnterForegroundNotification object:nil];
    [NSThread detachNewThreadSelector:@selector(render) toTarget:self withObject:nil];
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    CGFloat w = self.view.bounds.size.width, h = self.view.bounds.size.height;
    _status.frame = CGRectMake(20, self.view.safeAreaInsets.top + 16, w - 40, 190);
    _preview.frame = CGRectMake(16, self.view.safeAreaInsets.top + 210, w - 32, h - 280);
}
- (void)background:(NSNotification *)note { atomic_store(&_active, false); }
- (void)foreground:(NSNotification *)note {
    atomic_store(&_resetTiming, true); atomic_store(&_active, true);
}
- (void)fail:(NSString *)reason {
    NSDictionary *report = @{ @"status": @"failed", @"reason": reason };
    [[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil]
        writeToFile:resultPath options:NSDataWritingAtomic error:nil];
    dispatch_async(dispatch_get_main_queue(), ^{ self->_status.text = [@"GL Test\n" stringByAppendingString:reason]; });
}
- (void)render {
    @autoreleasepool {
        void *library = dlopen("/usr/lib/peridot-sweetgl.dylib", RTLD_NOW | RTLD_LOCAL);
        bool (*init)(void) = library ? dlsym(library, "sweetgl_init") : NULL;
        void *(*create)(int, void *) = library ? dlsym(library, "sweetgl_create") : NULL;
        bool (*current)(void *) = library ? dlsym(library, "sweetgl_make_current") : NULL;
        bool (*selftest)(void) = library ? dlsym(library, "sweetgl_selftest") : NULL;
        void *(*proc)(const char *) = library ? dlsym(library, "sweetgl_get_proc_address") : NULL;
        if (!init || !create || !current || !selftest || !proc || !peridotGLLoad(proc)) {
            [self fail:@"Не удалось загрузить API текущего драйвера"]; return;
        }
        if (!init()) { [self fail:@"Служба GPU недоступна"]; return; }
        BOOL pixelsOK = selftest();
        void *context = create(3, NULL);
        if (!context || !current(context)) { [self fail:@"Контекст GLES 3 не создан"]; return; }
        const char *name = (const char *)glGetString(GL_RENDERER);
        NSString *renderer = name ? [NSString stringWithUTF8String:name] : @"unknown";
        if ([renderer caseInsensitiveCompare:@"FD735"] != NSOrderedSame &&
            !([renderer.lowercaseString containsString:@"adreno"] && [renderer containsString:@"735"])) {
            [self fail:[@"Неожиданный renderer: " stringByAppendingString:renderer]]; return;
        }
        const int width = 512, height = 512;
        GLuint tex, fbo; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
        glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            [self fail:@"Неполный framebuffer"]; return;
        }
        ESContext sample = {0}; sample.width = width; sample.height = height;
        srand(1);
        if (!esMain(&sample)) { [self fail:@"Не запущен пример ParticleSystem"]; return; }
        double start = CACurrentMediaTime(), last = start, reportAt = start, previewAt = 0;
        double totalFinish = 0, maxFinish = 0; unsigned frames = 0, errors = 0;
        for (;;) {
            @autoreleasepool {
                if (!atomic_load(&_active)) {
                    [NSThread sleepForTimeInterval:.1]; last = reportAt = CACurrentMediaTime();
                    frames = 0; totalFinish = 0; maxFinish = 0; continue;
                }
                if (atomic_exchange(&_resetTiming, false)) {
                    last = reportAt = CACurrentMediaTime(); frames = 0;
                    totalFinish = 0; maxFinish = 0;
                }
                double now = CACurrentMediaTime(), dt = fmin(now - last, .1); last = now;
                sample.updateFunc(&sample, (float)dt);
                double drawStart = CACurrentMediaTime();
                sample.drawFunc(&sample); glFinish();
                double finish = CACurrentMediaTime() - drawStart;
                totalFinish += finish; maxFinish = fmax(maxFinish, finish); ++frames;
                GLenum error = glGetError(); if (error != GL_NO_ERROR) ++errors;
                if (now - previewAt >= .1 && !atomic_exchange(&_pending, true)) {
                    previewAt = now;
                    NSMutableData *data = [NSMutableData dataWithLength:width * height * 4];
                    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, data.mutableBytes);
                    GLenum readError = glGetError(); if (readError != GL_NO_ERROR) ++errors;
                    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
                    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
                    CGImageRef image = CGImageCreate(width, height, 8, 32, width * 4, space,
                        kCGBitmapByteOrder32Big | kCGImageAlphaPremultipliedLast, provider, NULL, false, kCGRenderingIntentDefault);
                    UIImage *preview = image ? [UIImage imageWithCGImage:image scale:1 orientation:UIImageOrientationDownMirrored] : nil;
                    if (image) CGImageRelease(image); CGColorSpaceRelease(space); CGDataProviderRelease(provider);
                    dispatch_async(dispatch_get_main_queue(), ^{
                        self->_preview.image = preview; atomic_store(&self->_pending, false);
                    });
                }
                double span = CACurrentMediaTime() - reportAt;
                if (span >= 2 && frames) {
                    double fps = frames / span, avg = totalFinish * 1000 / frames;
                    NSDictionary *report = @{ @"status": @"running", @"sample": @"ES3 book ParticleSystem",
                        @"renderer": renderer, @"pixel_selftest": @(pixelsOK), @"render_fps": @(fps),
                        @"draw_finish_ms": @(avg), @"max_draw_finish_ms": @(maxFinish * 1000),
                        @"GL_error_count": @(errors), @"preview_max_hz": @10,
                        @"physical_scanout_measured": @NO, @"render_size": @[@512, @512] };
                    [[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil]
                        writeToFile:resultPath options:NSDataWritingAtomic error:nil];
                    NSString *label = [NSString stringWithFormat:@"GL Test · 1000 частиц\n%@\nGPU draw+finish %.2f мс\nОтрисовка %.1f кадр/с\nПиксели %@ · ошибок %u\nПревью до 10 Гц", renderer, avg, fps, pixelsOK ? @"OK" : @"FAIL", errors];
                    dispatch_async(dispatch_get_main_queue(), ^{ self->_status.text = label; });
                    reportAt = CACurrentMediaTime(); frames = 0; totalFinish = 0; maxFinish = 0;
                }
                double wait = 1.0 / 120 - (CACurrentMediaTime() - now);
                if (wait > 0) [NSThread sleepForTimeInterval:wait];
            }
        }
    }
}
@end

@interface ProbeDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation ProbeDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options {
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [ProbeView new]; [self.window makeKeyAndVisible];
    [@"UIApplication didFinishLaunching" writeToFile:@"/var/mobile/Library/PeridotProbe/launch-confirmed.txt"
        atomically:YES encoding:NSUTF8StringEncoding error:nil];
    return YES;
}
@end
int main(int argc, char **argv) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(ProbeDelegate.class)); }
}
