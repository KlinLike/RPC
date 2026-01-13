#include <stdlib.h>
#include <pthread.h>
#include "pending.h"

// ---------- 全局变量 ----------
static rpc_pending_t* g_pending_table = NULL;
static pthread_mutex_t g_pending_mu = PTHREAD_MUTEX_INITIALIZER;

// ---------- API 实现 ----------

int pending_init(void) {
    pthread_mutex_lock(&g_pending_mu);
    // uthash 不需要显式初始化表头，只需确保指针为 NULL
    if (g_pending_table != NULL) {
        pthread_mutex_unlock(&g_pending_mu);
        return 0; // 已经初始化过
    }
    pthread_mutex_unlock(&g_pending_mu);
    return 0;
}

void pending_destroy(void) {
    pthread_mutex_lock(&g_pending_mu);
    rpc_pending_t *current, *tmp;
    HASH_ITER(hh, g_pending_table, current, tmp) {
        HASH_DEL(g_pending_table, current);
        free(current);
    }
    g_pending_table = NULL;
    pthread_mutex_unlock(&g_pending_mu);
    // 销毁互斥锁
    pthread_mutex_destroy(&g_pending_mu);
}

int pending_add(uint32_t id, int fd, rpc_async_cb cb, rpc_future_t* future, long long expire_ms) {
    rpc_pending_t* p = (rpc_pending_t*)calloc(1, sizeof(rpc_pending_t));
    if (!p) return -1;

    p->id = id;
    p->fd = fd;
    p->cb = cb;
    p->future = future;
    p->expire_ms = expire_ms;

    pthread_mutex_lock(&g_pending_mu);
    // 检查 ID 是否已存在，避免重复添加
    rpc_pending_t* existing = NULL;
    HASH_FIND_INT(g_pending_table, &id, existing);
    if (existing) {
        pthread_mutex_unlock(&g_pending_mu);
        free(p);
        return -1;
    }
    HASH_ADD_INT(g_pending_table, id, p);
    pthread_mutex_unlock(&g_pending_mu);
    return 0;
}

int pending_take(uint32_t id, rpc_pending_t* out) {
    rpc_pending_t* p = NULL;
    int ret = -1;
    pthread_mutex_lock(&g_pending_mu);
    HASH_FIND_INT(g_pending_table, &id, p);
    if (p) {
        if (out) {
            *out = *p; // 拷贝数据
        }
        HASH_DEL(g_pending_table, p);
        free(p);
        ret = 0;
    }
    pthread_mutex_unlock(&g_pending_mu);
    return ret;
}

void pending_check_timeouts(long long now_ms, pending_timeout_handler handler) {
    rpc_pending_t *p, *tmp;
    pthread_mutex_lock(&g_pending_mu);
    HASH_ITER(hh, g_pending_table, p, tmp) {
        if (now_ms >= p->expire_ms) {
            // 1. 从哈希表中移除
            HASH_DEL(g_pending_table, p);
            
            // 2. 调用外部处理逻辑（比如通知回调和归还连接）
            if (handler) {
                handler(p);
            }
            
            // 3. 释放内存
            free(p);
        }
    }
    pthread_mutex_unlock(&g_pending_mu);
}

int pending_delete(uint32_t id) {
    return pending_take(id, NULL);
}

int pending_take_by_fd(int fd, rpc_pending_t* out) {
    rpc_pending_t *p, *tmp;
    int ret = -1;
    pthread_mutex_lock(&g_pending_mu);
    HASH_ITER(hh, g_pending_table, p, tmp) {
        if (p->fd == fd) {
            if (out) {
                *out = *p; // 拷贝数据
            }
            HASH_DEL(g_pending_table, p);
            free(p);
            ret = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_pending_mu);
    return ret;
}
