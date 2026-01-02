#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

// ------------------------ 异步实现 Ver 2 ------------------------
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

/**
 * @brief 生成下一个唯一的异步请求 ID（线程安全）
 */
uint32_t rpc_async_next_id(void);

/**
 * @brief 同步（阻塞）调用封装：内部走异步管线，但在当前线程阻塞等待结果
 * 
 * @param json 请求 JSON 字符串
 * @param id 请求 ID
 * @param body_out 输出响应 body，调用方负责 free(*body_out)
 * @param body_len_out 输出响应 body 长度
 * @param status_out 输出状态码
 * @return int 0 成功，-1 失败
 */
int rpc_call_async_blocking(const char* json,
                            uint32_t id,
                            char** body_out,
                            size_t* body_len_out,
                            rpc_error_code* status_out);




// ------------------------ 异步实现 Ver 1 ------------------------

/**
 * @brief 回调函数签名
 */
typedef void (*rpc_async_cb)(uint32_t id,
                             rpc_error_code status,
                             const char* body,
                             size_t body_len,
                             void* user_data);

/**
 * @brief 初始化异步客户端子系统
 */
int rpc_async_init(const char* ip, int port, int max_conn, int queue_size, int timeout_ms);

/**
 * @brief 停止异步子系统，释放资源
 */
void rpc_async_shutdown(void);

/**
 * @brief 异步发起请求
 */
int rpc_async_call(const char* json, rpc_async_cb cb, void* user_data, uint32_t id);


