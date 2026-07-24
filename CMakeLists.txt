cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()
project(tipe VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(TIPE_ENABLE_SANITIZERS "Build TiPE with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(TIPE_ENABLE_SANITIZERS AND NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "TIPE_ENABLE_SANITIZERS requires a GCC- or Clang-compatible compiler")
endif()
if(TIPE_ENABLE_SANITIZERS)
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined -fno-omit-frame-pointer")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
    check_cxx_source_compiles("int main() { return 0; }" TIPE_SANITIZER_RUNTIME_AVAILABLE)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(NOT TIPE_SANITIZER_RUNTIME_AVAILABLE)
        message(FATAL_ERROR
            "TiPE sanitizer runtime probe failed. Install compiler-matched ASan/UBSan runtime libraries "
            "before using TIPE_ENABLE_SANITIZERS or scripts/build.sh --sanitize.")
    endif()
endif()

find_package(Fcitx5Core REQUIRED)
find_package(Fcitx5Utils REQUIRED)
find_package(Fcitx5ModuleWayland REQUIRED)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
include(GNUInstallDirs)
pkg_check_modules(GTK4 REQUIRED gtk4)
pkg_check_modules(GTK4_LAYER_SHELL REQUIRED gtk4-layer-shell-0)
pkg_check_modules(LIBPINYIN libpinyin)
find_package(LibIMEPinyin QUIET)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(PANGOCAIRO REQUIRED pangocairo)
pkg_check_modules(XCB REQUIRED xcb)
find_program(WAYLAND_SCANNER_EXECUTABLE wayland-scanner REQUIRED)
find_program(TIPE_MINGW_CXX_EXECUTABLE x86_64-w64-mingw32-g++)

set(TIPE_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(TIPE_INPUT_METHOD_V2_XML
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fcitx-wayland-5.1.19/input-method-v2/input-method-unstable-v2.xml)
set(TIPE_INPUT_METHOD_V2_HEADER
    ${TIPE_GENERATED_DIR}/wayland-input-method-unstable-v2-client-protocol.h)
set(TIPE_INPUT_METHOD_V2_CODE
    ${TIPE_GENERATED_DIR}/wayland-input-method-unstable-v2-protocol.c)
add_custom_command(
    OUTPUT ${TIPE_INPUT_METHOD_V2_HEADER} ${TIPE_INPUT_METHOD_V2_CODE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${TIPE_GENERATED_DIR}
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header ${TIPE_INPUT_METHOD_V2_XML} ${TIPE_INPUT_METHOD_V2_HEADER}
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code ${TIPE_INPUT_METHOD_V2_XML} ${TIPE_INPUT_METHOD_V2_CODE}
    DEPENDS ${TIPE_INPUT_METHOD_V2_XML}
    VERBATIM
)

add_library(tipe_state STATIC
    src/candidate_snapshot.cpp
    src/dictionary.cpp
    src/input_model.cpp
    src/pass_through_supervisor.cpp
    src/state.cpp
    src/supervision_snapshot.cpp
)
target_include_directories(tipe_state PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(tipe_state PRIVATE -Wall -Wextra -Wpedantic)
if(LIBPINYIN_FOUND)
    target_compile_definitions(tipe_state PRIVATE TIPE_HAVE_LIBPINYIN=1)
    target_include_directories(tipe_state PRIVATE ${LIBPINYIN_INCLUDE_DIRS})
    target_link_libraries(tipe_state PRIVATE ${LIBPINYIN_LIBRARIES})
    target_compile_options(tipe_state PRIVATE ${LIBPINYIN_CFLAGS_OTHER})
endif()
if(TARGET LibIME::Pinyin)
    target_compile_definitions(tipe_state PRIVATE TIPE_HAVE_LIBIME=1)
    target_link_libraries(tipe_state PRIVATE LibIME::Pinyin)
endif()
set_target_properties(tipe_state PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(tipe MODULE
    src/engine.cpp
    src/tipe_ui_public.h
)
target_link_libraries(tipe PRIVATE tipe_state Fcitx5::Core Fcitx5::Utils Threads::Threads)
target_compile_options(tipe PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(tipe PROPERTIES PREFIX "lib")

add_library(tipeui MODULE
    src/tipe_ui.cpp
    src/tipe_ui_public.h
    src/tipe_wayland_surface.cpp
    third_party/fcitx-wayland-5.1.19/input-method-v2/zwp_input_method_v2.cpp
    third_party/fcitx-wayland-5.1.19/input-method-v2/zwp_input_popup_surface_v2.cpp
    ${TIPE_INPUT_METHOD_V2_CODE}
)
target_link_libraries(tipeui PRIVATE tipe_state Fcitx5::Core Fcitx5::Utils)
target_link_libraries(tipeui PRIVATE Fcitx5::Module::Wayland)
target_include_directories(tipeui PRIVATE
    ${TIPE_GENERATED_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fcitx-wayland-5.1.19/core
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fcitx-wayland-5.1.19/input-method-v2
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${PANGOCAIRO_INCLUDE_DIRS}
)
target_link_libraries(tipeui PRIVATE ${WAYLAND_CLIENT_LIBRARIES} ${PANGOCAIRO_LIBRARIES})
target_compile_options(tipeui PRIVATE ${WAYLAND_CLIENT_CFLAGS_OTHER} ${PANGOCAIRO_CFLAGS_OTHER})
target_compile_options(tipeui PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(tipeui PROPERTIES PREFIX "lib")

add_executable(tipe-state-test tests/state_test.cpp)
target_link_libraries(tipe-state-test PRIVATE tipe_state)
target_compile_options(tipe-state-test PRIVATE -Wall -Wextra -Wpedantic)
if(TARGET LibIME::Pinyin)
    target_compile_definitions(tipe-state-test PRIVATE TIPE_TEST_HAVE_LIBIME=1)
endif()
target_compile_definitions(tipe-state-test PRIVATE
    TIPE_TEST_FALLBACK_DICTIONARY_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/state-dictionary.tsv")
enable_testing()
add_test(NAME state COMMAND tipe-state-test)
add_test(NAME training-export
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/training_export_test.py
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/training-export.py)
add_test(NAME personal-model
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/personal_model_test.py
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/personal-model.py
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/personal-model-train.sh)
add_test(NAME model-backend-http
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/model_backend_http_test.py
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/model-adapter.sh)
set_tests_properties(model-backend-http PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME toggle-helper
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/toggle_test.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/toggle.sh)
add_test(NAME restart-helper
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/restart_test.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/restart-fcitx5.sh)
add_test(NAME pass-through-pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/pass_through_pipeline_test.py
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/model-explain.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/model-adapter.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/training-export.py)

add_executable(tipe-nonblocking-pipe-test tests/nonblocking_pipe_test.cpp)
target_include_directories(tipe-nonblocking-pipe-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(tipe-nonblocking-pipe-test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME nonblocking-pipe COMMAND tipe-nonblocking-pipe-test)

add_executable(tipe-state-probe tools/state_probe.cpp)
target_link_libraries(tipe-state-probe PRIVATE tipe_state)
target_compile_options(tipe-state-probe PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS tipe-state-probe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(tipe-candidate-window
    src/candidate_window.cpp
    src/wine_caret_bridge.cpp
)
target_include_directories(tipe-candidate-window PRIVATE ${GTK4_INCLUDE_DIRS} ${GTK4_LAYER_SHELL_INCLUDE_DIRS}
                                                          ${XCB_INCLUDE_DIRS})
target_link_libraries(tipe-candidate-window PRIVATE ${GTK4_LIBRARIES} ${GTK4_LAYER_SHELL_LIBRARIES}
                                                     ${XCB_LIBRARIES})
target_compile_options(tipe-candidate-window PRIVATE -Wall -Wextra -Wpedantic ${GTK4_CFLAGS_OTHER}
                                                      ${GTK4_LAYER_SHELL_CFLAGS_OTHER} ${XCB_CFLAGS_OTHER})
if(TIPE_MINGW_CXX_EXECUTABLE)
    set(TIPE_WINE_CARET_BRIDGE ${CMAKE_CURRENT_BINARY_DIR}/tipe-wine-caret-bridge.exe)
    add_custom_command(
        OUTPUT ${TIPE_WINE_CARET_BRIDGE}
        COMMAND ${TIPE_MINGW_CXX_EXECUTABLE}
                -std=c++20 -Os -Wall -Wextra -Wpedantic -mwindows -static
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/wine_caret_bridge.cpp
                -o ${TIPE_WINE_CARET_BRIDGE}
                -lole32 -loleaut32 -loleacc -limm32
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/wine_caret_bridge.cpp
        VERBATIM
    )
    add_custom_target(tipe-wine-caret-bridge ALL DEPENDS ${TIPE_WINE_CARET_BRIDGE})
    add_dependencies(tipe-candidate-window tipe-wine-caret-bridge)
    target_compile_definitions(tipe-candidate-window PRIVATE
        TIPE_WINE_CARET_BRIDGE_PATH="${CMAKE_INSTALL_FULL_LIBEXECDIR}/tipe/tipe-wine-caret-bridge.exe")
    install(PROGRAMS ${TIPE_WINE_CARET_BRIDGE} DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/tipe)
else()
    message(STATUS "MinGW C++ compiler not found; Wine caret bridge will not be built")
endif()
add_test(NAME candidate-window COMMAND tipe-candidate-window --self-test)
install(TARGETS tipe-candidate-window RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(tipe-learning-panel-window src/learning_panel_window.cpp)
target_include_directories(tipe-learning-panel-window PRIVATE ${GTK4_INCLUDE_DIRS})
target_link_libraries(tipe-learning-panel-window PRIVATE ${GTK4_LIBRARIES})
target_compile_options(tipe-learning-panel-window PRIVATE -Wall -Wextra -Wpedantic ${GTK4_CFLAGS_OTHER})
target_compile_definitions(tipe-learning-panel-window PRIVATE
    TIPE_SUPPORT_DATA_DIR="${CMAKE_INSTALL_FULL_DATADIR}/tipe/support")
add_test(NAME learning-panel-window COMMAND tipe-learning-panel-window --self-test)
install(TARGETS tipe-learning-panel-window RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(TIPE_ENABLE_SANITIZERS)
    set(TIPE_SANITIZER_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
    foreach(target tipe_state tipe tipeui tipe-state-test tipe-state-probe tipe-nonblocking-pipe-test tipe-candidate-window
                   tipe-learning-panel-window)
        target_compile_options(${target} PRIVATE ${TIPE_SANITIZER_FLAGS})
    endforeach()
    foreach(target tipe tipeui tipe-state-test tipe-state-probe tipe-nonblocking-pipe-test tipe-candidate-window
                   tipe-learning-panel-window)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()

install(TARGETS tipe LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/fcitx5)
install(TARGETS tipeui LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/fcitx5)
install(FILES addon/tipe.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/fcitx5/addon)
install(FILES addon/tipeui.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/fcitx5/addon)
install(FILES addon/tipe-inputmethod.conf
    DESTINATION ${CMAKE_INSTALL_DATADIR}/fcitx5/inputmethod
    RENAME tipe.conf
)
install(FILES data/tipe-supervision.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/icons/hicolor/scalable/apps/tipe.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES data/icons/hicolor/scalable/apps/tipe.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
    RENAME dev.tipe.LearningPanel.svg)
install(FILES
    data/support/wechat.png
    data/support/alipay.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/tipe/support)
