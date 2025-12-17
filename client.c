
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "rpc.h"
#include "rpc_client.h"

int add(int a, int b){
    // 生成发送给服务器的 RPC JSON
    // 先用固定的JSON来测试功能，然后再实现序列化和反序列化
    char* rpc_json = "{ \
    \"request_id\": 1, \
    \"method\": \"add\", \
    \"param_types\": [\"int\", \"int\"],\
    \"params\": [\"1\", \"2\"],\
    \"return_type\": \"int\"\
    }";

    // 发送数据
    char* json = rpc_client_call("127.0.0.1", 8888, rpc_json);
    if (json != NULL){
        printf("recv %s\n", json);
        free(json);
    }

    return -1;
}

int main(int argc, char *argv[])
{
    add(1,2);
}
