#include "pgtypes.h"
#include <solidc/xtime.h>

// Get an integer value from a PGresult at the specified row and column.
// Sets *valid to true if successful, false if the value is null or invalid.
int pg_get_int(PGresult* res, int row, int col, bool* valid) {
    const char* val = PQgetvalue(res, row, col);
    if (!val || PQgetisnull(res, row, col)) {
        if (valid) *valid = false;
        return 0;
    }

    char* endptr;
    long result = strtol(val, &endptr, 10);
    if (*endptr != '\0' || result < INT_MIN || result > INT_MAX) {
        if (valid) *valid = false;
        return 0;
    }
    if (valid) *valid = true;
    return (int)result;
}

// Get a long value from a PGresult at the specified row and column.
long pg_get_long(PGresult* res, int row, int col, bool* valid) {
    const char* val = PQgetvalue(res, row, col);
    if (!val || PQgetisnull(res, row, col)) {
        if (valid) *valid = false;
        return 0L;
    }

    char* endptr;
    long result = strtol(val, &endptr, 10);
    if (*endptr != '\0') {
        if (valid) *valid = false;
        return 0L;
    }

    if (valid) *valid = true;
    return result;
}

// Get a long long value from a PGresult at the specified row and column.
long long pg_get_longlong(PGresult* res, int row, int col, bool* valid) {
    const char* val = PQgetvalue(res, row, col);
    if (!val || PQgetisnull(res, row, col)) {
        if (valid) *valid = false;
        return 0LL;
    }

    char* endptr;
    long long result = strtoll(val, &endptr, 10);
    if (*endptr != '\0') {
        if (valid) *valid = false;
        return 0LL;
    }

    if (valid) *valid = true;
    return result;
}

// Get a float value from a PGresult at the specified row and column.
float pg_get_float(PGresult* res, int row, int col, bool* valid) {
    const char* val = PQgetvalue(res, row, col);
    if (!val || PQgetisnull(res, row, col)) {
        if (valid) *valid = false;
        return 0.0f;
    }

    char* endptr;
    float result = strtof(val, &endptr);
    if (*endptr != '\0') {
        if (valid) *valid = false;
        return 0.0f;
    }

    if (valid) *valid = true;
    return result;
}

// Get a double value from a PGresult at the specified row and column.
double pg_get_double(PGresult* res, int row, int col, bool* valid) {
    const char* val = PQgetvalue(res, row, col);
    if (!val || PQgetisnull(res, row, col)) {
        if (valid) *valid = false;
        return 0.0;
    }

    char* endptr;
    double result = strtod(val, &endptr);
    if (*endptr != '\0') {
        if (valid) *valid = false;
        return 0.0;
    }

    if (valid) *valid = true;
    return result;
}

// Get a boolean value from a PGresult at the specified row and column.
bool pg_get_bool(PGresult* res, int row, int col, bool* valid) {
    const char* val = PQgetvalue(res, row, col);
    if (!val || PQgetisnull(res, row, col)) {
        if (valid) *valid = false;
        return false;
    }

    if (valid) *valid = true;
    // PostgreSQL boolean: 't'/'f', 'true'/'false', '1'/'0', 'yes'/'no', 'on'/'off'
    char first = val[0];
    return (first == 't' || first == 'T' || first == '1' || first == 'y' || first == 'Y' ||
            first == 'o' || first == 'O');
}

// Get a string value from a PGresult at the specified row and column.
// Returns pointer to the string data (valid until PQclear is called).
const char* pg_get_string(PGresult* res, int row, int col, bool* valid) {
    if (PQgetisnull(res, row, col)) {
        if (valid) *valid = false;
        return NULL;
    }

    if (valid) *valid = true;
    return PQgetvalue(res, row, col);
}

// Get a string value and copy it to a buffer with bounds checking.
// Returns number of characters copied (excluding null terminator).
size_t pg_get_string_buf(PGresult* res, int row, int col, char* buf, size_t buf_size, bool* valid) {
    if (PQgetisnull(res, row, col) || buf_size == 0) {
        if (valid) *valid = false;
        if (buf_size > 0) buf[0] = '\0';
        return 0;
    }

    const char* val = PQgetvalue(res, row, col);
    size_t len      = strlen(val);
    size_t copy_len = (len >= buf_size) ? buf_size - 1 : len;

    memcpy(buf, val, copy_len);
    buf[copy_len] = '\0';

    if (valid) *valid = true;
    return copy_len;
}

// Get binary data from a PGresult at the specified row and column.
// Returns pointer to binary data and sets *length to the data length.
const unsigned char* pg_get_binary(PGresult* res, int row, int col, size_t* length, bool* valid) {
    if (PQgetisnull(res, row, col)) {
        if (valid) *valid = false;
        if (length) *length = 0;
        return NULL;
    }

    if (length) *length = (size_t)PQgetlength(res, row, col);
    if (valid) *valid = true;
    return (const unsigned char*)PQgetvalue(res, row, col);
}

// Get a UUID as a string (PostgreSQL UUID type).
const char* pg_get_uuid(PGresult* res, int row, int col, bool* valid) {
    const char* val = pg_get_string(res, row, col, valid);
    if (valid && !*valid) return NULL;

    // Basic UUID format validation (36 chars: 8-4-4-4-12)
    size_t len = strlen(val);
    if (len != 36 || val[8] != '-' || val[13] != '-' || val[18] != '-' || val[23] != '-') {
        if (valid) *valid = false;
        return NULL;
    }

    return val;
}

// Get a timestamp as a string.
xtime_t pg_get_timestamp(PGresult* res, int row, int col, bool* valid) {
    const char* s = pg_get_string(res, row, col, valid);
    if ((valid && !*valid) || !s) {
        if (valid) *valid = false;
        return (xtime_t){0};
    }

    // Initialize timestamp.
    xtime_t timestamp = {0};
    xtime_error_t err;
    err = xtime_parse(s, XTIME_FMT_POSTGRES_TZ, &timestamp);
    if (err != XTIME_OK) {
        if (valid) *valid = false;
        return (xtime_t){0};
    }
    return timestamp;
}
