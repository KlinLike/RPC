/**
 * @file rpc_async_new.c
 * @brief 异步 RPC 客户端实现（基于状态机的非阻塞接收）
 * 
 * 模块说明：
 * 1. 接收上下文管理：为每个连接维护接收状态和缓冲区
 * 2. 接收线程：基于 epoll 的事件循环，处理可读事件
 * 3. 状态机：处理 Header 和 Body 的分阶段接收
 * 4. 异步调用：发送请求并注册回调
 * 5. 同步封装：基于异步实现的阻塞调用
 */

#include "pending.h"
#include "rpc_async_new.h"
#include "conn_pool.h"
#include "rpc.h"
#include "crc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <stdbool.h>

// ============================================================================
// 全局变量和配置
// ============================================================================

extern int g_epoll_fd;                  // epoll fd（在 conn_pool 中定义）
static int g_timeout_ms = 5000;         // 默认超时时间（毫秒）

// ============================================================================
// 接收上下文数据结构（状态机 + 缓冲区）
// ============================================================================

/**
 * @brief 接收状态枚举
 */
typedef enum {
    RECV_STATE_HEADER,  // 正在接收 Header
    RECV_STATE_BODY     // 正在接收 Body
} recv_state_t;

/**
 * @brief 每个 fd 的接收上下文（状态机 + 缓冲区）
 * 
 * 说明：
 * - 使用静态数组管理（连接数少时比哈希表更高效）
 * - fd == -1 表示该位置未使用
 * - Header 和 Body 缓冲区都是静态分配（避免 malloc/free 开销）
 */
typedef struct {
    int fd;                             // 连接 fd（-1 表示未使用）
    recv_state_t state;                 // 当前接收状态
    
    // Header 接收缓冲
    char header_buf[RPC_HEADER_LEN];    // Header 缓冲区
    size_t header_received;             // 已接收的 Header 字节数
    
    // Body 接收缓冲
    char body_buf[MAX_BODY_LEN];        // Body 缓冲区
    size_t body_len;                    // Body 总长度（从 Header 解析）
    size_t body_received;               // 已接收的 Body 字节数
    
    rpc_header_t header;                // 解析后的 Header
} fd_recv_ctx_t;

// 接收上下文静态数组
static fd_recv_ctx_t g_recv_ctx_array[MAX_CONNECTIONS];
static pthread_mutex_t g_recv_ctx_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_recv_ctx_initialized = false;

// ============================================================================
// 辅助工具函数（网络 I/O）
// ============================================================================

/**
 * @brief 发送固定长度数据（处理 EINTR，循环发送直到完成）
 * @return 0 成功，-1 失败
 */
static int send_retry(int fd, const void* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, (const char*)buf + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;  // 被中断，重试
            return -1;
        }
        if (n == 0) return -1;  // 连接关闭
        sent += (size_t)n;
    }
    return 0;
}



// ============================================================================
// 接收上下文管理函数
// ============================================================================

/**
 * @brief 初始化接收上下文数组
 */
static void recv_ctx_init(void) {
    if (g_recv_ctx_initialized) return;
    
    pthread_mutex_lock(&g_recv_ctx_mutex);
    if (!g_recv_ctx_initialized) {
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            g_recv_ctx_array[i].fd = -1;  // -1 表示未使用
            g_recv_ctx_array[i].state = RECV_STATE_HEADER;
        }
        g_recv_ctx_initialized = true;
    }
    pthread_mutex_unlock(&g_recv_ctx_mutex);
}

/**
 * @brief 获取或创建接收上下文
 * @param fd 连接 fd
 * @return 接收上下文指针，失败返回 NULL
 */
static fd_recv_ctx_t* recv_ctx_get_or_create(int fd) {
    recv_ctx_init();
    
    pthread_mutex_lock(&g_recv_ctx_mutex);
    
    // 1. 先查找是否已存在
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (g_recv_ctx_array[i].fd == fd) {
            pthread_mutex_unlock(&g_recv_ctx_mutex);
            return &g_recv_ctx_array[i];
        }
    }
    
    // 2. 找一个空闲位置创建新的
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (g_recv_ctx_array[i].fd == -1) {
            fd_recv_ctx_t* ctx = &g_recv_ctx_array[i];
            ctx->fd = fd;
            ctx->state = RECV_STATE_HEADER;
            ctx->header_received = 0;
            ctx->body_len = 0;
            ctx->body_received = 0;
            pthread_mutex_unlock(&g_recv_ctx_mutex);
            return ctx;
        }
    }
    
    // 3. 没有空闲位置
    pthread_mutex_unlock(&g_recv_ctx_mutex);
    fprintf(stderr, "[recv_ctx] 接收上下文数组已满，无法为 fd=%d 创建上下文\n", fd);
    return NULL;
}

/**
 * @brief 重置接收上下文，准备接收下一个响应
 */
static void recv_ctx_reset(fd_recv_ctx_t* ctx) {
    if (!ctx) return;
    
    ctx->state = RECV_STATE_HEADER;
    ctx->header_received = 0;
    ctx->body_len = 0;
    ctx->body_received = 0;
}

/**
 * @brief 删除接收上下文
 */
static void recv_ctx_delete(int fd) {
    pthread_mutex_lock(&g_recv_ctx_mutex);
    
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (g_recv_ctx_array[i].fd == fd) {
            // 重置为未使用状态
            g_recv_ctx_array[i].fd = -1;
            g_recv_ctx_array[i].state = RECV_STATE_HEADER;
            g_recv_ctx_array[i].header_received = 0;
            g_recv_ctx_array[i].body_len = 0;
            g_recv_ctx_array[i].body_received = 0;
            break;
        }
    }
    
    pthread_mutex_unlock(&g_recv_ctx_mutex);
}

// ============================================================================
// 接收线程和状态机（核心逻辑，待实现）
// ============================================================================

/**
 * @brief 处理完整的响应（CRC校验 + 调用回调）
 * 
 * @param ctx 接收上下文
 * @param fd 连接 fd
 */
static void handle_complete_response(fd_recv_ctx_t* ctx, int fd) {
    if (!ctx) return;
    
    // 1. CRC32 校验
    rpc_error_code status = RPC_OK;
    if (!rpc_crc32_verify(ctx->body_buf, ctx->body_len, ctx->header.crc32)) {
        fprintf(stderr, "[fd=%d] CRC32 校验失败\n", fd);
        status = RPC_CRC_ERR;
    }
    
    // 2. 根据 fd 找到对应的 pending（并移除）
    rpc_pending_t pending;
    if (pending_take_by_fd(fd, &pending) != 0) {
        // 没找到对应的 pending（可能已经超时或被取消）
        fprintf(stderr, "[fd=%d] 未找到对应的 pending 请求\n", fd);
        rpc_pool_put_conn(fd, false); // 归还连接
        return;
    }
    
    // 3. 调用回调通知结果
    if (pending.cb) {
        pending.cb(&pending, status, ctx->body_buf, ctx->body_len);
    }
}

/**
 * @brief 处理 fd 可读事件（状态机核心）
 * 
 * 两阶段接收：
 * 1. 接收 Header (12字节)：判断完成条件 header_received == 12
 * 2. 接收 Body (body_len字节)：判断完成条件 body_received == body_len
 * 
 * @param fd 连接 fd
 */
static void handle_epoll_in(int fd) {
    // 获取或创建接收上下文
    fd_recv_ctx_t* ctx = recv_ctx_get_or_create(fd);
    if (!ctx) {
        fprintf(stderr, "[fd=%d] 无法创建接收上下文\n", fd);
        // 通知错误并关闭连接
        rpc_pending_t pending;
        if (pending_take_by_fd(fd, &pending) == 0) {
            if (pending.cb) {
                pending.cb(&pending, RPC_RECV_ERR, NULL, 0);
            }
        }
        rpc_pool_put_conn(fd, true);
        return;
    }
    
    // 状态机循环（可能一次 EPOLLIN 就收到完整数据）
    while (1) {
        if (ctx->state == RECV_STATE_HEADER) {
            // ========== 阶段 1: 接收 Header ==========
            ssize_t n = recv(fd, 
                            ctx->header_buf + ctx->header_received,
                            RPC_HEADER_LEN - ctx->header_received, // 这里限制了最多读取的字节数！
                            0);
            
            if (n < 0) {
                if (errno == EINTR) continue;  // 被中断，重试
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 暂时没有数据，等待下次 EPOLLIN
                    return;
                }
                // 读取错误
                fprintf(stderr, "[fd=%d] recv header 失败: %s\n", fd, strerror(errno));
                goto error_cleanup;
            }
            
            if (n == 0) {
                // 连接关闭
                fprintf(stderr, "[fd=%d] 连接被对端关闭\n", fd);
                goto error_cleanup;
            }
            
            ctx->header_received += n;
            
            // 判断 Header 是否接收完成
            if (ctx->header_received == RPC_HEADER_LEN) {
                // ✅ Header 接收完成，解析并切换状态
                rpc_header_t* hdr_net = (rpc_header_t*)ctx->header_buf;
                ctx->header.version = ntohl(hdr_net->version);
                ctx->header.body_len = ntohl(hdr_net->body_len);
                ctx->header.crc32 = ntohl(hdr_net->crc32);
                
                // 校验 body_len 合法性
                if (ctx->header.body_len > MAX_BODY_LEN) {
                    fprintf(stderr, "[fd=%d] body_len 过大: %u\n", fd, ctx->header.body_len);
                    goto error_cleanup;
                }
                
                // 准备接收 Body
                ctx->body_len = ctx->header.body_len;
                ctx->body_received = 0;
                ctx->state = RECV_STATE_BODY;
                
                // 继续循环，尝试读取 Body（可能 Body 已经到了）
            } else {
                // Header 还没读完，等待下次 EPOLLIN
                return;
            }
        }
        else if (ctx->state == RECV_STATE_BODY) {
            // ========== 阶段 2: 接收 Body ==========
            ssize_t n = recv(fd,
                            ctx->body_buf + ctx->body_received,
                            ctx->body_len - ctx->body_received,
                            0);
            
            if (n < 0) {
                if (errno == EINTR) continue;  // 被中断，重试
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 暂时没有数据，等待下次 EPOLLIN
                    return;
                }
                // 读取错误
                fprintf(stderr, "[fd=%d] recv body 失败: %s\n", fd, strerror(errno));
                goto error_cleanup;
            }
            
            if (n == 0) {
                // 连接关闭
                fprintf(stderr, "[fd=%d] 连接在接收 Body 时被关闭\n", fd);
                goto error_cleanup;
            }
            
            ctx->body_received += n;
            
            // 判断 Body 是否接收完成
            if (ctx->body_received == ctx->body_len) {
                // ✅ 完整响应接收完成！
                ctx->body_buf[ctx->body_len] = '\0';  // 添加字符串结束符
                
                // 处理完整响应（CRC校验 + 调用回调）
                handle_complete_response(ctx, fd);
                
                // 重置上下文，准备接收下一个响应
                recv_ctx_reset(ctx);

                // 归还连接到连接池
                rpc_pool_put_conn(fd, false);
                
                // 返回，等待下次 EPOLLIN（如果还有数据会再次触发）
                return;
            } else {
                // Body 还没读完，等待下次 EPOLLIN
                return;
            }
        }
    }
    
error_cleanup:
    // 发生错误，清理资源并通知
    rpc_pending_t pending;
    if (pending_take_by_fd(fd, &pending) == 0) {
        if (pending.cb) {
            pending.cb(&pending, RPC_RECV_ERR, NULL, 0);
        }
    }
    recv_ctx_delete(fd);
    rpc_pool_put_conn(fd, true);  // 标记连接异常
}

/**
 * @brief 接收线程主循环
 */
static void read_thread(void) {
    // 初始化事件数组
    struct epoll_event events[10];

    while (1) {
        // 等待事件发生
        int nfds = epoll_wait(g_epoll_fd, events, 10, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                // 处理连接错误
                int fd = events[i].data.fd;
                // 在 Pending 中找到这个 fd 对应的数据，然后调用回调通知异常
                rpc_pending_t pending;
                if (pending_take_by_fd(fd, &pending) == 0) {
                    if (pending.cb) {
                        pending.cb(&pending, RPC_CONN_ERR, NULL, 0);
                    }
                }
                // 清理接收上下文
                recv_ctx_delete(fd);
                rpc_pool_put_conn(fd, true);
                continue;
            }

            if (events[i].events & EPOLLIN) {
                // 处理可读事件（状态机）
                int fd = events[i].data.fd;
                handle_epoll_in(fd);
            }
        }
    }
}

// ============================================================================
// 回调函数
// ============================================================================

/**
 * @brief 异步调用的回调函数（唤醒 future）
 * 
 * 在接收线程中调用，当响应到达或发生错误时触发
 */
static void rpc_async_callback(rpc_pending_t* pending, rpc_error_code status, const char* body, size_t body_len) {
    if (!pending || !pending->future) {
        return;
    }
    
    rpc_future_t* future = pending->future;
    
    pthread_mutex_lock(&future->mutex);
    
    // 设置结果
    future->error_code = status;
    
    if (status == RPC_OK && body && body_len > 0) {
        // 复制响应数据
        future->result = (char*)malloc(body_len + 1);
        if (future->result) {
            memcpy(future->result, body, body_len);
            future->result[body_len] = '\0';
            future->result_len = body_len;
        } else {
            future->error_code = RPC_OTHER_ERR;
        }
    } else {
        future->result = NULL;
        future->result_len = 0;
    }
    
    // 标记完成并唤醒等待线程
    future->is_ready = true;
    pthread_cond_signal(&future->cond);
    
    pthread_mutex_unlock(&future->mutex);
}

// ============================================================================
// 异步调用接口
// ============================================================================

/**
 * @brief 异步发起 RPC 调用
 * 
 * 工作流程：
 * 1. 从连接池中取出连接
 * 2. 注册 pending（超时管理）
 * 3. 发送请求（Header + Body）
 * 4. epoll_wait 等待响应
 * 5. 接收线程处理响应并调用回调
 * 
 * @param json 请求 JSON 字符串
 * @param cb 回调函数
 * @param future future 对象（用于同步等待）
 * @param id 请求 ID
 * @return 0 成功，-1 失败
 */
int rpc_async_call(const char* json, rpc_async_cb cb, rpc_future_t* future, uint32_t id) {
    if (!json || !cb || id == 0) {
        return -1;
    }
    size_t body_len = strlen(json);
    if (body_len > MAX_BODY_LEN) {
        return -1;
    }

    // 1. 从连接池中取出连接（连接已注册到 epoll）
    rpc_error_code status;
    int fd = rpc_pool_get_conn(&status);
    if (fd < 0) {
        return status; // 返回具体的错误码
    }

    // 2. 注册 pending（用于超时管理和响应匹配）
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long now_ms = (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
    long long expire_ms = now_ms + g_timeout_ms;
    if (pending_add(id, fd, cb, future, expire_ms) != 0) {
        rpc_pool_put_conn(fd, true);
        return -1;
    }

    // 3. 构造并发送请求
    rpc_header_t header;
    header.version = htonl(1);
    header.body_len = htonl((uint32_t)body_len);
    header.crc32 = htonl(rpc_crc32(json, body_len));

    // 3.1 发送 Header
    if (send_retry(fd, &header, RPC_HEADER_LEN) != 0) {
        // 发送失败：归还连接并清理 pending
        rpc_pool_put_conn(fd, true);
        rpc_pending_t p;
        if (pending_take(id, &p) != 0) {
            perror("rpc_async_call: pending_take failed");
        }
        return -1;
    }

    // 3.2 发送 Body
    if (send_retry(fd, json, body_len) != 0) {
        rpc_pool_put_conn(fd, true);
        rpc_pending_t p;
        if (pending_take(id, &p) != 0) {
            perror("rpc_async_call: pending_take failed");
        }
        return -1;
    }
    
    return 0;
}

// ============================================================================
// 同步调用封装
// ============================================================================

/**
 * @brief 同步（阻塞）RPC 调用
 * 
 * 内部走异步管线，但在当前线程阻塞等待结果
 * 
 * @param json 请求 JSON 字符串
 * @param id 请求 ID
 * @param body_out 输出响应 body，调用方负责 free(*body_out)
 * @param body_len_out 输出响应 body 长度
 * @param status_out 输出状态码
 * @return 0 成功，-1 失败
 */
int rpc_call_async_blocking(const char* json,
                            uint32_t id,
                            char** body_out,
                            size_t* body_len_out,
                            rpc_error_code* status_out) {

    if (!body_out || !body_len_out || !status_out) {
        fprintf(stderr, "rpc_call_async_blocking: invalid arguments\n");
        return -1;
    }
    *status_out = RPC_OK;

    // 初始化 future 结构体
    rpc_future_t future;
    pthread_mutex_init(&future.mutex, NULL);
    pthread_cond_init(&future.cond, NULL);
    future.is_ready = false;
    future.result = NULL;
    future.result_len = 0;
    future.error_code = RPC_OK;

    // 发起异步调用
    if (rpc_async_call(json, rpc_async_callback, &future, id) != 0) {
        *status_out = RPC_OTHER_ERR;
        pthread_mutex_destroy(&future.mutex);
        pthread_cond_destroy(&future.cond);
        return -1;
    }

    // 等待 future 完成
    pthread_mutex_lock(&future.mutex);
    while (!future.is_ready) {
        pthread_cond_wait(&future.cond, &future.mutex);
    }
    pthread_mutex_unlock(&future.mutex);

    // 复制结果
    *status_out = future.error_code;
    if (future.error_code == RPC_OK) {
        // 所有权转移给调用者
        *body_out = future.result;
        future.result = NULL;
        *body_len_out = future.result_len;
    } else {
        if (future.result) free(future.result);
        fprintf(stderr, "rpc_call_async_blocking: failed with status %d\n", future.error_code);
    }

    // 清理资源
    pthread_mutex_destroy(&future.mutex);
    pthread_cond_destroy(&future.cond);

    return (future.error_code == RPC_OK) ? 0 : -1;
}

// ============================================================================
// 初始化和销毁
// ============================================================================

/**
 * @brief 初始化异步客户端子系统
 * 
 * @param ip 服务器 IP
 * @param port 服务器端口
 * @param max_conn 最大连接数
 * @param timeout_ms 超时时间（毫秒）
 * @return 0 成功，-1 失败
 */
int rpc_async_init(const char* ip, int port, int max_conn, int timeout_ms) {
    // 1. 初始化连接池（内部会初始化 epoll）
    if (rpc_pool_init(ip, port, max_conn) != 0) {
        return -1;
    }

    // 2. 初始化 pending 管理模块
    if (pending_init() != 0) {
        rpc_pool_destroy();
        return -1;
    }

    // 3. 设置超时时间
    if (timeout_ms > 0) {
        g_timeout_ms = timeout_ms;
    }

    // 4. 初始化接收上下文数组
    recv_ctx_init();

    return 0;
}

/**
 * @brief 停止异步客户端子系统，释放资源
 */
void rpc_async_shutdown(void) {
    // 1. 销毁 pending 模块（释放所有节点和锁）
    pending_destroy();

    // 2. 销毁连接池（关闭 fd，销毁 pool 和 epoll）
    rpc_pool_destroy();
}
