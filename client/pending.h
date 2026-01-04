#ifndef EPC_PENDING_H
#define EPC_PENDING_H

#include <stdint.h>
#include "rpc_async.h"
#include "third_party/uthash.h"

/**
 * @file pending.h
 * @brief 挂起请求管理模块，基于 uthash 实现
 */

/**
 * @brief 挂起请求结构体
 */
typedef struct {
    uint32_t id;            // 请求 ID (Key)
    int fd;                 // 关联的连接 fd
    rpc_async_cb cb;        // 回调函数
    void* user_data;        // 用户自定义数据
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
 * @param user_data 用户数据
 * @param expire_ms 超时时间
 * @return 0 成功, -1 失败
 */
int pending_add(uint32_t id, int fd, rpc_async_cb cb, void* user_data, long long expire_ms);

/**
 * @brief 查找指定的挂起请求
 * @param id 请求 ID
 * @return 成功返回指针（注意线程安全，通常建议使用 take 接口），失败返回 NULL
 */
rpc_pending_t* pending_find(uint32_t id);

/**
 * @brief 查找并从哈希表中移除指定的挂起请求（原子操作）
 * @param id 请求 ID
 * @return 成功返回指针（调用者负责 free），失败返回 NULL
 */
rpc_pending_t* pending_take(uint32_t id);

/**
 * @brief 删除指定的挂起请求并释放内存
 * @param id 请求 ID
 * @return 0 成功, -1 失败
 */
int pending_delete(uint32_t id);

/**
 * @brief 根据 fd 查找并移除请求（通常用于连接断开时的清理）
 * @param fd 连接 fd
 * @return 成功返回指针，失败返回 NULL
 */
rpc_pending_t* pending_take_by_fd(int fd);

#endif // EPC_PENDING_H
