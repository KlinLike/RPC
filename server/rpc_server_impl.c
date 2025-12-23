/*
 * 手写业务实现的集中放置文件。
 * 依据自动生成的 rpc_server_impl.h 中的声明实现各 *_impl。
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "rpc_server_impl.h"

// 简单的 strdup 实现，返回堆上字符串，调用方负责 free()。
static char* rpc_strdup(const char* s)
{
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s);
    char* out = (char*)malloc(n + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, n + 1);
    return out;
}

// Return a + b (i32)
int32_t add_i32_impl(int32_t a, int32_t b)
{
    return a + b;
}

// Return a simple string for connectivity test
char* ping_impl(void)
{
    return rpc_strdup("pong");
}

// Echo back x (i64)
int64_t echo_i64_impl(int64_t x)
{
    return x;
}

// Return a * b (double)
double mul_double_impl(double a, double b)
{
    return a * b;
}

// Return true if n is even
bool is_even_impl(int32_t v)
{
    return (v % 2) == 0;
}

// Return length of s (bytes)
int32_t strlen_s_impl(const char* s)
{
    if (s == NULL) {
        return 0;
    }
    return (int32_t)strlen(s);
}

// Return a formatted string mixing i32/double/bool
char* mix3_impl(int32_t a, double b, bool ok)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "a=%d,b=%.2f,ok=%s", a, b, ok ? "true" : "false");
    return rpc_strdup(buf);
}
