/*
 * Copyright (c) 2026 Jonáš Rys
 * Licensed under the MIT License. See LICENSE file for details.
 */
/**
 * @file main.c
 * @brief Single communication example using httpbin.org.
 */

#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif
#include "restapi.h"

#define C_RESET   "\x1b[0m"
#define C_BOLD    "\x1b[1m"
#define C_DIM     "\x1b[2m"
#define C_RED     "\x1b[31m"
#define C_GREEN   "\x1b[32m"
#define C_YELLOW  "\x1b[33m"
#define C_BLUE    "\x1b[34m"
#define C_CYAN    "\x1b[36m"
#define C_MAGENTA "\x1b[35m"

static unsigned long long now_ms(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL;
#endif
}

static void print_step(int step, const char* msg)
{
    printf(C_BLUE C_BOLD "[%d/5]" C_RESET " %s\n", step, msg);
}

static void print_error(const char* msg)
{
    fprintf(stderr, C_RED C_BOLD "ERROR:" C_RESET " %s\n", msg);
}

static void print_kv(const char* key, const char* value)
{
    printf("  " C_CYAN "%-14s" C_RESET " : " C_YELLOW C_BOLD "%s" C_RESET "\n", key, value);
}

static void print_timing(const char* label, unsigned long long started_at)
{
    unsigned long long elapsed = now_ms() - started_at;
    printf("  " C_MAGENTA "time" C_RESET " %-26s : %llums\n", label, elapsed);
}

int main(void)
{
    unsigned long long total_started = now_ms();
    unsigned long long t = 0;

    puts(C_CYAN "==============================================" C_RESET);
    puts(C_CYAN C_BOLD " RESTAPI Example: Single Communication" C_RESET);
    puts(C_DIM " target: httpbin.org:80  endpoint: /anything" C_RESET);
    puts(C_CYAN "==============================================" C_RESET);

    print_step(1, "InitApi");
    t = now_ms();
    if (InitApi() != CONN_OK)
    {
        print_error("InitApi failed");
        return 100;
    }
    print_timing("InitApi", t);

    print_step(2, "CreateConnection(httpbin.org, 80)");
    t = now_ms();
    connection_t* conn = CreateConnection("httpbin.org", "80");
    if (conn == NULL)
    {
        print_error("CreateConnection failed");
        CleanupApi();
        return 101;
    }
    print_timing("CreateConnection", t);

    print_step(3, "JsonCommunication(POST /anything)");
    t = now_ms();
    int status = JsonCommunication(
        conn,
        "/anything",
        "{"
            "\"test connection\":"
            "{"
                "\"id\":%d,"
                "\"message\":"
                "{"
                    "\"secret\":\"Gargamel\""
                "}"
            "}"
        "}",
        22
    );

    if (status != CONN_OK)
    {
        fprintf(stderr, C_RED C_BOLD "ERROR:" C_RESET " JsonCommunication failed: %d\n", status);
        DestroyConnection(conn);
        CleanupApi();
        return 102;
    }
    print_timing("JsonCommunication", t);

    char* search_method[][4] = {
        {"json","test connection", "id"},
        {"json","test connection", "message", "secret"}
    };

    char* result = NULL;

    print_step(4, "SearchInJson for selected values");
    t = now_ms();
    if (SearchInConnection(conn, search_method[0], 3, &result) != PARSE_OK)
    {
        print_error("SearchInJson(id) failed");
        DestroyConnection(conn);
        CleanupApi();
        return 103;
    }

    print_kv("id", result);

    if (SearchInConnection(conn, search_method[1], 4, &result) != PARSE_OK)
    {
        print_error("SearchInJson(secret) failed");
        DestroyConnection(conn);
        CleanupApi();
        return 104;
    }

    print_kv("secret", result);
    print_timing("SearchInJson", t);

    print_step(5, "DestroyConnection + CleanupApi");
    t = now_ms();
    DestroyConnection(conn);
    CleanupApi();
    print_timing("Cleanup", t);

    puts(C_GREEN "----------------------------------------------" C_RESET);
    printf(C_MAGENTA C_BOLD "TOTAL" C_RESET " single communication          : %llums\n", now_ms() - total_started);
    puts(C_GREEN C_BOLD "OK: single communication finished" C_RESET);
    return 0;
}
