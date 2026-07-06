add_executable(pdfa_wasm pdfa_wasm.cpp)
target_link_libraries(pdfa_wasm PRIVATE pdfa_core embind)
set_target_properties(pdfa_wasm PROPERTIES OUTPUT_NAME "pdfa")
target_link_options(pdfa_wasm PRIVATE
  -fwasm-exceptions
  -sMODULARIZE=1
  -sEXPORT_ES6=1
  -sEXPORT_NAME=createPdfaModule
  -sALLOW_MEMORY_GROWTH=1
  -sMAXIMUM_MEMORY=4GB
  -sENVIRONMENT=web,worker,node
  -sSTACK_SIZE=2MB
)
