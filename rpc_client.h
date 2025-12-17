#pragma once

int rpc_client_init(const char* ip, int port);
char* rpc_client_call(const char* json);
