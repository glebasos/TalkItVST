cmake_minimum_required(VERSION 3.24)
project(TalkItVST VERSION 0.1.0 LANGUAGES C CXX)

# clap-wrapper's macOS glue is Objective-C++. It detects the compiler itself,
# but the compile rules only exist for languages enabled from the top level,
# so without this the generate step fails on CMAKE_OBJCXX_COMPILE_OBJECT.
if(APPLE)
    enable_language(OBJC)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Plugins load into someone else's process: never export more than the entry
# point, and never let a symbol here collide with the host's.
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# project() has already created this cache entry, empty, so a plain
# set(... CACHE ...) here would do nothing. It has to be filled: clap-wrapper's
# shared_prologue.cmake dereferences it bare in an if(), which is a CMake error
# when it is empty. FORCE is guarded so -D on the command line still wins.
# 11.0 is the oldest target the current SDK's libc++ still supports; below it
# every translation unit fails on an availability warning that clap-wrapper
# compiles with -Werror. It is also above 10.15, so clap-wrapper uses
# std::filesystem instead of fetching a replacement.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

option(TALKITVST_BUILD_PLUGIN "Build the CLAP plugin (fetches CLAP from GitHub)" ON)
option(TALKITVST_BUILD_VST3   "Build the VST3 wrapper" ON)
option(TALKITVST_BUILD_AUV2   "Build the AUv2 wrapper" ${APPLE})
option(TALKITVST_BUILD_TOOLS  "Build the command line renderer and tests" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(tispeech)

# ---------------------------------------------------------------------------
# Core: everything that is not a plugin format. The command line tool and the
# tests link exactly this, so what they prove holds inside a host too.
# ---------------------------------------------------------------------------
add_library(talkit_core STATIC
    src/core/Engine.cpp
    src/core/Latin1.cpp
    src/core/RenderQueue.cpp
    src/core/Resampler.cpp)
target_include_directories(talkit_core PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(talkit_core PUBLIC tispeech::static Threads::Threads)

# ---------------------------------------------------------------------------
# Command line renderer and tests. Deliberately independent of the plugin
# formats, so the core can be built and tested with no network access.
# ---------------------------------------------------------------------------
if(TALKITVST_BUILD_TOOLS)
    add_executable(talkit-render tools/talkit-render.cpp)
    target_link_libraries(talkit-render PRIVATE talkit_core)

    enable_testing()
    add_executable(test_core tests/test_core.cpp)
    target_link_libraries(test_core PRIVATE talkit_core)
    # TalkIt_OSS registers its own tests into the same CTest project, and we
    # build its library targets only. Label ours so `ctest -L talkit` runs
    # this repo's tests without reporting theirs as not run.
    add_test(NAME core COMMAND test_core)
    set_tests_properties(core PROPERTIES LABELS talkit)
endif()

# ---------------------------------------------------------------------------
# CLAP, and the VST3 / AUv2 wrappers built from the same binary.
# ---------------------------------------------------------------------------
if(TALKITVST_BUILD_PLUGIN)
    include(FetchContent)

    FetchContent_Declare(clap
        GIT_REPOSITORY https://github.com/free-audio/clap.git
        GIT_TAG 1.2.10
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(clap)

    # One module target per format. target_add_vst3_wrapper() and
    # target_add_auv2_wrapper() call target_sources() on the target handed to
    # them, so each rewrites that target into its own format: applying both to
    # one target makes the second clobber the first.
    function(talkit_add_plugin_module name)
        set(sources src/clap/Plugin.cpp)
        if(APPLE)
            list(APPEND sources src/clap/Gui.mm)
        endif()
        add_library(${name} MODULE ${sources})
        target_link_libraries(${name} PRIVATE talkit_core clap)
        if(APPLE)
            # The window is AppKit, written against ARC.
            set_source_files_properties(src/clap/Gui.mm PROPERTIES
                COMPILE_OPTIONS "-fobjc-arc")
            target_link_libraries(${name} PRIVATE "-framework Cocoa")
        endif()
    endfunction()

    talkit_add_plugin_module(talkit_clap)
    set_target_properties(talkit_clap PROPERTIES
        OUTPUT_NAME "TalkIt"
        BUNDLE TRUE
        BUNDLE_EXTENSION clap
        SUFFIX ""
        MACOSX_BUNDLE_GUI_IDENTIFIER com.glebasos.talkit
        MACOSX_BUNDLE_BUNDLE_NAME "Talk It!"
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
    if(NOT APPLE)
        set_target_properties(talkit_clap PROPERTIES PREFIX "" SUFFIX ".clap")
    endif()

    if(TALKITVST_BUILD_VST3 OR TALKITVST_BUILD_AUV2)
        # Let clap-wrapper fetch the VST3 and AudioUnit SDKs itself, and let it
        # create its own wrapper targets: CLAP_WRAPPER_DONT_ADD_TARGETS would
        # suppress exactly the targets we are asking for below.
        set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES TRUE CACHE BOOL "" FORCE)
        FetchContent_Declare(clap-wrapper
            GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git
            GIT_TAG v0.17.0
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(clap-wrapper)
    endif()

    if(TALKITVST_BUILD_VST3)
        talkit_add_plugin_module(talkit_vst3)
        target_add_vst3_wrapper(TARGET talkit_vst3
            OUTPUT_NAME "TalkIt"
            BUNDLE_IDENTIFIER "com.glebasos.talkit.vst3"
            BUNDLE_VERSION ${PROJECT_VERSION})
    endif()

    # Loads a built .clap and exercises it as a host would. Steinberg's own
    # validator needs full Xcode, so this is what verifies the plugin layer
    # that all three formats share.
    # Objective-C++ because it creates a real NSView to parent the plugin
    # window into, the way a host does.
    if(TALKITVST_BUILD_TOOLS AND APPLE)
        add_executable(talkit-host tools/talkit-host.mm)
        target_link_libraries(talkit-host PRIVATE clap "-framework Cocoa")
        set_source_files_properties(tools/talkit-host.mm PROPERTIES
            COMPILE_OPTIONS "-fobjc-arc")
        add_test(NAME plugin
                 COMMAND talkit-host $<TARGET_BUNDLE_DIR:talkit_clap>)
        set_tests_properties(plugin PROPERTIES LABELS talkit)
    endif()

    if(TALKITVST_BUILD_AUV2)
        # AUv2 needs the four-character codes up front; they identify the
        # plugin to the system and must not change once anything has saved a
        # session with it.
        talkit_add_plugin_module(talkit_auv2)
        target_add_auv2_wrapper(TARGET talkit_auv2
            OUTPUT_NAME "TalkIt"
            BUNDLE_IDENTIFIER "com.glebasos.talkit.auv2"
            BUNDLE_VERSION ${PROJECT_VERSION}
            MANUFACTURER_NAME "glebasos"
            MANUFACTURER_CODE "Glbs"
            SUBTYPE_CODE "Tlkt"
            INSTRUMENT_TYPE "aumu")
    endif()
endif()
