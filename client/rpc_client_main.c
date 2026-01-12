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
#include "rpc_client_manual.h"

#define THREAD_COUNT 10
#define CALLS_PER_THREAD 100

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
        int32_t a = rand() % 100;
        int32_t b = rand() % 100;
        int32_t expected = a + b;
        
        int32_t actual = add_i32_manual(a, b);
        
        if (actual == expected) {
            res->success_count++;
        } else {
            res->failure_count++;
            fprintf(stderr, "[Thread %d] Call %d failed: %d + %d expected %d, got %d\n",
                    res->thread_id, i, a, b, expected, actual);
        }
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
