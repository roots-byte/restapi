/*
 * Copyright (c) 2026 Jonáš Rys
 * Licensed under the MIT License. See LICENSE file for details.
 */
/**
 * @file main.c
 * @brief Multi-connection example using RESTAPI thread pool wrappers.
 */

#include <stdio.h>
#include <stdlib.h>
#include "thread_pool_wrapper.h"
#ifndef _WIN32
#include <time.h>
#endif
#include "restapi.h"

#define C_RESET   "\x1b[0m"
#define C_BOLD    "\x1b[1m"
#define C_RED     "\x1b[31m"
#define C_GREEN   "\x1b[32m"
#define C_YELLOW  "\x1b[33m"
#define C_BLUE    "\x1b[34m"
#define C_CYAN    "\x1b[36m"
#define C_MAGENTA "\x1b[35m"

/* ---------- User config ---------- */
#define CFG_HOST "httpbin.org"
#define CFG_PORT "80"
#define CFG_PATH "/anything"
#define CFG_THREADS 100
#define CFG_SEND_COUNT 1000
#define CFG_SECRET "smoula"
#define CFG_TAG_PREFIX "JOB"

static unsigned long long now_ms(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL;
#endif
}

static void banner(void)
{
    puts(C_CYAN "=====================================================" C_RESET);
    puts(C_CYAN C_BOLD " RESTAPI Example: Multi Communication via Pool" C_RESET);
    printf(" target: %s:%s  endpoint: %s\n", CFG_HOST, CFG_PORT, CFG_PATH);
    printf(" threads: %d  sends: %d  secret: %s\n", CFG_THREADS, CFG_SEND_COUNT, CFG_SECRET);
    puts(C_CYAN "=====================================================" C_RESET);
}

static void step(const char* txt)
{
    printf(C_BLUE C_BOLD "[step]" C_RESET " %s\n", txt);
}

static void ok(const char* txt)
{
    printf(C_GREEN "  OK   " C_RESET "%s\n", txt);
}

static void fail(const char* txt)
{
    fprintf(stderr, C_RED C_BOLD "ERROR" C_RESET ": %s\n", txt);
}

static void time_line(const char* label, unsigned long long started_at)
{
    unsigned long long elapsed = now_ms() - started_at;
    printf("  " C_MAGENTA "time" C_RESET " %-26s : %llums\n", label, elapsed);
}

typedef struct {
    void*       pool;
    int         index;
    const char* tag;
    void*       result;
} TaskArg;

static void* queue_task(void* arg)
{
    TaskArg* a = (TaskArg*)arg;
    a->result = SendJsonToPool(
        a->pool,
        CFG_PATH,
        "{"
            "\"job\":{"
                "\"id\":%d,"
                "\"tag\":\"%s\","
                "\"payload\":\"%s\""
            "}"
        "}",
        a->index + 1,
        a->tag,
        CFG_SECRET
    );
    return NULL;
}

int main(void)
{
    void*      pool    = NULL;
    void**     tasks   = NULL;
    char     (*tags)[16] = NULL;
    thread_t* threads = NULL;
    TaskArg*   args    = NULL;
    int        rc      = 0;
    unsigned long long total_started = now_ms();
    unsigned long long t = 0;

    if (CFG_THREADS <= 0 || CFG_SEND_COUNT <= 0)
    {
        fail("Invalid configuration: CFG_THREADS and CFG_SEND_COUNT must be > 0");
        return 99;
    }

    tasks   = (void**)calloc(CFG_SEND_COUNT, sizeof(void*));
    tags    = (char (*)[16])calloc(CFG_SEND_COUNT, sizeof(*tags));
    threads = (thread_t*)calloc(CFG_SEND_COUNT, sizeof(thread_t));
    args    = (TaskArg*)calloc(CFG_SEND_COUNT, sizeof(TaskArg));
    if (tasks == NULL || tags == NULL || threads == NULL || args == NULL)
    {
        fail("Allocation failed for task metadata");
        free(tasks);
        free(tags);
        free(threads);
        free(args);
        return 98;
    }

    banner();

    step("InitApi");
    t = now_ms();
    if (InitApi() != CONN_OK)
    {
        fail("InitApi failed");
        return 100;
    }
    time_line("InitApi", t);

    step("CreateConnectionPool");
    t = now_ms();
    pool = CreateConnectionPool(CFG_HOST, CFG_PORT, CFG_THREADS);
    if (pool == NULL)
    {
        fail("CreateConnectionPool failed");
        CleanupApi();
        free(tasks);
        free(tags);
        free(threads);
        free(args);
        return 101;
    }
    time_line("CreateConnectionPool", t);

    step("Queue requests (parallel)");
    t = now_ms();

    for (int i = 0; i < CFG_SEND_COUNT; ++i)
        snprintf(tags[i], sizeof(tags[i]), "%s-%02d", CFG_TAG_PREFIX, i + 1);

    for (int i = 0; i < CFG_SEND_COUNT; ++i)
    {
        args[i].pool   = pool;
        args[i].index  = i;
        args[i].tag    = tags[i];
        args[i].result = NULL;
        if (thread_create(&threads[i], queue_task, &args[i]) != 0)
        {
            fail("pthread_create failed");
            for (int j = 0; j < i; ++j)
                thread_join(threads[j]);
            rc = 102;
            goto cleanup;
        }
    }

    for (int i = 0; i < CFG_SEND_COUNT; ++i)
    {
        thread_join(threads[i]);
        tasks[i] = args[i].result;
        if (tasks[i] == NULL)
        {
            fail("SendJsonToPool failed");
            rc = 102;
            goto cleanup;
        }
        printf(C_YELLOW "  queued" C_RESET " task=%02d  tag=%s\n", i + 1, tags[i]);
    }
    time_line("Queue requests (parallel)", t);

    step("Wait + validate responses");
    t = now_ms();
    for (int i = 0; i < CFG_SEND_COUNT; ++i)
    {
        if (WaitForConnectionDone(tasks[i]) != 0)
        {
            fail("WaitForConnectionDone failed");
            rc = 103;
            goto cleanup;
        }

        if (GetConnectionResult(tasks[i]) != 3)
        {
            fail("Task did not finish with TASK_STATUS_FINISHED");
            rc = 104;
            goto cleanup;
        }

        char* search_path[][1] =
        {{"json"}};

        char* payload;
        
        if (SearchInTask(tasks[i], search_path[0], 1, &payload) != 0)
        {
            fail("key was not found in payload");
            rc = -20;
            goto cleanup;

        }
        
        printf(C_GREEN "  done  " C_RESET " task=%02d  tag=%s  payload=%s\n",
             i + 1, tags[i], payload);
    }
    time_line("Wait + validate", t);

    t = now_ms();
    if (WaitForFinishConnectionPool(pool) != 0)
    {
        fail("WaitForFinishConnectionPool failed");
        rc = 107;
        goto cleanup;
    }
    time_line("WaitForFinishConnectionPool", t);

    ok("All pool tasks finished successfully");

cleanup:
    for (int i = 0; i < CFG_SEND_COUNT; ++i)
    {
        if (tasks[i] != NULL)
        {
            DeletePoolTask(tasks[i]);
            tasks[i] = NULL;
        }
    }

    if (pool != NULL)
    {
        DestroyConnectionPool(pool);
    }

    CleanupApi();

    free(tasks);
    free(tags);
    free(threads);
    free(args);

    if (rc != 0)
    {
        return rc;
    }

    puts(C_CYAN "-----------------------------------------------------" C_RESET);
    printf(C_MAGENTA C_BOLD "TOTAL" C_RESET " multi communication           : %llums\n", now_ms() - total_started);
    puts(C_GREEN C_BOLD "OK: multi communication finished" C_RESET);
    return 0;
}
