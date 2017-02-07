#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#include "constants.h"
#include "lib.h"
#include "logger.h"
#include "string_builders.h"
#include "tcp_events.h"

long conf_opt_b;
long conf_opt_c;
char *conf_opt_d;
long conf_opt_e;
long conf_opt_f;
char *conf_opt_i;
long conf_opt_l;
long conf_opt_p;
long conf_opt_u;
long conf_opt_v;

FILE *_stdout;
FILE *_stderr;

static bool initialized = false;

#ifdef __ANDROID__
static pthread_mutex_t init_mutex = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER;
#else
static pthread_mutex_t init_mutex = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;
#endif

/* Private functions */

#ifndef __ANDROID__
static char *get_conf_opt_d(void) {
        DIR *dir;
        char *val = get_str_env(OPT_D);
        if (val) {
                if ((dir = opendir(val)))
                        closedir(dir);
                else
                        goto error1;
        } else
                goto error2;
        return val;
error1:
        LOG(ERROR, "opendir() failed on %s. %s.", val, strerror(errno));
        goto error_out;
error2:
        LOG(ERROR, "%s not set.", OPT_D);
error_out:
        LOG_FUNC_FAIL;
        return NULL;
}
#endif

static char *create_logs_dir(void) {
        char *base_path, *path;
        DIR *dir;
        if (!(base_path = alloc_base_dir_path(conf_opt_d))) goto error_out;

        // Find first directory available starting from base_path and by
        // concatenating increasing integers.
        int i = 0;
        if (!(path = alloc_append_int_to_path(base_path, i))) goto error1;
        while (true) {
                if ((dir = opendir(path))) {  // Already exists.
                        i++;
                        path = alloc_append_int_to_path(base_path, i);
                } else if (!dir && errno == ENOENT)
                        break;  // Free.
                else if (!dir)
                        goto error2;  // Failure for some other reason.
        }
        free(base_path);

        // Finally, create dir at path.
        if (mkdir(path, 0777)) goto error3;
        return path;
error3:
        LOG(ERROR, "mkdir() failed for %s. %s.", path, strerror(errno));
        free(path);
        goto error_out;
error2:
        LOG(ERROR, "opendir() failed. %s.", strerror(errno));
        free(path);
error1:
        free(base_path);
error_out:
        LOG_FUNC_FAIL;
        return NULL;
}

static void tcp_snitch_free(void) {
        free(conf_opt_d);
        mutex_destroy(&init_mutex);
}

static void cleanup(void) {
        LOG(INFO, "Performing library cleanup before end of process.");
        tcp_close_unclosed_connections();
        // tcp_free();
        // tcp_snitch_free();
}

/* Public functions */

/*  This function is used to reset the library after a fork() call. If a fork()
 *  is not followed by exec(), the global variables are not reinitialized.
 *  This means that 2 processes with differents PID would use the same
 * conf_opt_d,
 *  but more importantly, the child would inherit all the states from the
 *  fd_con_map of its parents. This leads to many issues:
 *     - The 2 processes will mix their logs in the same file.
 *     - If both the parent/child open a connection on the same FD, the last
 *     one the close it will overwrite the logs of the first to close.
 *     - ...
 *
 *  Our solution is to always reset the state of the library on fork(). One
 *  known issue of this solution is when both the child and the parent
 *  write/read on the SAME TCP connection opened by the parent. In that case,
 *  we will not have a complete view of the connection. Each process will have
 *  its own partial view. This is a known limitation.
 *
 *  Normally this function is called directly after fork() in the child process
 *  before it returns. There are normally no reasons to lock the mutex since at
 *  that moment, there should be a single thread of execution in the new child
 *  process. It thus better to still reset the library if we fail to acquire the
 *  mutex. Not resetting could have much more drastic consequences such as those
 *  explained above. */

void reset_tcpsnitch(void) {
        if (!initialized) return;  // Nothing to do.

        tcp_snitch_free();
        logger_init(NULL, 0, 0);
        initialized = false;
        mutex_init(&init_mutex);

        tcp_free();
        tcp_reset();
}

void init_tcpsnitch(void) {
        mutex_lock(&init_mutex);
        if (initialized) goto exit;

#ifndef __ANDROID__
        /* We need a way to unweave the main process and tcpsnitch standard
         * streams. To this purpose, we create 2 additionnal fd (3 & 4) with
         * some bash redirections (3>&1 4>&2 1>/dev/null 2>&). As a consequence,
         * tcpsnitch stderr/stdout do not have the regular 1 & 2 fd, but are
         * 3 and 4 instead. */
        if (!(_stdout = fdopen(STDOUT_FD, "w")))
                LOG(ERROR, "fdopen() failed. No buffered I/O for stdout.");
        if (!(_stderr = fdopen(STDERR_FD, "w")))
                LOG(ERROR, "fdopen() failed. No buffered I/O for stderr.");
#endif

        logger_init(NULL, WARN, WARN);
        atexit(cleanup);

        conf_opt_b = get_long_opt_or_defaultval(OPT_B, 4096);
        conf_opt_c = get_long_opt_or_defaultval(OPT_C, 0);
        conf_opt_e = get_long_opt_or_defaultval(OPT_E, 1000);
        conf_opt_f = get_long_opt_or_defaultval(OPT_F, WARN);
#ifdef __ANDROID__
        conf_opt_i = NULL;
#else
        conf_opt_i = get_str_env(OPT_I);
#endif
        conf_opt_l = get_long_opt_or_defaultval(OPT_L, WARN);
        conf_opt_p = get_long_opt_or_defaultval(OPT_P, 0);
        conf_opt_u = get_long_opt_or_defaultval(OPT_U, 0);
        conf_opt_v = get_long_opt_or_defaultval(OPT_V, 0);

#ifdef __ANDROID__
        // On Android, we don't chose the logs directory.
        // We always write under: /data/data/[app_name], which the internal
        // storage of the app.
        char *app_name = alloc_app_name();
        int n = 11 + strlen(app_name) + 1;  // "/data/data/" + APP_NAME + '\0'
        conf_opt_d = (char *)my_malloc(sizeof(char) * n);
        sprintf(conf_opt_d, "/data/data/%s", app_name);
        D("android path: %s", conf_opt_d);
#else
        if (!(conf_opt_d = get_conf_opt_d())) goto exit1;
#endif

        if (!(conf_opt_d = create_logs_dir())) goto exit1;
        const char *log_file_path;
        if (!(log_file_path = alloc_concat_path(conf_opt_d, MAIN_LOG_FILE)))
                goto exit2;
        else
                logger_init(log_file_path, conf_opt_l, conf_opt_f);

        goto exit;
exit1:
        LOG(ERROR, "Nothing will be written to file (log, pcap, json).");
        goto exit;
exit2:
        LOG(ERROR, "No logs to file.");
exit:
        initialized = true;
        mutex_unlock(&init_mutex);
        return;
}
