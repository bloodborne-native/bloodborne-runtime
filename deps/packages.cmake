# What the macOS build links against, and where the exact bytes come from: one call per package,
# version and checksum next to each other so they cannot drift. The version and the options are
# part of the package's stamp, so bumping one rebuilds that package alone.
#
# bb_dep(): a CMake project. Configure, build and install it; the arguments after the URL go to its
# configure step.
# bb_pin(): an archive with its own steps (CMakeLists.txt): a prebuilt dylib, or a build that is
# not CMake.
# bb_git(): a checkout. Its version is a tag (zydis needs its Zycore submodule) or a commit.

# Vulkan's headers and loader, MoltenVK as the driver (run.sh sets VK_DRIVER_FILES to it).
bb_dep(vulkan-headers v1.4.365
    ed832b3292cce324ebba6522fd2d3004bf1b797fa49bfbcad0b302ff23d56abc
    https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/@VERSION@.tar.gz)
bb_dep(vulkan-loader v1.4.365
    df0358f57c1936e4abd262a7327ba1f80ffb424dbbdcbcb01bb9288511f31eef
    https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/@VERSION@.tar.gz
    -DUPDATE_DEPS=OFF -DBUILD_WSI_XCB_SUPPORT=OFF -DBUILD_WSI_XLIB_SUPPORT=OFF
    -DBUILD_WSI_WAYLAND_SUPPORT=OFF)
bb_pin(moltenvk v1.4.2
    f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e
    https://github.com/KhronosGroup/MoltenVK/releases/download/@VERSION@/MoltenVK-macos.tar)

# SPIR-V to Metal Shading Language: the library the Metal backend calls, and the command line tool
# the FSR 4.1.1 assets need.
bb_dep(spirv-cross vulkan-sdk-1.4.363.0
    f708982e88b763ef5b0394ed468fc2c3628f68cdab89f2e7a946cd628c04e721
    https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/@VERSION@.tar.gz
    -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_CLI=OFF
    -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_ENABLE_HLSL=OFF -DSPIRV_CROSS_ENABLE_CPP=OFF
    -DSPIRV_CROSS_ENABLE_REFLECT=OFF -DSPIRV_CROSS_ENABLE_C_API=OFF -DSPIRV_CROSS_ENABLE_UTIL=OFF)
bb_dep(spirv-cross-cli vulkan-sdk-1.4.363.0
    f708982e88b763ef5b0394ed468fc2c3628f68cdab89f2e7a946cd628c04e721
    https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/@VERSION@.tar.gz
    -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_CLI=ON
    -DSPIRV_CROSS_ENABLE_TESTS=OFF)

# FSR 4.1.x on Metal: DXC compiles the 4.1 kernels, and the fsr4-native-macos tree carries
# them and the 4.1.2 asset tools.
bb_git(dxc v1.9.2607 https://github.com/microsoft/DirectXShaderCompiler.git)
bb_pin(fsr4-native-macos 87fe2167b500f0fe854f8024c23bf3edea38a595
    d8e501f07e962a809277421bac598b7faaf097d56240f5c53f33c1574b559689
    https://github.com/UzenUPoZiTiV4ik/fsr4-native-macos/archive/@VERSION@.tar.gz)

# The GPU library's dependencies.
bb_dep(sdl3 release-3.4.18
    c4b08b950bd29d83caae0ab8d884298d27046a9d30700fb42841c50992665c90
    https://github.com/libsdl-org/SDL/archive/refs/tags/@VERSION@.tar.gz
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF)
bb_dep(fmt 12.0.0
    aa3e8fbb6a0066c03454434add1f1fc23299e85758ceec0d7d2d974431481e40
    https://github.com/fmtlib/fmt/archive/refs/tags/@VERSION@.tar.gz
    -DFMT_TEST=OFF -DFMT_DOC=OFF)
bb_dep(magic-enum v0.9.7
    b403d3dad4ef542fdc3024fa37d3a6cedb4ad33c72e31b6d9bab89dcaf69edf7
    https://github.com/Neargye/magic_enum/archive/refs/tags/@VERSION@.tar.gz
    -DMAGIC_ENUM_OPT_BUILD_EXAMPLES=OFF -DMAGIC_ENUM_OPT_BUILD_TESTS=OFF)
bb_dep(robin-map v1.4.0
    7930dbf9634acfc02686d87f615c0f4f33135948130b8922331c16d90a03250c
    https://github.com/Tessil/robin-map/archive/refs/tags/@VERSION@.tar.gz)
bb_dep(vma v3.3.0
    c4f6bbe6b5a45c2eb610ca9d231158e313086d5b1a40c9922cb42b597419b14e
    https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/@VERSION@.tar.gz)
bb_dep(miniz 3.1.2
    98468f8924934b723276680f85238b6c78bf1f8b49b4459cc9b7214a20e2e9fb
    https://github.com/richgel999/miniz/archive/refs/tags/@VERSION@.tar.gz
    -DBUILD_EXAMPLES=OFF -DBUILD_FUZZERS=OFF -DBUILD_TESTS=OFF)
bb_dep(xbyak v7.43
    8fb0ed1ba0cc299b02aea3da84485083c9779a5c38b652640b2e331b7ca964f1
    https://github.com/herumi/xbyak/archive/refs/tags/@VERSION@.tar.gz)
bb_dep(xxhash v0.8.3
    aae608dfe8213dfd05d909a57718ef82f30722c392344583d3f39050c7f29a80
    https://github.com/Cyan4973/xxHash/archive/refs/tags/@VERSION@.tar.gz
    -DXXHASH_BUILD_XXHSUM=OFF -DBUILD_SHARED_LIBS=OFF)
set(BB_xxhash_SOURCE_SUBDIR cmake_unofficial) # its project files live there
# Header-only, the libraries the port uses: asio, container, container_hash, icl, intrusive, pool
# and preprocessor (boost::context). | separates them: a CMake list cannot be one configure
# argument, and ExternalProject turns | into ; again (LIST_SEPARATOR).
if (BB_ARCH STREQUAL "arm64")
    set(bb_boost_context -DBOOST_CONTEXT_ARCHITECTURE=arm64 -DBOOST_CONTEXT_ABI=aapcs)
else()
    set(bb_boost_context -DBOOST_CONTEXT_ARCHITECTURE=x86_64 -DBOOST_CONTEXT_ABI=sysv)
endif()
bb_dep(boost 1.89.0
    67acec02d0d118b5de9eb441f5fb707b3a1cdd884be00ca24b9a73c995511f74
    https://github.com/boostorg/boost/releases/download/boost-@VERSION@/boost-@VERSION@-cmake.tar.xz
    -DBOOST_INCLUDE_LIBRARIES=asio|container|container_hash|icl|intrusive|pool|preprocessor
    -DBOOST_SKIP_INSTALL_RULES=OFF ${bb_boost_context})

# AvPlayer: the decoders the game's movies need, as shared libraries.
bb_pin(ffmpeg n7.1.2
    8cb1bb8cfa9aeae13279b4da42ae8307ae6777456d4270f2e603c95aa08ca8ef
    https://github.com/FFmpeg/FFmpeg/archive/refs/tags/@VERSION@.tar.gz)
# bbcpu's x86 decoder. The tag comes with its Zycore submodule, which a tarball would lose.
bb_git(zydis v4.1.1 https://github.com/zyantific/zydis.git)

# The online module (gpu/bbnet20): shadNet's protocol buffers (protoc too, to generate them), JSON,
# TLS, UPnP port mapping, and epoll over kqueue. Static: they are linked into the module alone.
bb_dep(abseil 20260817.0
    f7e05179df39c45434cad433f5783840bb3788ef322976f9138bc6b72b3a107d
    https://github.com/abseil/abseil-cpp/archive/refs/tags/@VERSION@.tar.gz
    -DABSL_PROPAGATE_CXX_STD=ON -DABSL_BUILD_TESTING=OFF -DBUILD_SHARED_LIBS=OFF
    -DCMAKE_CXX_STANDARD=17 -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
bb_dep(protobuf 36.2
    3d9642a662d10e68ebae5e53f14dcce5105684212d5078f8e0d47d1ab3ae6b64
    https://github.com/protocolbuffers/protobuf/releases/download/v@VERSION@/protobuf-@VERSION@.tar.gz
    -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_ABSL_PROVIDER=package -Dprotobuf_BUILD_SHARED_LIBS=OFF
    -DBUILD_SHARED_LIBS=OFF -Dprotobuf_BUILD_PROTOC_BINARIES=ON -DCMAKE_CXX_STANDARD=17
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
bb_dep(nlohmann-json 3.12.0
    4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187
    https://github.com/nlohmann/json/archive/refs/tags/v@VERSION@.tar.gz
    -DJSON_BuildTests=OFF -DJSON_Install=ON)
bb_dep(miniupnpc 2.3.3
    8cf2c833b3e76fc4893ff29c2a376e3394962449e5970e373c0a91421724d222
    https://github.com/miniupnp/miniupnp/archive/refs/tags/miniupnpc_2_3_3.tar.gz
    -DUPNPC_BUILD_SHARED=OFF -DUPNPC_BUILD_STATIC=ON -DUPNPC_BUILD_TESTS=OFF -DUPNPC_BUILD_SAMPLE=OFF)
set(BB_miniupnpc_SOURCE_SUBDIR miniupnpc) # the repository holds the client and the daemon
bb_dep(epoll-shim 0.0.20240608
    8f5125217e4a0eeb96ab01f9dfd56c38f85ac3e8f26ef2578e538e72e87862cb
    https://github.com/jiixyj/epoll-shim/archive/refs/tags/v@VERSION@.tar.gz
    -DBUILD_SHARED_LIBS=OFF)
bb_pin(openssl 3.5.9
    603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
    https://github.com/openssl/openssl/releases/download/openssl-@VERSION@/openssl-@VERSION@.tar.gz)

# The app bundle's own runtime (packaging/macos.sh): run.sh needs bash 4.4 or newer and the
# start-up scripts need Python 3, so neither is left to the player's system.
bb_pin(app-bash 5.3
    0d5cd86965f869a26cf64f4b71be7b96f90a3ba8b3d74e27e8e9d9d5550f31ba
    https://ftp.gnu.org/gnu/bash/bash-@VERSION@.tar.gz)
bb_pin(app-python cpython-3.13.16+20261003
    9e01f63bbb08576cd9c8bc2d0564d098cb30c8453a0cd4bcf6aef458f6d2a147
    https://github.com/astral-sh/python-build-standalone/releases/download/20261003/cpython-3.13.16%2B20261003-aarch64-apple-darwin-install_only_stripped.tar.gz)
