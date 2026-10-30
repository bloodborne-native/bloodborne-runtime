# deps/: the libraries the macOS build links against, built once into deps/macos-<arch>.
# scripts/macos/build-deps.sh runs this; the game's build calls that script, which does nothing
# when every package is already installed. On Linux the libraries come from shell.nix instead.
#
#   cmake -S deps -B build/deps-macos-arm64 -G Ninja
#   cmake --build build/deps-macos-arm64                     # everything
#   cmake --build build/deps-macos-arm64 --target sdl3       # one package
#   cmake --build build/deps-macos-arm64 --target deps-clean # everything again
#
# deps/macos-<arch> holds everything a package needs, its build tree and its stamps included:
# removing that directory is the one way to make a package build again, and nothing can disagree
# about what is installed. Each package's directory carries its version and its configure options,
# so bumping one (packages.cmake) starts a fresh tree for it and leaves the others alone.
cmake_minimum_required(VERSION 3.24)
project(bbport-deps LANGUAGES NONE)
include(ExternalProject)

if (NOT APPLE)
    message(FATAL_ERROR "deps/ builds the macOS libraries (deps/macos-<arch>); on Linux they come "
                        "from shell.nix")
endif()

# Two builds share one prefix: the native one (bbcpu translates the game's x86-64 code) and the
# Rosetta 2 one. deps/macos-<arch> keeps them apart.
set(BB_ARCH "" CACHE STRING "arm64 or x86_64 (default: this machine's)")
if (NOT BB_ARCH)
    execute_process(COMMAND sysctl -n hw.optional.arm64 OUTPUT_VARIABLE bb_arm64 ERROR_QUIET)
    if (bb_arm64 MATCHES "1")
        set(BB_ARCH arm64)
    else()
        set(BB_ARCH x86_64)
    endif()
endif()
if (NOT BB_ARCH MATCHES "^(arm64|x86_64)$")
    message(FATAL_ERROR "BB_ARCH: arm64 or x86_64, not '${BB_ARCH}'")
endif()

set(BB_DEPS_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/macos-${BB_ARCH}" CACHE PATH "Prefix")
set(BB_DEPS_JOBS "" CACHE STRING "Parallel jobs (default: this machine's cores)")
if (NOT BB_DEPS_JOBS)
    execute_process(COMMAND sysctl -n hw.ncpu OUTPUT_VARIABLE BB_DEPS_JOBS
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()

# The minimum macOS, the same number as the app's manifest and the game's own build. Everything
# installed here is made relocatable afterwards (steps/relocatable.cmake), so moving the checkout
# does not cost an hour of rebuilding.
set(BB_DEPS_MIN_MACOS 15.0)
set(BB_DEPS_DOWNLOAD "${CMAKE_BINARY_DIR}/download" CACHE PATH "Downloaded archives")
set(BB_DEPS_STEPS "${CMAKE_CURRENT_SOURCE_DIR}/steps")
file(MAKE_DIRECTORY "${BB_DEPS_PREFIX}" "${BB_DEPS_DOWNLOAD}")
# The steps are part of the manifest (a package's key hashes them): configure again when one
# changes.
file(GLOB BB_DEPS_STEP_FILES CONFIGURE_DEPENDS ${BB_DEPS_STEPS}/*.cmake)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${BB_DEPS_STEP_FILES})

# FSR 4.1.x on Metal needs DXC and SPIRV-Cross commands, clang-format for DXC's generated sources
# and spirv-val to check the kernels.
option(BB_DEPS_FSR4 "FSR 4.1.x on Metal assets (clang-format and spirv-val needed)" ON)
find_program(BB_DEPS_CLANG_FORMAT clang-format)
find_program(BB_DEPS_SPIRV_VAL spirv-val)
if (BB_DEPS_FSR4 AND (NOT BB_DEPS_CLANG_FORMAT OR NOT BB_DEPS_SPIRV_VAL))
    message(WARNING "clang-format or spirv-val missing (brew install clang-format spirv-tools): "
                    "FSR 4.1.x on Metal is not built")
    set(BB_DEPS_FSR4 OFF)
endif()

if (POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW) # archive timestamps, so a fresh install never looks newer
endif()

# --- the manifest ---------------------------------------------------------------------------
# bb_dep() records a CMake project, bb_pin() an archive with its own steps, bb_git() a checkout.
function(bb_dep name version sha256 url)
    string(REPLACE "@VERSION@" "${version}" url "${url}") # the URL carries it once, like the pin
    set(BB_PACKAGES ${BB_PACKAGES} ${name} PARENT_SCOPE)
    set(BB_${name}_KIND "archive" PARENT_SCOPE)
    set(BB_${name}_VERSION "${version}" PARENT_SCOPE)
    set(BB_${name}_URL "${url}" PARENT_SCOPE)
    set(BB_${name}_SHA256 "${sha256}" PARENT_SCOPE)
    set(BB_${name}_ARGS "${ARGN}" PARENT_SCOPE) # everything after the URL: configure arguments
endfunction()

function(bb_pin name version sha256 url)
    string(REPLACE "@VERSION@" "${version}" url "${url}")
    set(BB_PACKAGES ${BB_PACKAGES} ${name} PARENT_SCOPE)
    set(BB_${name}_KIND "steps" PARENT_SCOPE)
    set(BB_${name}_VERSION "${version}" PARENT_SCOPE)
    set(BB_${name}_URL "${url}" PARENT_SCOPE)
    set(BB_${name}_SHA256 "${sha256}" PARENT_SCOPE)
endfunction()

function(bb_git name version url)
    set(BB_PACKAGES ${BB_PACKAGES} ${name} PARENT_SCOPE)
    set(BB_${name}_KIND "checkout" PARENT_SCOPE)
    set(BB_${name}_VERSION "${version}" PARENT_SCOPE)
    set(BB_${name}_URL "${url}" PARENT_SCOPE)
endfunction()

# bb_package(<name>): the package's directory, named after the version and the options it is
# configured with. Everything ExternalProject keeps for it (sources, build, stamps) is in there.
function(bb_package name)
    # The recipe counts: a package whose build or install steps changed is built again.
    if (BB_${name}_KIND STREQUAL "archive")
        set(recipe ${BB_DEPS_STEPS}/cmake-install.cmake)
    else()
        set(recipe ${BB_DEPS_STEPS}/${name}.cmake)
    endif()
    file(SHA256 "${recipe}" recipe)
    string(SHA256 key
           "${BB_${name}_VERSION};${BB_${name}_ARGS};${BB_${name}_SOURCE_SUBDIR};${recipe}")
    string(SUBSTRING "${key}" 0 8 key)
    # ExternalProject's own layout: its stamps live in <dir>/src/<name>-stamp, so the checkout and
    # its build tree sit next to them, never around them.
    set(dir "${BB_DEPS_PREFIX}/.build/${name}-${BB_${name}_VERSION}-${key}")
    set(BB_${name}_DIR "${dir}" PARENT_SCOPE)
    set(BB_${name}_SRC "${dir}/src/${name}" PARENT_SCOPE)
    set(BB_${name}_BIN "${dir}/src/${name}-build" PARENT_SCOPE)
endfunction()

# No -G: ExternalProject passes the generator this build was configured with.
set(BB_DEPS_CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_OSX_ARCHITECTURES=${BB_ARCH}
    -DCMAKE_OSX_DEPLOYMENT_TARGET=${BB_DEPS_MIN_MACOS}
    -DCMAKE_INSTALL_PREFIX=${BB_DEPS_PREFIX}
    -DCMAKE_PREFIX_PATH=${BB_DEPS_PREFIX}
    -DBUILD_TESTING=OFF
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5) # Zydis, Zycore and VMA still ask for CMake 3.10

# Each package downloads into its own directory: ExternalProject names the file after the URL, and
# two packages can carry the same tag (Vulkan-Headers and Vulkan-Loader are both v1.4.365), which
# would leave one file where the other's hash is expected.
#
# The variables the step scripts get: their directories, the architecture they build for and where
# the prefix is.
function(bb_step_args name script out)
    set(args
        -DBB_SRC=${BB_${name}_SRC}
        -DBB_BIN=${BB_${name}_BIN}
        -DBB_ARCH=${BB_ARCH}
        -DBB_CORES=${BB_DEPS_JOBS}
        -DBB_MIN_MACOS=${BB_DEPS_MIN_MACOS}
        -DBB_PREFIX=${BB_DEPS_PREFIX}
        -DBB_STEPS=${BB_DEPS_STEPS}
        -DBB_SPIRV_VAL=${BB_DEPS_SPIRV_VAL}
        -P ${BB_DEPS_STEPS}/${script}.cmake)
    set(${out} "${args}" PARENT_SCOPE)
endfunction()

# bb_archive(<name>): download, configure, build and install a CMake project.
function(bb_archive name)
    set(subdir "")
    if (BB_${name}_SOURCE_SUBDIR)
        set(subdir SOURCE_SUBDIR "${BB_${name}_SOURCE_SUBDIR}")
    endif()
    bb_step_args(${name} cmake-install install)
    ExternalProject_Add(${name}
        PREFIX "${BB_${name}_DIR}"
        DOWNLOAD_DIR "${BB_DEPS_DOWNLOAD}/${name}"
        URL "${BB_${name}_URL}"
        URL_HASH "SHA256=${BB_${name}_SHA256}"
        SOURCE_DIR "${BB_${name}_SRC}"
        BINARY_DIR "${BB_${name}_BIN}"
        ${subdir}
        CMAKE_ARGS ${BB_DEPS_CMAKE_ARGS} ${BB_${name}_ARGS}
        LIST_SEPARATOR |
        UPDATE_COMMAND ""
        INSTALL_COMMAND ${CMAKE_COMMAND} ${install}
        LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON)
endfunction()

# bb_steps(<name>): an archive whose configure, build and install steps are steps/<name>.cmake.
function(bb_steps name)
    bb_step_args(${name} ${name} steps)
    ExternalProject_Add(${name}
        PREFIX "${BB_${name}_DIR}"
        DOWNLOAD_DIR "${BB_DEPS_DOWNLOAD}/${name}"
        URL "${BB_${name}_URL}"
        URL_HASH "SHA256=${BB_${name}_SHA256}"
        SOURCE_DIR "${BB_${name}_SRC}"
        BINARY_DIR "${BB_${name}_BIN}"
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        UPDATE_COMMAND ""
        INSTALL_COMMAND ${CMAKE_COMMAND} ${steps}
        LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON)
endfunction()

# bb_checkout(<name>): a git package, cloned at its tag, built by steps/<name>.cmake.
function(bb_checkout name)
    bb_step_args(${name} ${name} steps)
    ExternalProject_Add(${name}
        PREFIX "${BB_${name}_DIR}"
        GIT_REPOSITORY "${BB_${name}_URL}"
        GIT_TAG "${BB_${name}_VERSION}"
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
        SOURCE_DIR "${BB_${name}_SRC}"
        BINARY_DIR "${BB_${name}_BIN}"
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        UPDATE_COMMAND ""
        INSTALL_COMMAND ${CMAKE_COMMAND} ${steps}
        LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON)
endfunction()

include(packages.cmake)

# Packages this machine or this configuration does not build.
set(BB_DEPS_SKIP "")
if (NOT BB_DEPS_FSR4)
    list(APPEND BB_DEPS_SKIP dxc fsr4-native-macos)
endif()
if (NOT BB_ARCH STREQUAL "arm64")
    list(APPEND BB_DEPS_SKIP app-python) # the app is arm64 only
endif()

set(BB_DEPS_TARGETS "")
foreach(name IN LISTS BB_PACKAGES)
    bb_package(${name})
    if (name IN_LIST BB_DEPS_SKIP)
        continue()
    endif()
    list(APPEND BB_DEPS_TARGETS ${name})
    if (BB_${name}_KIND STREQUAL "archive")
        bb_archive(${name})
    elseif (BB_${name}_KIND STREQUAL "steps")
        bb_steps(${name})
    else()
        bb_checkout(${name})
    endif()
endforeach()

# Who needs whom installed first: the loader's CMake asks for the headers' package, the
# fsr4-native-macos tree is assembled from DXC's and SPIRV-Cross's binaries, and Protobuf builds on
# Abseil.
if (TARGET vulkan-loader)
    add_dependencies(vulkan-loader vulkan-headers)
endif()
if (TARGET fsr4-native-macos)
    add_dependencies(fsr4-native-macos dxc spirv-cross-cli)
endif()
if (TARGET protobuf)
    add_dependencies(protobuf abseil) # protobuf_ABSL_PROVIDER=package: the installed one
endif()

# After every install: the libraries and the pkg-config files are made relocatable in one pass.
# Always out of date, because it is also what repairs a prefix installed by an older version.
add_custom_target(relocate ALL
    COMMAND ${CMAKE_COMMAND} -DBB_PREFIX=${BB_DEPS_PREFIX} -P ${BB_DEPS_STEPS}/relocatable.cmake
    COMMENT "deps: @rpath ids and relocatable pkg-config files")
add_dependencies(relocate ${BB_DEPS_TARGETS})

# The installed libraries, and the directory that has to go to install them again.
add_custom_target(deps-clean
    COMMAND ${CMAKE_COMMAND} -E echo "Removing ${BB_DEPS_PREFIX}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${BB_DEPS_PREFIX}"
    COMMENT "deps: every package fetches, builds and installs again (archives are kept in "
            "${BB_DEPS_DOWNLOAD})")
