cmake_minimum_required(VERSION 3.16)
project(RPC C)
set(CMAKE_C_STANDARD 99)

# 导出编译命令数据库，用于 IDE 代码跳转和补全
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(server
    server/rpc_server_main.c
    third_party/cJSON.c
    server/rpc_server_helpers.c
    server/rpc_server_impl.c
    server/gen/rpc_server_skeleton.c
    crc.c
)
add_executable(client
    client/rpc_client_main.c
    client/rpc_client_manual.c
    client/rpc_codec.c
    third_party/cJSON.c
    client/rpc_async.c
    client/pending.c
    client/conn_pool.c
    client/epoll_api.c
    crc.c
)

target_include_directories(client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/client
    ${CMAKE_CURRENT_SOURCE_DIR}/client/gen
    ${CMAKE_CURRENT_SOURCE_DIR}/client/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_include_directories(server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/server
    ${CMAKE_CURRENT_SOURCE_DIR}/server/gen
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_link_libraries(server PRIVATE z)
target_link_libraries(client PRIVATE z)
