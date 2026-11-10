# The ASTC decoder stores every texel of a block. A block at the right or bottom edge of an image
# whose size is not a multiple of the block size reaches past the image, and those stores are
# undefined: NVIDIA drops them, AMD lands them in other images' memory (black blocks and a
# crosshatch in Ocarina of Time on an RX 6750 XT, then a GPU fault). The PS5's GPU is AMD.
# From Nolar-Hack/Lichii eb518660ec (after CheezerWheezer/Lichii 132cc46c9e). The decoder is
# compiled again from a bounded copy into a header directory video_core sees first.
set(astc_source "${PROJECT_SOURCE_DIR}/src/video_core/host_shaders/astc_decoder.comp")
file(READ "${astc_source}" astc_text)
set(astc_store "imageStore(dest_image, coord + ivec3(i, j, 0), ")
string(REGEX MATCHALL "imageStore\\(dest_image, coord \\+ ivec3\\(i, j, 0\\), " astc_stores "${astc_text}")
list(LENGTH astc_stores astc_store_count)
set(astc_size_old "    if (any(greaterThanEqual(coord, imageSize(dest_image)))) {\n")
string(FIND "${astc_text}" "${astc_size_old}" astc_size_at)
set(astc_fill_old "\nvoid FillError(ivec3 coord) {\n")
string(FIND "${astc_text}" "${astc_fill_old}" astc_fill_at)
if(NOT astc_store_count EQUAL 3 OR astc_size_at LESS 0 OR astc_fill_at LESS 0)
    message(FATAL_ERROR "Pinned ASTC decoder stores changed")
endif()
string(REPLACE "${astc_store}" "StoreTexel(coord + ivec3(i, j, 0), " astc_text "${astc_text}")
string(REPLACE "${astc_fill_old}" [=[

ivec3 image_extent;

void StoreTexel(ivec3 texel, vec4 value) {
    if (all(lessThan(texel.xy, image_extent.xy))) {
        imageStore(dest_image, texel, value);
    }
}

void FillError(ivec3 coord) {
]=] astc_text "${astc_text}")
string(REPLACE "${astc_size_old}" [=[    image_extent = imageSize(dest_image);
    if (any(greaterThanEqual(coord, image_extent))) {
]=] astc_text "${astc_text}")
set(astc_dir "${PORT_BUILD_DIR}/astc-bounds")
write_derived("${astc_dir}/astc_decoder.comp" "${astc_text}")
set(astc_header "${astc_dir}/include/video_core/host_shaders/astc_decoder_comp_spv.h")
add_custom_command(
    OUTPUT "${astc_header}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${astc_dir}/include/video_core/host_shaders"
    COMMAND "${GLSLANGVALIDATOR}" -V -DUseUniformBlock=1 --variable-name ASTC_DECODER_COMP_SPV
            -o "${astc_header}" "${astc_dir}/astc_decoder.comp" --target-env spirv1.3
    MAIN_DEPENDENCY "${astc_dir}/astc_decoder.comp")
add_custom_target(eden_astc_bounds DEPENDS "${astc_header}")
add_dependencies(video_core eden_astc_bounds)
target_include_directories(video_core BEFORE PRIVATE "${astc_dir}/include")
