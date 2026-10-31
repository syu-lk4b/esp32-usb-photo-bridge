/*
 * photosync_launcher.c
 *
 * A tiny signed launcher so macOS can grant Full Disk Access to a stable
 * binary instead of /usr/bin/python3 (which TCC refuses to remember).
 *
 * Build (setup.sh does this for you):
 *   cc -O2 -DSCRIPT_PATH='"/absolute/path/to/photo_frame_sync.py"' \
 *      -o "$HOME/Applications/esp32-photo-bridge-sync" photosync_launcher.c
 *   codesign --force --sign - "$HOME/Applications/esp32-photo-bridge-sync"
 *
 * Then add that binary to System Settings -> Privacy & Security ->
 * Full Disk Access.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef SCRIPT_PATH
#error "compile with -DSCRIPT_PATH=\"/path/to/photo_frame_sync.py\""
#endif

static int is_allowed_arg(const char *arg) {
    return strcmp(arg, "--dry-run") == 0 ||
           strcmp(arg, "--list-people") == 0 ||
           strcmp(arg, "--force") == 0 ||
           strcmp(arg, "-h") == 0 ||
           strcmp(arg, "--help") == 0;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!is_allowed_arg(argv[i])) {
            fprintf(stderr, "disallowed argument: %s\n", argv[i]);
            return 2;
        }
    }

    char **args = calloc((size_t)argc + 3, sizeof(char *));
    if (!args) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    args[0] = "/usr/bin/python3";
    args[1] = "-I";
    args[2] = SCRIPT_PATH;
    for (int i = 1; i < argc; i++) {
        args[i + 2] = argv[i];
    }
    args[argc + 2] = NULL;

    const char *home = getenv("HOME");
    const char *tmpdir = getenv("TMPDIR");
    const char *user = getenv("USER");
    char home_env[512];
    char tmp_env[512];
    char user_env[128];

    char *envp[8];
    int ei = 0;
    envp[ei++] = "PATH=/usr/bin:/bin:/usr/sbin:/sbin";
    envp[ei++] = "LANG=en_US.UTF-8";
    if (home && snprintf(home_env, sizeof(home_env), "HOME=%s", home) < (int)sizeof(home_env)) {
        envp[ei++] = home_env;
    }
    if (tmpdir && snprintf(tmp_env, sizeof(tmp_env), "TMPDIR=%s", tmpdir) < (int)sizeof(tmp_env)) {
        envp[ei++] = tmp_env;
    }
    if (user && snprintf(user_env, sizeof(user_env), "USER=%s", user) < (int)sizeof(user_env)) {
        envp[ei++] = user_env;
    }
    envp[ei] = NULL;

    execve(args[0], args, envp);
    fprintf(stderr, "execve(%s) failed: %s\n", args[0], strerror(errno));
    return 127;
}
