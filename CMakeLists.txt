cmake_minimum_required(VERSION 4.0)

find_package(cmake-bare REQUIRED PATHS node_modules/cmake-bare)

project(bare_channel C)

add_bare_module(bare_channel)

target_sources(
  ${bare_channel}
  PRIVATE
    binding.c
)
