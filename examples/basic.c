#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  // for usleep
#include "../include/pgpool.h"
#include "../include/pgtypes.h"

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

/* =========================================================================
 * Multithreaded Transaction Test
 * ======================================================================= */

typedef struct {
    pgpool_t* pool;
    int thread_id;
    int iterations;
} tx_worker_args_t;

static void* tx_worker_thread(void* arg) {
    tx_worker_args_t* args = (tx_worker_args_t*)arg;

    for (int i = 0; i < args->iterations; i++) {
        pgconn_t* conn = pgpool_acquire(args->pool, 5000);
        if (!conn) {
            fprintf(stderr, "[TX Worker %d] Failed to acquire connection\n", args->thread_id);
            continue;
        }

        // Begin transaction block
        if (!pgpool_begin(conn, 5000)) {
            fprintf(stderr, "[TX Worker %d] BEGIN failed: %s\n", args->thread_id, pgpool_error_message(conn));
            pgpool_release(args->pool, conn);
            continue;
        }

        // Alternate transfer directions based on iteration / thread ID
        const char* query1;
        const char* query2;
        if ((args->thread_id + i) % 2 == 0) {
            query1 = "UPDATE pgpool_tx_test SET balance = balance - 10 WHERE id = 1";
            query2 = "UPDATE pgpool_tx_test SET balance = balance + 10 WHERE id = 2";
        } else {
            query1 = "UPDATE pgpool_tx_test SET balance = balance - 10 WHERE id = 2";
            query2 = "UPDATE pgpool_tx_test SET balance = balance + 10 WHERE id = 1";
        }

        bool success = pgpool_execute(conn, query1, 5000) && pgpool_execute(conn, query2, 5000);

        if (success) {
            // Intentionally rollback 10% of the transactions to verify rollback behavior
            if (i % 10 == 9) {
                if (!pgpool_rollback(conn, 5000)) {
                    fprintf(stderr, "[TX Worker %d] ROLLBACK failed: %s\n", args->thread_id,
                            pgpool_error_message(conn));
                }
            } else {
                if (!pgpool_commit(conn, 5000)) {
                    fprintf(stderr, "[TX Worker %d] COMMIT failed: %s\n", args->thread_id, pgpool_error_message(conn));
                }
            }
        } else {
            // Rollback on execution error
            pgpool_rollback(conn, 5000);
        }

        pgpool_release(args->pool, conn);

        // Brief sleep to yield control and mix execution order
        usleep(10000);  // 10ms
    }

    return NULL;
}

static void test_multithreaded_transactions(const char* conninfo) {
    printf("\n=== MULTITHREADED TRANSACTION TEST ===\n");
    printf("Simulating concurrent balance transfers using a connection pool...\n");

    pgpool_config_t config = {
        .conninfo = conninfo,
        .min_connections = 4,
        .max_connections = 8,
        .connect_timeout = 5,
        .auto_reconnect = true,
        .connection_init = NULL,
        .connection_close = NULL,
    };

    pgpool_t* pool = pgpool_create(&config);
    if (!pool) {
        fprintf(stderr, "Failed to create connection pool\n");
        return;
    }

    // Set up database table structure
    pgconn_t* conn = pgpool_acquire(pool, 5000);
    if (!conn) {
        fprintf(stderr, "Failed to acquire setup connection\n");
        pgpool_destroy(pool, 1000);
        return;
    }

    pgpool_execute(conn, "DROP TABLE IF EXISTS pgpool_tx_test", 5000);
    if (!pgpool_execute(conn, "CREATE TABLE pgpool_tx_test (id INT PRIMARY KEY, balance INT)", 5000)) {
        fprintf(stderr, "Failed to create test table: %s\n", pgpool_error_message(conn));
        pgpool_release(pool, conn);
        pgpool_destroy(pool, 1000);
        return;
    }

    pgpool_execute(conn, "INSERT INTO pgpool_tx_test (id, balance) VALUES (1, 500), (2, 500)", 5000);
    pgpool_release(pool, conn);

    printf("Table 'pgpool_tx_test' created. Initial balances: ID 1 = $500, ID 2 = $500. Total = $1000.\n");

#define NUM_TX_THREADS        4
#define ITERATIONS_PER_THREAD 30
    pthread_t threads[NUM_TX_THREADS];
    tx_worker_args_t thread_args[NUM_TX_THREADS];

    printf("Spawning %d threads, each executing %d transfer operations...\n", NUM_TX_THREADS, ITERATIONS_PER_THREAD);

    for (int i = 0; i < NUM_TX_THREADS; i++) {
        thread_args[i].pool = pool;
        thread_args[i].thread_id = i;
        thread_args[i].iterations = ITERATIONS_PER_THREAD;
        pthread_create(&threads[i], NULL, tx_worker_thread, &thread_args[i]);
    }

    for (int i = 0; i < NUM_TX_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    // Acquire connection to evaluate test results
    conn = pgpool_acquire(pool, 5000);
    if (conn) {
        PGresult* res = pgpool_query(conn, "SELECT id, balance FROM pgpool_tx_test ORDER BY id", 5000);
        if (res && PQntuples(res) >= 2) {
            int b1 = atoi(PQgetvalue(res, 0, 1));
            int b2 = atoi(PQgetvalue(res, 1, 1));
            int total = b1 + b2;

            printf("\nFinal balance check:\n");
            printf("  Account 1: $%d\n", b1);
            printf("  Account 2: $%d\n", b2);
            printf("  Total Sum: $%d (Expected: $1000)\n", total);

            if (total == 1000) {
                printf("Result: Consistent transaction states maintained.\n");
            } else {
                fprintf(stderr, "Result Error: Transaction isolation or rollback anomaly. Balance is %d.\n", total);
            }
            PQclear(res);
        } else {
            fprintf(stderr, "Failed to query result table: %s\n", pgpool_error_message(conn));
        }

        pgpool_execute(conn, "DROP TABLE pgpool_tx_test", 5000);
        pgpool_release(pool, conn);
    }

    pgpool_destroy(pool, 5000);
    printf("=== MULTITHREADED TRANSACTION TEST COMPLETE ===\n");
}

/* =========================================================================
 * Main Entrypoint
 * ======================================================================= */

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <conninfo> [--test-forced-shutdown | --test-multithread-tx]\n", argv[0]);
        fprintf(stderr, "Example: %s \"host=localhost dbname=mydb user=postgres\"\n", argv[0]);
        fprintf(stderr, "\nOptions:\n");
        fprintf(stderr, "  --test-forced-shutdown   Run forced pool shutdown simulation\n");
        fprintf(stderr, "  --test-multithread-tx    Run only the multithreaded transaction tests\n");
        return 1;
    }

    const char* conninfo = argv[1];

    // Branch to specific test modes if requested
    if (argc >= 3) {
        if (strcmp(argv[2], "--test-forced-shutdown") == 0) {
            test_forced_shutdown(conninfo);
            return 0;
        } else if (strcmp(argv[2], "--test-multithread-tx") == 0) {
            test_multithreaded_transactions(conninfo);
            return 0;
        }
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
            printf("Sum result: %s\n", PQgetvalue(res, 0, 0));
            PQclear(res);
        }
    } else {
        fprintf(stderr, "Prepare failed: %s\n", pgpool_error_message(conn));
    }

    // Test transaction
    printf("\n--- Testing Single-threaded Transaction ---\n");
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

    // Select created_at from users table if available
    res = pgpool_query(conn, "SELECT created_at FROM users LIMIT 10", -1);
    if (res) {
        int n = PQntuples(res);
        printf("Users: %d\n", n);
        bool valid;
        for (int i = 0; i < n; i++) {
            char buf[64];
            const char* val = PQgetvalue(res, i, 0);
            xtime_t dt = pg_get_timestamp(res, i, 0, &valid);
            xtime_format(&dt, XTIME_FMT_POSTGRES_TZ, buf, sizeof(buf));
            printf("Formatted: %s, Original: %s\n", buf, val);
        }
        PQclear(res);
    }

    // Release connection back to pool
    pgpool_release(pool, conn);
    printf("\nConnection released\n");
    printf("Idle connections: %zu\n", pgpool_idle_connections(pool));
    printf("Active connections: %zu\n", pgpool_active_connections(pool));

    // Cleanup first pool session
    pgpool_destroy(pool, 5000);
    printf("\nPool destroyed\n");

    // Execute multithreaded transaction validation
    test_multithreaded_transactions(conninfo);

    return 0;
}
