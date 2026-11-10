# Bound PS5 pipeline-build synchronization objects (backported from ProsperoEden).
# Keep the replacement ABI-compatible with headers seen outside video_core.
#
# The pair moves with the core: graphics pipelines used to keep their own build_condvar/build_mutex
# and now share one PipelineBuild per pipeline (pipeline_build.h), while a compute pipeline still
# keeps its own. Each pinned text is the whole member pair, so a core that renames or reorders them
# stops the build here rather than silently leaving a pipeline with an unbounded wait object.
function(eden_striped_wait header old new)
    file(READ "${PROJECT_SOURCE_DIR}/src/video_core/renderer_vulkan/${header}" pipeline_source)
    string(FIND "${pipeline_source}" "${old}" pipeline_wait_at)
    if(pipeline_wait_at LESS 0)
        message(FATAL_ERROR "Pinned ${header} build wait changed")
    endif()
    string(REPLACE "${old}" "${new}" pipeline_source "${pipeline_source}")
    string(PREPEND pipeline_source "#include \"${EDEN_PORT_DIR}/striped_wait.h\"\n")
    write_derived("${PORT_BUILD_DIR}/pipeline-wait/video_core/renderer_vulkan/${header}"
        "${pipeline_source}")
endfunction()

eden_striped_wait(pipeline_build.h
    "        std::mutex mutex;\n        std::condition_variable condition;\n"
    "        ::Eden::StripedMutex mutex;\n        ::Eden::StripedCondition condition;\n")

eden_striped_wait(vk_compute_pipeline.h
    "    std::condition_variable build_condvar;\n    std::mutex build_mutex;\n"
    "    ::Eden::StripedCondition build_condvar;\n    ::Eden::StripedMutex build_mutex;\n")

target_include_directories(video_core BEFORE PRIVATE "${PORT_BUILD_DIR}/pipeline-wait")
