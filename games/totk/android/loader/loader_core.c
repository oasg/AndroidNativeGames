/* Module: core. */
#include "loader_internal.h"

/* Entry point for the Android app shell.  The shell owns the Activity and
 * Surface; this call runs the full loader (including the original nnMain on
 * request) on its worker thread.  `host_window` is accepted for the shared
 * shell contract but the VI layer creates/reuses the game's own HostNativeWindow
 * and the pending Android surface is attached when that window appears. */
int totk_android_loader_entry(void *host_window, int argc, char **argv) {
    (void)host_window;
    return loader_main(argc, argv);
}

#ifndef TOTK_ANDROID_LIBRARY
int main(int argc, char **argv) {
    return loader_main(argc, argv);
}
#endif
