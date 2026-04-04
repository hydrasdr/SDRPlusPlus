# Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.13)
project(if_average)

file(GLOB SRC "src/*.cpp" "src/*.h")
include(${SDRPP_MODULE_CMAKE})
target_include_directories(if_average PRIVATE "src/")
