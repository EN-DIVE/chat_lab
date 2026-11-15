cmake_minimum_required(VERSION 3.17)
project(chat_lab CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra)

# 第 2 块会用到 std::thread，提前打开，避免到时出现 pthread_create 未定义
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# common/ 里的头文件被服务端、客户端、测试三方共用，只在这里声明一次
add_library(chat_common INTERFACE)
target_include_directories(chat_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(chat_common INTERFACE Threads::Threads)

add_executable(chat_server server/main.cpp)
target_link_libraries(chat_server PRIVATE chat_common)

add_executable(chat_client client/main.cpp)
target_link_libraries(chat_client PRIVATE chat_common)

# 测试目标 + 注册到 CTest，之后可以用 ctest 跑，而不是手动 ./protocol_test
enable_testing()
add_executable(protocol_test tests/protocol_test.cpp)
target_link_libraries(protocol_test PRIVATE chat_common)
add_test(NAME protocol_test COMMAND protocol_test)

