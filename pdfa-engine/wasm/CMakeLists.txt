add_executable(pdfa_wasm pdfa_wasm.cpp)
target_link_libraries(pdfa_wasm PRIVATE pdfa_core embind)

if(PDFA_WITH_PDFIUM)
  target_link_libraries(pdfa_wasm PRIVATE kura_raster)
  target_compile_definitions(pdfa_wasm PRIVATE KURA_WITH_PDFIUM=1)
endif()
set_target_properties(pdfa_wasm PROPERTIES OUTPUT_NAME "kura")
target_link_options(pdfa_wasm PRIVATE
  -Wl,--allow-multiple-definition
  -fwasm-exceptions
  -sMODULARIZE=1
  -sEXPORT_ES6=1
  -sEXPORT_NAME=createKuraModule
  -sALLOW_MEMORY_GROWTH=1
  -sMAXIMUM_MEMORY=4GB
  -sENVIRONMENT=web,worker,node
  -sSTACK_SIZE=2MB
)
