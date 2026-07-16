option(PDFA_FUZZ_LIBFUZZER
  "Build the coverage-guided libFuzzer binary (needs a clang with the fuzzer runtime; Linux CI)"
  OFF)

add_executable(fuzz_replay fuzz_convert.cpp replay_main.cpp)
target_link_libraries(fuzz_replay PRIVATE pdfa_core)

if(PDFA_FUZZ_LIBFUZZER)
  add_executable(fuzz_convert fuzz_convert.cpp)
  target_link_libraries(fuzz_convert PRIVATE pdfa_core)
  target_compile_options(fuzz_convert PRIVATE -fsanitize=fuzzer)
  target_link_options(fuzz_convert PRIVATE -fsanitize=fuzzer)
endif()
