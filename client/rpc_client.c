
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>

#include "rpc.h"
#include "rpc_client.h"
#include "crc.h"


static char g_rpc_server_ip[64] = {0};
static int g_rpc_server_port = 0;
static bool g_rpc_client_inited = false;


// 连接server
static int conn_server(const char *ip, int port)
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
    servaddr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &servaddr.sin_addr);

    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
    {
        perror("connect");
        return -1;
    }

    return sockfd;
}

static char* send_recv_close(int fd, const char* json) {
    // 组织RPC Header
    // 只有先用size_t检查长度，才是安全的，避免uint32_t溢出
    size_t json_len = strlen(json);
    if (json_len > MAX_BODY_LEN) {
        fprintf(stderr, "JSON body too large: %zu bytes\n", json_len);
        return NULL;
    }

    rpc_header_t header_send;
    header_send.version = 1;
    header_send.body_len = (uint32_t)json_len;
    header_send.crc32 = rpc_crc32(json, json_len); // 只对Body进行CRC32校验，简单够用

    char rpc_header[RPC_HEADER_LEN];
    // 如果是跨平台，不进行字节序的转换就会有问题
    uint32_t v = htonl(header_send.version);
    uint32_t len = htonl(header_send.body_len);
    uint32_t crc = htonl(header_send.crc32);
    memcpy(rpc_header, &v, 4);
    memcpy(rpc_header + 4, &len, 4);
    memcpy(rpc_header + 8, &crc, 4);

    // 发送头
    ssize_t send_bytes = 0;
    while (send_bytes < RPC_HEADER_LEN) {
        // send的len参数是size_t类型，因此设置body_len为32位无符号整数，避免32位系统上的截断问题
        ssize_t n = send(fd, rpc_header + send_bytes, RPC_HEADER_LEN - send_bytes, 0);
        if (n == -1 && errno == EINTR){
            continue; // EINTR等临时错误，继续重试
        }
        if (n < 0) {
            perror("send header");
            return NULL;
        }
        if (n == 0) {
            fprintf(stderr, "send header: connection closed\n");
            return NULL;
        }
        send_bytes += n;
    }

    // 发送RPC请求体
    send_bytes = 0;
    while (send_bytes < header_send.body_len) {
        ssize_t n = send(fd, json + send_bytes, header_send.body_len - send_bytes, 0);
        if (n == -1 && errno == EINTR) {
            continue; // EINTR等临时错误，继续重试
        }
        if (n < 0) {
            perror("send rpc body");
            return NULL;
        }
        if (n == 0) {
            fprintf(stderr, "send rpc body: connection closed\n");
            return NULL;
        }
        send_bytes += n;
    }

    // 接收RPC Header
    ssize_t recv_bytes = 0;
    memset(rpc_header, 0, RPC_HEADER_LEN);
    while (recv_bytes < RPC_HEADER_LEN) {
        recv_bytes = recv(fd, rpc_header + recv_bytes, RPC_HEADER_LEN - recv_bytes, 0);
        if (recv_bytes == 0) {
            fprintf(stderr, "recv header: connection closed\n");
            return NULL;
        }
        if (recv_bytes == -1 && errno == EINTR) {
            continue;
        }
        if (recv_bytes < 0) {
            perror("recv header");
            return NULL;
        }
    }

    // 解析RPC Header
    rpc_header_t *header_recv = (rpc_header_t *)rpc_header;
    uint32_t version = ntohl(header_recv->version);
    uint32_t body_len = ntohl(header_recv->body_len);
    uint32_t crc32 = ntohl(header_recv->crc32);

    // 接收RPC Body
    recv_bytes = 0;
    char* body = malloc(body_len + 1);
    while (recv_bytes < body_len) {
        recv_bytes = recv(fd, body + recv_bytes, body_len - recv_bytes, 0);
        if (recv_bytes == 0) {
            fprintf(stderr, "recv header: connection closed\n");
            free(body);
            return NULL;
        }
        if (recv_bytes == -1 && errno == EINTR) {
            continue;
        }
        if (recv_bytes < 0) {
            perror("recv header");
            free(body);
            return NULL;
        }
    }
    body[body_len] = '\0';

    // 校验响应的CRC32（只对Body校验）
    if (!rpc_crc32_verify(body, body_len, crc32)) {
        fprintf(stderr, "resp crc32 mismatch: recv %u\n", crc32);
        free(body);
        close(fd);
        return NULL;
    }

    close(fd);
    return body;
}

int rpc_client_init(const char* ip, int port)
{
    if (ip == NULL || ip[0] == '\0')
    {
        fprintf(stderr, "rpc_client_init: invalid ip\n");
        return -1;
    }
    if (port <= 0 || port > 65535)
    {
        fprintf(stderr, "rpc_client_init: invalid port %d\n", port);
        return -1;
    }

    strncpy(g_rpc_server_ip, ip, sizeof(g_rpc_server_ip) - 1);
    g_rpc_server_ip[sizeof(g_rpc_server_ip) - 1] = '\0';
    g_rpc_server_port = port;
    g_rpc_client_inited = true;
    return 0;
}

char* rpc_client_call(const char* json)
{
    if (!g_rpc_client_inited)
    {
        fprintf(stderr, "rpc_client_call: client not initialized, call rpc_client_init() first\n");
        return NULL;
    }

    int sockfd = conn_server(g_rpc_server_ip, g_rpc_server_port);
    if (sockfd < 0)
    {
        perror("conn_server");
        return NULL;
    }

    return send_recv_close(sockfd, json);
}
