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

int main(int argc, char **argv) {
    char **args = calloc((size_t)argc + 2, sizeof(char *));
    if (!args) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    args[0] = "/usr/bin/python3";
    args[1] = SCRIPT_PATH;
    for (int i = 1; i < argc; i++) {
        args[i + 1] = argv[i];
    }
    args[argc + 1] = NULL;

    execv(args[0], args);
    fprintf(stderr, "execv(%s) failed: %s\n", args[0], strerror(errno));
    return 127;
}
