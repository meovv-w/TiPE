cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()
project(tipe VERSION 0.1.0 LANGUAGES CXX)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()
# The behavioral test suite uses assertions as executable checks. Keep them
# active in optimized builds as well as in the dedicated recovery build.
add_compile_options(-UNDEBUG)
option(TIPE_TEST_CRASH_INJECTION "Enable SIGKILL persistence test hooks" OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FCITX5 REQUIRED IMPORTED_TARGET Fcitx5Core Fcitx5Utils)
pkg_check_modules(FCITX5MODULE QUIET IMPORTED_TARGET Fcitx5Module)
find_package(LibIMEPinyin QUIET)
if(LibIMEPinyin_FOUND)
  add_compile_definitions(TIPE_HAS_LIBIME=1)
endif()
find_package(Qt6 COMPONENTS Widgets Network QUIET)
pkg_check_modules(GTK4 QUIET IMPORTED_TARGET gtk4)
pkg_check_modules(PANGOCAIRO QUIET IMPORTED_TARGET pangocairo)
pkg_check_modules(CAIROXCB QUIET IMPORTED_TARGET cairo-xcb xcb)
pkg_check_modules(WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
find_package(Fcitx5ModuleTestFrontend QUIET)
add_library(tipe_core src/tipe_core.cpp src/learning.cpp src/model.cpp src/cloud_transport.cpp src/cloud_worker.cpp src/cloud_candidates.cpp src/candidate_grid.cpp src/correction.cpp src/local_language_model.cpp)
if(PANGOCAIRO_FOUND)
  target_link_libraries(tipe_core PRIVATE PkgConfig::PANGOCAIRO)
  target_compile_definitions(tipe_core PRIVATE TIPE_HAS_PANGO_METRICS=1)
endif()
if(TIPE_TEST_CRASH_INJECTION)
  target_compile_definitions(tipe_core PRIVATE TIPE_TEST_CRASH_INJECTION=1)
endif()
if(LibIMEPinyin_FOUND)
  target_link_libraries(tipe_core PUBLIC LibIME::Pinyin LibIME::Core)
  # LibIME's package exports the canonical data directory through its Core
  # config on current distros. Keep a portable default for older packages;
  # runtime discovery still checks XDG and common architecture-specific model
  # locations and can be overridden with TIPE_LIBIME_DATA_DIR.
  if(DEFINED LIBIME_INSTALL_PKGDATADIR AND NOT LIBIME_INSTALL_PKGDATADIR STREQUAL "")
    set(TIPE_LIBIME_DATA_DIR "${LIBIME_INSTALL_PKGDATADIR}")
  else()
    set(TIPE_LIBIME_DATA_DIR "/usr/share/libime")
  endif()
  target_compile_definitions(tipe_core PRIVATE
    "TIPE_LIBIME_DATA_DIR=\"${TIPE_LIBIME_DATA_DIR}\"")
endif()
set_target_properties(tipe_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(tipe_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tipe_core PUBLIC Threads::Threads)
target_link_libraries(tipe_core PRIVATE OpenSSL::SSL OpenSSL::Crypto)
add_executable(tipe-demo src/tipe_demo.cpp)
target_link_libraries(tipe-demo PRIVATE tipe_core)
add_executable(tipe-diagnose src/tipe_diagnose.cpp)
target_link_libraries(tipe-diagnose PRIVATE tipe_core)
add_executable(tipe-trainer src/tipe_trainer.cpp)
target_link_libraries(tipe-trainer PRIVATE tipe_core)
add_test(NAME tipe-trainer-tests
         COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_trainer.sh"
                 "$<TARGET_FILE:tipe-trainer>")
set_tests_properties(tipe-trainer-tests PROPERTIES TIMEOUT 45)
if(LibIMEPinyin_FOUND)
  add_executable(tipe-libime-probe src/libime_probe.cpp)
  target_link_libraries(tipe-libime-probe PRIVATE LibIME::Pinyin LibIME::Core)
  add_executable(tipe-quality-tests tests/test_quality.cpp)
  target_link_libraries(tipe-quality-tests PRIVATE tipe_core LibIME::Pinyin LibIME::Core)
  target_include_directories(tipe-quality-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(tipe-quality-tests PRIVATE
                              TIPE_QUALITY_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  add_test(NAME tipe-quality-tests COMMAND tipe-quality-tests)
  add_executable(tipe-quality-isolation-tests tests/test_quality_isolation.cpp)
  target_link_libraries(tipe-quality-isolation-tests PRIVATE tipe_core LibIME::Pinyin LibIME::Core)
  target_compile_definitions(tipe-quality-isolation-tests PRIVATE
                              TIPE_QUALITY_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  add_test(NAME tipe-quality-isolation-tests COMMAND tipe-quality-isolation-tests)
  add_executable(tipe-correction-flow-tests tests/test_correction_flow.cpp)
  target_link_libraries(tipe-correction-flow-tests PRIVATE tipe_core)
  add_test(NAME tipe-correction-flow-tests COMMAND tipe-correction-flow-tests)
  add_executable(tipe-decoder-resource-tests tests/test_decoder_resources.cpp)
  target_link_libraries(tipe-decoder-resource-tests PRIVATE tipe_core)
  add_test(NAME tipe-decoder-resource-tests COMMAND tipe-decoder-resource-tests)
endif()
add_library(tipe MODULE src/tipe_fcitx.cpp)
target_link_libraries(tipe PRIVATE tipe_core PkgConfig::FCITX5)
set_target_properties(tipe PROPERTIES PREFIX "lib" OUTPUT_NAME "tipe")
if(Qt6_FOUND)
  add_library(tipe_control_ui src/support_page.cpp src/control_window.cpp)
  qt6_add_resources(tipe_control_ui "support_payments"
    PREFIX "/tipe/support" BASE "packaging/support"
    FILES packaging/support/wechat.jpg packaging/support/alipay.jpg)
  qt6_add_resources(tipe_control_ui "control_icons"
    PREFIX "/tipe/ui" BASE "packaging/ui"
    FILES packaging/ui/chevron-down.png)
  target_include_directories(tipe_control_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(tipe_control_ui PUBLIC tipe_core Qt6::Widgets)
  add_executable(tipe-control src/tipe_control.cpp)
  # Static control UI calls Learner symbols; list the UI archive before its
  # core dependency so traditional linkers resolve the transitive archive.
  target_link_libraries(tipe-control PRIVATE tipe_control_ui tipe_core Qt6::Network)
  add_test(NAME tipe-control-single-instance
    COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_control_single_instance.py"
                    "$<TARGET_FILE:tipe-control>")
  set_tests_properties(tipe-control-single-instance PROPERTIES TIMEOUT 15)
endif()

# Optional real-toolkit harnesses.  These are intentionally not CTest tests:
# they open a real window and require the user to run an Fcitx5 session with
# TiPE selected.  The accompanying script records toolkit-side preedit,
# commit, surrounding-text and cursor-rectangle observations without changing
# the user's profile or restarting a desktop session.
if(GTK4_FOUND)
  add_executable(tipe-gtk-harness tests/real_gtk_harness.cpp)
  target_link_libraries(tipe-gtk-harness PRIVATE PkgConfig::GTK4)
endif()
if(Qt6_FOUND)
  add_executable(tipe-qt-harness tests/real_qt_harness.cpp)
  target_link_libraries(tipe-qt-harness PRIVATE Qt6::Widgets)
endif()
enable_testing()
add_test(NAME tipe-daemon-mapping COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_daemon_mapping.py")
add_test(NAME tipe-user-installation COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_user_installation.py")
set_tests_properties(tipe-user-installation PROPERTIES TIMEOUT 30)
add_test(NAME tipe-activation-readiness COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_activation_readiness.py")
set_tests_properties(tipe-activation-readiness PROPERTIES TIMEOUT 35)
if(Qt6_FOUND)
  # Isolated real-widget fallback regression. The script creates its own
  # private Fcitx/X11 session and exits cleanly with a diagnostic when this
  # host has no display backend, so it never touches the user's daemon.
  add_test(NAME tipe-qt-surrounding-fallback
           COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run-isolated-qt-surrounding-fallback.sh"
                   "${CMAKE_CURRENT_BINARY_DIR}"
                   "${CMAKE_CURRENT_BINARY_DIR}/qt-surrounding-fallback")
  set_tests_properties(tipe-qt-surrounding-fallback PROPERTIES TIMEOUT 90)
  add_executable(tipe-support-tests tests/test_support_page.cpp)
  target_link_libraries(tipe-support-tests PRIVATE tipe_control_ui)
  add_test(NAME tipe-support-tests COMMAND tipe-support-tests)
  add_executable(tipe-control-window-tests tests/test_control_window.cpp)
  target_link_libraries(tipe-control-window-tests PRIVATE tipe_control_ui tipe_core)
  add_test(NAME tipe-control-window-tests COMMAND tipe-control-window-tests)
  set_tests_properties(tipe-control-window-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  set_tests_properties(tipe-support-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  add_test(NAME tipe-support-hidpi-tests COMMAND tipe-support-tests)
  set_tests_properties(tipe-support-hidpi-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=2")
endif()
add_executable(tipe-grid-tests tests/test_candidate_grid.cpp)
target_link_libraries(tipe-grid-tests PRIVATE tipe_core)
add_test(NAME tipe-grid-tests COMMAND tipe-grid-tests)
if(PANGOCAIRO_FOUND)
  add_library(tipe_painter src/candidate_painter.cpp)
  set_target_properties(tipe_painter PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(tipe_painter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(tipe_painter PUBLIC tipe_core PkgConfig::PANGOCAIRO)
  add_executable(tipe-painter-tests tests/test_candidate_painter.cpp)
  target_link_libraries(tipe-painter-tests PRIVATE tipe_painter)
  add_test(NAME tipe-painter-tests COMMAND tipe-painter-tests)
  # Protocol-neutral adapter for GTK/Qt client-side popups.  It is kept
  # separate from the Fcitx UI addon: native toolkit frontends own their
  # parent surface and can embed this frame without a second input-method
  # connection or compositor-coordinate guesses.
  add_library(tipe_client_side_adapter STATIC
              src/client_side_candidate_adapter.cpp)
  target_include_directories(tipe_client_side_adapter PUBLIC
                             ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(tipe_client_side_adapter PUBLIC tipe_painter)
  add_executable(tipe-client-side-adapter-tests
                 tests/test_client_side_adapter.cpp)
  target_link_libraries(tipe-client-side-adapter-tests PRIVATE
                        tipe_client_side_adapter)
  add_test(NAME tipe-client-side-adapter-tests
           COMMAND tipe-client-side-adapter-tests)

  # Optional public-API Fcitx UI addon. It intentionally does not replace
  # Classic UI by default: snapshot/event output and the explicit TIPE_UI_X11
  # mode are opt-in. When enabled, it consumes only public InputPanel state;
  # diagnostics use HiDPI Cairo snapshots and X11 mode owns an override-
  # redirect popup when a connection is available.
  add_library(tipe_ui MODULE src/tipe_ui.cpp)
  target_include_directories(tipe_ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(tipe_ui PRIVATE tipe_painter PkgConfig::FCITX5)
  # Fcitx5Module installs the public Wayland connection callback header.  It
  # does not expose a popup-surface factory; the optional link only lets the
  # addon report whether the daemon has a Wayland connection available.
  if(FCITX5MODULE_FOUND)
    target_link_libraries(tipe_ui PRIVATE PkgConfig::FCITX5MODULE)
    # The public Wayland header supplies only types and callback contracts;
    # the registry capability probe also calls the client library directly.
    # Keep this optional so headless/X11-only builds remain unchanged.
    if(WAYLAND_CLIENT_FOUND)
      target_link_libraries(tipe_ui PRIVATE PkgConfig::WAYLAND_CLIENT)
      target_compile_definitions(tipe_ui PRIVATE TIPE_HAS_FCITX_WAYLAND_PUBLIC=1)
    endif()
  endif()
  if(CAIROXCB_FOUND)
    target_link_libraries(tipe_ui PRIVATE PkgConfig::CAIROXCB)
    target_compile_definitions(tipe_ui PRIVATE TIPE_HAS_XCB_POPUP=1)
  endif()
  set_target_properties(tipe_ui PROPERTIES PREFIX "lib" OUTPUT_NAME "tipe-ui")
  # Test frontends discover addon modules through the generated test/addon
  # directory. Keep a symlink there so the UI addon can be loaded in the same
  # isolated instance as the input method.
  add_custom_command(TARGET tipe_ui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/test/addon"
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            "$<TARGET_FILE:tipe_ui>"
            "${CMAKE_CURRENT_BINARY_DIR}/test/addon/libtipe-ui.so")
endif()
add_executable(tipe-correction-tests tests/test_correction.cpp)
target_link_libraries(tipe-correction-tests PRIVATE tipe_core)
add_test(NAME tipe-correction-tests COMMAND tipe-correction-tests)
add_executable(tipe-tests tests/test_core.cpp)
target_link_libraries(tipe-tests PRIVATE tipe_core)
add_test(NAME tipe-tests COMMAND tipe-tests)
add_executable(tipe-english-corpus-tests tests/test_english_corpus.cpp)
target_link_libraries(tipe-english-corpus-tests PRIVATE tipe_core)
add_test(NAME tipe-english-corpus-tests COMMAND tipe-english-corpus-tests)
add_executable(tipe-libime-fallback-tests tests/test_libime_fallback.cpp)
target_link_libraries(tipe-libime-fallback-tests PRIVATE tipe_core)
add_test(NAME tipe-libime-fallback-tests COMMAND tipe-libime-fallback-tests)
add_executable(tipe-learning-tests tests/test_learning.cpp)
target_link_libraries(tipe-learning-tests PRIVATE tipe_core)
if(TIPE_TEST_CRASH_INJECTION)
  target_compile_definitions(tipe-learning-tests PRIVATE TIPE_TEST_CRASH_INJECTION=1)
endif()
add_test(NAME tipe-learning-tests COMMAND tipe-learning-tests)
add_executable(tipe-resource-stress-tests tests/test_resource_stress.cpp)
target_link_libraries(tipe-resource-stress-tests PRIVATE tipe_core)
add_test(NAME tipe-resource-stress-tests COMMAND tipe-resource-stress-tests)
set_tests_properties(tipe-resource-stress-tests PROPERTIES
  ENVIRONMENT "TIPE_RESOURCE_STRESS_ROUNDS=120;TIPE_RESOURCE_STRESS_EVENTS=128;TIPE_RESOURCE_STRESS_PAUSE_MS=10"
  TIMEOUT 30)
add_test(NAME tipe-trained-resource-stress-tests COMMAND tipe-resource-stress-tests)
set_tests_properties(tipe-trained-resource-stress-tests PROPERTIES
  ENVIRONMENT "TIPE_RESOURCE_STRESS_TRAINED=1;TIPE_RESOURCE_STRESS_ROUNDS=120;TIPE_RESOURCE_STRESS_EVENTS=128;TIPE_RESOURCE_STRESS_PAUSE_MS=10"
  TIMEOUT 30)
add_executable(tipe-model-tests tests/test_model.cpp)
target_link_libraries(tipe-model-tests PRIVATE tipe_core)
add_test(NAME tipe-model-tests COMMAND tipe-model-tests)
add_executable(tipe-cloud-transport-tests tests/test_cloud_transport.cpp)
target_link_libraries(tipe-cloud-transport-tests PRIVATE tipe_core)
add_test(NAME tipe-cloud-transport-tests COMMAND tipe-cloud-transport-tests)
add_executable(tipe-cloud-worker-tests tests/test_cloud_worker.cpp)
target_link_libraries(tipe-cloud-worker-tests PRIVATE tipe_core)
add_test(NAME tipe-cloud-worker-tests COMMAND tipe-cloud-worker-tests)
add_executable(tipe-cloud-candidate-tests tests/test_cloud_candidates.cpp)
target_link_libraries(tipe-cloud-candidate-tests PRIVATE tipe_core)
add_test(NAME tipe-cloud-candidate-tests COMMAND tipe-cloud-candidate-tests)
add_executable(tipe-local-small-tests tests/test_local_small.cpp)
target_link_libraries(tipe-local-small-tests PRIVATE tipe_core)
add_test(NAME tipe-local-small-tests COMMAND tipe-local-small-tests)
if(TARGET Fcitx5::Module::TestFrontend)
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test/addon")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/packaging/tipe-addon.conf"
                 "${CMAKE_CURRENT_BINARY_DIR}/test/addon/tipe.conf" COPYONLY)
  if(TARGET tipe_ui)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/packaging/tipe-ui.conf"
                   "${CMAKE_CURRENT_BINARY_DIR}/test/addon/tipe-ui.conf" COPYONLY)
  endif()
  add_executable(tipe-fcitx-tests tests/test_fcitx.cpp)
  # The test loads these modules at runtime. Building only the test target
  # must refresh the implementation as well, rather than testing yesterday's
  # shared library against today's assertions.
  add_dependencies(tipe-fcitx-tests tipe)
  if(TARGET tipe_ui)
    add_dependencies(tipe-fcitx-tests tipe_ui)
  endif()
  target_link_libraries(tipe-fcitx-tests PRIVATE PkgConfig::FCITX5
                        Fcitx5::Module::TestFrontend)
  if(TARGET tipe_painter)
    target_link_libraries(tipe-fcitx-tests PRIVATE tipe_painter)
    target_compile_definitions(tipe-fcitx-tests PRIVATE TIPE_TEST_PAINTER=1)
  endif()
  target_compile_definitions(tipe-fcitx-tests PRIVATE
                              TIPE_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
  add_test(NAME tipe-fcitx-tests COMMAND tipe-fcitx-tests)
  add_executable(tipe-fcitx-lifecycle-tests tests/test_fcitx_lifecycle.cpp)
  add_dependencies(tipe-fcitx-lifecycle-tests tipe)
  target_link_libraries(tipe-fcitx-lifecycle-tests PRIVATE PkgConfig::FCITX5
                        Fcitx5::Module::TestFrontend)
  target_compile_definitions(tipe-fcitx-lifecycle-tests PRIVATE
                             TIPE_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
  add_test(NAME tipe-fcitx-lifecycle-tests COMMAND tipe-fcitx-lifecycle-tests)
  # Exercise the optional X11/XWayland path when a display is available and
  # its no-display fallback in headless CI. The addon is opt-in and the test
  # frontend remains the authoritative lifecycle/input harness.
  add_test(NAME tipe-ui-x11-smoke COMMAND tipe-fcitx-tests)
  set_tests_properties(tipe-ui-x11-smoke PROPERTIES ENVIRONMENT "TIPE_UI_X11=1")
  add_test(NAME tipe-ui-headless-fallback COMMAND tipe-fcitx-tests)
  set_tests_properties(tipe-ui-headless-fallback PROPERTIES
                       ENVIRONMENT "TIPE_UI_X11=1;DISPLAY=;WAYLAND_DISPLAY=")
  add_test(NAME tipe-ui-page-offset-smoke COMMAND tipe-fcitx-tests)
  set_tests_properties(tipe-ui-page-offset-smoke PROPERTIES
                       ENVIRONMENT "TIPE_UI_EVENT_LOG=${CMAKE_CURRENT_BINARY_DIR}/tipe-ui-page-offset.jsonl")
  add_test(NAME tipe-ui-wayland-capability-diagnostic COMMAND tipe-fcitx-tests)
  set_tests_properties(tipe-ui-wayland-capability-diagnostic PROPERTIES
                       ENVIRONMENT "TIPE_UI_EVENT_LOG=${CMAKE_CURRENT_BINARY_DIR}/tipe-ui-wayland.jsonl;WAYLAND_DISPLAY=tipe-diagnostic")
  add_executable(tipe-protocol-tests tests/test_protocols.cpp)
  target_link_libraries(tipe-protocol-tests PRIVATE PkgConfig::FCITX5
                        Fcitx5::Module::TestFrontend)
  target_compile_definitions(tipe-protocol-tests PRIVATE
                              TIPE_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
  add_test(NAME tipe-protocol-tests COMMAND tipe-protocol-tests)
  # Read-only compositor capability probe. It is valid both on a native
  # Wayland desktop (status=ok) and in headless CI (status=skipped); either
  # way it verifies the diagnostic schema without binding private popup APIs.
  add_test(NAME tipe-wayland-capability-probe
           COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/probe-wayland-capabilities.sh"
                   "${CMAKE_CURRENT_BINARY_DIR}/wayland-capability-probe")
  set_tests_properties(tipe-wayland-capability-probe PROPERTIES TIMEOUT 15)
endif()
install(TARGETS tipe-demo tipe-diagnose tipe-trainer RUNTIME DESTINATION bin)
install(TARGETS tipe LIBRARY DESTINATION lib/fcitx5)
if(TARGET tipe_ui)
  install(TARGETS tipe_ui LIBRARY DESTINATION lib/fcitx5)
endif()
if(TARGET tipe-control)
  install(TARGETS tipe-control RUNTIME DESTINATION bin)
endif()
install(FILES packaging/tipe.desktop DESTINATION share/applications)
install(FILES packaging/tipe.conf DESTINATION share/fcitx5/inputmethod)
install(FILES packaging/tipe-addon.conf DESTINATION share/fcitx5/addon RENAME tipe.conf)
if(TARGET tipe_ui)
  install(FILES packaging/tipe-ui.conf DESTINATION share/fcitx5/addon)
endif()
install(FILES packaging/tipe.svg DESTINATION share/icons/hicolor/scalable/apps)
install(DIRECTORY packaging/themes/MacOS-Dark
        DESTINATION share/fcitx5/themes)
