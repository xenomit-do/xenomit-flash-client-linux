cmake_minimum_required(VERSION 3.16)
project(xenomit_flash_hook LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(SUBHOOK_STATIC ON CACHE BOOL "" FORCE)
set(SUBHOOK_TESTS OFF CACHE BOOL "" FORCE)
set(SUBHOOK_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/subhook subhook)

add_library(xenomit_flash_hook SHARED
  src/sha256.cpp
  src/capture.cpp
  src/preload.cpp
)
target_include_directories(xenomit_flash_hook PRIVATE src)
target_link_libraries(xenomit_flash_hook PRIVATE subhook)
target_compile_definitions(xenomit_flash_hook PRIVATE _GNU_SOURCE)
target_compile_options(xenomit_flash_hook PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_target_properties(xenomit_flash_hook PROPERTIES
  OUTPUT_NAME xenomit_flash_hook
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../build
)

add_executable(xenomit_flash_sha_test src/sha256.cpp src/sha256_test.cpp)
target_include_directories(xenomit_flash_sha_test PRIVATE src)
target_compile_definitions(xenomit_flash_sha_test PRIVATE _GNU_SOURCE)

set(XENOMIT_FLASH_CLIENT "${CMAKE_CURRENT_SOURCE_DIR}/../../xenomit-flash-client")
if(EXISTS "${XENOMIT_FLASH_CLIENT}/src/swf_bridge.cpp")
  set(INJECT_OUT "${CMAKE_CURRENT_SOURCE_DIR}/../build")
  add_executable(xenomit_flash_inject
    src/inject_main.cpp
    "${XENOMIT_FLASH_CLIENT}/src/swf_bridge.cpp"
    "${XENOMIT_FLASH_CLIENT}/third_party/miniz/miniz.c"
    "${XENOMIT_FLASH_CLIENT}/third_party/miniz/miniz_tdef.c"
    "${XENOMIT_FLASH_CLIENT}/third_party/miniz/miniz_tinfl.c"
    "${XENOMIT_FLASH_CLIENT}/third_party/lzma/LzmaDec.c"
  )
  target_include_directories(xenomit_flash_inject PRIVATE
    "${XENOMIT_FLASH_CLIENT}/src"
    "${XENOMIT_FLASH_CLIENT}/generated"
    "${XENOMIT_FLASH_CLIENT}/third_party/miniz"
    "${XENOMIT_FLASH_CLIENT}/third_party/lzma"
  )
  target_compile_definitions(xenomit_flash_inject PRIVATE _GNU_SOURCE _LARGEFILE64_SOURCE)
  set_source_files_properties(
    "${XENOMIT_FLASH_CLIENT}/third_party/miniz/miniz.c"
    "${XENOMIT_FLASH_CLIENT}/third_party/miniz/miniz_tdef.c"
    "${XENOMIT_FLASH_CLIENT}/third_party/miniz/miniz_tinfl.c"
    "${XENOMIT_FLASH_CLIENT}/third_party/lzma/LzmaDec.c"
    PROPERTIES COMPILE_OPTIONS "-w"
  )
  set_target_properties(xenomit_flash_inject PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${INJECT_OUT}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${INJECT_OUT}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${INJECT_OUT}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${INJECT_OUT}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${INJECT_OUT}"
  )
else()
  message(WARNING "xenomit-flash-client is missing; the reflection-bridge injector will not be built")
endif()

add_executable(xenomit_flash_capture_test src/capture_test.cpp)
target_include_directories(xenomit_flash_capture_test PRIVATE src)
target_compile_definitions(xenomit_flash_capture_test PRIVATE _GNU_SOURCE)
enable_testing()
add_test(NAME capture COMMAND xenomit_flash_capture_test)
add_test(NAME sha256 COMMAND xenomit_flash_sha_test)
