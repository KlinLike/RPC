
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <stdbool.h>

#include "rpc.h"
#include "rpc_client.h"
#include "rpc_client_api.h"

typedef struct {
    int idx;
    int loops;
} thread_arg_t;

static void* worker(void* arg) {
    thread_arg_t* a = (thread_arg_t*)arg;
    for (int i = 0; i < a->loops; ++i) {
        int32_t lhs = a->idx;
        int32_t rhs = i;
        int32_t sum = add_i32(lhs, rhs);
        printf("[T%d][%d] add_i32(%d,%d)=%d\n", a->idx, i, lhs, rhs, sum);

        char* pong = ping();
        if (pong) {
            printf("[T%d][%d] ping=%s\n", a->idx, i, pong);
            free(pong);
        }

        int64_t x = 9223372036854775807LL - i;
        int64_t y = echo_i64(x);
        printf("[T%d][%d] echo_i64 in=%" PRId64 " out=%" PRId64 "\n", a->idx, i, x, y);

        // 额外测试 IDL 中的其余接口
        double a1 = 1.5 * lhs;
        double a2 = 2.5 * rhs;
        double prod = mul_double(a1, a2);
        printf("[T%d][%d] mul_double(%.2f, %.2f)=%.2f\n", a->idx, i, a1, a2, prod);

        bool even = is_even(rhs);
        printf("[T%d][%d] is_even(%d)=%s\n", a->idx, i, rhs, even ? "true" : "false");

        const char* s = "hello";
        int32_t slen = strlen_s(s);
        printf("[T%d][%d] strlen_s(%s)=%d\n", a->idx, i, s, slen);

        char* mixed = mix3(lhs, a1, (rhs % 2) == 0);
        if (mixed) {
            printf("[T%d][%d] mix3=%s\n", a->idx, i, mixed);
            free(mixed);
        }

        // 节奏放缓，便于观察交错
        usleep(50 * 1000);
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    int threads = 4;    // 线程数量
    int loops = 5;      // 每个线程执行的次数
    if (argc >= 2) threads = atoi(argv[1]);
    if (argc >= 3) loops = atoi(argv[2]);
    if (threads <= 0) threads = 4;
    if (loops <= 0) loops = 5;

    printf("Starting RPC client with %d threads, %d loops each\n", threads, loops);

    if (rpc_client_init("127.0.0.1", 8888) != 0) {
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
    for (int i = 0; i < threads; ++i) {
        pthread_join(tids[i], NULL);
    }

    free(tids);
    free(args);
    return 0;
}
