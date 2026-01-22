#include "../include/pgpool.h"
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Global pool for signal handler access
static pgpool_t* g_pool = NULL;
static volatile sig_atomic_t shutdown_requested = 0;

/**
 * Signal handler for graceful shutdown.
 * Sets flag and initiates pool destruction with timeout.
 */
static void signal_handler(int signum) {
    (void)signum;  // Unused
    shutdown_requested = 1;
    fprintf(stderr, "\nShutdown signal received, cleaning up...\n");
}

/**
 * Worker thread that holds a connection and simulates work.
 * Used to test forced shutdown scenarios.
 */
typedef struct {
    pgpool_t* pool;
    int worker_id;
    int sleep_seconds;
    bool release_connection;
} worker_args_t;

static void* worker_thread(void* arg) {
    worker_args_t* args = (worker_args_t*)arg;

    printf("[Worker %d] Starting, will work for %d seconds\n", args->worker_id, args->sleep_seconds);

    // Acquire connection
    pgconn_t* conn = pgpool_acquire(args->pool, 5000);
    if (!conn) {
        fprintf(stderr, "[Worker %d] Failed to acquire connection\n", args->worker_id);
        return NULL;
    }

    printf("[Worker %d] Connection acquired\n", args->worker_id);

    // Simulate work by executing queries periodically
    for (int i = 0; i < args->sleep_seconds && !shutdown_requested; i++) {
        PGresult* res = pgpool_query(conn, "SELECT pg_sleep(1)", 2000);
        if (res) {
            PQclear(res);
            printf("[Worker %d] Query %d/%d completed\n", args->worker_id, i + 1, args->sleep_seconds);
        } else {
            fprintf(stderr, "[Worker %d] Query failed: %s\n", args->worker_id, pgpool_error_message(conn));
            break;
        }
    }

    if (args->release_connection) {
        printf("[Worker %d] Releasing connection normally\n", args->worker_id);
        pgpool_release(args->pool, conn);
    } else {
        printf("[Worker %d] INTENTIONALLY NOT RELEASING (simulating bug/hang)\n", args->worker_id);
        // Leak the connection to simulate a stuck worker
    }

    printf("[Worker %d] Exiting\n", args->worker_id);
    return NULL;
}

/**
 * Demonstrates forced shutdown when workers don't release connections in time.
 */
static void test_forced_shutdown(const char* conninfo) {
    printf("\n=== FORCED SHUTDOWN TEST ===\n");
    printf("This test simulates workers that don't release connections in time.\n");
    printf("Press Ctrl+C to trigger shutdown, or wait for automatic timeout.\n\n");

    // Setup signal handler
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    pgpool_config_t config = {
      .conninfo = conninfo,
      .min_connections = 2,
      .max_connections = 5,
      .connect_timeout = 5,
      .auto_reconnect = true,
      .connection_init = NULL,
      .connection_close = NULL,
    };

    g_pool = pgpool_create(&config);
    if (!g_pool) {
        fprintf(stderr, "Failed to create pool\n");
        return;
    }

    printf("Pool created. Starting workers...\n\n");

    // Create workers with different behaviors
    pthread_t threads[3];
    worker_args_t worker_args[3] = {
      // Worker 0: Works for 3 seconds, releases normally
      {.pool = g_pool, .worker_id = 0, .sleep_seconds = 3, .release_connection = true},
      // Worker 1: Works for 10 seconds, releases normally (might timeout)
      {.pool = g_pool, .worker_id = 1, .sleep_seconds = 10, .release_connection = true},
      // Worker 2: Works for 5 seconds, NEVER releases (simulates bug)
      {.pool = g_pool, .worker_id = 2, .sleep_seconds = 5, .release_connection = false},
    };

    for (int i = 0; i < 3; i++) {
        pthread_create(&threads[i], NULL, worker_thread, &worker_args[i]);
    }

    // Wait a bit to let workers start
    sleep(2);

    printf("\nPool status:\n");
    printf("  Idle: %zu, Active: %zu\n", pgpool_idle_connections(g_pool), pgpool_active_connections(g_pool));

    // Simulate either user pressing Ctrl+C or automatic shutdown after 6 seconds
    printf("\nWaiting for shutdown signal or 6 second timeout...\n");
    for (int i = 0; i < 6 && !shutdown_requested; i++) {
        sleep(1);
    }

    if (!shutdown_requested) {
        printf("\nAutomatic shutdown timeout reached\n");
        shutdown_requested = 1;
    }

    // Attempt graceful shutdown with 3 second timeout
    printf("\nDestroying pool with 3 second timeout...\n");
    printf("Expected: Worker 0 finishes, Workers 1&2 cause timeout\n\n");

    pgpool_destroy(g_pool, 3000);

    // Don't join threads - they may still be running and that's OK
    // In a real shutdown, the process would exit here
    sleep(2);
    printf("\n=== FORCED SHUTDOWN TEST COMPLETE ===\n");
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <conninfo> [--test-forced-shutdown]\n", argv[0]);
        fprintf(stderr, "Example: %s \"host=localhost dbname=mydb user=postgres\"\n", argv[0]);
        fprintf(stderr, "\nOptions:\n");
        fprintf(stderr, "  --test-forced-shutdown  Run forced shutdown simulation\n");
        return 1;
    }

    const char* conninfo = argv[1];

    // Check if forced shutdown test requested
    if (argc >= 3 && strcmp(argv[2], "--test-forced-shutdown") == 0) {
        test_forced_shutdown(conninfo);
        return 0;
    }

    // ===== Normal Operation Tests =====

    // Configure the pool
    pgpool_config_t config = {
      .conninfo = conninfo,
      .min_connections = 2,
      .max_connections = 10,
      .connect_timeout = 5,
      .auto_reconnect = true,
      .connection_init = NULL,
      .connection_close = NULL,
    };

    // Create the pool
    pgpool_t* pool = pgpool_create(&config);
    if (!pool) {
        fprintf(stderr, "Failed to create connection pool\n");
        return 1;
    }

    printf("Pool created successfully\n");
    printf("Idle connections: %zu\n", pgpool_idle_connections(pool));
    printf("Active connections: %zu\n", pgpool_active_connections(pool));

    // Acquire a connection
    pgconn_t* conn = pgpool_acquire(pool, 5000);  // 5 second timeout
    if (!conn) {
        fprintf(stderr, "Failed to acquire connection\n");
        pgpool_destroy(pool, 1000);
        return 1;
    }

    printf("\nConnection acquired\n");
    printf("Idle connections: %zu\n", pgpool_idle_connections(pool));
    printf("Active connections: %zu\n", pgpool_active_connections(pool));

    // Execute a simple query
    printf("\n--- Testing Simple Query ---\n");
    PGresult* res = pgpool_query(conn, "SELECT version()", 5000);
    if (res) {
        // No need to check status - pgpool_query already verified it
        printf("PostgreSQL Version: %s\n", PQgetvalue(res, 0, 0));
        PQclear(res);
    } else {
        fprintf(stderr, "Query failed: %s\n", pgpool_error_message(conn));
    }

    // Test parameterized query
    printf("\n--- Testing Parameterized Query ---\n");
    const char* params[] = {"test_value"};
    res = pgpool_query_params(conn, "SELECT $1::text as value", 1, params, 5000);
    if (res) {
        // No need to check status - already verified
        printf("Returned value: %s\n", PQgetvalue(res, 0, 0));
        PQclear(res);
    } else {
        fprintf(stderr, "Parameterized query failed: %s\n", pgpool_error_message(conn));
    }

    // Test prepared statement
    printf("\n--- Testing Prepared Statement ---\n");
    if (pgpool_prepare(conn, "test_stmt", "SELECT $1::int + $2::int as sum", 2, 5000)) {
        printf("Statement prepared successfully\n");

        const char* sum_params[] = {"10", "20"};
        res = pgpool_execute_prepared(conn, "test_stmt", 2, sum_params, 5000);
        if (res) {
            // No need to check status - already verified
            printf("Sum result: %s\n", PQgetvalue(res, 0, 0));
            PQclear(res);
        }
    } else {
        fprintf(stderr, "Prepare failed: %s\n", pgpool_error_message(conn));
    }

    // Test transaction
    printf("\n--- Testing Transaction ---\n");
    if (pgpool_begin(conn, 5000)) {
        printf("Transaction started\n");

        bool success = pgpool_execute(conn, "SELECT 1", 5000);
        printf("Query in transaction: %s\n", success ? "SUCCESS" : "FAILED");

        if (pgpool_commit(conn, 5000)) {
            printf("Transaction committed\n");
        } else {
            printf("Transaction commit failed\n");
        }
    }

    // Release connection back to pool
    pgpool_release(pool, conn);
    printf("\nConnection released\n");
    printf("Idle connections: %zu\n", pgpool_idle_connections(pool));
    printf("Active connections: %zu\n", pgpool_active_connections(pool));

    // Cleanup
    pgpool_destroy(pool, 5000);
    printf("\nPool destroyed\n");
    return 0;
}
