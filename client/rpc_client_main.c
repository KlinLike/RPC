
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdarg.h>

#include "rpc.h"
#include "rpc_async.h"
#include "rpc_client_api.h"


typedef struct {
    int idx;
    int loops;
} thread_arg_t;

typedef struct {
    int thread_idx;
    int loop_idx;
    const char* label;
} req_ctx_t;

static atomic_uint g_req_id = 1;
static pthread_mutex_t g_wait_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wait_cv = PTHREAD_COND_INITIALIZER;
static int g_pending = 0;

static void on_resp(uint32_t id, rpc_async_status_t status, const char* body, size_t body_len, void* user_data) {
    req_ctx_t* ctx = (req_ctx_t*)user_data;
    const char* st = "OK";
    switch (status) {
        case RPC_ASYNC_OK: st = "OK"; break;
        case RPC_ASYNC_TIMEOUT: st = "TIMEOUT"; break;
        case RPC_ASYNC_CONN_ERR: st = "CONN_ERR"; break;
        case RPC_ASYNC_SEND_ERR: st = "SEND_ERR"; break;
        case RPC_ASYNC_RECV_ERR: st = "RECV_ERR"; break;
        case RPC_ASYNC_CRC_ERR: st = "CRC_ERR"; break;
        default: st = "UNKNOWN"; break;
    }
    printf("[T%d][%d][id=%u][%s] status=%s body=%.*s\n",
           ctx->thread_idx, ctx->loop_idx, id, ctx->label, st, (int)body_len, body ? body : "");
    free(ctx);

    pthread_mutex_lock(&g_wait_mu);
    g_pending--;
    if (g_pending == 0) {
        pthread_cond_signal(&g_wait_cv);
    }
    pthread_mutex_unlock(&g_wait_mu);
}

// 构造 JSON 并发送异步请求
static void enqueue_call(int tid, int loop, const char* label, const char* method, const char* params_fmt, ...) {
    uint32_t id = atomic_fetch_add(&g_req_id, 1);
    char params[256];
    va_list ap;
    va_start(ap, params_fmt);
    vsnprintf(params, sizeof(params), params_fmt, ap);
    va_end(ap);

    char json[MAX_BODY_LEN];
    snprintf(json, sizeof(json),
             "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s,\"id\":%u}",
             method, params, id);

    req_ctx_t* ctx = malloc(sizeof(req_ctx_t));
    if (!ctx) return;
    ctx->thread_idx = tid;
    ctx->loop_idx = loop;
    ctx->label = label;

    int ret = rpc_async_call(json, on_resp, ctx, id);
    if (ret == 0) {
        pthread_mutex_lock(&g_wait_mu);
        g_pending++;
        pthread_mutex_unlock(&g_wait_mu);
    } else {
        free(ctx);
    }
}

static void* worker(void* arg) {
    thread_arg_t* a = (thread_arg_t*)arg;
    for (int i = 0; i < a->loops; ++i) {
        int32_t lhs = a->idx;
        int32_t rhs = i;
        int64_t x = 9223372036854775807LL - i;
        double a1 = 1.5 * lhs;
        double a2 = 2.5 * rhs;
        const char* s = "hello";
        bool ok = (rhs % 2) == 0;

        // ping
        enqueue_call(a->idx, i, "ping", "ping", "{}", NULL);

        // echo_i64
        enqueue_call(a->idx, i, "echo_i64", "echo_i64", "{\"x\":\"%" PRId64 "\"}", x);

        // add_i32
        enqueue_call(a->idx, i, "add_i32", "add_i32", "{\"a\":%d,\"b\":%d}", lhs, rhs);

        // mul_double
        enqueue_call(a->idx, i, "mul_double", "mul_double", "{\"a\":%.2f,\"b\":%.2f}", a1, a2);

        // is_even
        enqueue_call(a->idx, i, "is_even", "is_even", "{\"n\":%d}", rhs);

        // strlen_s
        enqueue_call(a->idx, i, "strlen_s", "strlen_s", "{\"s\":\"%s\"}", s);

        // mix3
        enqueue_call(a->idx, i, "mix3", "mix3", "{\"a\":%d,\"b\":%.2f,\"ok\":%s}", lhs, a1, ok ? "true" : "false");
    }
    return NULL;
}

#define MULTI_THREAD 0

int main(int argc, char *argv[])
{
#if MULTI_THREAD
    int threads = 4;    // 线程数量
    int loops = 5;      // 每个线程执行的次数
    if (argc >= 2) threads = atoi(argv[1]);
    if (argc >= 3) loops = atoi(argv[2]);
    if (threads <= 0) threads = 4;
    if (loops <= 0) loops = 5;

    printf("Starting async RPC client with %d threads, %d loops each\n", threads, loops);

    if (rpc_async_init("127.0.0.1", 8888, 4, 64, 3000) != 0) {
        fprintf(stderr, "rpc_async_init failed\n");
        return -1;
    }

    pthread_t* tids = calloc((size_t)threads, sizeof(pthread_t));
    thread_arg_t* args = calloc((size_t)threads, sizeof(thread_arg_t));
    if (!tids || !args) {
        fprintf(stderr, "alloc threads failed\n");
        return -1;
    }

    for (int i = 0; i < threads; ++i) {
        args[i].idx = i;
        args[i].loops = loops;
        pthread_create(&tids[i], NULL, worker, &args[i]);
    }

    pthread_mutex_lock(&g_wait_mu);
    while (g_pending > 0) {
        pthread_cond_wait(&g_wait_cv, &g_wait_mu);
    }
    pthread_mutex_unlock(&g_wait_mu);

    for (int i = 0; i < threads; ++i) {
        pthread_join(tids[i], NULL);
    }

    free(tids);
    free(args);
    rpc_async_shutdown();

# else

    int res = add_i32(1, 2);
    printf("add_i32(1, 2) = %d\n", res);

#endif
    return 0;
}
