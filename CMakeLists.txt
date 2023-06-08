cmake_minimum_required(VERSION 3.25)

project(bare_atomics C)

include(bare)

add_bare_module(bare_atomics)

target_sources(
  bare_atomics
  PRIVATE
    binding.c
)
