#ifndef RPC_ASYNC_NEW_H

/**
 * @brief 回调函数签名
 */
typedef void (*rpc_async_cb)(rpc_pending_t* pending,
                             rpc_error_code status,
                             const char* body,
                             size_t body_len);


#define RPC_ASYNC_NEW_H