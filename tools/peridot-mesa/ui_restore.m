/* SPDX-License-Identifier: MIT */
/* Launch only our exact signed restoration helper from a private mobile directory. */
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sweet_drm.h"
#include "ui_restore_payload.h"

void peridot_ui_restore(void)
{
    if (strcmp(getprogname(), "SpringBoard")) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 35LL * NSEC_PER_SEC),
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool {
            const char *root = "/var/mobile/Library/xlamiOS-Restore0171";
            NSString *helperPath = [NSString stringWithFormat:@"%s/%s", root, ui_restore_filename];
            const char *path = helperPath.fileSystemRepresentation;
            struct stat st;
            if (lstat(root, &st)) {
                if (errno != ENOENT || mkdir(root, 0700)) { sweet_drm_log("UIRestore: helper directory failed errno=%d", errno); return; }
            } else if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0077)) {
                sweet_drm_log("UIRestore: helper directory guard refused"); return;
            }
            char temporary[256];
            int length = snprintf(temporary, sizeof(temporary), "%s/helper-stage-XXXXXX", root);
            if (length < 0 || (size_t)length >= sizeof(temporary)) return;
            int fd = mkstemp(temporary);
            if (fd >= 0) {
                size_t offset = 0;
                while (offset < sizeof(ui_restore_payload)) {
                    ssize_t n = write(fd, ui_restore_payload + offset, sizeof(ui_restore_payload) - offset);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) break;
                    offset += (size_t)n;
                }
                int ok = offset == sizeof(ui_restore_payload) && !fchmod(fd, 0700) && !fsync(fd);
                close(fd);
                if (!ok) { sweet_drm_log("UIRestore: helper write failed; own temporary file removed"); unlink(temporary); return; }
                /* Publish a complete inode atomically; an existing final file is never overwritten. */
                int published = link(temporary, path), publishError = errno;
                unlink(temporary);
                if (published && publishError != EEXIST) {
                    sweet_drm_log("UIRestore: helper publication failed errno=%d", publishError); return;
                }
            } else {
                sweet_drm_log("UIRestore: helper creation refused errno=%d", errno); return;
            }
            fd = open(path, O_RDONLY | O_NOFOLLOW);
            if (fd < 0) { sweet_drm_log("UIRestore: helper verify open failed errno=%d", errno); return; }
            BOOL exact = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == getuid() &&
                !(st.st_mode & 0077) && (st.st_mode & S_IXUSR) && st.st_size == sizeof(ui_restore_payload);
            unsigned char buffer[4096]; size_t offset = 0;
            while (exact && offset < sizeof(ui_restore_payload)) {
                size_t count = sizeof(ui_restore_payload) - offset;
                if (count > sizeof(buffer)) count = sizeof(buffer);
                ssize_t n = read(fd, buffer, count);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0 || memcmp(buffer, ui_restore_payload + offset, (size_t)n)) { exact = NO; break; }
                offset += (size_t)n;
            }
            close(fd);
            if (!exact || offset != sizeof(ui_restore_payload)) { sweet_drm_log("UIRestore: existing helper exact-byte guard refused"); return; }
            char *const arguments[] = {(char *)path, NULL};
            /* This standalone client must not inherit SpringBoard's injected GL library. */
            NSDictionary *inherited = NSProcessInfo.processInfo.environment;
            NSMutableArray<NSString *> *pairs = [NSMutableArray array];
            for (NSString *key in inherited) {
                if ([key hasPrefix:@"DYLD_"] || [key hasPrefix:@"SWEETGL_"]) continue;
                [pairs addObject:[NSString stringWithFormat:@"%@=%@", key, inherited[key]]];
            }
            char **environment = calloc(pairs.count + 1, sizeof(char *));
            if (!environment) { sweet_drm_log("UIRestore: helper environment allocation failed"); return; }
            for (NSUInteger index = 0; index < pairs.count; ++index) environment[index] = (char *)pairs[index].UTF8String;
            NSString *logName = [NSString stringWithFormat:@"helper-output-%@.log", NSUUID.UUID.UUIDString];
            NSString *logPath = [[NSString stringWithUTF8String:root] stringByAppendingPathComponent:logName];
            int output = open(logPath.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (output >= 0 && output < 3) {
                int higher = fcntl(output, F_DUPFD_CLOEXEC, 3); close(output); output = higher;
            }
            if (output < 0) { free(environment); sweet_drm_log("UIRestore: helper log creation failed errno=%d", errno); return; }
            posix_spawn_file_actions_t actions;
            int error = posix_spawn_file_actions_init(&actions);
            BOOL initialized = error == 0;
            if (!error) error = posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
            if (!error) error = posix_spawn_file_actions_adddup2(&actions, output, STDERR_FILENO);
            if (!error) error = posix_spawn_file_actions_addclose(&actions, output);
            if (!error && ![logName writeToFile:[[NSString stringWithUTF8String:root] stringByAppendingPathComponent:@"last-helper-log.txt"]
                atomically:YES encoding:NSUTF8StringEncoding error:nil]) error = EIO;
            if (error) {
                if (initialized) posix_spawn_file_actions_destroy(&actions);
                close(output); free(environment); sweet_drm_log("UIRestore: helper log setup failed error=%d", error); return;
            }
            pid_t child = 0;
            error = posix_spawn(&child, path, &actions, NULL, arguments, environment);
            posix_spawn_file_actions_destroy(&actions);
            close(output);
            free(environment);
            sweet_drm_log("UIRestore: entitled helper spawn error=%d pid=%d", error, child);
            sweet_drm_log("UIRestore: helper private log %s", logName.UTF8String);
            if (!error) {
                int status = 0; pid_t waited;
                do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
                if (waited == child) sweet_drm_log("UIRestore: helper exited=%u code=%d signal=%d", WIFEXITED(status),
                    WIFEXITED(status) ? WEXITSTATUS(status) : -1, WIFSIGNALED(status) ? WTERMSIG(status) : 0);
                else sweet_drm_log("UIRestore: helper wait failed errno=%d", errno);
            }
        }
    });
}
