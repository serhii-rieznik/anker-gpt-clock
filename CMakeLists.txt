cmake_minimum_required(VERSION 3.25)
project(AnkerClock VERSION 1.0.0 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "Anker Clock requires Windows")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(clock_core STATIC src/settings.cpp src/renderer.cpp src/hub.cpp src/ui_contract.cpp src/lock_session.cpp src/codex_usage.cpp)
target_compile_features(clock_core PUBLIC cxx_std_20)
target_compile_definitions(clock_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_include_directories(clock_core PUBLIC src)
target_link_libraries(clock_core PUBLIC gdiplus ole32 oleaut32 windowscodecs shell32 advapi32 setupapi user32 gdi32 uuid runtimeobject)

add_executable(AnkerClock WIN32 src/main.cpp resources/app.rc resources/app.manifest)
target_link_libraries(AnkerClock PRIVATE clock_core wtsapi32)
set_target_properties(AnkerClock PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

add_library(clock_ui_native SHARED src/ui_bridge.cpp)
target_link_libraries(clock_ui_native PRIVATE clock_core)
set_target_properties(clock_ui_native PROPERTIES OUTPUT_NAME "AnkerClock.Native" INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
find_program(DOTNET_EXECUTABLE dotnet REQUIRED)
add_custom_target(clock_settings ALL
    COMMAND ${CMAKE_COMMAND} -E env
        "DOTNET_CLI_HOME=${CMAKE_BINARY_DIR}/dotnet-cli"
        "NUGET_PACKAGES=${CMAKE_BINARY_DIR}/packages"
        "DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1"
        "LIB="
        ${DOTNET_EXECUTABLE} build "${CMAKE_SOURCE_DIR}/ui/AnkerClock.Settings.csproj"
        --configuration $<CONFIG> --output "$<TARGET_FILE_DIR:AnkerClock>"
        -p:UseSharedCompilation=false
    DEPENDS clock_ui_native
    VERBATIM)

install(TARGETS AnkerClock clock_ui_native RUNTIME DESTINATION .)
install(FILES "$<TARGET_FILE_DIR:AnkerClock>/AnkerClock.Settings.exe"
              "$<TARGET_FILE_DIR:AnkerClock>/AnkerClock.Settings.exe.config" DESTINATION .)
install(FILES LICENSE README.md DESTINATION .)
install(DIRECTORY docs/images DESTINATION docs)

if(MSVC)
    target_compile_options(clock_core PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(AnkerClock PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(clock_ui_native PRIVATE /W4 /permissive- /utf-8)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(AnkerClockTests tests/core_tests.cpp)
    target_link_libraries(AnkerClockTests PRIVATE clock_core)
    target_compile_options(AnkerClockTests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME clock_core COMMAND AnkerClockTests)
    find_program(POWERSHELL_EXECUTABLE powershell REQUIRED)
    add_test(NAME settings_interop COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/interop-smoke.ps1"
        -SettingsExe "$<TARGET_FILE_DIR:AnkerClock>/AnkerClock.Settings.exe")
endif()
