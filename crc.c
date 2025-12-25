#include <zlib.h> // sudo apt-get install zlib1g-dev
#include "crc.h"

uint32_t rpc_crc32(const void *data, size_t len)
{
    uLong c = crc32(0L, Z_NULL, 0);
    c = crc32(c, (const Bytef *)data, len);
    return (uint32_t)c;
}

bool rpc_crc32_verify(const void *data, size_t len, uint32_t expected_crc)
{
    return rpc_crc32(data, len) == expected_crc;
}
