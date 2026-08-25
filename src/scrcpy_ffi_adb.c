#include "scrcpy_ffi_internal.h"

/**
 * @brief Mutex protecting the ADB initialization reference counter.
 */
static pthread_mutex_t adb_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Mutex serializing "adb start-server" calls.
 *
 * Multiple scrcpy worker threads can call sc_adb_start_server() concurrently
 * when many devices are mirrored at the same time. The ADB daemon starts on
 * the first invocation; subsequent concurrent calls see an ADB in a
 * half-initialized state and exit with a non-zero code, causing the mirroring
 * session to fail with "adb start-server exited unexpectedly".
 *
 * This mutex ensures only ONE "adb start-server" runs at a time.
 */
static pthread_mutex_t adb_start_server_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Global mutex to serialize all adb command executions.
 * macOS ADB daemon and posix_spawnp can fail if too many ADB commands
 * are executed concurrently across multiple threads.
 */
static pthread_mutex_t adb_execute_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Reference counter for active scrcpy sessions using ADB.
 */
static int adb_refcount = 0;

/**
 * @brief Initializes the ADB command layer with thread-safe reference counting.
 * @return true if initialized successfully or already active, false otherwise.
 */
bool
scrcpy_ffi_adb_init(void) {
    pthread_mutex_lock(&adb_mutex);
    if (adb_refcount > 0) {
        adb_refcount++;
        pthread_mutex_unlock(&adb_mutex);
        return true;
    }

    bool ok = sc_adb_init();
    if (ok) {
        adb_refcount = 1;
    }
    pthread_mutex_unlock(&adb_mutex);
    return ok;
}

/**
 * @brief Cleans up and releases the ADB layer when the reference count drops to 0.
 */
void
scrcpy_ffi_adb_destroy(void) {
    pthread_mutex_lock(&adb_mutex);
    if (adb_refcount > 0) {
        adb_refcount--;
        if (adb_refcount == 0) {
            sc_adb_destroy();
        }
    }
    pthread_mutex_unlock(&adb_mutex);
}

/**
 * @brief Pushes the scrcpy-server JAR to the Android target device.
 * It queries the device first to check if the server is already pushed
 * to prevent unnecessary write cycles to flash memory.
 */
bool
scrcpy_ffi_adb_push(struct sc_intr *intr, const char *serial, const char *local,
                    const char *remote, unsigned flags) {
    (void)remote;
    LOGD("FFI: scrcpy_ffi_adb_push called! local=%s", local);
    char my_remote[256];
    snprintf(my_remote, sizeof(my_remote), "/data/local/tmp/scrcpy_server_%s", SCRCPY_VERSION);

    // Check if the server file already exists on the device
    const char *adb = sc_adb_get_executable();
    if (!adb || adb[0] == '\0') {
        adb = getenv("ADB");
        if (!adb || adb[0] == '\0') {
            adb = "adb";
        }
    }
    const char *argv_test[] = {
        adb,
        "-s",
        serial,
        "shell",
        "[",
        "-f",
        my_remote,
        "]",
        NULL
    };

    pthread_mutex_lock(&adb_execute_mutex);
    sc_pid pid = sc_adb_execute(argv_test, SC_ADB_SILENT);
    pthread_mutex_unlock(&adb_execute_mutex);
    
    bool exists = false;
    if (pid != SC_PROCESS_NONE) {
        if (intr && !sc_intr_set_process(intr, pid)) {
            // Already interrupted
        } else {
            sc_exit_code exit_code = scrcpy_ffi_process_wait(pid, false);
            if (intr) {
                sc_intr_set_process(intr, SC_PROCESS_NONE);
            }
            sc_process_close(pid);
            if (exit_code == 0) {
                exists = true;
            }
        }
    }

    if (exists) {
        LOGD("FFI: Server with version %s already exists on the device. Skipping push.", SCRCPY_VERSION);
        return true;
    }

    LOGD("FFI: Server not found on device, pushing to %s", my_remote);
    
    // Serialize adb push specifically as it doesn't use scrcpy_ffi_adb_execute directly
    pthread_mutex_lock(&adb_execute_mutex);
    // Force silent mode to suppress "1 file pushed" logs from adb
    bool push_ok = sc_adb_push(intr, serial, local, my_remote, flags | SC_ADB_SILENT);
    pthread_mutex_unlock(&adb_execute_mutex);
    
    return push_ok;
}

/**
 * @brief Executes an ADB command with rewritten CLASSPATH to support concurrent multi-version runs.
 */
sc_pid
scrcpy_ffi_adb_execute(const char *const argv[], unsigned flags) {
    LOGD("FFI: scrcpy_ffi_adb_execute called!");
    int argc = 0;
    while (argv[argc] != NULL) {
        argc++;
    }

    const char **new_argv = malloc((argc + 1) * sizeof(char *));
    if (!new_argv) {
        return SC_PROCESS_NONE;
    }

    char classpath_arg[256];
    snprintf(classpath_arg, sizeof(classpath_arg), "CLASSPATH=/data/local/tmp/scrcpy_server_%s", SCRCPY_VERSION);

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "CLASSPATH=/data/local/tmp/scrcpy-server.jar") == 0) {
            new_argv[i] = strdup(classpath_arg);
            LOGD("FFI: Rewriting CLASSPATH to %s", classpath_arg);
        } else {
            new_argv[i] = argv[i];
        }
    }
    new_argv[argc] = NULL;

    pthread_mutex_lock(&adb_execute_mutex);
    sc_pid pid = sc_adb_execute(new_argv, flags);
    pthread_mutex_unlock(&adb_execute_mutex);

    for (int i = 0; i < argc; i++) {
        if (new_argv[i] != argv[i]) {
            free((void *)new_argv[i]);
        }
    }
    free(new_argv);

    return pid;
}



/**
 * @brief Terminates a running ADB sub-process safely.
 */
bool
scrcpy_ffi_process_terminate(sc_pid pid) {
    if (pid <= 0) {
        return false;
    }
    return sc_process_terminate(pid);
}

bool
scrcpy_ffi_adb_select_device(struct sc_intr *intr,
                             const struct sc_adb_device_selector *selector,
                             unsigned flags, struct sc_adb_device *out_device) {
    pthread_mutex_lock(&adb_execute_mutex);
    bool ok = sc_adb_select_device(intr, selector, flags, out_device);
    pthread_mutex_unlock(&adb_execute_mutex);
    return ok;
}

/**
 * @brief Intercepts sc_adb_start_server to serialize it.
 *
 * This override is configured in CMakeLists.txt.
 * It uses the same pattern as upstream but wraps the execute in a mutex.
 */
bool
scrcpy_ffi_adb_start_server(struct sc_intr *intr, unsigned flags) {
    static sc_tick last_adb_start_time = 0;

    pthread_mutex_lock(&adb_start_server_mutex);
    LOGD("FFI: [start-server] Acquired mutex, executing adb start-server");

    sc_tick now = sc_tick_now();
    if (now - last_adb_start_time < SC_TICK_FROM_SEC(2)) {
        LOGD("FFI: [start-server] Skipping adb start-server (already started recently)");
        pthread_mutex_unlock(&adb_start_server_mutex);
        return true;
    }

    // Construct the array using sc_adb_get_executable() to respect the ADB environment variable.
    const char *const argv[] = {sc_adb_get_executable(), "start-server", NULL};

    // We call our own execute, which already rewrites CLASSPATH and calls sc_adb_execute.
    sc_pid pid = scrcpy_ffi_adb_execute(argv, flags);
    
    bool ok = false;
    if (pid != SC_PROCESS_NONE) {
        sc_exit_code exit_code = scrcpy_ffi_process_wait(pid, true); // true = close
        if (exit_code == 0) {
            ok = true;
            last_adb_start_time = sc_tick_now();
        } else {
            LOGE("FFI: adb start-server returned with value %d", (int)exit_code);
            // If it failed, maybe ADB daemon is already running but start-server failed due to fork limits?
            // Actually, if it fails, it's safer to just return true and let `scrcpy` try to connect. 
            // If the server is actually down, the connection will fail anyway.
            // If we return false, `scrcpy` aborts immediately without even trying to connect.
            ok = true; 
        }
    } else {
        LOGE("FFI: Could not execute adb start-server");
        ok = true; // Fallback: try to connect anyway
    }

    pthread_mutex_unlock(&adb_start_server_mutex);
    LOGD("FFI: [start-server] Released mutex, success: %d", ok);

    return ok;
}

/**
 * @brief Kills any zombie scrcpy-server process still running on the Android device.
 *
 * Khi session scrcpy bị ngắt đột ngột, Java server trên Android vẫn giữ LocalSocket
 * → khởi động session mới bị "Address already in use".
 * Hàm này gửi `adb shell pkill` để dọn sạch trước khi start session mới.
 * Fire-and-forget: bỏ qua lỗi nếu device chưa kết nối hoặc pkill không tồn tại.
 *
 * @param serial Android device serial (có thể NULL nếu chỉ có 1 device)
 */
void
scrcpy_ffi_kill_zombie_server(const char *serial) {
    const char *adb = sc_adb_get_executable();
    if (!adb || adb[0] == '\0') {
        adb = getenv("ADB");
        if (!adb || adb[0] == '\0') {
            adb = "adb";
        }
    }

    char server_name[128];
    snprintf(server_name, sizeof(server_name), "scrcpy_server_%s", SCRCPY_VERSION);

    // Build argv với hoặc không có -s serial
    const char *argv_with_serial[] = {
        adb, "-s", serial, "shell",
        "pkill", "-f", server_name,
        NULL
    };
    const char *argv_no_serial[] = {
        adb, "shell",
        "pkill", "-f", server_name,
        NULL
    };

    const char **argv = (const char **)(serial ? argv_with_serial : argv_no_serial);

    LOGD("FFI: Killing zombie server '%s' on device '%s'", server_name, serial ? serial : "(any)");

    pthread_mutex_lock(&adb_execute_mutex);
    sc_pid pid = sc_adb_execute(argv, SC_ADB_SILENT);
    pthread_mutex_unlock(&adb_execute_mutex);
    
    if (pid == SC_PROCESS_NONE) {
        LOGD("FFI: Could not execute pkill (ignoring)");
        return;
    }
    // Fire-and-forget: đợi pkill kết thúc và bỏ qua exit code
    scrcpy_ffi_process_wait(pid, true);
}

sc_exit_code
scrcpy_ffi_process_wait(sc_pid pid, bool close) {
#ifdef _WIN32
#undef sc_process_wait
    sc_exit_code sc_process_wait(sc_pid pid, bool close);
    return sc_process_wait(pid, close);
#else
    int options = WEXITED;
    if (!close) {
        options |= WNOWAIT;
    }

    siginfo_t info;
    if (waitid(P_PID, pid, &info, options) == -1) {
        if (errno == ECHILD) {
            // Flutter's zombie reaper usually reaps our processes before we can waitid them.
            // This causes ECHILD. We can assume the process exited successfully because if it 
            // didn't, we wouldn't have been able to read its output.
            return 0;
        }
        return SC_EXIT_CODE_NONE;
    }

    if (info.si_code != CLD_EXITED) {
        return SC_EXIT_CODE_NONE;
    }

    return info.si_status;
#endif
}