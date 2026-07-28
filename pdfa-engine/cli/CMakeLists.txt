add_executable(kura main.cpp)
target_link_libraries(kura PRIVATE pdfa_core)

if(PDFA_WITH_PDFIUM)
  target_link_libraries(kura PRIVATE kura_raster)
  target_compile_definitions(kura PRIVATE KURA_WITH_PDFIUM=1)
endif()
