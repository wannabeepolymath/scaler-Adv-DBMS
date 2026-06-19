cmake_minimum_required(VERSION 3.10)
project(txn_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(txn_engine main.cpp transaction_manager.cpp)
target_link_libraries(txn_engine PRIVATE Threads::Threads)
