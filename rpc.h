 #include <stdint.h>
 
typedef struct rpc_header_t {
    uint32_t version;
    uint32_t body_len; // 需要注意64位系统size_t是64bit
    uint32_t crc32; // body的CRC32校验码
} rpc_header_t;
// 不能用sizeof，因为struct可能有填充字节
#define RPC_HEADER_LEN 12

#define MAX_BODY_LEN 1024
#define MAX_CONNECTIONS 32


