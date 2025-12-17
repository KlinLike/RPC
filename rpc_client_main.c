
#include <stdio.h>
#include <stdlib.h>

#include "rpc.h"
#include "rpc_client.h"
#include "rpc_api.h"

int main(int argc, char *argv[])
{
    if (rpc_client_init("127.0.0.1", 8888) != 0) {
        return -1;
    }

    int32_t sum = add_i32(1, 2);
    printf("add_i32 result=%d\n", sum);

    char* pong = ping();
    if (pong != NULL) {
        printf("ping result=%s\n", pong);
        free(pong);
    }
}
