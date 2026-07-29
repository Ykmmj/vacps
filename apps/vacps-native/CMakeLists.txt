cmake_minimum_required(VERSION 3.24)
project(vacps-native
  VERSION 0.1.0
  LANGUAGES C CXX
  DESCRIPTION "VACPS native agent (musl static ELF)"
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VACPS_STATIC "Link the agent as a fully static ELF" ON)
option(VACPS_ENABLE_ASAN "Enable AddressSanitizer (Debug only, dynamic)" OFF)
option(VACPS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

# ── Compiler / linker ──────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wformat
    -Wformat-security
  )
endif()

if(VACPS_STATIC)
  # Prefer lld when available (Alpine image sets clang-22 + lld).
  add_link_options(-fuse-ld=lld)
  add_link_options(-static)
  add_link_options(-Wl,--gc-sections)
  add_compile_options(-ffunction-sections -fdata-sections)
endif()

if(VACPS_ENABLE_ASAN)
  if(VACPS_STATIC)
    message(FATAL_ERROR "ASan is incompatible with VACPS_STATIC=ON")
  endif()
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()

if(VACPS_ENABLE_UBSAN)
  if(VACPS_STATIC)
    message(FATAL_ERROR "UBSan is incompatible with VACPS_STATIC=ON")
  endif()
  add_compile_options(-fsanitize=undefined)
  add_link_options(-fsanitize=undefined)
endif()

# ── Agent binary ───────────────────────────────────────────────────
add_executable(vacps-agent
  src/main.cpp
  src/version.cpp
)

set_target_properties(vacps-agent PROPERTIES
  OUTPUT_NAME "vacps-agent-linux-x86_64"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_include_directories(vacps-agent PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(vacps-agent PRIVATE
  VACPS_NATIVE_VERSION="${PROJECT_VERSION}"
)

# Placeholders for later FetchContent targets:
# target_link_libraries(vacps-agent PRIVATE Boost::asio OpenSSL::SSL ...)

include(GNUInstallDirs)
install(TARGETS vacps-agent RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

message(STATUS "vacps-native ${PROJECT_VERSION}")
message(STATUS "  C++ compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "  VACPS_STATIC: ${VACPS_STATIC}")
message(STATUS "  Build type:   ${CMAKE_BUILD_TYPE}")
