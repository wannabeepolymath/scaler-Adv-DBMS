cmake_minimum_required(VERSION 3.10)
project(query_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(query_engine main.cpp shunting_yard.cpp sql_parser.cpp)
