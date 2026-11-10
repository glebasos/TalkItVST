# ---------------------------------------------------------------------------
# A STATIC facade over TalkIt_OSS's engine.
#
# Upstream only exposes a SHARED `tispeech_shared` (for P/Invoke); every
# internal piece is already STATIC. A plugin wants none of a dylib's rpath
# and bundle-copy problems, so we compile upstream's own src/capi.c into a
# static library and link the same archives tispeech_shared does.
#
# Upstream gates those archives on local variables (_have_ENG, ...) that are
# not visible here, so we gate on target existence instead, which is
# equivalent: the targets are created inside exactly those branches.
# tispeech_capabilities() is checked at run time regardless.
# ---------------------------------------------------------------------------

if(NOT DEFINED TALKITVST_TISPEECH_DIR)
    set(TALKITVST_TISPEECH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../TalkIt_OSS"
        CACHE PATH "TalkIt_OSS checkout")
endif()
get_filename_component(TALKITVST_TISPEECH_DIR "${TALKITVST_TISPEECH_DIR}" ABSOLUTE)

if(NOT EXISTS "${TALKITVST_TISPEECH_DIR}/include/tispeech/capi.h")
    message(FATAL_ERROR
        "TalkIt_OSS not found at ${TALKITVST_TISPEECH_DIR}.\n"
        "Check it out beside this repo, or configure with "
        "-DTALKITVST_TISPEECH_DIR=/path/to/TalkIt_OSS")
endif()

# Upstream's own tests and svsay are not ours to build.
add_subdirectory("${TALKITVST_TISPEECH_DIR}" tispeech EXCLUDE_FROM_ALL)

add_library(tispeech_static STATIC "${TALKITVST_TISPEECH_DIR}/src/capi.c")
target_include_directories(tispeech_static
    PUBLIC "${TALKITVST_TISPEECH_DIR}/include")
target_link_libraries(tispeech_static PUBLIC tispeech_narrate)
set_target_properties(tispeech_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Mirrors CMakeLists.txt's tispeech_shared block, target-for-target.
if(TARGET tispeech_lang_eng AND TARGET tispeech_frontend_eng)
    target_link_libraries(tispeech_static PUBLIC tispeech_lang_eng tispeech_frontend_eng)
    target_compile_definitions(tispeech_static PRIVATE TISPEECH_HAVE_ENG=1)
endif()

if(TARGET tispeech_lang_span)
    if(NOT TARGET tispeech_frontend_span)   # which links it itself
        target_link_libraries(tispeech_static PUBLIC tispeech_lang_span)
    endif()
    target_compile_definitions(tispeech_static PRIVATE TISPEECH_HAVE_SPAN=1)
endif()

if(TARGET tispeech_synth_eng)
    target_link_libraries(tispeech_static PUBLIC tispeech_synth_eng)
    target_compile_definitions(tispeech_static PRIVATE TISPEECH_HAVE_SYNTH_ENG=1)
endif()

if(TARGET tispeech_frontend_span)
    target_link_libraries(tispeech_static PUBLIC tispeech_frontend_span tispeech_span_desc)
    target_compile_definitions(tispeech_static PRIVATE TISPEECH_HAVE_FRONTEND_SPAN=1)
    if(TARGET tispeech_synth_eng)
        target_compile_definitions(tispeech_static PRIVATE TISPEECH_HAVE_SYNTH_SPAN=1)
    endif()
endif()

add_library(tispeech::static ALIAS tispeech_static)
