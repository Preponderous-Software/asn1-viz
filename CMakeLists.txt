cmake_minimum_required(VERSION 3.24)
project(asn1_viz VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# The extractor needs the asn1c fork (autotools) and GNU ld's --wrap, so it is
# a Linux build. The viewer reads the asn1-layout/1 and asn1-schema/1 files in
# datasets/, so it builds anywhere Strata does (Linux, Windows, browser).
set(_asn1viz_asn1c_default OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT EMSCRIPTEN)
  set(_asn1viz_asn1c_default ON)
endif()
option(ASN1VIZ_WITH_ASN1C "Fetch and build the pinned asn1c fork, the extractor and the schema walker (Linux)"
       ${_asn1viz_asn1c_default})
option(ASN1VIZ_WITH_STRATA "Build the desktop viewer on Strata" ON)
option(ASN1VIZ_WEB "Build the browser viewer (Emscripten + Strata's WebGL 2 backend)" OFF)
option(ASN1VIZ_BUILD_TESTS "Build the test suite" ON)
option(ASN1VIZ_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(EMSCRIPTEN)
  # Strata compiles C++ exceptions as wasm exceptions; everything in one .wasm must match.
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-fwasm-exceptions>")
  add_link_options(-fwasm-exceptions -sALLOW_MEMORY_GROWTH=1)
  if(ASN1VIZ_WITH_STRATA)
    message(FATAL_ERROR "ASN1VIZ_WITH_STRATA builds the desktop viewer: use -DASN1VIZ_WITH_STRATA=OFF -DASN1VIZ_WEB=ON")
  endif()
endif()
if(ASN1VIZ_WEB AND NOT EMSCRIPTEN)
  message(FATAL_ERROR "ASN1VIZ_WEB needs Emscripten (emcmake cmake ...)")
endif()

function(asn1viz_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
    if(ASN1VIZ_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    if(ASN1VIZ_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------- the model (no engine)
# Readers for the two document formats and the geometry built from them. No
# engine and no GPU, so these build and test everywhere in seconds.
add_library(asn1viz_model STATIC
  src/model/json.cpp
  src/model/layout.cpp
  src/scene/camera.cpp
  src/scene/mesh.cpp)
target_include_directories(asn1viz_model PUBLIC src)
asn1viz_warnings(asn1viz_model)

if(ASN1VIZ_BUILD_TESTS)
  enable_testing()
  foreach(t json mesh camera layout)
    add_executable(test_${t} tests/test_${t}.cpp)
    target_link_libraries(test_${t} PRIVATE asn1viz_model)
    target_include_directories(test_${t} PRIVATE tests)
    asn1viz_warnings(test_${t})
    if(EMSCRIPTEN)
      target_link_options(test_${t} PRIVATE -sNODERAWFS=1)
    endif()
    add_test(NAME ${t} COMMAND test_${t} "${CMAKE_CURRENT_SOURCE_DIR}")
  endforeach()
endif()

# ---------------------------------------------------------------- the asn1c fork, extractors, datasets
if(ASN1VIZ_WITH_ASN1C)
  include(cmake/Asn1cFork.cmake)
  set(ASN1VIZ_DATASETS_OUT "${CMAKE_BINARY_DIR}/datasets")

  # The test module (tests/fixtures/demo.asn1): every structural element PER
  # writes, in 107 UPER bits.
  asn1viz_add_extractor(demo PDU Msg SCHEMAS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/demo.asn1"
                        FLAGS -fcompound-names)
  set(DEMO_LAYOUT "${ASN1VIZ_DATASETS_OUT}/demo/msg.layout.json")
  add_custom_command(OUTPUT "${DEMO_LAYOUT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ASN1VIZ_DATASETS_OUT}/demo"
    COMMAND asn1-viz-extract-demo --value "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/demo-msg.xer"
            --value-codec xer --schema demo.asn1 --out "${DEMO_LAYOUT}"
    DEPENDS asn1-viz-extract-demo tests/fixtures/demo-msg.xer
    COMMENT "Laying out the demo message")

  # PKIX (RFC 3280): the modules are extracted from the pinned fork's copy of
  # rfc3280.txt; the value is the fork's sample certificate (DER).
  asn1viz_rfc_modules(PKIX_MODULES "${ASN1C_SOURCE_DIR}/examples/rfc3280.txt" "${CMAKE_BINARY_DIR}/schemas/pkix")
  asn1viz_add_extractor(pkix PDU Certificate SCHEMAS ${PKIX_MODULES} FLAGS -fcompound-names -fwide-types)
  set(PKIX_SAMPLES "${ASN1C_SOURCE_DIR}/examples/sample.source.PKIX1")
  set(PKIX_LAYOUT "${ASN1VIZ_DATASETS_OUT}/pkix/certificate.layout.json")
  set(_pkix_schema_args "")
  foreach(m IN LISTS PKIX_MODULES)
    list(APPEND _pkix_schema_args --schema "${m}")
  endforeach()
  add_custom_command(OUTPUT "${PKIX_LAYOUT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ASN1VIZ_DATASETS_OUT}/pkix"
    COMMAND asn1-viz-extract-pkix --value "${PKIX_SAMPLES}/sample-Certificate-1.der" --value-codec der
            ${_pkix_schema_args} --out "${PKIX_LAYOUT}"
    DEPENDS asn1-viz-extract-pkix
    COMMENT "Laying out the PKIX sample certificate")

  add_custom_target(datasets ALL DEPENDS "${DEMO_LAYOUT}" "${PKIX_LAYOUT}")
  # Copies the regenerated, redistributable datasets into datasets/ (commit them).
  add_custom_target(update-datasets
    COMMAND ${CMAKE_COMMAND} -E copy "${PKIX_LAYOUT}" "${CMAKE_CURRENT_SOURCE_DIR}/datasets/pkix/certificate.layout.json"
    COMMAND ${CMAKE_COMMAND} -E copy "${DEMO_LAYOUT}" "${CMAKE_CURRENT_SOURCE_DIR}/datasets/demo/msg.layout.json"
    DEPENDS datasets)

  if(ASN1VIZ_BUILD_TESTS)
    # The pinned compiler parses and fixes the test module.
    add_test(NAME asn1c_parses_fixture
             COMMAND "${ASN1C_EXECUTABLE}" -E -F "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/demo.asn1")
    # The extractor's layouts: the demo's spans as the RFC's spike measured
    # them, and the PKIX encodings byte for byte equal to the fork's samples.
    add_executable(test_extractor tests/test_extractor.cpp)
    target_link_libraries(test_extractor PRIVATE asn1viz_model)
    target_include_directories(test_extractor PRIVATE tests)
    asn1viz_warnings(test_extractor)
    add_test(NAME extractor COMMAND test_extractor "${DEMO_LAYOUT}" "${PKIX_LAYOUT}"
             "${PKIX_SAMPLES}/sample-Certificate-1.per" "${PKIX_SAMPLES}/sample-Certificate-1.aper")
    # The committed PKIX layout is what the pinned fork produces today.
    add_test(NAME pkix_layout_up_to_date
             COMMAND ${CMAKE_COMMAND} -E compare_files "${PKIX_LAYOUT}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/datasets/pkix/certificate.layout.json")
    add_test(NAME demo_layout_up_to_date
             COMMAND ${CMAKE_COMMAND} -E compare_files "${DEMO_LAYOUT}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/datasets/demo/msg.layout.json")
  endif()
endif()

# ---------------------------------------------------------------- the viewer on Strata
# Strata is fetched at the commit in strata.ref, or taken from a local
# checkout with -DFETCHCONTENT_SOURCE_DIR_STRATA=<path>.
if(ASN1VIZ_WITH_STRATA OR ASN1VIZ_WEB)
  include(FetchContent)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/strata.ref" ASN1VIZ_STRATA_REF_DEFAULT)
  string(STRIP "${ASN1VIZ_STRATA_REF_DEFAULT}" ASN1VIZ_STRATA_REF_DEFAULT)
  set(ASN1VIZ_STRATA_REF "${ASN1VIZ_STRATA_REF_DEFAULT}" CACHE STRING "Strata git ref (a full commit SHA)")
  set(STRATA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(STRATA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(STRATA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(STRATA_WITH_SIM OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(strata
    GIT_REPOSITORY https://github.com/Preponderous-Software/strata.git
    GIT_TAG ${ASN1VIZ_STRATA_REF})
  FetchContent_MakeAvailable(strata)

  add_library(asn1viz_view STATIC src/view/shell.cpp)
  target_include_directories(asn1viz_view PUBLIC src)
  target_link_libraries(asn1viz_view PUBLIC asn1viz_model strata::static)
  asn1viz_warnings(asn1viz_view)
endif()

if(ASN1VIZ_WITH_STRATA)
  add_executable(asn1-viz app/main.cpp)
  target_link_libraries(asn1-viz PRIVATE asn1viz_view)
  target_compile_definitions(asn1-viz PRIVATE ASN1VIZ_VERSION="${PROJECT_VERSION}")
  asn1viz_warnings(asn1-viz)
endif()
