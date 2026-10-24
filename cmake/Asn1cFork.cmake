# The pinned asn1c fork (asn1c.ref), fetched and built at configure time.
#
# asn1c builds with autotools, so this file drives it directly: autoreconf in
# the fetched source, an out-of-tree ./configure, then make in the six
# directories asn1-viz needs (libasn1common, libasn1parser, libasn1fix,
# libasn1print, libasn1compiler, asn1c). It runs once per asn1c.ref; a stamp
# file holding the commit skips it on later configures.
#
# It is done at configure time, not as an ExternalProject, because the code
# asn1c generates for a schema is a list of files nobody can name before
# asn1c has run, and CMake needs that list to build the extractor.
#
# Provides:
#   ASN1C_SOURCE_DIR   the fork's source tree (skeletons/, examples/, tests/)
#   ASN1C_BUILD_DIR    its build tree (config.h, lib*/.libs/*.a, asn1c/asn1c)
#   ASN1C_EXECUTABLE   the asn1c compiler
#   asn1c::parser      imported static libraries: libasn1parser, libasn1fix,
#   asn1c::fix         libasn1common, with the fork's config.h force-included
#   asn1c::common      (the parser's struct layouts depend on it)
#
# A local checkout is used instead of a download with
# -DFETCHCONTENT_SOURCE_DIR_ASN1C=<path>. That checkout must not have been
# configured in its own tree (autotools refuses an out-of-tree build then).

include(FetchContent)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/asn1c.ref" ASN1VIZ_ASN1C_REF_DEFAULT)
string(STRIP "${ASN1VIZ_ASN1C_REF_DEFAULT}" ASN1VIZ_ASN1C_REF_DEFAULT)
set(ASN1VIZ_ASN1C_REF "${ASN1VIZ_ASN1C_REF_DEFAULT}" CACHE STRING "asn1c fork git ref (a full commit SHA)")

# SOURCE_SUBDIR names a directory without a CMakeLists.txt, so the fork is
# only populated, never added as a subproject.
FetchContent_Declare(asn1c
  GIT_REPOSITORY https://github.com/dmccoystephenson/asn1c.git
  GIT_TAG ${ASN1VIZ_ASN1C_REF}
  GIT_SHALLOW FALSE
  SOURCE_SUBDIR asn1viz-no-cmake)
FetchContent_MakeAvailable(asn1c)

set(ASN1C_SOURCE_DIR "${asn1c_SOURCE_DIR}")
set(ASN1C_BUILD_DIR "${CMAKE_BINARY_DIR}/asn1c-build")
set(ASN1C_EXECUTABLE "${ASN1C_BUILD_DIR}/asn1c/asn1c")
set(_asn1c_stamp "${ASN1C_BUILD_DIR}/asn1viz-built.stamp")

set(_asn1c_built "")
if(EXISTS "${_asn1c_stamp}")
  file(READ "${_asn1c_stamp}" _asn1c_built)
  string(STRIP "${_asn1c_built}" _asn1c_built)
endif()

if(NOT _asn1c_built STREQUAL ASN1VIZ_ASN1C_REF OR NOT EXISTS "${ASN1C_EXECUTABLE}")
  find_program(ASN1VIZ_AUTORECONF autoreconf REQUIRED)
  find_program(ASN1VIZ_MAKE NAMES gmake make REQUIRED)
  include(ProcessorCount)
  ProcessorCount(_ncpu)
  if(_ncpu EQUAL 0)
    set(_ncpu 2)
  endif()
  message(STATUS "asn1c fork ${ASN1VIZ_ASN1C_REF}: building (autoreconf, configure, make)")
  if(NOT EXISTS "${ASN1C_SOURCE_DIR}/configure")
    execute_process(COMMAND ${ASN1VIZ_AUTORECONF} -i
      WORKING_DIRECTORY "${ASN1C_SOURCE_DIR}"
      RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "asn1c: autoreconf -i failed:\n${_err}")
    endif()
  endif()
  file(MAKE_DIRECTORY "${ASN1C_BUILD_DIR}")
  execute_process(COMMAND "${ASN1C_SOURCE_DIR}/configure" "CFLAGS=-O2 -g0"
    WORKING_DIRECTORY "${ASN1C_BUILD_DIR}"
    RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "asn1c: configure failed:\n${_err}")
  endif()
  foreach(_dir libasn1common libasn1parser libasn1fix libasn1print libasn1compiler asn1c)
    execute_process(COMMAND ${ASN1VIZ_MAKE} -j${_ncpu}
      WORKING_DIRECTORY "${ASN1C_BUILD_DIR}/${_dir}"
      RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "asn1c: make in ${_dir} failed:\n${_err}")
    endif()
  endforeach()
  file(WRITE "${_asn1c_stamp}" "${ASN1VIZ_ASN1C_REF}\n")
endif()
message(STATUS "asn1c fork ${ASN1VIZ_ASN1C_REF}: ${ASN1C_EXECUTABLE}")

foreach(_lib common parser fix)
  add_library(asn1c::${_lib} STATIC IMPORTED GLOBAL)
  set_target_properties(asn1c::${_lib} PROPERTIES
    IMPORTED_LOCATION "${ASN1C_BUILD_DIR}/libasn1${_lib}/.libs/libasn1${_lib}.a")
  target_include_directories(asn1c::${_lib} INTERFACE
    "${ASN1C_BUILD_DIR}" "${ASN1C_SOURCE_DIR}/libasn1${_lib}" "${ASN1C_SOURCE_DIR}/libasn1common")
  target_compile_options(asn1c::${_lib} INTERFACE -include "${ASN1C_BUILD_DIR}/config.h")
endforeach()
# libasn1fix needs the parser, and both need the common helpers.
set_property(TARGET asn1c::fix APPEND PROPERTY INTERFACE_LINK_LIBRARIES asn1c::parser asn1c::common)
set_property(TARGET asn1c::parser APPEND PROPERTY INTERFACE_LINK_LIBRARIES asn1c::common)

# asn1viz_generate(<name> PDU <Type> SCHEMAS <file>... [FLAGS <flag>...])
#
# Runs the pinned asn1c over SCHEMAS into ${CMAKE_BINARY_DIR}/gen/<name>, at
# configure time, and sets <name>_GENERATED_SOURCES and <name>_GENERATED_DIR
# in the caller's scope. The generated code is never edited. It is rerun when
# a schema or the asn1c pin changes.
function(asn1viz_generate name)
  cmake_parse_arguments(G "" "PDU" "SCHEMAS;FLAGS" ${ARGN})
  set(out "${CMAKE_BINARY_DIR}/gen/${name}")
  set(key "${ASN1VIZ_ASN1C_REF};${G_PDU};${G_FLAGS}")
  foreach(s IN LISTS G_SCHEMAS)
    file(SHA256 "${s}" h)
    string(APPEND key ";${h}")
  endforeach()
  set(stamp "${out}/asn1viz-generated.stamp")
  set(old "")
  if(EXISTS "${stamp}")
    file(READ "${stamp}" old)
  endif()
  if(NOT old STREQUAL key)
    file(REMOVE_RECURSE "${out}")
    file(MAKE_DIRECTORY "${out}")
    execute_process(
      COMMAND "${ASN1C_EXECUTABLE}" -S "${ASN1C_SOURCE_DIR}/skeletons" -no-gen-example
              ${G_FLAGS} -pdu=${G_PDU} ${G_SCHEMAS}
      WORKING_DIRECTORY "${out}"
      RESULT_VARIABLE rc OUTPUT_VARIABLE log ERROR_VARIABLE log)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "asn1c failed on ${G_SCHEMAS}:\n${log}")
    endif()
    file(WRITE "${stamp}" "${key}")
  endif()
  file(GLOB srcs "${out}/*.c")
  set(${name}_GENERATED_SOURCES "${srcs}" PARENT_SCOPE)
  set(${name}_GENERATED_DIR "${out}" PARENT_SCOPE)
endfunction()
