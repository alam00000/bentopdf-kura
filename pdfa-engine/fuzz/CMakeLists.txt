option(PDFA_FUZZ_LIBFUZZER
  "Build the coverage-guided libFuzzer binaries (needs a clang with the fuzzer runtime; Linux CI)"
  OFF)

set(PDFA_FUZZ_TARGETS convert profile invoice icc password)

foreach(t ${PDFA_FUZZ_TARGETS})
  add_executable(fuzz_replay_${t} fuzz_${t}.cpp replay_main.cpp)
  target_link_libraries(fuzz_replay_${t} PRIVATE pdfa_core)
  target_include_directories(fuzz_replay_${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

add_custom_target(fuzz_replay DEPENDS fuzz_replay_convert)

if(PDFA_FUZZ_LIBFUZZER)
  foreach(t ${PDFA_FUZZ_TARGETS})
    add_executable(fuzz_${t} fuzz_${t}.cpp)
    target_link_libraries(fuzz_${t} PRIVATE pdfa_core)
    target_include_directories(fuzz_${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(fuzz_${t} PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_${t} PRIVATE -fsanitize=fuzzer)
  endforeach()
endif()
