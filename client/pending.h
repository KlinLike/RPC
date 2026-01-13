#ifndef EPC_PENDING_H
#define EPC_PENDING_H

#include <stdint.h>

#include "third_party/uthash.h"
#include "rpc_types.h"

/**
 * @file pending.h
 * @brief 挂起请求管理模块，基于 uthash 实现
 */

/**
 * @brief 挂起请求结构体
 */
typedef struct rpc_pending_t {
    uint32_t id;            // 请求 ID (Key)
    int fd;                 // 关联的连接 fd
    rpc_async_cb cb;        // 回调函数
    rpc_future_t* future;   // 关联的 future
    long long expire_ms;    // 超时时间戳 (绝对时间)
    UT_hash_handle hh;      // uthash 句柄
} rpc_pending_t;

/**
 * @brief 初始化 pending 管理模块
 * @return 0 成功, -1 失败
 */
int pending_init(void);

/**
 * @brief 销毁 pending 管理模块，释放所有资源
 */
void pending_destroy(void);

/**
 * @brief 添加一个挂起请求
 * @param id 请求 ID
 * @param fd 连接 fd
 * @param cb 回调函数
 * @param future 关联的 future
 * @param expire_ms 超时时间
 * @return 0 成功, -1 失败
 */
int pending_add(uint32_t id, int fd, rpc_async_cb cb, rpc_future_t* future, long long expire_ms);

/**
 * @brief 查找并从哈希表中移除指定的挂起请求（原子操作）
 * @param id 请求 ID
 * @param out 输出找到的请求数据
 * @return 0 成功（找到并移除）, -1 失败（未找到）
 */
int pending_take(uint32_t id, rpc_pending_t* out);

/**
 * @brief 根据 fd 查找并移除请求（通常用于连接断开时的清理）
 * @param fd 连接 fd
 * @param out 输出找到的请求数据
 * @return 0 成功, -1 失败
 */
int pending_take_by_fd(int fd, rpc_pending_t* out);

/**
 * @brief 删除指定的挂起请求并释放内存
 * @param id 请求 ID
 * @return 0 成功, -1 失败
 */
int pending_delete(uint32_t id);

/**
 * @brief 超时处理回调
 */
typedef void (*pending_timeout_handler)(rpc_pending_t* pending);

/**
 * @brief 扫描并清理超时请求
 * @param now_ms 当前时间戳
 * @param handler 发现超时请求时的处理回调
 */
void pending_check_timeouts(long long now_ms, pending_timeout_handler handler);

#endif // EPC_PENDING_H
