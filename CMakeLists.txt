# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(VS_Code)

# BLE 低功耗从机用 main2.c；按钮 demo 用 main.c（需改回 main.c 再编译）
target_sources(app PRIVATE src/main.c)
