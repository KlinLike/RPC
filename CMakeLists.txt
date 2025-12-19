cmake_minimum_required(VERSION 3.16)
project(RPC C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(server
    rpc_server_main.c
    cJSON.c
)
add_executable(client
    rpc_client_main.c
    rpc_client.c
    cJSON.c
    gen/rpc_client_stub.c
)

target_include_directories(client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/gen
)
