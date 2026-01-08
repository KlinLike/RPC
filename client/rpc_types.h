#ifndef RPC_TYPES_H
#define RPC_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

/**
 * @file rpc_types.h
 * @brief RPC 通用类型定义（避免循环包含）
 */

// ============================================================================
// 错误码
// ============================================================================

/**
 * @brief RPC 错误码
 */
typedef enum {
    RPC_OK = 0,
    RPC_TIMEOUT = 1,
    RPC_CONN_ERR = 2,
    RPC_SEND_ERR = 3,
    RPC_RECV_ERR = 4,
    RPC_CRC_ERR = 5,
    RPC_OTHER_ERR = 6,
} rpc_error_code;

// ============================================================================
// Future 对象
// ============================================================================

/**
 * @brief Future 对象（用于同步等待异步结果）
 */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool is_ready;
    char* result;           // 响应 body，调用者负责 free
    size_t result_len;
    rpc_error_code error_code;
} rpc_future_t;

// ============================================================================
// 前向声明
// ============================================================================

struct rpc_pending_t;

/**
 * @brief 异步回调函数签名
 */
typedef void (*rpc_async_cb)(struct rpc_pending_t* pending,
                             rpc_error_code status,
                             const char* body,
                             size_t body_len);

#endif // RPC_TYPES_H
