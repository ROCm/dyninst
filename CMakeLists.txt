cmake_minimum_required(VERSION 3.14.0 FATAL_ERROR)

# There is a bug in 3.19.0 that causes .S files to be treated like C files
if(CMAKE_VERSION VERSION_EQUAL "3.19.0")
  message(FATAL_ERROR "Dyninst cannot use CMake version 3.19.0")
endif()

# find_package() uses upper-case <PACKAGENAME>_ROOT variables.
if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR AND CMAKE_CURRENT_SOURCE_DIR STREQUAL
                                                  CMAKE_SOURCE_DIR)
    message(STATUS "Warning! Building from the source directory is not recommended.")
    message(
        STATUS
            "If this is not intentional, please remove 'CMakeCache.txt' and 'CMakeFiles' and build from a separate directory"
        )
    message(AUTHOR_WARNING "In-source build")
endif()

set(DYNINST_MAJOR_VERSION 13)
set(DYNINST_MINOR_VERSION 0)
set(DYNINST_PATCH_VERSION 0)

set(DYNINST_SOVERSION "${DYNINST_MAJOR_VERSION}.${DYNINST_MINOR_VERSION}")
set(DYNINST_VERSION "${DYNINST_SOVERSION}.${DYNINST_PATCH_VERSION}")

project(
  Dyninst
  VERSION ${DYNINST_VERSION}
  DESCRIPTION "Tools for binary instrumentation, analysis, and modification"
  HOMEPAGE_URL "https://github.com/dyninst/dyninst"
  LANGUAGES C CXX ASM)

set(DYNINST_ROOT ${PROJECT_SOURCE_DIR})
set(CMAKE_SKIP_BUILD_RPATH OFF)
set(CMAKE_BUILD_WITH_INSTALL_RPATH OFF)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_INSTALL_MESSAGE LAZY) # only report new installations

list(INSERT CMAKE_MODULE_PATH 0 "${PROJECT_SOURCE_DIR}/cmake"
     "${PROJECT_SOURCE_DIR}/cmake/tpls" "${PROJECT_SOURCE_DIR}/cmake/Modules")

include(DyninstUtilities)

dyninst_add_option(
    STERILE_BUILD "Do not download/build any third-party dependencies from source" OFF
    ADVANCED)

# These need to be done before anything else
include(DyninstLibrarySettings)
include(DyninstOptions)
include(DyninstInstallDirs)

# Set up Dyninst internals
include(DyninstPlatform)
include(DyninstCapArchDef)
include(DyninstLanguageStandards)
include(DyninstWarnings)
include(DyninstOptimization)

# Locate third-party libraries
include(DyninstThreads)
include(DyninstThread_DB)
include(DyninstValgrind)
include(DyninstOpenMP)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW) # DOWNLOAD_EXTRACT_TIMESTAMP
endif()

# Setup custom job pools to control parallelism
set_property(GLOBAL PROPERTY JOB_POOLS external_deps_pool=1) # serial pool for external deps

set(TPL_STAGING_PREFIX
    "${PROJECT_BINARY_DIR}/tpls"
    CACHE PATH "Third-party library build-tree install prefix")
file(MAKE_DIRECTORY "${TPL_STAGING_PREFIX}")
file(MAKE_DIRECTORY "${TPL_STAGING_PREFIX}/include")

# Add external dependencies to be built
include(DyninstBoost)
if(TARGET Boost-External)
    # Make Boost build serially
    set_target_properties(Boost-External PROPERTIES
        JOB_POOL_COMPILE external_deps_pool
        JOB_POOL_LINK external_deps_pool)
    # Create a prebuild target that depends on Boost
    add_custom_target(external-prebuild)
    add_dependencies(external-prebuild Boost-External)
endif()

include(DyninstTBB)
if(TARGET TBB-External AND TARGET external-prebuild)
    # Make TBB build serially and wait for Boost
    set_target_properties(TBB-External PROPERTIES
        JOB_POOL_COMPILE external_deps_pool
        JOB_POOL_LINK external_deps_pool)
    add_dependencies(external-prebuild TBB-External)
endif()

include(DyninstElfUtils)
if(TARGET ElfUtils-External AND TARGET external-prebuild)
    set_target_properties(ElfUtils-External PROPERTIES
        JOB_POOL_COMPILE external_deps_pool
        JOB_POOL_LINK external_deps_pool)
    add_dependencies(external-prebuild ElfUtils-External)
endif()

include(DyninstLibIberty)
if(TARGET LibIberty-External AND TARGET external-prebuild)
    set_target_properties(LibIberty-External PROPERTIES
        JOB_POOL_COMPILE external_deps_pool
        JOB_POOL_LINK external_deps_pool)
    add_dependencies(external-prebuild LibIberty-External)
endif()

# for packaging
if(_DYNINST_BUILDING_TPLS)
    install(
        DIRECTORY ${TPL_STAGING_PREFIX}/include/
        DESTINATION ${INSTALL_LIB_DIR}/${TPL_INSTALL_INCLUDE_DIR}
        COMPONENT "tpls"
        OPTIONAL)

    install(
        DIRECTORY ${TPL_STAGING_PREFIX}/bin/
        DESTINATION ${INSTALL_LIB_DIR}/${TPL_INSTALL_BIN_DIR}
        COMPONENT "tpls"
        OPTIONAL)

    install(
        DIRECTORY ${TPL_STAGING_PREFIX}/share/
        DESTINATION ${INSTALL_LIB_DIR}/${TPL_INSTALL_DOC_DIR}
        COMPONENT "tpls"
        OPTIONAL)

    install(
        DIRECTORY ${TPL_STAGING_PREFIX}/lib/
        DESTINATION ${INSTALL_LIB_DIR}/${TPL_INSTALL_LIB_DIR}
        COMPONENT "tpls"
        OPTIONAL FILES_MATCHING
        PATTERN "*${CMAKE_SHARED_LIBRARY_SUFFIX}*"
        PATTERN "*${CMAKE_STATIC_LIBRARY_SUFFIX}")

    install(
        DIRECTORY ${TPL_STAGING_PREFIX}/lib/pkgconfig/
        DESTINATION ${INSTALL_LIB_DIR}/${TPL_INSTALL_LIB_DIR}/pkgconfig
        COMPONENT "tpls"
        OPTIONAL FILES_MATCHING
        PATTERN "*.pc")
endif()

include(DyninstLibrary)

# Create a dummy target to ensure external dependencies are fully built
add_custom_target(external-deps-complete)
if(TARGET external-prebuild)
    add_dependencies(external-deps-complete external-prebuild)
endif()

# Add subdirectories with dependencies on external libraries
add_subdirectory(common)
if(TARGET common AND TARGET external-deps-complete)
    add_dependencies(common external-deps-complete)
endif()

add_subdirectory(elf)
if(TARGET elf AND TARGET external-deps-complete)
    add_dependencies(elf external-deps-complete)
endif()

add_subdirectory(dwarf)
if(TARGET dwarf AND TARGET external-deps-complete)
    add_dependencies(dwarf external-deps-complete)
endif()

add_subdirectory(symlite)
if(TARGET symlite AND TARGET external-deps-complete)
    add_dependencies(symlite external-deps-complete)
endif()

add_subdirectory(instructionAPI)
if(TARGET instructionAPI AND TARGET external-deps-complete)
    add_dependencies(instructionAPI external-deps-complete)
endif()

add_subdirectory(symtabAPI)
if(TARGET symtabAPI AND TARGET external-deps-complete)
    add_dependencies(symtabAPI external-deps-complete)
endif()

add_subdirectory(parseAPI)
if(TARGET parseAPI AND TARGET external-deps-complete)
    add_dependencies(parseAPI external-deps-complete)
endif()

add_subdirectory(proccontrol)
if(TARGET pcontrol AND TARGET external-deps-complete)
    add_dependencies(pcontrol external-deps-complete)
endif()

add_subdirectory(stackwalk)
if(TARGET stackwalk AND TARGET external-deps-complete)
    add_dependencies(stackwalk external-deps-complete)
endif()

add_subdirectory(patchAPI)
if(TARGET patchAPI AND TARGET external-deps-complete)
    add_dependencies(patchAPI external-deps-complete)
endif()

add_subdirectory(dyninstAPI)
if(TARGET dyninstAPI AND TARGET external-deps-complete)
    add_dependencies(dyninstAPI external-deps-complete)
endif()

add_subdirectory(dynC_API)
if(TARGET dynC_API AND TARGET external-deps-complete)
    add_dependencies(dynC_API external-deps-complete)
endif()

add_subdirectory(parseThat)
if(TARGET parseThat AND TARGET external-deps-complete)
    add_dependencies(parseThat external-deps-complete)
endif()

if(BUILD_RTLIB)
    add_subdirectory(dyninstAPI_RT)
else()
    message(
        STATUS
            "Skipping DyninstAPI_RT. Be sure to build this library if you're using instrumentation."
        )
endif()
set(VERSION_STRING
    "${DYNINST_MAJOR_VERSION}.${DYNINST_MINOR_VERSION}.${DYNINST_PATCH_VERSION}")
set(DYNINST_NAME "DyninstAPI-${VERSION_STRING}")

if(BUILD_TARBALLS)
    find_package(Git)
    if(GIT_FOUND)
        if(EXISTS "${DYNINST_ROOT}/.git/")
            message(STATUS "Source tree is repository, building archive target")
            add_custom_target(package ALL)
            add_custom_command(
                TARGET package
                COMMAND ${GIT_EXECUTABLE} archive --prefix="${DYNINST_NAME}/"
                        --format=tar.gz -o "${CMAKE_BINARY_DIR}/${DYNINST_NAME}.tgz" HEAD
                WORKING_DIRECTORY ${DYNINST_ROOT}
                COMMENT "Packaging Dyninst")
        endif()
    endif()
endif()

if(BUILD_DOCS)
    add_custom_target(doc)
    set(LATEX_DOCS
        dynC_API
        dataflowAPI
        instructionAPI
        parseAPI
        patchAPI
        symtabAPI
        stackwalk)
    set(WORD_DOCS proccontrol dyninstAPI)
    add_custom_target(proccontrol-doc
                      DEPENDS ${CMAKE_SOURCE_DIR}/proccontrol/doc/proccontrol.pdf)
    add_custom_target(dyninstAPI-doc
                      DEPENDS ${CMAKE_SOURCE_DIR}/dyninstAPI/doc/dyninstAPI.pdf)
    foreach(COMPONENT ${WORD_DOCS})
        add_dependencies(doc ${COMPONENT}-doc)
        set_target_properties(${COMPONENT}-doc PROPERTIES EXCLUDE_FROM_DEFAULT_BUILD 1)
        install(
            FILES ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}/doc/${COMPONENT}.pdf
            DESTINATION ${INSTALL_DOC_DIR}
            RENAME ${COMPONENT}-${VERSION_STRING}.pdf
            OPTIONAL)
    endforeach()

    find_package(LATEX)
    if(PDFLATEX_COMPILER)
        file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/common/doc
             DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/common)
        foreach(COMPONENT ${LATEX_DOCS})
            file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/${COMPONENT}/doc
                 DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT})
            set(DEPS "")
            file(GLOB_RECURSE DEPS ${CMAKE_CURRENT_SOURCE_DIR}/${COMPONENT}/doc/*.tex
                 ${CMAKE_CURRENT_SOURCE_DIR}/common/doc/*.tex)
            add_custom_command(
                OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}/doc/${COMPONENT}.aux
                DEPENDS ${DEPS}
                COMMAND ${PDFLATEX_COMPILER} ARGS -interaction=batchmode ${COMPONENT}.tex
                WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}/doc
                COMMENT "Latex (first pass)")
            add_custom_command(
                OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}/doc/${COMPONENT}.log
                DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}/doc/${COMPONENT}.aux
                COMMAND ${PDFLATEX_COMPILER} ARGS -interaction=batchmode ${COMPONENT}.tex
                WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}/doc
                COMMENT "Latex (second pass)")
            add_custom_target(
                ${COMPONENT}-doc
                echo
                DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}/doc/${COMPONENT}.log)
            add_dependencies(doc ${COMPONENT}-doc)
            set_target_properties(${COMPONENT}-doc PROPERTIES EXCLUDE_FROM_DEFAULT_BUILD
                                                              1)
            install(
                FILES ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}/doc/${COMPONENT}.pdf
                DESTINATION ${INSTALL_DOC_DIR}
                RENAME ${COMPONENT}-${VERSION_STRING}.pdf
                OPTIONAL)
        endforeach()
    else()
        message(STATUS "LaTeX not found")
    endif()
endif()

if(DYNINST_ENABLE_TESTS)
  enable_testing()
  message(STATUS "Enabling tests")
  add_subdirectory(tests)
endif()

include(DyninstInstall)

dyninst_print_features()
