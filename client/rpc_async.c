#include "rpc_async.h"

#include <arpa/inet.h>
#include <errno.h>

#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "rpc.h"
#include "crc.h"
#include "third_party/uthash.h"

// --------------- 连接池（简单版，占位实现） ---------------

typedef struct {
    int fd;
    bool in_use;
} rpc_conn_t;

static rpc_conn_t* g_pool = NULL;   // 连接池
static int g_pool_size = 0;         // 连接池大小
static char g_ip[64] = {0};         // 服务 IP
static int g_port = 0;              // 服务端口

static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;

// 简单连接函数（阻塞）
static int rpc_connect_once(const char* ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        // IP地址无效
        perror("inet_pton");
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

// 获取连接（简单策略：先找空闲可用，若没有且有空槽则新建，否则返回忙）
// 从连接池获取可用 fd；无可用且有空槽则新建，否则返回 -1/EBUSY
static int rpc_pool_get_conn(void) {
    int fd = -1;
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_pool_size; ++i) {
        if (g_pool[i].in_use)
            continue;
        if (g_pool[i].fd >= 0) {
            g_pool[i].in_use = true;
            fd = g_pool[i].fd;
            pthread_mutex_unlock(&g_pool_mu);
            return fd;
        }
    }
    // 尝试新建
    for (int i = 0; i < g_pool_size; ++i) {
        if (g_pool[i].fd < 0 && !g_pool[i].in_use) {
            g_pool[i].in_use = true; // 先占位
            pthread_mutex_unlock(&g_pool_mu);
            fd = rpc_connect_once(g_ip, g_port);
            pthread_mutex_lock(&g_pool_mu);
            if (fd < 0) {
                g_pool[i].in_use = false;
                pthread_mutex_unlock(&g_pool_mu);
                return -1;
            }
            g_pool[i].fd = fd;
            g_pool[i].in_use = true;
            pthread_mutex_unlock(&g_pool_mu);
            return fd;
        }
    }
    pthread_mutex_unlock(&g_pool_mu);
    errno = EBUSY;
    return -1;
}

// 归还 fd；若 bad=true 则关闭并回收槽位
static void rpc_pool_put_conn(int fd, bool bad) {
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_pool_size; ++i) {
        if (g_pool[i].fd == fd) {
            if (bad) {
                close(g_pool[i].fd);
                g_pool[i].fd = -1;
            }
            g_pool[i].in_use = false;
            pthread_mutex_unlock(&g_pool_mu);
            return;
        }
    }
    pthread_mutex_unlock(&g_pool_mu);
    // 未找到：关闭避免泄漏
    if (fd >= 0) close(fd);
}

// 进来前需要保证没有线程在使用连接池
static void rpc_pool_destroy(void) {
    pthread_mutex_lock(&g_pool_mu);
    if (g_pool) {
        for (int i = 0; i < g_pool_size; ++i) {
            if (g_pool[i].fd >= 0) {
                close(g_pool[i].fd);
                g_pool[i].fd = -1;
            }
        }
        free(g_pool);
        g_pool = NULL;
    }
    g_pool_size = 0;
    pthread_mutex_unlock(&g_pool_mu);
}

// --------------- pending 表（基础操作） ---------------

typedef struct rpc_pending_s {
    uint32_t id;
    int fd;
    rpc_async_cb cb;
    void* user_data;
    long long expire_ms; // 绝对超时，后续使用
    UT_hash_handle hh; // uthash的hash表
} rpc_pending_t;

static rpc_pending_t* g_pending = NULL;
static pthread_mutex_t g_pending_mu = PTHREAD_MUTEX_INITIALIZER; // uthash不线程安全
static uint32_t g_next_id = 1;

static uint32_t rpc_next_id(void) {
    // 简单递增，不做溢出处理（实践中可跳过0）
    return __sync_fetch_and_add(&g_next_id, 1);
}

// 注册一个挂起请求 添加 pending，成功返回指针，失败返回 NULL
static rpc_pending_t* rpc_pending_add(uint32_t id, int fd, rpc_async_cb cb, void* user_data, long long expire_ms) {
    // 并发量不大，忽略ID冲突
    rpc_pending_t* p = calloc(1, sizeof(rpc_pending_t));
    if (!p) return NULL;
    p->id = id;
    p->fd = fd;
    p->cb = cb;
    p->user_data = user_data;
    p->expire_ms = expire_ms;
    pthread_mutex_lock(&g_pending_mu);
    HASH_ADD_INT(g_pending, id, p);
    pthread_mutex_unlock(&g_pending_mu);
    return p;
}

// 按 id 取出并移除挂起请求
static rpc_pending_t* rpc_pending_take(uint32_t id) {
    rpc_pending_t* p = NULL;
    pthread_mutex_lock(&g_pending_mu);
    HASH_FIND_INT(g_pending, &id, p);
    if (p) {
        HASH_DEL(g_pending, p);
    }
    pthread_mutex_unlock(&g_pending_mu);
    return p;
}

// 移除指定 fd 的所有 pending（例如连接坏掉）
// 移除某 fd 上的所有挂起请求并触发错误回调
static void rpc_pending_remove_by_fd(int fd, rpc_async_status_t status) {
    rpc_pending_t *p, *tmp;
    pthread_mutex_lock(&g_pending_mu);
    HASH_ITER(hh, g_pending, p, tmp) {
        if (p->fd == fd) {
            HASH_DEL(g_pending, p);
            // 回调放到此处后续可改为投递队列；先直接调用占位
            if (p->cb) {
                p->cb(p->id, status, NULL, 0, p->user_data);
            }
            free(p);
        }
    }
    pthread_mutex_unlock(&g_pending_mu);
}

// --------------- 回调队列与消费线程骨架 ---------------

typedef struct {
    uint32_t id;
    rpc_async_status_t status;
    char* body;     // body由生产者malloc/copy，保存RPC Request Body，消费者调用毁掉后用rpc_resp_event_cleanup函数释放
    size_t body_len;
    rpc_async_cb cb;
    void* user_data;
} rpc_resp_event_t;

typedef struct {
    rpc_resp_event_t* buf;  // 事件数组
    int capacity;           // 容量
    int head;               // 头指针下标
    int tail;               // 尾指针下标
    int size;               // 当前队中元素个数
    bool stop;              // 停止标志，通知线程退出
    pthread_mutex_t mu;
    pthread_cond_t not_empty;   // 消费者用，等待队列非空
    pthread_cond_t not_full;    // 生产者用，等待队列不满
} rpc_resp_queue_t;

static rpc_resp_queue_t g_resp_q;   // 回调队列
static pthread_t g_cb_thread;       // 回调线程
static int g_default_queue_cap = 16;

// 释放事件里可能有的 body 缓冲
static void rpc_resp_event_cleanup(rpc_resp_event_t* ev) {
    if (ev->body) {
        free(ev->body);
        ev->body = NULL;
    }
}

static int rpc_resp_queue_init(int capacity) {
    if (capacity <= 0) return -1;
    g_resp_q.buf = calloc((size_t)capacity, sizeof(rpc_resp_event_t));
    if (!g_resp_q.buf) return -1;
    g_resp_q.capacity = capacity;
    g_resp_q.head = g_resp_q.tail = g_resp_q.size = 0;
    g_resp_q.stop = false;
    pthread_mutex_init(&g_resp_q.mu, NULL);
    pthread_cond_init(&g_resp_q.not_empty, NULL);
    pthread_cond_init(&g_resp_q.not_full, NULL);
    return 0;
}

static void rpc_resp_queue_destroy(void) {
    pthread_mutex_lock(&g_resp_q.mu);
    for (int i = 0; i < g_resp_q.size; ++i) {
        int idx = (g_resp_q.head + i) % g_resp_q.capacity;
        rpc_resp_event_cleanup(&g_resp_q.buf[idx]);
    }
    free(g_resp_q.buf);
    g_resp_q.buf = NULL;
    g_resp_q.size = g_resp_q.capacity = g_resp_q.head = g_resp_q.tail = 0;
    pthread_mutex_unlock(&g_resp_q.mu);
    pthread_mutex_destroy(&g_resp_q.mu);
    pthread_cond_destroy(&g_resp_q.not_empty);
    pthread_cond_destroy(&g_resp_q.not_full);
}

// 队列满会阻塞等待
// 推送事件到回调队列（满则等待，stop 返回 -1）
static int rpc_resp_queue_push(const rpc_resp_event_t* ev) {
    pthread_mutex_lock(&g_resp_q.mu);
    if (g_resp_q.stop) {
        pthread_mutex_unlock(&g_resp_q.mu);
        return -1;
    }
    while (g_resp_q.size == g_resp_q.capacity) {
        // NOTE: 不满足条件的时候，用条件变量等待，条件变量会自动解锁+上锁，这就是为什么wait函数也需要传入互斥锁
        pthread_cond_wait(&g_resp_q.not_full, &g_resp_q.mu);
    }

    g_resp_q.buf[g_resp_q.tail] = *ev;
    g_resp_q.tail = (g_resp_q.tail + 1) % g_resp_q.capacity;
    g_resp_q.size++;
    pthread_cond_signal(&g_resp_q.not_empty);
    pthread_mutex_unlock(&g_resp_q.mu);
    return 0;
}

// 队列空会阻塞等待
// 从回调队列取事件（空则等待，stop+空返回 -1）
static int rpc_resp_queue_pop(rpc_resp_event_t* ev) {
    pthread_mutex_lock(&g_resp_q.mu);
    while (!g_resp_q.stop && g_resp_q.size == 0) {
        // stop = false且队列为空，继续等待
        pthread_cond_wait(&g_resp_q.not_empty, &g_resp_q.mu);
    }
    if (g_resp_q.stop && g_resp_q.size == 0) {
        // stop = true且队列为空，退出
        // stop = true且队列不为空，正常取元素
        pthread_mutex_unlock(&g_resp_q.mu);
        return -1;
    }
    // 这里 size>0，可正常取元素

    *ev = g_resp_q.buf[g_resp_q.head];
    g_resp_q.head = (g_resp_q.head + 1) % g_resp_q.capacity;
    g_resp_q.size--;
    pthread_cond_signal(&g_resp_q.not_full);
    pthread_mutex_unlock(&g_resp_q.mu);
    return 0;
}

static void* rpc_cb_thread_fn(void* arg) {
    (void)arg;
    rpc_resp_event_t ev;
    // pop为空会等待，这里不用做特别处理了
    while (rpc_resp_queue_pop(&ev) == 0) {
        if (ev.cb) {
            ev.cb(ev.id, ev.status, ev.body, ev.body_len, ev.user_data);
        }
        rpc_resp_event_cleanup(&ev);
    }
    return NULL;
}

// 启动回调线程
static int rpc_start_cb_thread(int queue_capacity) {
    if (rpc_resp_queue_init(queue_capacity) != 0) {
        return -1;
    }
    if (pthread_create(&g_cb_thread, NULL, rpc_cb_thread_fn, NULL) != 0) {
        rpc_resp_queue_destroy();
        return -1;
    }
    return 0;
}

// 停止回调线程
static void rpc_stop_cb_thread(void) {
    pthread_mutex_lock(&g_resp_q.mu);
    g_resp_q.stop = true;
    // NOTE: 唤醒后的线程会阻塞在加锁
    pthread_cond_broadcast(&g_resp_q.not_empty);
    pthread_cond_broadcast(&g_resp_q.not_full);
    pthread_mutex_unlock(&g_resp_q.mu);
    pthread_join(g_cb_thread, NULL);
    rpc_resp_queue_destroy();
}

// 带body拷贝的事件推送，body_in可为NULL；若len>0则拷贝
// 生产者侧封装事件并推送回调队列（body 会被拷贝并在消费后释放）
static void rpc_emit_event(uint32_t id,
                           rpc_async_status_t status,
                           const char* body_in,
                           size_t body_len,
                           rpc_async_cb cb,
                           void* user_data) {
    rpc_resp_event_t ev;
    ev.id = id;
    ev.status = status;
    ev.cb = cb;
    ev.user_data = user_data;
    ev.body = NULL;
    ev.body_len = 0;
    if (body_in && body_len > 0) {
        ev.body = malloc(body_len + 1);
        if (ev.body) {
            memcpy(ev.body, body_in, body_len);
            ev.body[body_len] = '\0';
            ev.body_len = body_len;
        }
    }
    if (rpc_resp_queue_push(&ev) != 0) {
        // 队列停或push失败，释放拷贝
        rpc_resp_event_cleanup(&ev);
    }
}

// --------------- 异步接口占位 ---------------

int rpc_async_init(const char* ip, int port, int max_conn, int queue_size, int timeout_ms)
{
    (void)queue_size;
    (void)timeout_ms;
    if (!ip || ip[0] == '\0' || port <= 0 || max_conn <= 0) {
        return -1;
    }
    if ((size_t)snprintf(g_ip, sizeof(g_ip), "%s", ip) >= sizeof(g_ip)) {
        return -1;
    }
    g_port = port;
    g_pool_size = max_conn;
    g_pool = calloc((size_t)g_pool_size, sizeof(rpc_conn_t));
    if (!g_pool) {
        return -1;
    }
    for (int i = 0; i < g_pool_size; ++i) {
        g_pool[i].fd = -1;
        g_pool[i].in_use = false;
    }
    if (rpc_start_cb_thread(queue_size > 0 ? queue_size : g_default_queue_cap) != 0) {
        rpc_pool_destroy();
        return -1;
    }
    return 0;
}

void rpc_async_shutdown(void)
{
    rpc_stop_cb_thread();
    rpc_pool_destroy();
}

// 异步请求占位实现：注册 pending，发送头体，记录 fd，返回 id
int rpc_async_call(const char* json, rpc_async_cb cb, void* user_data, uint32_t* id_out)
{
    if (!json || !cb) {
        return -1;
    }
    size_t body_len = strlen(json);
    if (body_len > MAX_BODY_LEN) {
        return -1;
    }

    // 生成请求ID并注册pending
    uint32_t id = rpc_next_id();
    long long expire_ms = 0; // TODO: 引入超时计算
    if (!rpc_pending_add(id, -1, cb, user_data, expire_ms)) {
        return -1;
    }

    int fd = rpc_pool_get_conn();
    if (fd < 0) {
        rpc_pending_t* p = rpc_pending_take(id);
        if (p) free(p);
        return -1;
    }

    // 组装并发送请求（同步发送，占位实现）
    rpc_header_t header;
    header.version = htonl(1);
    header.body_len = htonl((uint32_t)body_len);
    header.crc32 = htonl(rpc_crc32(json, body_len));

    ssize_t n = send(fd, &header, RPC_HEADER_LEN, 0);
    if (n != RPC_HEADER_LEN) {
        rpc_pool_put_conn(fd, 1);
        rpc_pending_t* p = rpc_pending_take(id);
        if (p) {
            rpc_emit_event(id, RPC_ASYNC_SEND_ERR, NULL, 0, p->cb, p->user_data);
            free(p);
        }
        return -1;
    }

    size_t sent = 0;
    while (sent < body_len) {
        n = send(fd, json + sent, body_len - sent, 0);
        if (n <= 0) {
            rpc_pool_put_conn(fd, 1);
            rpc_pending_t* p = rpc_pending_take(id);
            if (p) {
                rpc_emit_event(id, RPC_ASYNC_SEND_ERR, NULL, 0, p->cb, p->user_data);
                free(p);
            }
            return -1;
        }
        sent += (size_t)n;
    }

    // 暂存fd到pending，供后续接收线程使用
    rpc_pending_t* p = rpc_pending_take(id);
    if (p) {
        p->fd = fd;
        // 此处应放回hash，但我们刚take了，需要再add回去
        rpc_pending_add(p->id, p->fd, p->cb, p->user_data, p->expire_ms);
        free(p);
    }
    rpc_pool_put_conn(fd, 0);

    if (id_out) {
        *id_out = id;
    }
    // TODO: 后续接收线程会基于fd读取响应、匹配pending并emit事件
    return 0;
}
