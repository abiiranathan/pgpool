# pgpool - PostgreSQL Connection Pool Library

A robust, thread-safe PostgreSQL connection pool library for C/C++ applications using libpq.

## Features

- **Thread-safe connection pooling** with configurable min/max connections
- **Non-blocking I/O** with timeout support on all operations
- **Automatic reconnection** for broken connections
- **Simplified API** - removed rarely-used NULL parameters
- **Prepared statements** with full lifecycle management
- **Transaction support** with begin/commit/rollback
- **Cross-platform** - works on Linux, macOS, and Windows

## Project Structure

```
pgpool/
├── CMakeLists.txt
├── README.md
├── include/
│   └── pgpool.h
├── src/
│   └── pgpool.c
├── cmake/
│   ├── pgpoolConfig.cmake.in
│   └── pgpool.pc.in
└── examples/
    ├── CMakeLists.txt
    └── basic.c
```

## Prerequisites

- CMake 3.15 or higher
- PostgreSQL development libraries (libpq)
- C11 compatible compiler
- POSIX threads (pthread)

### Installing Prerequisites

**Ubuntu/Debian:**
```bash
sudo apt-get install cmake libpq-dev build-essential
```

**Fedora/RHEL:**
```bash
sudo dnf install cmake postgresql-devel gcc make
```

**macOS (Homebrew):**
```bash
brew install cmake postgresql
```

**Windows:**
- Install PostgreSQL from https://www.postgresql.org/download/windows/
- Install CMake from https://cmake.org/download/
- Use Visual Studio 2019+ or MinGW

## Building

### Linux/macOS

```bash
# Clone or extract the source
cd pgpool

# Create build directory
mkdir build && cd build

# Configure (shared library)
cmake -DCMAKE_BUILD_TYPE=Release ..

# Or configure for static library
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF ..

# Build
cmake --build .

# Run tests (if enabled)
ctest

# Install (may require sudo)
sudo cmake --install .
```

### Windows (Visual Studio)

```bash
# Open "x64 Native Tools Command Prompt for VS"
cd pgpool
mkdir build
cd build

# Configure
cmake -G "Visual Studio 17 2022" -A x64 ^
      -DPostgreSQL_ROOT="C:\Program Files\PostgreSQL\16" ..

# Build
cmake --build . --config Release

# Install (as Administrator)
cmake --install . --config Release
```

### Windows (MinGW)

```bash
mkdir build
cd build
cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
cmake --install .
```

## CMake Options

- `BUILD_SHARED_LIBS` - Build shared libraries (default: ON)
- `PGPOOL_BUILD_EXAMPLES` - Build example programs (default: ON)
- `PGPOOL_BUILD_TESTS` - Build test suite (default: OFF)
- `CMAKE_INSTALL_PREFIX` - Installation directory (default: system default)

Example with options:
```bash
cmake -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=ON \
      -DPGPOOL_BUILD_EXAMPLES=ON \
      -DCMAKE_INSTALL_PREFIX=/usr/local \
      ..
```

## Usage

### Basic Example

```c
#include <pgpool.h>
#include <stdio.h>

int main() {
    // Configure pool
    pgpool_config_t config = {
        .conninfo = "host=localhost dbname=mydb user=postgres",
        .min_connections = 2,
        .max_connections = 10,
        .connect_timeout = 5,
        .auto_reconnect = true
    };

    // Create pool
    pgpool_t* pool = pgpool_create(&config);
    if (!pool) {
        fprintf(stderr, "Failed to create pool\n");
        return 1;
    }

    // Acquire connection (5 second timeout)
    pgconn_t* conn = pgpool_acquire(pool, 5000);
    if (!conn) {
        fprintf(stderr, "Failed to acquire connection\n");
        pgpool_destroy(pool);
        return 1;
    }

    // Execute query
    PGresult* res = pgpool_query(conn, "SELECT version()", 5000);
    if (res && PQresultStatus(res) == PGRES_TUPLES_OK) {
        printf("Version: %s\n", PQgetvalue(res, 0, 0));
        PQclear(res);
    }

    // Release connection
    pgpool_release(pool, conn);

    // Cleanup
    pgpool_destroy(pool);
    return 0;
}
```

### Parameterized Queries

```c
// Query with parameters
const char* params[] = {"John", "30"};
PGresult* res = pgpool_query_params(conn, 
    "SELECT * FROM users WHERE name = $1 AND age > $2",
    2, params, 5000);

// Execute without result
bool success = pgpool_execute_params(conn,
    "INSERT INTO users (name, age) VALUES ($1, $2)",
    2, params, 5000);
```

### Prepared Statements

```c
// Prepare statement
pgpool_prepare(conn, "get_user", 
    "SELECT * FROM users WHERE id = $1", 1, 5000);

// Execute prepared statement
const char* id_param[] = {"42"};
PGresult* res = pgpool_execute_prepared(conn, "get_user", 
    1, id_param, 5000);

// Cleanup
pgpool_deallocate(conn, "get_user", 5000);
```

### Transactions

```c
pgpool_begin(conn, 5000);

bool success = pgpool_execute(conn, 
    "UPDATE accounts SET balance = balance - 100 WHERE id = 1", 5000);
success &= pgpool_execute(conn,
    "UPDATE accounts SET balance = balance + 100 WHERE id = 2", 5000);

if (success) {
    pgpool_commit(conn, 5000);
} else {
    pgpool_rollback(conn, 5000);
}
```

## Linking Against pgpool

### Using CMake

```cmake
find_package(pgpool REQUIRED)
target_link_libraries(your_app PRIVATE pgpool::pgpool)
```

### Using pkg-config

```bash
gcc myapp.c $(pkg-config --cflags --libs pgpool) -o myapp
```

### Manual Linking

```bash
gcc myapp.c -I/usr/local/include -L/usr/local/lib -lpgpool -lpq -lpthread -o myapp
```

## API Simplifications

Compared to the original design, this implementation removes rarely-used NULL parameters:

- **No `Oid* param_types`** - Type inference handles most cases
- **No `param_lengths`** - Only needed for binary data
- **No `param_formats`** - Text format is standard
- **No `result_format`** - Text format is standard
- **Added `timeout_ms` to transactions** - Consistent API

All functions now require only essential parameters plus timeout control.

## Thread Safety

pgpool is fully thread-safe. Multiple threads can safely:
- Acquire/release connections
- Execute queries concurrently on different connections
- Create/destroy pools (though not recommended during operation)

## Performance Tips

1. **Set appropriate pool sizes** - min_connections should handle typical load
2. **Reuse prepared statements** - Prepare once, execute many times
3. **Use parameterized queries** - Safer and often faster than string interpolation
4. **Set realistic timeouts** - Avoid blocking indefinitely
5. **Monitor pool statistics** - Use `pgpool_active_connections()` and `pgpool_idle_connections()`

## Error Handling

Always check return values:
- `NULL` return indicates failure for pointer returns
- `false` indicates failure for bool returns
- Use `pgpool_error_message()` to get detailed error info

```c
PGresult* res = pgpool_query(conn, "SELECT ...", 5000);
if (!res) {
    fprintf(stderr, "Query failed: %s\n", pgpool_error_message(conn));
}
```

## License
MIT

## Contributing

Contributions are welcome! Areas for enhancement:
- Connection health checks
- Idle connection timeout
- Connection pooling statistics
- Async query support
- Statement caching