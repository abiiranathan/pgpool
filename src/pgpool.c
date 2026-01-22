#include "../include/pgpool.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_MIN_CONNECTIONS 1
#define DEFAULT_MAX_CONNECTIONS 10
#define DEFAULT_CONNECT_TIMEOUT 5
#define TIMEOUT_MSG "Timeout waiting for result"

/* --- Internal Structures --- */

/* Structure to track prepared statements per connection */
typedef struct {
    char* name;
    // We could store a hash of the query here to ensure the SQL hasn't changed
    // for the same name, but for this implementation, we assume the user
    // manages names consistently.
} prepared_stmt_t;

/** Internal connection wrapper */
struct pgconn {
    PGconn* raw_conn;
    bool in_use;
    bool broken;
    char error_msg[256];

    // Cache for prepared statements (Sorted Vector)
    prepared_stmt_t* stmts;
    size_t stmt_count;
    size_t stmt_capacity;
};

/** Connection pool structure */
struct pgpool {
    pgconn_t** connections;
    size_t capacity;
    size_t size;             // Total physical connections created
    size_t available_count;  // Optimization: fast check for idle conns

    size_t min_connections;
    size_t max_connections;
    size_t pending_connections;  // Connections currently being created (outside lock)
    size_t active_count;         // Connections held by worker.
    char* conninfo;
    int connect_timeout;
    bool auto_reconnect;

    void (*connection_init)(PGconn*);
    void (*connection_close)(PGconn*);

    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool shutdown;
};

/* --- Statement Cache Helpers --- */

static int stmt_cmp(const void* a, const void* b) {
    const prepared_stmt_t* sa = (const prepared_stmt_t*)a;
    const prepared_stmt_t* sb = (const prepared_stmt_t*)b;
    return strcmp(sa->name, sb->name);
}

static void cache_statement_add(pgconn_t* conn, const char* name) {
    if (conn->stmt_count >= conn->stmt_capacity) {
        size_t new_cap = conn->stmt_capacity == 0 ? 8 : conn->stmt_capacity * 2;
        prepared_stmt_t* new_arr = realloc(conn->stmts, new_cap * sizeof(prepared_stmt_t));
        if (!new_arr) return;  // Silent fail, we just won't cache this one
        conn->stmts = new_arr;
        conn->stmt_capacity = new_cap;
    }

    conn->stmts[conn->stmt_count].name = strdup(name);
    conn->stmt_count++;
    qsort(conn->stmts, conn->stmt_count, sizeof(prepared_stmt_t), stmt_cmp);
}

static bool cache_statement_exists(pgconn_t* conn, const char* name) {
    if (conn->stmt_count == 0) return false;
    prepared_stmt_t key = {.name = (char*)name};
    return bsearch(&key, conn->stmts, conn->stmt_count, sizeof(prepared_stmt_t), stmt_cmp) != NULL;
}

static void cache_statement_remove(pgconn_t* conn, const char* name) {
    if (conn->stmt_count == 0) return;

    prepared_stmt_t key = {.name = (char*)name};
    prepared_stmt_t* found = bsearch(&key, conn->stmts, conn->stmt_count, sizeof(prepared_stmt_t), stmt_cmp);

    if (found) {
        free(found->name);
        size_t index = (size_t)(found - conn->stmts);
        size_t move_count = conn->stmt_count - index - 1;
        if (move_count > 0) {
            memmove(found, found + 1, move_count * sizeof(prepared_stmt_t));
        }
        conn->stmt_count--;
    }
}

static void cache_clear(pgconn_t* conn) {
    for (size_t i = 0; i < conn->stmt_count; i++) {
        free(conn->stmts[i].name);
    }
    free(conn->stmts);
    conn->stmts = NULL;
    conn->stmt_count = 0;
    conn->stmt_capacity = 0;
}

/* --- Helper Functions --- */

/* Helper: create a new physical connection */
static PGconn* create_physical_connection(pgpool_t* pool) {
    PGconn* conn = PQconnectdb(pool->conninfo);
    if (!conn) return NULL;

    if (PQstatus(conn) != CONNECTION_OK) {
        PQfinish(conn);
        return NULL;
    }

    if (PQsetnonblocking(conn, 1) != 0) {
        PQfinish(conn);
        return NULL;
    }

    if (pool->connection_init) {
        pool->connection_init(conn);
    }

    return conn;
}

static pgconn_t* create_wrapped_connection(pgpool_t* pool) {
    pgconn_t* conn = calloc(1, sizeof(pgconn_t));
    if (!conn) return NULL;

    conn->raw_conn = create_physical_connection(pool);
    if (!conn->raw_conn) {
        free(conn);
        return NULL;
    }

    conn->in_use = false;
    conn->broken = false;
    return conn;
}

static void destroy_wrapped_connection(pgpool_t* pool, pgconn_t* conn) {
    if (!conn) return;

    if (conn->raw_conn) {
        if (pool->connection_close) {
            pool->connection_close(conn->raw_conn);
        }
        PQfinish(conn->raw_conn);
    }

    cache_clear(conn);  // Free statement cache
    free(conn);
}

/* Helper: Wait for socket readability */
static int wait_socket(int fd, int timeout_ms, bool for_read) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = for_read ? POLLIN : POLLOUT;

    // Handle EINTR retry loop internally
    while (true) {
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        return ret;
    }
}

static PGresult* wait_for_result(PGconn* conn, int timeout_ms) {
    int fd = PQsocket(conn);
    time_t start_s;
    long start_ns;
    struct timespec ts;

    if (timeout_ms > 0) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        start_s = ts.tv_sec;
        start_ns = ts.tv_nsec;
    }

    while (PQisBusy(conn)) {
        int remaining = timeout_ms;
        if (timeout_ms > 0) {
            clock_gettime(CLOCK_MONOTONIC, &ts);
            long elapsed_ms = (ts.tv_sec - start_s) * 1000 + (ts.tv_nsec - start_ns) / 1000000;
            remaining = timeout_ms - (int)elapsed_ms;
            if (remaining <= 0) return NULL;
        }

        // Optimization: Flush first if needed (unlikely here but safe)
        int flush_res = PQflush(conn);
        if (flush_res < 0) return NULL;

        int ret = wait_socket(fd, remaining, true);  // true = POLLIN
        if (ret <= 0) return NULL;                   // Timeout or Error

        if (!PQconsumeInput(conn)) {
            return NULL;
        }
    }
    return PQgetResult(conn);
}

static bool flush_output(PGconn* conn, int timeout_ms) {
    int fd = PQsocket(conn);
    time_t start_s;
    long start_ns;
    struct timespec ts;

    if (timeout_ms > 0) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        start_s = ts.tv_sec;
        start_ns = ts.tv_nsec;
    }

    while (true) {
        int flush_result = PQflush(conn);
        if (flush_result == 0) return true;
        if (flush_result < 0) return false;

        int remaining = timeout_ms;
        if (timeout_ms > 0) {
            clock_gettime(CLOCK_MONOTONIC, &ts);
            long elapsed_ms = (ts.tv_sec - start_s) * 1000 + (ts.tv_nsec - start_ns) / 1000000;
            remaining = timeout_ms - (int)elapsed_ms;
            if (remaining <= 0) return false;
        }

        int ret = wait_socket(fd, remaining, false);  // false = POLLOUT
        if (ret <= 0) return false;
    }
}

static bool consume_remaining_results(PGconn* conn) {
    PGresult* next;
    bool ok = true;
    while ((next = PQgetResult(conn)) != NULL) {
        if (PQresultStatus(next) == PGRES_FATAL_ERROR) ok = false;
        PQclear(next);
    }
    return ok;
}

static bool is_result_ok(ExecStatusType status) {
    return (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK || status == PGRES_COPY_IN ||
            status == PGRES_COPY_OUT);
}

/* --- API Implementation --- */

pgpool_t* pgpool_create(const pgpool_config_t* config) {
    if (!config || !config->conninfo) return NULL;

    pgpool_t* pool = calloc(1, sizeof(pgpool_t));
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
    pool->connections = calloc(pool->capacity, sizeof(pgconn_t*));
    if (!pool->connections) {
        perror("calloc");
        free(pool->conninfo);
        free(pool);
        return NULL;
    }

    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond, NULL);

    // Initial connections
    for (size_t i = 0; i < pool->min_connections; i++) {
        pgconn_t* conn = create_wrapped_connection(pool);
        if (conn) {
            pool->connections[pool->size++] = conn;
        }
    }

    pool->available_count = pool->size;

    return pool;
}

/**
 * Destroys a connection pool and releases all resources.
 * 
 * Waits up to timeout_ms for all workers to release their connections.
 * Always frees idle connections and pool metadata. If timeout expires with
 * active connections still held, those connections are leaked (safer than
 * use-after-free in active worker threads).
 * 
 * @param pool The pool to destroy. NULL is safe (no-op).
 * @param timeout_ms Maximum milliseconds to wait for graceful shutdown.
 *                   If 0, waits indefinitely.
 *                   Recommended: 5000-30000ms for production use.
 * 
 * @return true if all connections were freed (clean shutdown),
 *         false if some connections were leaked due to timeout.
 * 
 * @note On timeout, idle connections ARE freed, but active connections
 *       held by workers are leaked. This is safe during process shutdown
 *       as the OS will reclaim all resources. The pool structure itself
 *       is always freed.
 */
bool pgpool_destroy(pgpool_t* pool, unsigned int timeout_ms) {
    if (!pool) return true;

    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = true;
    pthread_cond_broadcast(&pool->cond);

    if (timeout_ms == 0) {
        // Wait indefinitely
        while (pool->active_count > 0 || pool->pending_connections > 0) {
            pthread_cond_wait(&pool->cond, &pool->mutex);
        }
    } else {
        // Wait with timeout
        struct timespec abs_timeout;
        clock_gettime(CLOCK_REALTIME, &abs_timeout);
        abs_timeout.tv_sec += timeout_ms / 1000;
        abs_timeout.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (abs_timeout.tv_nsec >= 1000000000L) {
            abs_timeout.tv_sec += abs_timeout.tv_nsec / 1000000000L;
            abs_timeout.tv_nsec %= 1000000000L;
        }

        while (pool->active_count > 0 || pool->pending_connections > 0) {
            int wait_result = pthread_cond_timedwait(&pool->cond, &pool->mutex, &abs_timeout);
            if (wait_result == ETIMEDOUT) {
                break;  // Proper timeout
            } else if (wait_result != 0 && wait_result != EINTR) {
                break;  // pthread_cond_timedwait failed
            }
        }
    }

    // Count active connections for return value
    size_t active_count = 0;

    // Free all idle connections (safe - not held by any worker)
    for (size_t i = 0; i < pool->size; i++) {
        if (pool->connections[i]) {
            if (pool->connections[i]->in_use) {
                active_count++;
            } else {
                destroy_wrapped_connection(pool, pool->connections[i]);
                pool->connections[i] = NULL;
            }
        }
    }

    pthread_mutex_unlock(&pool->mutex);

    // Always free pool metadata
    free(pool->connections);
    free(pool->conninfo);
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->cond);
    free(pool);
    return (active_count == 0);
}

pgconn_t* pgpool_acquire(pgpool_t* pool, int timeout_ms) {
    if (!pool) return NULL;

    struct timespec ts;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
    }

    pthread_mutex_lock(&pool->mutex);

    while (true) {
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }

        // 1. Try to find an existing idle connection
        if (pool->available_count > 0) {
            for (size_t i = 0; i < pool->size; i++) {
                pgconn_t* conn = pool->connections[i];
                if (!conn->in_use && !conn->broken) {
                    conn->in_use = true;
                    pool->available_count--;

                    // Light health check (non-blocking)
                    if (PQstatus(conn->raw_conn) != CONNECTION_OK) {
                        conn->broken = true;
                        // Don't decrement available_count here, loop will continue
                        // and find next or fail
                        conn->in_use = false;  // Release logical lock
                        continue;
                    }

                    pthread_mutex_unlock(&pool->mutex);
                    return conn;
                }
            }
        }

        // 2. Optimization: If we can create a new connection, do it OUTSIDE the lock
        if ((pool->size + pool->pending_connections) < pool->max_connections) {
            pool->pending_connections++;
            pthread_mutex_unlock(&pool->mutex);

            // Heavy lifting (DNS, SSL, Handshake) happens here
            pgconn_t* new_conn = create_wrapped_connection(pool);

            pthread_mutex_lock(&pool->mutex);
            pool->pending_connections--;

            if (new_conn) {
                if (!pool->shutdown && pool->size < pool->max_connections) {
                    pool->connections[pool->size++] = new_conn;
                    new_conn->in_use = true;  // We grabbed it immediately
                    pthread_mutex_unlock(&pool->mutex);
                    return new_conn;
                } else {
                    // Race condition: pool filled up or shutdown while we were creating
                    destroy_wrapped_connection(pool, new_conn);
                }
            }
            // If creation failed or race lost, loop again
            continue;
        }

        // 3. Wait
        if (timeout_ms == 0) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        } else if (timeout_ms < 0) {
            pthread_cond_wait(&pool->cond, &pool->mutex);
        } else {
            int ret = pthread_cond_timedwait(&pool->cond, &pool->mutex, &ts);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&pool->mutex);
                return NULL;
            }
        }
    }
}

void pgpool_release(pgpool_t* pool, pgconn_t* conn) {
    if (!pool || !conn) return;

    // Optimization: Check if the user left the connection inside a transaction
    // This is done before locking the pool to avoid holding the lock during cleanup.
    if (!conn->broken && PQstatus(conn->raw_conn) == CONNECTION_OK) {
        PGTransactionStatusType txn_status = PQtransactionStatus(conn->raw_conn);
        if (txn_status == PQTRANS_INTRANS || txn_status == PQTRANS_INERROR || txn_status == PQTRANS_UNKNOWN) {
            // User forgot to commit/rollback. Rollback now.
            PGresult* res = PQexec(conn->raw_conn, "ROLLBACK");
            PQclear(res);
        }
    }

    pthread_mutex_lock(&pool->mutex);

    // Standard health check
    if (conn->broken || PQstatus(conn->raw_conn) != CONNECTION_OK) {
        if (pool->auto_reconnect) {
            // We do the reconnect logic.
            // Ideally, we should do this asynchronously or outside the lock,
            // but for simplicity in release(), we mark broken and let the next acquire fix it
            // or fix it here. To avoid blocking release, we just destroy it.
            // The next acquire will see size < max and create a new one.

            // Remove from array (swap with last)
            for (size_t i = 0; i < pool->size; i++) {
                if (pool->connections[i] == conn) {
                    pool->connections[i] = pool->connections[pool->size - 1];
                    pool->connections[pool->size - 1] = NULL;
                    pool->size--;
                    break;
                }
            }
            // Unlock before destroying to allow others to work
            pthread_mutex_unlock(&pool->mutex);
            destroy_wrapped_connection(pool, conn);

            pthread_mutex_lock(&pool->mutex);
            // Notify waiters that a slot (size < max) opened up
            pthread_cond_broadcast(&pool->cond);
            pthread_mutex_unlock(&pool->mutex);
            return;
        } else {
            conn->broken = true;
        }
    }

    conn->in_use = false;
    if (!conn->broken) {
        pool->available_count++;
    }

    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);
}

/* --- Query Execution --- */

PGresult* pgpool_query(pgconn_t* conn, const char* query, int timeout_ms) {
    if (!conn || !query) return NULL;

    // Attempt non-blocking send
    if (!PQsendQuery(conn->raw_conn, query)) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "%s", PQerrorMessage(conn->raw_conn));
        return NULL;
    }

    if (!flush_output(conn->raw_conn, timeout_ms)) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "Timeout flushing output");
        return NULL;
    }

    PGresult* result = wait_for_result(conn->raw_conn, timeout_ms);
    if (!result) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), TIMEOUT_MSG);
        return NULL;
    }

    ExecStatusType status = PQresultStatus(result);
    if (!is_result_ok(status)) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "Query failed: %s", PQresultErrorMessage(result));
        PQclear(result);
        consume_remaining_results(conn->raw_conn);
        return NULL;
    }

    consume_remaining_results(conn->raw_conn);
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

    if (!PQsendQueryParams(conn->raw_conn, query, n_params, NULL, param_values, NULL, NULL, 0)) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "%s", PQerrorMessage(conn->raw_conn));
        return NULL;
    }

    if (!flush_output(conn->raw_conn, timeout_ms)) return NULL;

    PGresult* result = wait_for_result(conn->raw_conn, timeout_ms);
    if (!result) return NULL;

    if (!is_result_ok(PQresultStatus(result))) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "Query failed: %s", PQresultErrorMessage(result));
        PQclear(result);
        consume_remaining_results(conn->raw_conn);
        return NULL;
    }
    consume_remaining_results(conn->raw_conn);
    return result;
}

/* --- Optimized Prepare/Execute --- */

bool pgpool_prepare(pgconn_t* conn, const char* stmt_name, const char* query, int n_params, int timeout_ms) {
    if (!conn || !stmt_name || !query) return false;

    // Optimization: Check Client-Side Cache
    if (cache_statement_exists(conn, stmt_name)) {
        return true;  // Already prepared on this physical connection
    }

    if (!PQsendPrepare(conn->raw_conn, stmt_name, query, n_params, NULL)) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "%s", PQerrorMessage(conn->raw_conn));
        return false;
    }

    if (!flush_output(conn->raw_conn, timeout_ms)) return false;

    PGresult* result = wait_for_result(conn->raw_conn, timeout_ms);
    if (!result) return false;

    ExecStatusType status = PQresultStatus(result);
    bool success = (status == PGRES_COMMAND_OK);

    if (!success) {
        // Handle case where statement already exists on DB but not in our cache (e.g. from manual SQL)
        if (strstr(PQresultErrorMessage(result), "already exists")) {
            success = true;
        } else {
            snprintf(conn->error_msg, sizeof(conn->error_msg), "Prepare failed: %s", PQresultErrorMessage(result));
        }
    }

    PQclear(result);
    consume_remaining_results(conn->raw_conn);

    if (success) {
        cache_statement_add(conn, stmt_name);
    }

    return success;
}

PGresult* pgpool_execute_prepared(pgconn_t* conn, const char* stmt_name, int n_params, const char* const* param_values,
                                  int timeout_ms) {
    if (!conn || !stmt_name) return NULL;
    // Note: We don't check cache here. If it's not prepared, DB returns error.
    // Checking cache is optimization for pgpool_prepare, not execute.
    if (!PQsendQueryPrepared(conn->raw_conn, stmt_name, n_params, param_values, NULL, NULL, 0)) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "%s", PQerrorMessage(conn->raw_conn));
        return NULL;
    }

    if (!flush_output(conn->raw_conn, timeout_ms)) return NULL;

    PGresult* result = wait_for_result(conn->raw_conn, timeout_ms);
    if (!result) return NULL;

    if (!is_result_ok(PQresultStatus(result))) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "Exec prepared failed: %s", PQresultErrorMessage(result));
        PQclear(result);
        consume_remaining_results(conn->raw_conn);
        return NULL;
    }
    consume_remaining_results(conn->raw_conn);
    return result;
}

bool pgpool_deallocate(pgconn_t* conn, const char* stmt_name, int timeout_ms) {
    if (!conn || !stmt_name) return false;

    char query[256];
    snprintf(query, sizeof(query), "DEALLOCATE %s", stmt_name);

    bool res = pgpool_execute(conn, query, timeout_ms);

    // Always remove from cache if we attempt to deallocate
    cache_statement_remove(conn, stmt_name);
    return res;
}

/* --- Transaction Helpers --- */

bool pgpool_begin(pgconn_t* conn, int timeout_ms) {
    return pgpool_execute(conn, "BEGIN", timeout_ms);
}
bool pgpool_commit(pgconn_t* conn, int timeout_ms) {
    return pgpool_execute(conn, "COMMIT", timeout_ms);
}
bool pgpool_rollback(pgconn_t* conn, int timeout_ms) {
    return pgpool_execute(conn, "ROLLBACK", timeout_ms);
}

PGconn* pgpool_get_raw_connection(pgconn_t* conn) {
    return conn ? conn->raw_conn : NULL;
}

const char* pgpool_error_message(pgconn_t* conn) {
    if (!conn) return "Invalid connection";
    return conn->error_msg[0] ? conn->error_msg : PQerrorMessage(conn->raw_conn);
}

/* --- Stats --- */

size_t pgpool_active_connections(pgpool_t* pool) {
    if (!pool) return 0;
    pthread_mutex_lock(&pool->mutex);
    size_t count = 0;
    for (size_t i = 0; i < pool->size; i++)
        if (pool->connections[i]->in_use) count++;
    pthread_mutex_unlock(&pool->mutex);
    return count;
}

size_t pgpool_idle_connections(pgpool_t* pool) {
    if (!pool) return 0;

    pthread_mutex_lock(&pool->mutex);
    size_t count = 0;
    for (size_t i = 0; i < pool->size; i++) {
        if (!pool->connections[i]->in_use && !pool->connections[i]->broken) {
            count++;
        }
    }
    pthread_mutex_unlock(&pool->mutex);

    return count;
}
