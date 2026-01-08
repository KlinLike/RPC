/**
 * @file rpc_client_main.c
 * @brief RPC 客户端测试程序（简化版）
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "rpc.h"
#include "rpc_async.h"
#include "rpc_client_manual.h"

int main(int argc, char *argv[])
{
    // 初始化异步客户端
    // 参数：IP, 端口, 最大连接数, 超时时间(ms)
    if (rpc_async_init("127.0.0.1", 8888, 4, 3000) != 0) {
        fprintf(stderr, "rpc_async_init failed\n");
        return -1;
    }

    // 测试手动实现的 add_i32
    int32_t res = add_i32_manual(1, 2);
    printf("add_i32_manual(1, 2) = %d\n", res);

    // 清理资源
    rpc_async_shutdown();

    return 0;
}
