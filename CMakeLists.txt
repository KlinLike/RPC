cmake_minimum_required(VERSION 3.16)
project(RPC C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(server
    server/rpc_server_main.c
    cJSON.c
    server/rpc_server_helpers.c
    server/rpc_server_impl.c
    server/gen/rpc_server_skeleton.c
    crc.c
)
add_executable(client
    client/rpc_client_main.c
    client/rpc_client.c
    cJSON.c
    client/gen/rpc_client_api.c
    crc.c
)

target_include_directories(client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/client
    ${CMAKE_CURRENT_SOURCE_DIR}/client/gen
)
target_include_directories(server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/server
    ${CMAKE_CURRENT_SOURCE_DIR}/server/gen
)

target_link_libraries(server PRIVATE z)
target_link_libraries(client PRIVATE z)
