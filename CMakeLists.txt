cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

project(rdma_echo VERSION 1.0)

# ${CMAKE_SOURCE_DIR} is the root directory of this project;
# ${CMAKE_BINARY_DIR} is the build directory;

add_executable(server_rdma
    src/server.cpp
)

add_executable(client_rdma
    src/client.cpp
)

target_link_libraries(server_rdma PRIVATE ibverbs rdmacm pthread)
target_link_libraries(client_rdma PRIVATE ibverbs rdmacm pthread)
