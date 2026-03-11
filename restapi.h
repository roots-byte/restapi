/* 
 * Copyright (c) 2026 Jonáš Rys
 * Licensed under the MIT License. See LICENSE file for details.
 */
/**
 * @file restapi.h
 * @brief Public API for socket-based JSON communication.
 */

#ifndef RESTAPI_H
#define RESTAPI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "restapi_compat.h"

#ifndef DEBUG_RESTAPI
#define DEBUG_RESTAPI 0
#endif

#ifndef LOG_RESTAPI
#define LOG_RESTAPI 1
#endif

/**
 * @brief Timeout in milliseconds applied to every socket operation (connect, send, recv).
 *
 * Override before including this header to change the limit globally:
 * @code
 *   #define RESTAPI_COMM_TIMEOUT_MS 5000
 *   #include "restapi.h"
 * @endcode
 */
#ifndef RESTAPI_COMM_TIMEOUT_MS
#define RESTAPI_COMM_TIMEOUT_MS 10000
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
  #ifdef RESTAPI_BUILD_DLL
    #define RESTAPI_EXPORT __declspec(dllexport)
  #else
    #define RESTAPI_EXPORT
  #endif
#else
  #define RESTAPI_EXPORT
#endif

typedef struct connection_t connection_t;

/** @brief Status codes returned by API and transport functions. */
typedef enum RestApiStatus
{
  CONN_OK = 0,               /**< Success. */
  CONN_ERROR = 1,            /**< Generic connection error. */
  CONN_TIMEOUT = 2,          /**< Operation timed out. */
  CONN_CLOSED = 3,           /**< Peer closed connection. */
  CONN_NOT_CONNECTED = 4,    /**< No active connection. */
  CONN_SETUP_ERROR = 5,      /**< Address/socket setup failed. */
  MEMORY_ERROR = 6,          /**< Memory allocation failed. */
  CREATE_SOCKET_ERROR = 7,   /**< Socket creation failed. */
  CONN_SET_TIMEOUT_ERROR = 8,/**< Socket timeout setup failed. */
  WSA_STARTUP_ERROR = 9,     /**< WSA startup failed on Windows. */
  INVALID_ARGUMENT = 10,     /**< Invalid input argument. */
  INIT_ERROR = 11,           /**< API is not initialized. */
  INNIT_ERROR = INIT_ERROR,  /**< Backward-compatible typo alias. */
  CREATE_BUFFER_ERROR = 12   /**< Request buffer creation failed. */
} RestApiStatus;

/** @brief Parser result codes for headers/JSON payloads. */
typedef enum ParseError
{
  PARSE_OK,          /**< Parse succeeded. */
  INVALID_CODE,      /**< Unsupported HTTP status code. */
  READ_CODE_ERROR,   /**< Failed to read HTTP status code. */
  CODE_NOT_FOUND,    /**< Status code not found in header. */
  READ_LENGTH_ERROR, /**< Failed to parse Content-Length. */
  LENGTH_NOT_FOUND,  /**< Content-Length not found. */
  UNKNOWN_ERROR,     /**< Unspecified parse error. */
  PARSE_ERROR,       /**< Generic parser state error. */
  INVALID_JSON,      /**< Malformed JSON payload. */
  VALUE_NOT_FOUND    /**< Requested value/path not found. */
} ParseError;

/**
 * @brief Initialize global networking state.
 * @return `CONN_OK` on success, otherwise #RestApiStatus.
 */
RESTAPI_EXPORT int InitApi();

/**
 * @brief Find a value in parsed JSON response data.
 * @param[in]  connection Active connection context.
 * @param[in]  search     Key path tokens (array of strings).
 * @param[in]  search_len Number of tokens in @p search.
 * @param[out] result     Receives a pointer to the extracted value.
 * @return `PARSE_OK` on success, otherwise #ParseError.
 * @note @p result points into internal buffer memory owned by @p connection.
 */
RESTAPI_EXPORT int SearchInTask(void* task_v, char *search[], int search_len, char** result);
RESTAPI_EXPORT int SearchInConnection(connection_t* conn, char *search[], int search_len, char** result);

/**
 * @brief Send a JSON request and receive a response.
 * @param[in] connection Active connection handle.
 * @param[in] path API endpoint path.
 * @param[in] json_text JSON body or format string.
 * @param[in] ... Optional varargs used by `json_text`.
 * @return `CONN_OK` on success, otherwise #RestApiStatus.
 */
RESTAPI_EXPORT int JsonCommunication(connection_t* connection, const char* path, const char* json_text, ...);

/**
 * @brief Release global networking resources.
 */
RESTAPI_EXPORT void CleanupApi(void);

/**
 * @brief Create and connect a new API connection.
 * @param[in] address Hostname or IP address.
 * @param[in] port Service port.
 * @return New connection instance, or `NULL` on failure.
 */
RESTAPI_EXPORT connection_t* CreateConnection(char* address, char* port);

/**
 * @brief Destroy a connection and free all related resources.
 * @param[in,out] connection Connection returned by CreateConnection().
 */
RESTAPI_EXPORT void DestroyConnection(connection_t* connection);

/**
 * @brief Create a connection pool for parallel JSON requests.
 * @param[in] host_addres Hostname or IP address.
 * @param[in] port Service port.
 * @param[in] thread_size Number of worker threads.
 * @return Opaque pool handle, or `NULL` on failure.
 */
RESTAPI_EXPORT void* CreateConnectionPool(char* host_addres, char* port, int thread_size);

/**
 * @brief Destroy a pool created by CreateConnectionPool.
 * @param[in,out] pool_v Opaque pool handle.
 */
RESTAPI_EXPORT void DestroyConnectionPool(void* pool_v);

/**
 * @brief Submit one JSON request into the pool queue.
 * @param[in] pool_v Opaque pool handle.
 * @param[in] path API endpoint path.
 * @param[in] json JSON body or format string.
 * @param[in] ... Optional varargs used by `json`.
 * @return Opaque task handle, or `NULL` on failure.
 */
RESTAPI_EXPORT void* SendJsonToPool(void* pool_v, const char* path, const char* json, ...);

/**
 * @brief Wait until the submitted task finishes.
 * @param[in] task_v Opaque task handle.
 * @return `0` on success, otherwise an error code.
 */
RESTAPI_EXPORT int WaitForConnectionDone(void* task_v);

/**
 * @brief Get task status from a finished or running task.
 * @param[in] task_v Opaque task handle.
 * @return Task status value.
 */
RESTAPI_EXPORT int GetConnectionResult(void* task_v);

/**
 * @brief Get JSON response buffer stored in task.
 * @param[in] task_v Opaque task handle.
 * @return Pointer to response JSON text, or `NULL`.
 */
RESTAPI_EXPORT char* GetConnectionParseJson(void* task_v);

/**
 * @brief Wait until all queued/running pool tasks are completed.
 * @param[in] pool_v Opaque pool handle.
 * @return `0` on success, otherwise an error code.
 */
RESTAPI_EXPORT int WaitForFinishConnectionPool(void* pool_v);

/**
 * @brief Free one task and its buffers.
 * @param[in,out] task_v Opaque task handle.
 */
RESTAPI_EXPORT void DeletePoolTask(void* task_v);

#ifdef __cplusplus
}
#endif

#endif /* RESTAPI_H */
