#ifndef RPC_FUTURE_H
#define RPC_FUTURE_H

#include <pthread.h>

/**
 * @brief 异步请求的 Future 对象，用于同步等待异步结果
 */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool is_ready;
    char* result;           // 响应 body，调用者负责 free
    size_t result_len;
    rpc_error_code error_code;
} rpc_future_t;

#endif // RPC_FUTURE_H