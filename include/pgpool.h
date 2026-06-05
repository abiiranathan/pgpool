#ifndef PGPOOL_H
#define PGPOOL_H

/**
 * @file pgpool.h
 * @brief Public API for a libpq-based PostgreSQL connection pool.
 *
 * The pool manages a bounded set of non-blocking connections that can be
 * acquired and released by callers. Helpers are provided to run queries,
 * parameterized queries, and prepared statements with optional timeouts.
 */

#include <libpq-fe.h>
#include <solidc/defer.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque type for the pool wrapper. */
typedef struct pgpool pgpool_t;

/** Opaque type for the connection wrapper. */
typedef struct pgconn pgconn_t;

/**
 * Connection pool configuration.
 */
typedef struct {
    // Required parameters
    const char* conninfo;  // PostgreSQL connection string

    // Optional parameters with defaults
    size_t min_connections;  // Minimum number of connections to maintain (default: 1)
    size_t max_connections;  // Maximum number of connections (default: 10)
    int connect_timeout;     // Connection timeout in seconds (default: 5)
    bool auto_reconnect;     // Automatically reconnect broken connections (default: true)

    // Callbacks
    void (*connection_init)(PGconn*);   // Called when a new connection is established
    void (*connection_close)(PGconn*);  // Called before closing a connection
} pgpool_config_t;

/**
 * Create and initialize a connection pool.
 * @param config Pool configuration (must provide `conninfo`).
 * @return Initialized pool handle, or NULL on failure.
 */
pgpool_t* pgpool_create(const pgpool_config_t* config);

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
bool pgpool_destroy(pgpool_t* pool, unsigned int timeout_ms);

/**
 * Acquire a connection from the pool.
 * @param pool Pool handle.
 * @param timeout_ms <0 wait forever; 0 return immediately; >0 wait up to ms.
 * @return Connection handle, or NULL on timeout/failure.
 */
pgconn_t* pgpool_acquire(pgpool_t* pool, int timeout_ms);

/** Release a previously acquired connection back to the pool. */
void pgpool_release(pgpool_t* pool, pgconn_t* conn);

/**
 * Execute a query without returning a result.
 * @return true if server returned PGRES_TUPLES_OK or PGRES_COMMAND_OK.
 */
bool pgpool_execute(pgconn_t* conn, const char* query, int timeout_ms);

/**
 * Execute a query and return the result to the caller.
 * Caller must `PQclear` the result.
 */
PGresult* pgpool_query(pgconn_t* conn, const char* query, int timeout_ms);

/**
 * Execute a parameterized query and return the result.
 * Caller must `PQclear` the returned result.
 * @param param_values Array of parameter values as strings (NULL for SQL NULL)
 */
PGresult* pgpool_query_params(pgconn_t* conn, const char* query, int n_params,
                              const char* const* param_values, int timeout_ms);

/**
 * Execute a parameterized query without returning a result.
 * @return true if server returned PGRES_TUPLES_OK or PGRES_COMMAND_OK.
 */
bool pgpool_execute_params(pgconn_t* conn, const char* query, int n_params,
                           const char* const* param_values, int timeout_ms);

/**
 * Prepare a named statement on the connection.
 * 
 * Prepared statements are cached per physical connection and persist across
 * pgpool_acquire()/pgpool_release() cycles. Subsequent calls with the same
 * statement name on the same physical connection are no-ops (cache hit).
 * 
 * Best Practice:
 * - Prepare once, execute many times - no need to re-prepare or deallocate
 * - Use consistent statement names across your application
 * - Statements are automatically cleaned up when connection is destroyed
 * 
 * @param conn The connection handle.
 * @param stmt_name Unique statement name (e.g., "get_user_by_id").
 * @param query SQL with $1..$n placeholders.
 * @param n_params Number of parameters in the statement.
 * @param timeout_ms Query timeout in milliseconds.
 * @return true on success, false on error (check pgpool_error_message()).
 * 
 * @note Thread-safe: Different threads can prepare different statements
 *       on different connections concurrently.
 */
bool pgpool_prepare(pgconn_t* conn, const char* stmt_name, const char* query, int n_params,
                    int timeout_ms);

/**
 * Execute a previously prepared statement.
 * Caller must `PQclear` the returned result on success.
 */
PGresult* pgpool_execute_prepared(pgconn_t* conn, const char* stmt_name, int n_params,
                                  const char* const* param_values, int timeout_ms);

/**
 * Deallocates a prepared statement on the server.
 * 
 * Optional: Only needed in special cases such as:
 * - Freeing statement slots if hitting PostgreSQL's limit
 * - Replacing a statement with different SQL for the same name
 * - Debugging statement lifetime issues
 * 
 * In normal usage, prepared statements are cached per connection and
 * automatically cleaned up when the connection is destroyed. There is
 * no need to deallocate statements between uses.
 * 
 * @param conn The connection handle.
 * @param stmt_name The name of the statement to deallocate.
 * @param timeout_ms Query timeout in milliseconds.
 * @return true on success, false on failure.
 */
bool pgpool_deallocate(pgconn_t* conn, const char* stmt_name, int timeout_ms);

/** Begin a transaction on a pooled connection. */
bool pgpool_begin(pgconn_t* conn, int timeout_ms);

/** Commit a transaction on a pooled connection. */
bool pgpool_commit(pgconn_t* conn, int timeout_ms);

/** Rollback a transaction on a pooled connection. */
bool pgpool_rollback(pgconn_t* conn, int timeout_ms);

/** Get the underlying libpq `PGconn*` (use with caution). */
PGconn* pgpool_get_raw_connection(pgconn_t* conn);

/** Return the last error message for a connection. */
const char* pgpool_error_message(pgconn_t* conn);

/** Get the number of active (in-use) connections in the pool. */
size_t pgpool_active_connections(pgpool_t* pool);

/** Get the number of idle connections in the pool. */
size_t pgpool_idle_connections(pgpool_t* pool);

#define defer_pqclear(res)  \
    defer {                 \
        if ((res)) {        \
            PQclear((res)); \
        }                   \
    }

#define defer_release(pool, conn)           \
    defer {                                 \
        if ((conn)) {                       \
            pgpool_release((pool), (conn)); \
        }                                   \
    }

#define defer_deallocate(conn, stmt_name, timeout_ms)             \
    defer {                                                       \
        if ((conn)) {                                             \
            pgpool_deallocate((conn), (stmt_name), (timeout_ms)); \
        }                                                         \
    }

#define defer_destroy(pool, timeout_ms)           \
    defer {                                       \
        if ((pool)) {                             \
            pgpool_destroy((pool), (timeout_ms)); \
        }                                         \
    }

#ifdef __cplusplus
}
#endif

#endif  // PGPOOL_H
