- 排查并修复连接已关闭的问题
- 这个实现用起来不方便，因为有回调，做不到用户无感 -> 实现future/promise模式
    - 我希望用户可以只include一个文件，就可以使用异步调用

- 重构后的异步调用流程
    - Stub 层 ( add_i32 ) :
    - 把 int a, int b 放入一个 cJSON 对象。
    - 调用 Codec 层 生成完整的 JSON-RPC 字符串。
    - 调用 传输层 的同步接口 rpc_call_async_blocking 。
    
    - 传输层 ( rpc_async ) :
    - 从 连接池 获取一个空闲连接。
    - 给请求分配一个唯一的 id （线程安全）。
    - 加上 rpc_header_t （包含长度和 CRC）。
    - 发送数据并 阻塞等待 该 id 的响应（利用 pthread_cond_t ）。

    - Stub 层 :
    - 拿到响应字符串，调用 Codec 层 解析出 result 。
    - 返回 int 结果给用户。


----------------------------------------------------------------

1. 多线程测试手动的实现 DONE
2. 实现脚本 DONE
3. 超时线程 DONE
4. ping/pong
5. JSON健壮性，防止畸形包攻击