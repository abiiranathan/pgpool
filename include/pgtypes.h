#ifndef __PGPOOL_PGTYPES_H__
#define __PGPOOL_PGTYPES_H__

#include <libpq-fe.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__APPLE__) || defined(__unix__) || defined(__linux__)
#include <sys/time.h>  // for gettimeofday
#endif

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

#if defined(_MSC_VER)
    #include "win_strptime.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Retrieve an integer value from a PGresult.
 *
 * @param res   Pointer to the PGresult structure.
 * @param row   Row index in the result set.
 * @param col   Column index in the result set.
 * @param valid Optional pointer to a bool that will be set to true if the value
 *              was successfully parsed and not NULL, false otherwise.
 * @return      The parsed integer value or 0 on error or NULL.
 */
int pg_get_int(PGresult* res, int row, int col, bool* valid);

/**
 * @brief Retrieve a long value from a PGresult.
 *
 * @param res   Pointer to the PGresult structure.
 * @param row   Row index in the result set.
 * @param col   Column index in the result set.
 * @param valid Optional pointer to a bool that will be set to true if the value
 *              was successfully parsed and not NULL, false otherwise.
 * @return      The parsed long value or 0L on error or NULL.
 */
long pg_get_long(PGresult* res, int row, int col, bool* valid);

/**
 * @brief Retrieve a long long value from a PGresult.
 *
 * @param res   Pointer to the PGresult structure.
 * @param row   Row index in the result set.
 * @param col   Column index in the result set.
 * @param valid Optional pointer to a bool that will be set to true if the value
 *              was successfully parsed and not NULL, false otherwise.
 * @return      The parsed long long value or 0LL on error or NULL.
 */
long long pg_get_longlong(PGresult* res, int row, int col, bool* valid);

/**
 * @brief Retrieve a float value from a PGresult.
 *
 * @param res   Pointer to the PGresult structure.
 * @param row   Row index in the result set.
 * @param col   Column index in the result set.
 * @param valid Optional pointer to a bool that will be set to true if the value
 *              was successfully parsed and not NULL, false otherwise.
 * @return      The parsed float value or 0.0f on error or NULL.
 */
float pg_get_float(PGresult* res, int row, int col, bool* valid);

/**
 * @brief Retrieve a double value from a PGresult.
 *
 * @param res   Pointer to the PGresult structure.
 * @param row   Row index in the result set.
 * @param col   Column index in the result set.
 * @param valid Optional pointer to a bool that will be set to true if the value
 *              was successfully parsed and not NULL, false otherwise.
 * @return      The parsed double value or 0.0 on error or NULL.
 */
double pg_get_double(PGresult* res, int row, int col, bool* valid);

/**
 * @brief Retrieve a boolean value from a PGresult.
 *
 * @param res   Pointer to the PGresult structure.
 * @param row   Row index in the result set.
 * @param col   Column index in the result set.
 * @param valid Optional pointer to a bool that will be set to true if the value
 *              was successfully parsed and not NULL, false otherwise.
 * @return      The parsed boolean value. Returns false if the value is invalid or NULL.
 */
bool pg_get_bool(PGresult* res, int row, int col, bool* valid);

/**
 * @brief Retrieve a string value from a PGresult.
 *
 * @param res   Pointer to the PGresult structure.
 * @param row   Row index in the result set.
 * @param col   Column index in the result set.
 * @param valid Optional pointer to a bool that will be set to true if the value is not NULL.
 * @return      Pointer to the string data, valid until PQclear is called. Returns NULL if value is NULL.
 */
const char* pg_get_string(PGresult* res, int row, int col, bool* valid);

/**
 * @brief Retrieve a string value and copy it into a user-provided buffer.
 *
 * @param res      Pointer to the PGresult structure.
 * @param row      Row index in the result set.
 * @param col      Column index in the result set.
 * @param buf      Buffer to copy the string into.
 * @param buf_size Size of the buffer in bytes.
 * @param valid    Optional pointer to a bool that will be set to true if the value is not NULL.
 * @return         Number of characters copied (excluding null terminator). Returns 0 on failure.
 */
size_t pg_get_string_buf(PGresult* res, int row, int col, char* buf, size_t buf_size, bool* valid);

/**
 * @brief Retrieve binary data from a PGresult.
 *
 * @param res    Pointer to the PGresult structure.
 * @param row    Row index in the result set.
 * @param col    Column index in the result set.
 * @param length Pointer to store the length of the binary data.
 * @param valid  Optional pointer to a bool that will be set to true if the value is not NULL.
 * @return       Pointer to the binary data. Valid until PQclear is called. NULL on failure.
 */
const unsigned char* pg_get_binary(PGresult* res, int row, int col, size_t* length, bool* valid);

/**
 * @brief Retrieve a UUID value from a PGresult.
 *
 * @param res   Pointer to the PGresult structure.
 * @param row   Row index in the result set.
 * @param col   Column index in the result set.
 * @param valid Optional pointer to a bool that will be set to true if the value is valid and not NULL.
 * @return      Pointer to the UUID string (36 characters). NULL on error or invalid format.
 */
const char* pg_get_uuid(PGresult* res, int row, int col, bool* valid);

/**
 * @brief Retrieve a timestamp value from a PGresult and convert it to a struct timespec.
 *
 * Supports multiple PostgreSQL timestamp formats, including ISO 8601.
 *
 * @param res   Pointer to the PGresult structure.
 * @param row   Row index in the result set.
 * @param col   Column index in the result set.
 * @param valid Optional pointer to a bool that will be set to true if the timestamp was parsed successfully.
 * @return      A struct timespec with the parsed timestamp. On failure, returns {0, 0}.
 */
struct timespec pg_get_timestamp(PGresult* res, int row, int col, bool* valid);

#ifdef __cplusplus
}
#endif

#endif  // __PGPOOL_PGTYPES_H__
