add_clang_executable(c-arcmt-test
  c-arcmt-test.c
  )

target_link_libraries(c-arcmt-test
  PRIVATE
  libclang
  clangAST
  clangBasic
  clangCodeGen
  clangFrontend
  clangIndex
  clangSerialization
)

set_target_properties(c-arcmt-test
  PROPERTIES
  LINKER_LANGUAGE CXX)
