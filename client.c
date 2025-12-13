
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


// 连接server
int conn_server(char *ip, int port)
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

int main(int argc, char *argv[])
{
    int sockfd = conn_server("127.0.0.1", 8888);
    if (sockfd < 0)
    {
        perror("conn_server");
        return -1;
    }

    // 发送数据
    char *msg = "hello server";
    int n = send(sockfd, msg, strlen(msg), 0);
    if (n < 0)
    {
        perror("send");
        return -1;
    }

    // 接收数据
    char buf[1024];
    n = recv(sockfd, buf, sizeof(buf), 0);
    if (n < 0)
    {
        perror("recv");
        return -1;
    }
    if (n == 0)
    {
        // 对端关闭连接
        close(sockfd);
        return 0;
    }
    // 处理收到的数据
    if (n > 0 && n < (int)sizeof(buf)) buf[n] = '\0';
    printf("recv %d bytes: %s\n", n, buf);
    // 关闭连接
    close(sockfd);
}
