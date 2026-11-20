# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(virtqueue_restricted_pool)

target_sources(app PRIVATE src/main.c)
# white-box test: it looks at the pool and calls the interrupt handler of the transports
target_include_directories(app PRIVATE ${ZEPHYR_BASE}/drivers/virtio)
