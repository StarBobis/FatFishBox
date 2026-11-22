cmake_minimum_required(VERSION 3.26)
project(FatFishBox VERSION 0.1.10 LANGUAGES CXX RC)

set(NATIVE_WIN_UI_SOURCE "C:/Dev/NativeWinUI" CACHE PATH "NativeWinUI source checkout")
include(CTest)
include(FetchContent)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(httplib
    URL https://codeload.github.com/yhirose/cpp-httplib/tar.gz/refs/tags/v0.67.0
    URL_HASH SHA256=be711ddaf7a3655d8f23aa5655709250b24d247aedd4b2629989d18b6164e5be
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR unused)
FetchContent_Declare(zstd
    URL https://codeload.github.com/meta/zstd/tar.gz/refs/tags/v1.5.7
    URL_HASH SHA256=37d7284556b20954e56e1ca85b80226768902e2edabd3b649e9e72c0c9012ee3
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR unused)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json httplib zstd)
add_subdirectory("${zstd_SOURCE_DIR}/build/cmake" "${zstd_BINARY_DIR}" EXCLUDE_FROM_ALL)

function(fatfish_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
endfunction()

foreach(role principal lead worker)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/prompts/${role}.md")
    string(TOUPPER "${role}" role_upper)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/prompts/${role}.md" ${role_upper}_PROMPT)
    # MSVC limits an individual UTF-8 literal. Keep detailed role instructions
    # intact by concatenating one raw literal per source line.
    string(REPLACE "\n" "\n)ffprompt\"\nR\"ffprompt(" ${role_upper}_PROMPT "${${role_upper}_PROMPT}")
endforeach()
configure_file(src/team_prompts.h.in team_prompts.h @ONLY)
add_library(fatfish_core STATIC src/diagnostics.cpp src/team.cpp src/cluster.cpp src/evidence.cpp src/work_items.cpp src/handoff_artifacts.cpp src/dsh.cpp src/dsh_recovery.cpp src/dsh_history.cpp src/legacy.cpp)
fatfish_target(fatfish_core)
target_include_directories(fatfish_core PUBLIC src PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(fatfish_core PUBLIC nlohmann_json::nlohmann_json PRIVATE dbghelp libzstd_static version winhttp ole32 user32 shell32 advapi32 bcrypt)

if(NOT EXISTS "${NATIVE_WIN_UI_SOURCE}/ui/cmake/NativeTargets.cmake")
    message(FATAL_ERROR "Set NATIVE_WIN_UI_SOURCE to the NativeWinUI source directory")
endif()
include("${NATIVE_WIN_UI_SOURCE}/ui/cmake/NativeTargets.cmake")
block()
    set(BUILD_TESTING OFF)
    set(NATIVE_UI_BUILD_GALLERY OFF)
    add_subdirectory("${NATIVE_WIN_UI_SOURCE}/ui" native_ui EXCLUDE_FROM_ALL)
endblock()
set(DSH_DESKTOP_PLUGIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dsh-desktop-plugin")
set(DSH_DESKTOP_PLUGIN_FILES package.json index.cjs cordis.patch.yml)
foreach(plugin_file IN LISTS DSH_DESKTOP_PLUGIN_FILES)
    set(plugin_path "${DSH_DESKTOP_PLUGIN_DIR}/${plugin_file}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${plugin_path}")
    file(SHA256 "${plugin_path}" plugin_hash)
    string(APPEND DSH_DESKTOP_PLUGIN_HASHES "${plugin_hash} ")
endforeach()
configure_file(src/dsh_desktop_plugin.rc.in dsh_desktop_plugin.rc @ONLY)
add_executable(FatFishBox WIN32 src/app.cpp src/app.rc "${CMAKE_CURRENT_BINARY_DIR}/dsh_desktop_plugin.rc" src/mcp.cpp src/mcp_http.cpp)
fatfish_target(FatFishBox)
target_compile_definitions(FatFishBox PRIVATE FATFISHBOX_VERSION="${PROJECT_VERSION}")
target_include_directories(FatFishBox SYSTEM PRIVATE "${httplib_SOURCE_DIR}")
target_link_options(FatFishBox PRIVATE /MANIFEST:NO)
target_link_libraries(FatFishBox PRIVATE fatfish_core NativeUI::UI user32 ole32 shell32 shcore ws2_32 comctl32)
install(TARGETS FatFishBox RUNTIME DESTINATION .)
install(FILES README.md DESTINATION .)
install(DIRECTORY skills DESTINATION .)
install(DIRECTORY prompts DESTINATION .)
install(DIRECTORY docs DESTINATION .)

if(BUILD_TESTING)
    add_executable(fatfish_scheduler_test tests/scheduler-test.cpp)
    fatfish_target(fatfish_scheduler_test)
    target_link_libraries(fatfish_scheduler_test PRIVATE fatfish_core)
    add_test(NAME team_scheduler COMMAND $<TARGET_FILE:fatfish_scheduler_test>)
    find_program(NODE_EXECUTABLE node)
    if(NODE_EXECUTABLE)
        add_executable(fatfish_diagnostics_test tests/diagnostics-test.cpp)
        fatfish_target(fatfish_diagnostics_test)
        target_link_libraries(fatfish_diagnostics_test PRIVATE fatfish_core)
        add_test(NAME diagnostic_capture COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostics.mjs" $<TARGET_FILE:fatfish_diagnostics_test>)
        add_executable(fatfish_monitor_test tests/monitor-test.cpp "${CMAKE_CURRENT_BINARY_DIR}/dsh_desktop_plugin.rc")
        fatfish_target(fatfish_monitor_test)
        target_link_libraries(fatfish_monitor_test PRIVATE fatfish_core)
        add_executable(fatfish_events_test tests/events-test.cpp)
        fatfish_target(fatfish_events_test)
        target_link_libraries(fatfish_events_test PRIVATE fatfish_core)
        add_test(NAME event_retention COMMAND $<TARGET_FILE:fatfish_events_test>)
        add_test(NAME session_monitor
            COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/session-monitor.mjs" $<TARGET_FILE:fatfish_monitor_test>)
        add_test(NAME message_supplements
            COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/messages.mjs" $<TARGET_FILE:FatFishBox>)
        add_test(NAME evidence_manifests
            COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/evidence.mjs" $<TARGET_FILE:FatFishBox>)
        add_test(NAME work_ledger
            COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/work-items.mjs" $<TARGET_FILE:FatFishBox>)
        add_test(NAME team_execution
            COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/teams.mjs" $<TARGET_FILE:FatFishBox>)
        set_tests_properties(team_execution PROPERTIES TIMEOUT 180)
        add_test(NAME offscreen_render
            COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/offscreen-render.mjs" $<TARGET_FILE:FatFishBox>)
    endif()
endif()
