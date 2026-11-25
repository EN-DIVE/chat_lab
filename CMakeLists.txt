cmake_minimum_required(VERSION 3.17)
project(chat_lab CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 第 2 块起服务端用 std::thread，必须有 -pthread
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

# common/ 被三个目标共用，只声明一次头文件路径
add_library(chat_common INTERFACE)
target_include_directories(chat_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(chat_common INTERFACE Threads::Threads)

add_executable(chat_server server/main.cpp)
target_link_libraries(chat_server PRIVATE chat_common)

add_executable(chat_client client/main.cpp)
target_link_libraries(chat_client PRIVATE chat_common)

enable_testing()

add_executable(protocol_test tests/protocol_test.cpp)
target_link_libraries(protocol_test PRIVATE chat_common)
add_test(NAME protocol_test COMMAND protocol_test)

# 集成测试：ctest 会以 $<TARGET_FILE:chat_server> 作为参数调用脚本
add_test(NAME integration
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.sh
                 $<TARGET_FILE:chat_server>)
set_tests_properties(integration PROPERTIES TIMEOUT 60)

# 客户端端到端：真的启动 chat_client，用管道喂输入
add_test(NAME client_e2e
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/client_e2e.sh
                 $<TARGET_FILE:chat_server> $<TARGET_FILE:chat_client>)
set_tests_properties(client_e2e PROPERTIES TIMEOUT 60)