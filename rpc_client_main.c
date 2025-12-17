
#include <stdio.h>

#include "rpc.h"
#include "rpc_client.h"
#include "rpc_api.h"

int main(int argc, char *argv[])
{
    if (rpc_client_init("127.0.0.1", 8888) != 0) {
        return -1;
    }
    int32_t result = add(1, 2);
    printf("result=%d\n", result);
}
