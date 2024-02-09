cmake_minimum_required(VERSION 3.25)

project(bare_channel C)

include(bare)

add_bare_module(bare_channel)

target_sources(
  ${bare_channel}
  PRIVATE
    binding.c
)

if(MSVC)
  target_compile_options(
    ${bare_channel}
    PRIVATE
      /experimental:c11atomics
  )
endif()
