#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// 计算CRC32校验码（使用zlib）
uint32_t rpc_crc32(const void *data, size_t len);

// 校验CRC32，expected_crc为主机字节序
bool rpc_crc32_verify(const void *data, size_t len, uint32_t expected_crc);
