cmake_minimum_required(VERSION 4.0)

find_package(cmake-bare REQUIRED PATHS node_modules/cmake-bare)
find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)

project(bare_channel C)

fetch_package("github:holepunchto/libintrusive#01eae7f")

add_bare_module(bare_channel)

target_sources(
  ${bare_channel}
  PRIVATE
    binding.c
)

target_link_libraries(
  ${bare_channel}
  PUBLIC
    intrusive
)
