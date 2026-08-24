option(PDFA_FUZZ_LIBFUZZER
  "Build the coverage-guided fuzzer binaries (needs a fuzzing engine such as libFuzzer)"
  OFF)

set(PDFA_FUZZ_ENGINE "$ENV{LIB_FUZZING_ENGINE}" CACHE STRING
  "Fuzzing engine to link; defaults to LIB_FUZZING_ENGINE from the environment (OSS-Fuzz)")

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
    if(PDFA_FUZZ_ENGINE)
      target_link_libraries(fuzz_${t} PRIVATE ${PDFA_FUZZ_ENGINE})
    else()
      target_compile_options(fuzz_${t} PRIVATE -fsanitize=fuzzer)
      target_link_options(fuzz_${t} PRIVATE -fsanitize=fuzzer)
    endif()
  endforeach()
endif()
