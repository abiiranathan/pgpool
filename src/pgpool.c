#include "../include/pgpool.h"
#include <errno.h>    // for EINTR, ETIMEDOUT
#include <poll.h>     // for poll, pollfd, POLLIN, POLLOUT
#include <pthread.h>  // for pthread_mutex_t, pthread_cond_t
#include <stdio.h>    // for fprintf, snprintf
#include <stdlib.h>   // for calloc, free, realloc
#include <string.h>   // for strdup, strcmp, strstr, memmove
#include <time.h>     // for clock_gettime, CLOCK_MONOTONIC, CLOCK_REALTIME

#define DEFAULT_MIN_CONNECTIONS 1
#define DEFAULT_MAX_CONNECTIONS 10
#define DEFAULT_CONNECT_TIMEOUT 5

/** Milliseconds to wait when draining trailing results on release. */
#define DRAIN_TIMEOUT_MS 5000

/** Milliseconds to wait when issuing a cleanup ROLLBACK on release. */
#define RELEASE_ROLLBACK_TIMEOUT_MS 5000

/* =========================================================================
 * Internal Structures
 * ======================================================================= */

/** Tracks a single prepared statement name cached on a physical connection. */
typedef struct {
    char* name; /** Heap-allocated statement name. */
} prepared_stmt_t;

/** Internal wrapper around a single libpq connection. */
struct pgconn {
    PGconn* raw_conn;    /** Underlying libpq connection. Always non-blocking. */
    bool in_use;         /** True while held by a caller. */
    bool broken;         /** True when the connection is no longer usable. */
    char error_msg[256]; /** Last error string, for pgpool_error_message(). */

    /** Sorted vector of prepared statement names cached on this connection. */
    prepared_stmt_t* stmts;
    size_t stmt_count;
    size_t stmt_capacity;
};

/** Connection pool. */
struct pgpool {
    pgconn_t** connections; /** Array of all wrapped connections (idle + active). */
    size_t capacity;        /** Allocated length of connections[]. */
    size_t size;            /** Number of physical connections created so far. */
    size_t available_count; /** Fast count of idle, non-broken connections. */

    size_t min_connections;
    size_t max_connections;
    size_t pending_connections; /** Connections being created outside the lock. */
    size_t active_count;        /** Connections currently held by callers. */

    char* conninfo;
    int connect_timeout;
    bool auto_reconnect;

    void (*connection_init)(PGconn*);
    void (*connection_close)(PGconn*);

    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool shutdown;
};

/* =========================================================================
 * Statement Cache Helpers
 * ======================================================================= */

static int stmt_cmp(const void* a, const void* b) {
    return strcmp(((const prepared_stmt_t*)a)->name, ((const prepared_stmt_t*)b)->name);
}

static void cache_statement_add(pgconn_t* conn, const char* name) {
    if (conn->stmt_count >= conn->stmt_capacity) {
        size_t new_cap = conn->stmt_capacity == 0 ? 8 : conn->stmt_capacity * 2;
        prepared_stmt_t* arr = realloc(conn->stmts, new_cap * sizeof(*arr));
        if (!arr) return;
        conn->stmts = arr;
        conn->stmt_capacity = new_cap;
    }
    conn->stmts[conn->stmt_count].name = strdup(name);
    if (!conn->stmts[conn->stmt_count].name) return;
    conn->stmt_count++;
    qsort(conn->stmts, conn->stmt_count, sizeof(*conn->stmts), stmt_cmp);
}

static bool cache_statement_exists(const pgconn_t* conn, const char* name) {
    if (conn->stmt_count == 0) return false;
    prepared_stmt_t key = {.name = (char*)name};
    return bsearch(&key, conn->stmts, conn->stmt_count, sizeof(*conn->stmts), stmt_cmp) != NULL;
}

static void cache_statement_remove(pgconn_t* conn, const char* name) {
    if (conn->stmt_count == 0) return;
    prepared_stmt_t key = {.name = (char*)name};
    prepared_stmt_t* found = bsearch(&key, conn->stmts, conn->stmt_count, sizeof(*conn->stmts), stmt_cmp);
    if (!found) return;
    free(found->name);
    size_t idx = (size_t)(found - conn->stmts);
    size_t tail = conn->stmt_count - idx - 1;
    if (tail > 0) memmove(found, found + 1, tail * sizeof(*conn->stmts));
    conn->stmt_count--;
}

static void cache_clear(pgconn_t* conn) {
    for (size_t i = 0; i < conn->stmt_count; i++)
        free(conn->stmts[i].name);
    free(conn->stmts);
    conn->stmts = NULL;
    conn->stmt_count = 0;
    conn->stmt_capacity = 0;
}

/* =========================================================================
 * Physical Connection Helpers
 * ======================================================================= */

static PGconn* create_physical_connection(pgpool_t* pool) {
    PGconn* conn = PQconnectdb(pool->conninfo);
    if (!conn) return NULL;

    if (PQstatus(conn) != CONNECTION_OK) {
        fprintf(stderr, "pgpool: connection failed: %s", PQerrorMessage(conn));
        PQfinish(conn);
        return NULL;
    }

    // All connections are non-blocking. Every send/flush/receive path in this
    // file is written for non-blocking sockets. PQexec must never be used on
    // these connections as its behaviour is undefined when non-blocking is set.
    if (PQsetnonblocking(conn, 1) != 0) {
        fprintf(stderr, "pgpool: PQsetnonblocking failed: %s", PQerrorMessage(conn));
        PQfinish(conn);
        return NULL;
    }

    if (pool->connection_init) pool->connection_init(conn);
    return conn;
}

static pgconn_t* create_wrapped_connection(pgpool_t* pool) {
    pgconn_t* conn = calloc(1, sizeof(*conn));
    if (!conn) return NULL;

    conn->raw_conn = create_physical_connection(pool);
    if (!conn->raw_conn) {
        free(conn);
        return NULL;
    }
    return conn;
}

static void destroy_wrapped_connection(pgpool_t* pool, pgconn_t* conn) {
    if (!conn) return;
    if (conn->raw_conn) {
        if (pool->connection_close) pool->connection_close(conn->raw_conn);
        PQfinish(conn->raw_conn);
    }
    cache_clear(conn);
    free(conn);
}

/* =========================================================================
 * Low-Level Async I/O Helpers
 *
 * All three helpers operate on a raw PGconn* so they can be called from
 * both the normal query path and the cleanup path in pgpool_release without
 * requiring a fully intact pgconn_t wrapper.
 * ======================================================================= */

/**
 * Waits for @p fd to become readable or writable.
 *
 * Retries automatically on EINTR.
 *
 * @param fd         Socket file descriptor.
 * @param timeout_ms Maximum wait in milliseconds. -1 = indefinite.
 * @param for_read   true = wait for POLLIN, false = wait for POLLOUT.
 * @return 1 if the event fired, 0 on timeout, -1 on error.
 */
static int wait_socket(int fd, int timeout_ms, bool for_read) {
    struct pollfd pfd = {.fd = fd, .events = for_read ? POLLIN : POLLOUT};
    while (true) {
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret < 0 && errno == EINTR) continue;
        return ret;
    }
}

/**
 * Flushes any buffered output for @p raw_conn to the server.
 *
 * Must be called after every PQsend* before waiting for results.
 *
 * @param raw_conn   Non-blocking libpq connection.
 * @param timeout_ms Maximum total flush time in milliseconds.
 * @return true on success, false on timeout or send error.
 */
static bool flush_output(PGconn* raw_conn, int timeout_ms) {
    int fd = PQsocket(raw_conn);
    struct timespec ts;
    time_t start_s = 0;
    long start_ns = 0;

    if (timeout_ms > 0) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        start_s = ts.tv_sec;
        start_ns = ts.tv_nsec;
    }

    while (true) {
        int flush_result = PQflush(raw_conn);
        if (flush_result == 0) return true;
        if (flush_result < 0) return false;

        int remaining = timeout_ms;
        if (timeout_ms > 0) {
            clock_gettime(CLOCK_MONOTONIC, &ts);
            long elapsed = (ts.tv_sec - start_s) * 1000 + (ts.tv_nsec - start_ns) / 1000000;
            remaining = timeout_ms - (int)elapsed;
            if (remaining <= 0) return false;
        }

        int ret = wait_socket(fd, remaining, false);
        if (ret <= 0) return false;
    }
}

/**
 * Waits for @p raw_conn to stop being busy, then returns one PGresult.
 *
 * On a non-blocking connection PQisBusy() must return false before
 * PQgetResult() is safe to call; this function enforces that invariant.
 *
 * @param raw_conn   Non-blocking libpq connection.
 * @param timeout_ms Maximum wait time in milliseconds.
 * @return A PGresult that the caller must PQclear(), or NULL on timeout /
 *         I/O error / true end-of-pipeline sentinel.
 */
static PGresult* wait_for_result(PGconn* raw_conn, int timeout_ms) {
    int fd = PQsocket(raw_conn);
    struct timespec ts;
    time_t start_s = 0;
    long start_ns = 0;

    if (timeout_ms > 0) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        start_s = ts.tv_sec;
        start_ns = ts.tv_nsec;
    }

    while (PQisBusy(raw_conn)) {
        int remaining = timeout_ms;
        if (timeout_ms > 0) {
            clock_gettime(CLOCK_MONOTONIC, &ts);
            long elapsed = (ts.tv_sec - start_s) * 1000 + (ts.tv_nsec - start_ns) / 1000000;
            remaining = timeout_ms - (int)elapsed;
            if (remaining <= 0) return NULL;
        }

        // Flush any pending write data before waiting to read; harmless if
        // nothing is queued.
        if (PQflush(raw_conn) < 0) return NULL;

        int ret = wait_socket(fd, remaining, true);
        if (ret <= 0) return NULL;

        if (!PQconsumeInput(raw_conn)) return NULL;
    }

    return PQgetResult(raw_conn);
}

/**
 * Fully drains all remaining results from @p raw_conn's result queue.
 *
 * After the caller has consumed the first (meaningful) PGresult, libpq may
 * still have a trailing NULL sentinel — or even additional result sets — in
 * its internal queue. On a non-blocking connection these may not have arrived
 * on the socket yet, so calling bare PQgetResult() in a tight loop exits
 * prematurely and leaves the connection dirty.
 *
 * This function polls the socket for each pending result, exactly like
 * wait_for_result(), until PQgetResult() returns the true NULL sentinel that
 * signals an empty pipeline.
 *
 * @param raw_conn   Non-blocking libpq connection.
 * @param timeout_ms Maximum wait per result in milliseconds.
 * @return true if all results were drained without a fatal error,
 *         false if a timeout or fatal error was encountered.
 */
static bool drain_results(PGconn* raw_conn, int timeout_ms) {
    bool ok = true;
    while (true) {
        // Poll until non-busy before calling PQgetResult — mandatory for
        // non-blocking connections to avoid premature NULL returns.
        int fd = PQsocket(raw_conn);
        struct timespec ts;
        time_t start_s = 0;
        long start_ns = 0;

        if (timeout_ms > 0) {
            clock_gettime(CLOCK_MONOTONIC, &ts);
            start_s = ts.tv_sec;
            start_ns = ts.tv_nsec;
        }

        while (PQisBusy(raw_conn)) {
            int remaining = timeout_ms;
            if (timeout_ms > 0) {
                clock_gettime(CLOCK_MONOTONIC, &ts);
                long elapsed = (ts.tv_sec - start_s) * 1000 + (ts.tv_nsec - start_ns) / 1000000;
                remaining = timeout_ms - (int)elapsed;
                if (remaining <= 0) return false;
            }

            if (PQflush(raw_conn) < 0) return false;

            int ret = wait_socket(fd, remaining, true);
            if (ret <= 0) return false;

            if (!PQconsumeInput(raw_conn)) return false;
        }

        PGresult* res = PQgetResult(raw_conn);
        if (!res) break;  // True end-of-pipeline sentinel.

        if (PQresultStatus(res) == PGRES_FATAL_ERROR) ok = false;
        PQclear(res);
    }
    return ok;
}

static bool is_result_ok(ExecStatusType status) {
    return status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK || status == PGRES_COPY_IN ||
           status == PGRES_COPY_OUT;
}

/* =========================================================================
 * Connection Cleanup (used by pgpool_release)
 *
 * Ensures a connection that is about to be returned to the pool is in a
 * clean, idle state. Any active transaction is rolled back using the full
 * async send/flush/drain path. PQexec is never used here because it is
 * undefined on non-blocking connections.
 * ======================================================================= */

/**
 * Resets @p conn to a clean idle state before it re-enters the pool.
 *
 * Inspects the transaction status and, if the connection is mid-transaction
 * or in an error state, issues an async ROLLBACK and drains the result. If
 * the rollback cannot be sent or the connection is broken, the connection is
 * marked broken so the pool will discard it.
 *
 * @param conn Wrapper whose raw_conn must be a non-blocking PGconn*.
 */
static void reset_connection_state(pgconn_t* conn) {
    if (conn->broken) return;
    if (PQstatus(conn->raw_conn) != CONNECTION_OK) {
        conn->broken = true;
        return;
    }

    PGTransactionStatusType txn = PQtransactionStatus(conn->raw_conn);

    // PQTRANS_IDLE  — clean, nothing to do.
    // PQTRANS_ACTIVE — a command is still in progress; we cannot issue ROLLBACK
    //                  until the in-flight result is drained first.
    // PQTRANS_INTRANS  — inside an open transaction; must ROLLBACK.
    // PQTRANS_INERROR  — failed transaction block; must ROLLBACK to clear.
    // PQTRANS_UNKNOWN  — connection is bad; mark broken.

    if (txn == PQTRANS_IDLE) return;

    if (txn == PQTRANS_UNKNOWN) {
        conn->broken = true;
        return;
    }

    if (txn == PQTRANS_ACTIVE) {
        // Drain whatever in-flight result is pending before we can send ROLLBACK.
        if (!drain_results(conn->raw_conn, DRAIN_TIMEOUT_MS)) {
            conn->broken = true;
            return;
        }
        // Re-check; drain may have cleared a savepoint result leaving us in INTRANS.
        txn = PQtransactionStatus(conn->raw_conn);
        if (txn == PQTRANS_IDLE) return;
        if (txn != PQTRANS_INTRANS && txn != PQTRANS_INERROR) {
            conn->broken = true;
            return;
        }
    }

    // Issue ROLLBACK via the full async path (never PQexec on non-blocking conn).
    if (!PQsendQuery(conn->raw_conn, "ROLLBACK")) {
        conn->broken = true;
        return;
    }

    if (!flush_output(conn->raw_conn, RELEASE_ROLLBACK_TIMEOUT_MS)) {
        conn->broken = true;
        return;
    }

    // Consume the ROLLBACK result plus any trailing sentinel.
    PGresult* res = wait_for_result(conn->raw_conn, RELEASE_ROLLBACK_TIMEOUT_MS);
    if (res) {
        PQclear(res);
    } else {
        conn->broken = true;
        return;
    }

    if (!drain_results(conn->raw_conn, RELEASE_ROLLBACK_TIMEOUT_MS)) {
        conn->broken = true;
        return;
    }

    // Final guard: verify the connection is truly idle before returning it.
    if (PQtransactionStatus(conn->raw_conn) != PQTRANS_IDLE) { conn->broken = true; }
}

/* =========================================================================
 * Public API
 * ======================================================================= */

pgpool_t* pgpool_create(const pgpool_config_t* config) {
    if (!config || !config->conninfo) return NULL;

    pgpool_t* pool = calloc(1, sizeof(*pool));
    if (!pool) return NULL;

    pool->conninfo = strdup(config->conninfo);
    if (!pool->conninfo) {
        free(pool);
        return NULL;
    }

    pool->min_connections = config->min_connections > 0 ? config->min_connections : DEFAULT_MIN_CONNECTIONS;
    pool->max_connections = config->max_connections > 0 ? config->max_connections : DEFAULT_MAX_CONNECTIONS;
    pool->connect_timeout = config->connect_timeout > 0 ? config->connect_timeout : DEFAULT_CONNECT_TIMEOUT;
    pool->auto_reconnect = config->auto_reconnect;
    pool->connection_init = config->connection_init;
    pool->connection_close = config->connection_close;

    if (pool->min_connections > pool->max_connections) pool->min_connections = pool->max_connections;

    pool->capacity = pool->max_connections;
    pool->connections = calloc(pool->capacity, sizeof(*pool->connections));
    if (!pool->connections) {
        free(pool->conninfo);
        free(pool);
        return NULL;
    }

    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond, NULL);

    for (size_t i = 0; i < pool->min_connections; i++) {
        pgconn_t* conn = create_wrapped_connection(pool);
        if (conn) pool->connections[pool->size++] = conn;
    }
    pool->available_count = pool->size;

    return pool;
}

/**
 * Destroys the pool and releases all resources.
 *
 * Waits up to @p timeout_ms for active connections to be returned.
 * Idle connections are always freed. If the timeout expires with active
 * connections still held, those connection structs are leaked rather than
 * freed under the caller's feet — safe during process shutdown.
 *
 * @param pool       Pool to destroy. NULL is a no-op.
 * @param timeout_ms Maximum wait in milliseconds. 0 = wait indefinitely.
 * @return true if all connections were freed cleanly, false if any leaked.
 */
bool pgpool_destroy(pgpool_t* pool, unsigned int timeout_ms) {
    if (!pool) return true;

    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = true;
    pthread_cond_broadcast(&pool->cond);

    if (timeout_ms == 0) {
        while (pool->active_count > 0 || pool->pending_connections > 0)
            pthread_cond_wait(&pool->cond, &pool->mutex);
    } else {
        struct timespec abs_timeout;
        clock_gettime(CLOCK_REALTIME, &abs_timeout);
        abs_timeout.tv_sec += timeout_ms / 1000;
        abs_timeout.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (abs_timeout.tv_nsec >= 1000000000L) {
            abs_timeout.tv_sec++;
            abs_timeout.tv_nsec -= 1000000000L;
        }
        while (pool->active_count > 0 || pool->pending_connections > 0) {
            int rc = pthread_cond_timedwait(&pool->cond, &pool->mutex, &abs_timeout);
            if (rc == ETIMEDOUT || (rc != 0 && rc != EINTR)) break;
        }
    }

    size_t leaked = 0;
    for (size_t i = 0; i < pool->size; i++) {
        if (!pool->connections[i]) continue;
        if (pool->connections[i]->in_use) {
            leaked++;
        } else {
            destroy_wrapped_connection(pool, pool->connections[i]);
            pool->connections[i] = NULL;
        }
    }
    pthread_mutex_unlock(&pool->mutex);

    free(pool->connections);
    free(pool->conninfo);
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->cond);
    free(pool);

    return leaked == 0;
}

pgconn_t* pgpool_acquire(pgpool_t* pool, int timeout_ms) {
    if (!pool) return NULL;

    struct timespec ts;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
    }

    pthread_mutex_lock(&pool->mutex);

    while (true) {
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }

        // Scan for an idle, healthy connection.
        if (pool->available_count > 0) {
            for (size_t i = 0; i < pool->size; i++) {
                pgconn_t* conn = pool->connections[i];
                if (!conn || conn->in_use || conn->broken) continue;

                // Physical health check.
                if (PQstatus(conn->raw_conn) != CONNECTION_OK) {
                    conn->broken = true;
                    pool->available_count--;
                    continue;
                }

                // Transaction-state health check: a connection is only safe
                // to hand out when libpq reports it is truly idle. Anything
                // else means reset_connection_state() failed to clean it up,
                // which should not happen — but if it does, discard rather
                // than hand a dirty connection to a caller.
                if (PQtransactionStatus(conn->raw_conn) != PQTRANS_IDLE) {
                    fprintf(stderr,
                            "pgpool: discarding connection with unexpected "
                            "transaction state %d\n",
                            (int)PQtransactionStatus(conn->raw_conn));
                    conn->broken = true;
                    pool->available_count--;
                    continue;
                }

                conn->in_use = true;
                pool->available_count--;
                pool->active_count++;
                pthread_mutex_unlock(&pool->mutex);
                return conn;
            }
            // available_count was stale (all candidates were broken); reset.
            pool->available_count = 0;
        }

        // Grow the pool if capacity allows; do the expensive work outside lock.
        if ((pool->size + pool->pending_connections) < pool->max_connections) {
            pool->pending_connections++;
            pthread_mutex_unlock(&pool->mutex);

            pgconn_t* new_conn = create_wrapped_connection(pool);

            pthread_mutex_lock(&pool->mutex);
            pool->pending_connections--;

            if (new_conn) {
                if (!pool->shutdown && pool->size < pool->max_connections) {
                    pool->connections[pool->size++] = new_conn;
                    new_conn->in_use = true;
                    pool->active_count++;
                    pthread_cond_broadcast(&pool->cond);
                    pthread_mutex_unlock(&pool->mutex);
                    return new_conn;
                }
                destroy_wrapped_connection(pool, new_conn);
            }
            pthread_cond_broadcast(&pool->cond);
            continue;
        }

        // All connections are in use; wait.
        if (timeout_ms == 0) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        } else if (timeout_ms < 0) {
            pthread_cond_wait(&pool->cond, &pool->mutex);
        } else {
            int rc = pthread_cond_timedwait(&pool->cond, &pool->mutex, &ts);
            if (rc == ETIMEDOUT) {
                pthread_mutex_unlock(&pool->mutex);
                return NULL;
            }
        }
    }
}

/**
 * Returns @p conn to the pool.
 *
 * Before re-queuing, reset_connection_state() is called to ROLLBACK any open
 * transaction and fully drain any pending results using the async I/O path.
 * This guarantees the connection is in PQTRANS_IDLE when the next caller
 * acquires it.
 *
 * @param pool Pool that owns @p conn.
 * @param conn Connection to return. NULL is a no-op.
 */
void pgpool_release(pgpool_t* pool, pgconn_t* conn) {
    if (!pool || !conn) return;

    // Clean up outside the lock: reset_connection_state() may block on I/O
    // (poll + read) for the ROLLBACK round-trip. Holding the mutex during
    // that would stall every other thread trying to acquire a connection.
    reset_connection_state(conn);

    pthread_mutex_lock(&pool->mutex);

    if (conn->broken || PQstatus(conn->raw_conn) != CONNECTION_OK) {
        if (pool->auto_reconnect) {
            // Remove from array by swapping with the last element.
            for (size_t i = 0; i < pool->size; i++) {
                if (pool->connections[i] == conn) {
                    pool->connections[i] = pool->connections[pool->size - 1];
                    pool->connections[pool->size - 1] = NULL;
                    pool->size--;
                    break;
                }
            }
            pool->active_count--;
            pthread_cond_broadcast(&pool->cond);
            pthread_mutex_unlock(&pool->mutex);

            destroy_wrapped_connection(pool, conn);
            return;
        }
        conn->broken = true;
    }

    conn->in_use = false;
    pool->active_count--;
    if (!conn->broken) pool->available_count++;

    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);
}

/* =========================================================================
 * Query Execution
 *
 * Every function follows the same pattern:
 *   1. PQsend*()        — enqueue the command (non-blocking).
 *   2. flush_output()   — push the bytes to the server.
 *   3. wait_for_result()— poll until the first result arrives.
 *   4. Inspect status.
 *   5. drain_results()  — consume the trailing NULL sentinel (and any
 *                         additional result sets) so the connection is clean.
 * ======================================================================= */

PGresult* pgpool_query(pgconn_t* conn, const char* query, int timeout_ms) {
    if (!conn || !query) return NULL;

    conn->error_msg[0] = '\0';

    if (!PQsendQuery(conn->raw_conn, query)) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "%s", PQerrorMessage(conn->raw_conn));
        return NULL;
    }

    if (!flush_output(conn->raw_conn, timeout_ms)) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "Timeout flushing query output");
        return NULL;
    }

    PGresult* result = wait_for_result(conn->raw_conn, timeout_ms);
    if (!result) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "Timeout waiting for query result");
        drain_results(conn->raw_conn, timeout_ms);
        return NULL;
    }

    if (!is_result_ok(PQresultStatus(result))) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "Query failed: %s", PQresultErrorMessage(result));
        PQclear(result);
        drain_results(conn->raw_conn, timeout_ms);
        return NULL;
    }

    drain_results(conn->raw_conn, timeout_ms);
    return result;
}

bool pgpool_execute(pgconn_t* conn, const char* query, int timeout_ms) {
    PGresult* res = pgpool_query(conn, query, timeout_ms);
    if (!res) return false;
    PQclear(res);
    return true;
}

PGresult* pgpool_query_params(pgconn_t* conn, const char* query, int n_params, const char* const* param_values,
                              int timeout_ms) {
    if (!conn || !query) return NULL;

    conn->error_msg[0] = '\0';

    if (!PQsendQueryParams(conn->raw_conn, query, n_params, NULL, param_values, NULL, NULL, 0)) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "%s", PQerrorMessage(conn->raw_conn));
        return NULL;
    }

    if (!flush_output(conn->raw_conn, timeout_ms)) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "Timeout flushing parameterised query output");
        return NULL;
    }

    PGresult* result = wait_for_result(conn->raw_conn, timeout_ms);
    if (!result) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "Timeout waiting for parameterised query result");
        drain_results(conn->raw_conn, timeout_ms);
        return NULL;
    }

    if (!is_result_ok(PQresultStatus(result))) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "Query failed: %s", PQresultErrorMessage(result));
        PQclear(result);
        drain_results(conn->raw_conn, timeout_ms);
        return NULL;
    }

    drain_results(conn->raw_conn, timeout_ms);
    return result;
}

bool pgpool_execute_params(pgconn_t* conn, const char* query, int n_params, const char* const* param_values,
                           int timeout_ms) {
    PGresult* res = pgpool_query_params(conn, query, n_params, param_values, timeout_ms);
    if (!res) return false;
    PQclear(res);
    return true;
}

/* =========================================================================
 * Prepared Statements
 * ======================================================================= */

bool pgpool_prepare(pgconn_t* conn, const char* stmt_name, const char* query, int n_params, int timeout_ms) {
    if (!conn || !stmt_name || !query) return false;

    conn->error_msg[0] = '\0';

    if (cache_statement_exists(conn, stmt_name)) return true;

    if (!PQsendPrepare(conn->raw_conn, stmt_name, query, n_params, NULL)) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "%s", PQerrorMessage(conn->raw_conn));
        return false;
    }

    if (!flush_output(conn->raw_conn, timeout_ms)) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "Timeout flushing prepare output");
        return false;
    }

    PGresult* result = wait_for_result(conn->raw_conn, timeout_ms);
    if (!result) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "Timeout waiting for prepare result");
        drain_results(conn->raw_conn, timeout_ms);
        return false;
    }

    bool success = (PQresultStatus(result) == PGRES_COMMAND_OK);
    if (!success) {
        // Tolerate "already exists" — another session or prior run may have
        // prepared the same statement on this physical connection.
        if (strstr(PQresultErrorMessage(result), "already exists")) {
            success = true;
        } else {
            snprintf(conn->error_msg, sizeof(conn->error_msg), "Prepare failed: %s", PQresultErrorMessage(result));
        }
    }
    PQclear(result);
    drain_results(conn->raw_conn, timeout_ms);

    if (success) cache_statement_add(conn, stmt_name);
    return success;
}

PGresult* pgpool_execute_prepared(pgconn_t* conn, const char* stmt_name, int n_params, const char* const* param_values,
                                  int timeout_ms) {
    if (!conn || !stmt_name) return NULL;

    conn->error_msg[0] = '\0';

    if (!PQsendQueryPrepared(conn->raw_conn, stmt_name, n_params, param_values, NULL, NULL, 0)) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "%s", PQerrorMessage(conn->raw_conn));
        return NULL;
    }

    if (!flush_output(conn->raw_conn, timeout_ms)) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "Timeout flushing prepared execute output");
        return NULL;
    }

    PGresult* result = wait_for_result(conn->raw_conn, timeout_ms);
    if (!result) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "Timeout waiting for prepared execute result");
        drain_results(conn->raw_conn, timeout_ms);
        return NULL;
    }

    if (!is_result_ok(PQresultStatus(result))) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "Exec prepared failed: %s", PQresultErrorMessage(result));
        PQclear(result);
        drain_results(conn->raw_conn, timeout_ms);
        return NULL;
    }

    drain_results(conn->raw_conn, timeout_ms);
    return result;
}

bool pgpool_deallocate(pgconn_t* conn, const char* stmt_name, int timeout_ms) {
    if (!conn || !stmt_name) return false;
    char query[256];
    snprintf(query, sizeof(query), "DEALLOCATE %s", stmt_name);
    bool ok = pgpool_execute(conn, query, timeout_ms);
    cache_statement_remove(conn, stmt_name);
    return ok;
}

/* =========================================================================
 * Transaction Helpers
 * ======================================================================= */

bool pgpool_begin(pgconn_t* conn, int timeout_ms) {
    return pgpool_execute(conn, "BEGIN", timeout_ms);
}

bool pgpool_commit(pgconn_t* conn, int timeout_ms) {
    return pgpool_execute(conn, "COMMIT", timeout_ms);
}

bool pgpool_rollback(pgconn_t* conn, int timeout_ms) {
    return pgpool_execute(conn, "ROLLBACK", timeout_ms);
}

/* =========================================================================
 * Accessors / Stats
 * ======================================================================= */

PGconn* pgpool_get_raw_connection(pgconn_t* conn) {
    return conn ? conn->raw_conn : NULL;
}

const char* pgpool_error_message(pgconn_t* conn) {
    if (!conn) return "Invalid connection";
    return conn->error_msg[0] ? conn->error_msg : PQerrorMessage(conn->raw_conn);
}

size_t pgpool_active_connections(pgpool_t* pool) {
    if (!pool) return 0;
    pthread_mutex_lock(&pool->mutex);
    size_t count = pool->active_count;
    pthread_mutex_unlock(&pool->mutex);
    return count;
}

size_t pgpool_idle_connections(pgpool_t* pool) {
    if (!pool) return 0;
    pthread_mutex_lock(&pool->mutex);
    size_t count = pool->available_count;
    pthread_mutex_unlock(&pool->mutex);
    return count;
}
