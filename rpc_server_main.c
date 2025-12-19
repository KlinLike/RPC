#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>

#include "rpc.h"
#include "cJSON.h"

static int32_t add_i32_impl(int32_t a, int32_t b);
static char* ping_impl(void);
static int64_t echo_i64_impl(int64_t x);

// strdup 的简单替代：复制字符串到堆上，调用者负责 free()。
static char* rpc_strdup(const char* s)
{
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s);
    char* out = (char*)malloc(n + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, n + 1);
    return out;
}

// 组装 JSON-RPC 2.0 error 响应：{jsonrpc, id, error:{code,message}}。
// 返回堆上分配的 JSON 字符串，调用者负责 free()。
static char* rpc_build_error_response(uint32_t id, int code, const char* message)
{
    cJSON* root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    if (cJSON_AddStringToObject(root, "jsonrpc", "2.0") == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddNumberToObject(root, "id", (double)id);

    cJSON* err = cJSON_CreateObject();
    if (err == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddItemToObject(root, "error", err);
    cJSON_AddNumberToObject(err, "code", (double)code);
    cJSON_AddStringToObject(err, "message", message ? message : "error");

    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

// 组装 JSON-RPC 2.0 result 响应：{jsonrpc, id, result}。
// 入参 result 的所有权会被转移到响应对象中；函数失败也会释放 result。
// 返回堆上分配的 JSON 字符串，调用者负责 free()。
static char* rpc_build_result_response(uint32_t id, cJSON* result)
{
    cJSON* root = cJSON_CreateObject();
    if (root == NULL) {
        cJSON_Delete(result);
        return NULL;
    }
    if (cJSON_AddStringToObject(root, "jsonrpc", "2.0") == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(result);
        return NULL;
    }
    cJSON_AddNumberToObject(root, "id", (double)id);
    cJSON_AddItemToObject(root, "result", result);

    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

// 解析 JSON 值为 int64：优先从 string 解析（避免 double 精度问题），兼容 number。
static bool rpc_parse_i64(const cJSON* v, int64_t* out)
{
    if (out == NULL) {
        return false;
    }
    if (v == NULL) {
        return false;
    }
    if (cJSON_IsString(v) && v->valuestring != NULL) {
        char* endptr = NULL;
        long long tmp = strtoll(v->valuestring, &endptr, 10);
        if (endptr == v->valuestring || *endptr != '\0') {
            return false;
        }
        *out = (int64_t)tmp;
        return true;
    }
    if (cJSON_IsNumber(v)) {
        *out = (int64_t)v->valuedouble;
        return true;
    }
    return false;
}

// 入参是客户端发来的JSON-RPC请求字符串，返回值是JSON-RPC响应字符串
// 服务端 dispatch：解析请求 JSON，按 method 分发到对应 *_impl，并组装 JSON-RPC 响应。
// 返回堆上分配的 JSON 响应字符串，调用者负责 free()。
static char* rpc_server_dispatch(const char* req_json)
{
    if (req_json == NULL) {
        return rpc_build_error_response(0, -32600, "Invalid Request");
    }

    cJSON* root = cJSON_Parse(req_json);
    if (root == NULL) {
        return rpc_build_error_response(0, -32700, "Parse error");
    }

    cJSON* id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
    uint32_t id = 0;
    if (cJSON_IsNumber(id_item)) {
        if (id_item->valuedouble < 0 || id_item->valuedouble > (double)UINT32_MAX) {
            cJSON_Delete(root);
            return rpc_build_error_response(0, -32600, "Invalid Request");
        }
        id = (uint32_t)id_item->valuedouble;
    }

    cJSON* method = cJSON_GetObjectItemCaseSensitive(root, "method");
    if (!cJSON_IsString(method) || method->valuestring == NULL) {
        cJSON_Delete(root);
        return rpc_build_error_response(id, -32600, "Invalid Request");
    }

    cJSON* params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (params == NULL) {
        params = cJSON_CreateObject();
        if (params == NULL) {
            cJSON_Delete(root);
            return rpc_build_error_response(id, -32603, "Internal error");
        }
    } else {
        params = cJSON_Duplicate(params, 1);
        if (params == NULL) {
            cJSON_Delete(root);
            return rpc_build_error_response(id, -32603, "Internal error");
        }
    }

    const char* m = method->valuestring;
    char* resp_json = NULL;

    if (strcmp(m, "add_i32") == 0) {
        cJSON* a_item = cJSON_GetObjectItemCaseSensitive(params, "a");
        cJSON* b_item = cJSON_GetObjectItemCaseSensitive(params, "b");
        if (!cJSON_IsNumber(a_item) || !cJSON_IsNumber(b_item)) {
            cJSON_Delete(params);
            cJSON_Delete(root);
            return rpc_build_error_response(id, -32602, "Invalid params");
        }
        int32_t a = (int32_t)a_item->valuedouble;
        int32_t b = (int32_t)b_item->valuedouble;
        int32_t r = add_i32_impl(a, b);
        cJSON* result = cJSON_CreateNumber((double)r);
        resp_json = rpc_build_result_response(id, result);
    } else if (strcmp(m, "ping") == 0) {
        char* s = ping_impl();
        if (s == NULL) {
            cJSON_Delete(params);
            cJSON_Delete(root);
            return rpc_build_error_response(id, -32603, "Internal error");
        }
        cJSON* result = cJSON_CreateString(s);
        free(s);
        resp_json = rpc_build_result_response(id, result);
    } else if (strcmp(m, "echo_i64") == 0) {
        cJSON* x_item = cJSON_GetObjectItemCaseSensitive(params, "x");
        int64_t x = 0;
        if (!rpc_parse_i64(x_item, &x)) {
            cJSON_Delete(params);
            cJSON_Delete(root);
            return rpc_build_error_response(id, -32602, "Invalid params");
        }
        int64_t y = echo_i64_impl(x);
        char buf[32];
        snprintf(buf, sizeof(buf), "%" PRId64, y);
        cJSON* result = cJSON_CreateString(buf);
        resp_json = rpc_build_result_response(id, result);
    } else {
        resp_json = rpc_build_error_response(id, -32601, "Method not found");
    }

    cJSON_Delete(params);
    cJSON_Delete(root);
    return resp_json;
}

static int32_t add_i32_impl(int32_t a, int32_t b)
{
    return a + b;
}

static char* ping_impl(void)
{
    return rpc_strdup("pong");
}

static int64_t echo_i64_impl(int64_t x)
{
    return x;
}

// 初始化服务端socket
int init_server(int port)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("socket");
        return -1;
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
    {
        perror("bind");
        return -1;
    }

    if (listen(sockfd, 5) < 0)
    {
        perror("listen");
        return -1;
    }

    printf("server listen on port %d\n", port);
    return sockfd;
}

int main(int argc, char *argv[])
{
    // 初始化epoll
    int epollfd = epoll_create(1);
    if (epollfd < 0)
    {
        perror("epoll_create");
        return -1;
    }

    // 初始化服务端socket
    int port = 8888;
    int sockfd = init_server(port);
    if (sockfd < 0)
    {
        perror("init_server");
        return -1;
    }

    // 注册服务端socket到epoll
    struct epoll_event ev;
    ev.data.fd = sockfd;
    ev.events = EPOLLIN;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sockfd, &ev) < 0)
    {
        perror("epoll_ctl");
        return -1;
    }

    // 主循环
    struct epoll_event events[10];
    while (1)
    {
        int nfds = epoll_wait(epollfd, events, 10, -1);
        if (nfds < 0)
        {
            perror("epoll_wait");
            return -1;
        }

        for (int i = 0; i < nfds; i++)
        {
            // if 处理新连接
            if (events[i].data.fd == sockfd)
            {
                int connfd = accept(sockfd, NULL, NULL);
                if (connfd < 0)
                {
                    perror("accept");
                    return -1;
                }

                // 注册新连接到epoll
                ev.data.fd = connfd;
                ev.events = EPOLLIN;
                if (epoll_ctl(epollfd, EPOLL_CTL_ADD, connfd, &ev) < 0)
                {
                    perror("epoll_ctl");
                    return -1;
                }
            }
            else // else 处理已连接socket
            {
                int connfd = events[i].data.fd;
                if (connfd < 0)
                {
                    perror("connfd");
                    return -1;
                }

                bool is_error = false;

                // 接收RPC Header
                char rpc_header[RPC_HEADER_LEN] = {0};
                ssize_t recv_bytes = 0;
                while (recv_bytes < RPC_HEADER_LEN)
                {
                    ssize_t n = recv(connfd, rpc_header + recv_bytes, RPC_HEADER_LEN - recv_bytes, 0);
                    if (n == -1 && errno == EINTR){
                        continue;
                    }
                    if (n == 0){
                        // 对端关闭连接
                        perror("recv header: connection closed");
                        is_error = true;
                        break;
                    }
                    if (n < 0){
                        // 异常
                        perror("recv header");
                        is_error = true;
                        break;
                    }
                    recv_bytes += n;
                }
                if (is_error){
                    close(connfd);
                    epoll_ctl(epollfd, EPOLL_CTL_DEL, connfd, NULL);
                    continue;
                }

                // 解析RPC Header
                rpc_header_t *header = (rpc_header_t *)rpc_header;
                uint32_t version = ntohl(header->version);
                uint32_t body_len = ntohl(header->body_len);
                uint32_t crc32 = ntohl(header->crc32);

                // 接收RPC Body
                char* body = malloc(body_len + 1);
                recv_bytes = 0;
                while (recv_bytes < body_len){
                    ssize_t n = recv(connfd, body + recv_bytes, body_len - recv_bytes, 0);
                    if (n == -1 && errno == EINTR){
                        continue;
                    }
                    if (n == 0){
                        // 对端关闭连接
                        printf("recv body: connection closed");
                        is_error = true;
                        break;
                    }
                    if (n < 0){
                        perror("recv body");
                        is_error = true;
                        break;
                    }
                    recv_bytes += n;
                }
                if (is_error){
                    close(connfd);
                    epoll_ctl(epollfd, EPOLL_CTL_DEL, connfd, NULL);
                    free(body);
                    continue;
                }
                body[body_len] = '\0';
                printf("recv %d bytes: %s\n", body_len, body);

                // 执行RPC处理函数
                char* resp_json = rpc_server_dispatch(body);
                if (resp_json == NULL) {
                    printf("rpc_server_dispatch failed\n");
                    // 注意: 处理失败会关闭连接
                    close(connfd);
                    epoll_ctl(epollfd, EPOLL_CTL_DEL, connfd, NULL);
                    free(body);
                    continue;
                }

                uint32_t resp_body_len = (uint32_t)strlen(resp_json);
                rpc_header_t resp_header;
                resp_header.version = htonl(version);
                resp_header.body_len = htonl(resp_body_len);
                resp_header.crc32 = htonl(0);

                ssize_t send_bytes = 0;
                while (send_bytes < RPC_HEADER_LEN){
                    ssize_t n = send(connfd, ((char*)&resp_header) + send_bytes, RPC_HEADER_LEN - send_bytes, 0);
                    if (n == -1 && errno == EINTR) {
                        continue; // EINTR等临时错误，继续重试
                    }
                    if (n < 0) {
                        perror("send rpc body");
                        is_error = true;
                        break;
                    }
                    if (n == 0) {
                        printf("send rpc body: connection closed\n");
                        is_error = true;
                        break;
                    }
                    send_bytes += n;
                }
                if (is_error) {
                    close(connfd);
                    epoll_ctl(epollfd, EPOLL_CTL_DEL, connfd, NULL);
                    free(resp_json);
                    free(body);
                    continue;
                }

                send_bytes = 0;
                while (send_bytes < resp_body_len){
                    ssize_t n = send(connfd, resp_json + send_bytes, resp_body_len - send_bytes, 0);
                    if (n == -1 && errno == EINTR) {
                        continue; // EINTR等临时错误，继续重试
                    }
                    if (n < 0) {
                        perror("send rpc body");
                        is_error = true;
                        break;
                    }
                    if (n == 0) {
                        printf("send rpc body: connection closed\n");
                        is_error = true;
                        break;
                    }
                    send_bytes += n;
                }
                if (is_error){
                    close(connfd);
                    epoll_ctl(epollfd, EPOLL_CTL_DEL, connfd, NULL);
                    free(resp_json);
                    free(body);
                    continue;
                }

                free(resp_json);
                free(body);
            }
        }
    }

    close(sockfd);
    close(epollfd);
    return 0;
}
