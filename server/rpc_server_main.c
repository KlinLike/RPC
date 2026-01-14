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
#include "crc.h"
#include "cJSON.h"
#include "rpc_server_skeleton.h"
#include "rpc_server_impl.h"

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

                // 先检查异常/半关闭事件
                uint32_t evts = events[i].events;
                if (evts & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                    printf("epoll event error/hup on fd %d, close\n", connfd);
                    close(connfd);
                    epoll_ctl(epollfd, EPOLL_CTL_DEL, connfd, NULL);
                    continue;
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
                        printf("recv header: connection closed\n");
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
                uint16_t version = ntohs(header->version);
                uint16_t type = ntohs(header->type);
                uint32_t body_len = ntohl(header->body_len);
                uint32_t crc32 = ntohl(header->crc32);

                // 处理协议级 PING
                if (type == RPC_TYPE_PING) {
                    uint8_t pong_buf[RPC_HEADER_LEN];
                    uint16_t v = htons(version);
                    uint16_t t = htons(RPC_TYPE_PONG);
                    uint32_t bl = 0;
                    uint32_t c = 0;

                    memcpy(pong_buf, &v, 2);
                    memcpy(pong_buf + 2, &t, 2);
                    memcpy(pong_buf + 4, &bl, 4);
                    memcpy(pong_buf + 8, &c, 4);

                    send(connfd, pong_buf, RPC_HEADER_LEN, 0);
                    continue;
                }

                if (body_len > MAX_BODY_LEN) {
                    // Body长度超过限制，关闭连接
                    fprintf(stderr, "body too large: %u\n", body_len);
                    close(connfd);
                    epoll_ctl(epollfd, EPOLL_CTL_DEL, connfd, NULL);
                    continue;
                }

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
                        printf("recv body: connection closed\n");
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

                // 校验CRC32（只对Body校验）
                if (!rpc_crc32_verify(body, body_len, crc32)) {
                    // CRC32校验失败，关闭连接
                    fprintf(stderr, "crc32 mismatch: recv %u\n", crc32);
                    close(connfd);
                    epoll_ctl(epollfd, EPOLL_CTL_DEL, connfd, NULL);
                    free(body);
                    continue;
                }
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
                uint32_t resp_crc32 = rpc_crc32(resp_json, resp_body_len);
                printf("send %u bytes: %s\n", resp_body_len, resp_json);

                // 构造响应 Header
                uint8_t resp_header_buf[RPC_HEADER_LEN];
                uint16_t rv = htons(version);
                uint16_t rt = htons(RPC_TYPE_DATA);
                uint32_t rbl = htonl(resp_body_len);
                uint32_t rc = htonl(resp_crc32);

                memcpy(resp_header_buf, &rv, 2);
                memcpy(resp_header_buf + 2, &rt, 2);
                memcpy(resp_header_buf + 4, &rbl, 4);
                memcpy(resp_header_buf + 8, &rc, 4);

                if (send(connfd, resp_header_buf, RPC_HEADER_LEN, 0) < 0) {
                    perror("send header");
                    close(connfd);
                    epoll_ctl(epollfd, EPOLL_CTL_DEL, connfd, NULL);
                    free(body);
                    free(resp_json);
                    continue;
                }

                size_t sent_body_bytes = 0;
                while (sent_body_bytes < resp_body_len){
                    ssize_t n = send(connfd, resp_json + sent_body_bytes, resp_body_len - sent_body_bytes, 0);
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
                    sent_body_bytes += n;
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
