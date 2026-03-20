# restapi

Lightweight C99 library for socket-based JSON REST communication.  
Supports both single-connection and thread-pool (parallel) request modes.  
Cross-platform: Linux/POSIX and Windows (Winsock2).

---

## Features

- **Single connection** — connect, POST JSON, receive and parse the response
- **Connection pool** — submit many requests in parallel using a built-in thread pool
- **JSON path search** — extract values from nested JSON by key path
- **Automatic reconnect** — up to 3 attempts on disconnect
- **Zero external dependencies** — pure C99, POSIX sockets / Winsock2

---

## Build

Requires **CMake ≥ 3.16** and a C99 compiler.

```sh
cmake -S . -B build
cmake --build build
```

Outputs:
- `out_lib/librestapi_static.a` — static library
- `out_bin/single_communication_example`
- `out_bin/multi_communication_example`

To skip building examples:

```sh
cmake -S . -B build -DRESTAPI_BUILD_EXAMPLES=OFF
```

On Windows, link against `ws2_32` (done automatically by CMake).

---

## Quick start

### Single connection

```c
#include "restapi.h"

int main(void)
{
    InitApi();

    connection_t* conn = CreateConnection("httpbin.org", "80");

    JsonCommunication(conn, "/anything",
        "{\"id\":%d,\"msg\":\"hello\"}", 1);

    char* val;
    char* path[] = {"json", "msg"};
    SearchInConnection(conn, path, 2, &val);
    printf("msg = %s\n", val);

    DestroyConnection(conn);
    CleanupApi();
}
```

### Connection pool (parallel requests)

```c
#include "restapi.h"

int main(void)
{
    InitApi();

    void* pool = CreateConnectionPool("httpbin.org", "80", 8);

    void* tasks[100];
    for (int i = 0; i < 100; i++)
        tasks[i] = SendJsonToPool(pool, "/anything", "{\"id\":%d}", i);

    for (int i = 0; i < 100; i++) {
        WaitForConnectionDone(tasks[i]);
        DeletePoolTask(tasks[i]);
    }

    DestroyConnectionPool(pool);
    CleanupApi();
}
```

---

## API reference

### Lifecycle

| Function | Description |
|---|---|
| `InitApi()` | Initialise global networking state (call once). |
| `CleanupApi()` | Release global resources (call once at exit). |

### Single connection

| Function | Description |
|---|---|
| `CreateConnection(address, port)` | Open a TCP connection. Returns `connection_t*` or `NULL`. |
| `DestroyConnection(conn)` | Close socket and free all resources. |
| `JsonCommunication(conn, path, json, ...)` | POST a JSON body to `path`, receive and parse the response. |
| `SearchInConnection(conn, keys, n, &result)` | Find a value by key path in the last parsed response. |

### Connection pool

| Function | Description |
|---|---|
| `CreateConnectionPool(host, port, threads)` | Create a pool with `threads` persistent connections. |
| `DestroyConnectionPool(pool)` | Shut down pool and close all connections. |
| `SendJsonToPool(pool, path, json, ...)` | Queue a JSON request; returns an opaque task handle. |
| `SendArgumentToPool(pool, path, json, args)` | Same as `SendJsonToPool` but accepts a pre-built `va_list` — use inside variadic wrapper functions. |
| `WaitForConnectionDone(task)` | Block until the task finishes. |
| `WaitForFinishConnectionPool(pool)` | Block until all tasks in the pool are done. |
| `GetConnectionResult(task)` | Get the task completion status (`TASK_STATUS_FINISHED = 3`). |
| `GetConnectionParseJson(task)` | Get the raw parsed JSON buffer of a finished task. |
| `SearchInTask(task, keys, n, &result)` | Find a value by key path in a pool task response. |
| `DeletePoolTask(task)` | Free a task and its buffers. |

---

## Logging

By default the library writes a log file `restapi.log` in the working directory (or `C:\LOG\restapi.log` on Windows). Control with the macros below, defined before including `restapi.h`:

```c
#define LOG_RESTAPI   1   /* 0 = disable all logging      */
#define DEBUG_RESTAPI 0   /* 1 = enable verbose DEBUG logs */
```

---

## Status codes

`RestApiStatus` and `ParseError` are defined in `restapi.h`. Common values:

| Code | Value | Meaning |
|---|---|---|
| `CONN_OK` | 0 | Success |
| `CONN_ERROR` | 1 | Generic connection error |
| `CONN_TIMEOUT` | 2 | Operation timed out (default: 10 s) |
| `CONN_CLOSED` | 3 | Peer closed the connection |
| `MEMORY_ERROR` | 6 | Allocation failed |
| `PARSE_OK` | 0 | JSON parse/search succeeded |
| `VALUE_NOT_FOUND` | 9 | Key path not found in response |

---

## License

[MIT](LICENSE) © 2026 Jonáš Rys
