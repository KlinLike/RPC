 #include <stdint.h>
 
typedef struct rpc_header_t {
    uint16_t version;
    uint16_t type;     // 消息类型: 0-DATA, 1-PING, 2-PONG
    uint32_t body_len; 
    uint32_t crc32;    
} __attribute__((packed)) rpc_header_t;

// 消息类型定义
#define RPC_TYPE_DATA 0
#define RPC_TYPE_PING 1
#define RPC_TYPE_PONG 2
// 不能用sizeof，因为struct可能有填充字节
#define RPC_HEADER_LEN 12

#define MAX_BODY_LEN 1024
#define MAX_CONNECTIONS 32


