/**
 * @file rpc_client_main.c
 * @brief RPC 客户端测试程序（简化版）
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>

#include "rpc.h"
#include "rpc_async.h"
#include "rpc_client_gen.h"
#include <string.h>

#define THREAD_COUNT 10
#define CALLS_PER_THREAD 10 // 每个线程执行多少轮，每轮都会调用所有的 RPC 接口

typedef struct {
    int thread_id;
    int success_count;
    int failure_count;
} thread_result_t;

void* test_worker(void* arg) {
    thread_result_t* res = (thread_result_t*)arg;
    res->success_count = 0;
    res->failure_count = 0;

    for (int i = 0; i < CALLS_PER_THREAD; i++) {
        // 1. Test ping (generated)
        char* ping_res = ping();
        if (!ping_res) {
            res->failure_count++;
            fprintf(stderr, "[Thread %d] ping failed\n", res->thread_id);
            continue;
        }
        free(ping_res);

        // 2. Test echo_i64 (generated)
        int64_t x_i64 = 1234567890123LL;
        int64_t actual_i64 = echo_i64(x_i64);
        if (actual_i64 != x_i64) {
            res->failure_count++;
            fprintf(stderr, "[Thread %d] echo_i64 failed: expected %lld, got %lld\n",
                    res->thread_id, (long long)x_i64, (long long)actual_i64);
            continue;
        }

        // 3. Test add_i32 (generated)
        int32_t a = rand() % 100;
        int32_t b = rand() % 100;
        int32_t expected_add = a + b;
        int32_t actual_add = add_i32(a, b);
        if (actual_add != expected_add) {
            res->failure_count++;
            fprintf(stderr, "[Thread %d] add_i32 failed: %d + %d expected %d, got %d\n",
                    res->thread_id, a, b, expected_add, actual_add);
            continue;
        }

        // 4. Test mul_double (generated)
        double da = 1.5, db = 2.0;
        double expected_mul = da * db;
        double actual_mul = mul_double(da, db);
        if (actual_mul != expected_mul) {
            res->failure_count++;
            fprintf(stderr, "[Thread %d] mul_double failed: %f * %f expected %f, got %f\n",
                    res->thread_id, da, db, expected_mul, actual_mul);
            continue;
        }

        // 5. Test is_even (generated)
        int32_t n = rand() % 100;
        bool expected_even = (n % 2 == 0);
        bool actual_even = is_even(n);
        if (actual_even != expected_even) {
            res->failure_count++;
            fprintf(stderr, "[Thread %d] is_even failed: %d expected %d, got %d\n",
                    res->thread_id, n, expected_even, actual_even);
            continue;
        }

        // 6. Test strlen_s (generated)
        const char* test_str = "hello rpc";
        int32_t expected_len = (int32_t)strlen(test_str);
        int32_t actual_len = strlen_s(test_str);
        if (actual_len != expected_len) {
            res->failure_count++;
            fprintf(stderr, "[Thread %d] strlen_s failed: expected %d, got %d\n",
                    res->thread_id, expected_len, actual_len);
            continue;
        }

        // 7. Test mix3 (generated)
        char* mix_res = mix3(10, 2.5, true);
        if (!mix_res) {
            res->failure_count++;
            fprintf(stderr, "[Thread %d] mix3 failed\n", res->thread_id);
            continue;
        }
        free(mix_res);

        res->success_count++;
    }
    
    printf("[Thread %d] Finished: %d success, %d failure\n", 
           res->thread_id, res->success_count, res->failure_count);
    return NULL;
}

int main(int argc, char *argv[])
{
    // 初始化异步客户端
    // 参数：IP, 端口, 最大连接数, 超时时间(ms)
    // 增加连接数以支持并发，池大小设为线程数的两倍以确保稳定
    if (rpc_async_init("127.0.0.1", 8888, 20, 3000) != 0) {
        fprintf(stderr, "rpc_async_init failed\n");
        return -1;
    }

    pthread_t threads[THREAD_COUNT];
    thread_result_t results[THREAD_COUNT];

    printf("Starting %d threads, each doing %d calls...\n", THREAD_COUNT, CALLS_PER_THREAD);

    for (int i = 0; i < THREAD_COUNT; i++) {
        results[i].thread_id = i;
        if (pthread_create(&threads[i], NULL, test_worker, &results[i]) != 0) {
            perror("pthread_create");
            return -1;
        }
    }

    int total_success = 0;
    int total_failure = 0;
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
        total_success += results[i].success_count;
        total_failure += results[i].failure_count;
    }

    printf("\nTest Summary:\n");
    printf("Total Threads: %d\n", THREAD_COUNT);
    printf("Calls Per Thread: %d\n", CALLS_PER_THREAD);
    printf("Total Success: %d\n", total_success);
    printf("Total Failure: %d\n", total_failure);

    // 清理资源
    rpc_async_shutdown();

    return total_failure > 0 ? -1 : 0;
}
