# The fsr4-native-macos v2.2 tree (gpu/fsr4_metal/build_assets.py reads it): its third-party
# sources and tools (4.1's HLSL, the 4.1.2 imported-shader catalogue, the native generators), with
# the commands they call pointed at the prefix's own and AMD's initializers left out (they stay
# with the player: tools/fsr4_metal_initializers.sh).
set(bb_tree "${BB_PREFIX}/share/fsr4-native-macos")
file(REMOVE_RECURSE "${bb_tree}")
file(MAKE_DIRECTORY "${bb_tree}/tools/bin")
execute_process(COMMAND ditto "${BB_SRC}/third_party" "${bb_tree}/third_party" RESULT_VARIABLE rc
                COMMAND_ECHO STDOUT)
if (rc EQUAL 0)
    execute_process(COMMAND ditto "${BB_SRC}/tools" "${bb_tree}/tools" RESULT_VARIABLE rc
                    COMMAND_ECHO STDOUT)
endif()
if (NOT rc EQUAL 0)
    message(FATAL_ERROR "copying the fsr4-native-macos tree failed (${rc})")
endif()
file(GLOB_RECURSE bb_initializers "${bb_tree}/initializers*.bin")
file(REMOVE ${bb_initializers})
foreach(tool dxc spirv-cross)
    execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink
                    "${BB_PREFIX}/bin/${tool}" "${bb_tree}/tools/bin/${tool}")
endforeach()
execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink
                "${BB_SPIRV_VAL}" "${bb_tree}/tools/bin/spirv-val")
