cmake_minimum_required(VERSION 3.20)
project(hla_ftp_patch VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
  add_compile_options(/W4 /permissive- /EHsc)
endif()
add_library(ftp_core STATIC src/pe_image.cpp src/patcher.cpp)
target_include_directories(ftp_core PUBLIC src)
add_executable(ftp_tests tests/ftp_tests.cpp)
target_link_libraries(ftp_tests PRIVATE ftp_core)
enable_testing()
add_test(NAME ftp_core_tests COMMAND ftp_tests)

if(WIN32)
  if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The OpenVR proxy requires MSVC x64")
  endif()
  enable_language(RC)
  # Generate the export object separately. MSVC's combined import-library phase
  # can try to resolve forwarded exports as local symbols when objects are present.
  # The documented LIB -> EXP -> LINK flow keeps forwarders explicit.
  set(forwarder_dir "${CMAKE_CURRENT_BINARY_DIR}/forwarders")
  set(forwarder_exp "${forwarder_dir}/openvr_api.exp")
  add_custom_command(OUTPUT "${forwarder_exp}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${forwarder_dir}"
    COMMAND ${CMAKE_AR} /NOLOGO /MACHINE:X64 "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/exports.def"
      "/OUT:${forwarder_dir}/openvr_api.lib"
    DEPENDS src/exports.def VERBATIM)
  set_source_files_properties("${forwarder_exp}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  add_library(openvr_api SHARED src/dllmain.cpp src/log.cpp src/version.rc "${forwarder_exp}")
  target_link_libraries(openvr_api PRIVATE ftp_core)
  target_compile_definitions(openvr_api PRIVATE NOMINMAX)
  add_executable(ftp_check tools/ftp_check.cpp)
  target_link_libraries(ftp_check PRIVATE ftp_core)
  add_test(NAME checker_tests COMMAND powershell -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_SOURCE_DIR}/tests/checker_tests.ps1" -Checker $<TARGET_FILE:ftp_check>)
  # CTest launched from PowerShell 7 otherwise passes its incompatible module
  # directories through to Windows PowerShell 5.1.
  set_tests_properties(checker_tests PROPERTIES
    ENVIRONMENT "PSModulePath=$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/Modules")
  install(TARGETS openvr_api ftp_check RUNTIME DESTINATION .)
  install(FILES scripts/install.ps1 scripts/uninstall.ps1 scripts/common.ps1
    scripts/install.bat scripts/uninstall.bat README.md LICENSE CHANGELOG.md CONTRIBUTING.md DESTINATION .)
  install(FILES docs/HOW_IT_WORKS.md docs/RESEARCH_VERIFICATION.md docs/TESTING.md DESTINATION docs)
  install(FILES docs/superpowers/specs/2026-10-28-hla-ftp-patch-design.md DESTINATION docs/superpowers/specs)
  install(FILES docs/media/ftp.gif DESTINATION docs/media)
  add_library(fixture_vrclient SHARED tests/fixture_vrclient.cpp)
  set_target_properties(fixture_vrclient PROPERTIES OUTPUT_NAME vrclient_x64)
  add_library(fixture_original SHARED tests/fixture_original.cpp tests/fixture_original.def)
  set_target_properties(fixture_original PROPERTIES OUTPUT_NAME openvr_api_ftp_original)
  add_test(NAME installer_tests COMMAND powershell -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_SOURCE_DIR}/tests/installer_tests.ps1" -Proxy $<TARGET_FILE:openvr_api>
    -Original $<TARGET_FILE:fixture_original> -Checker $<TARGET_FILE:ftp_check>)
  set_tests_properties(installer_tests PROPERTIES RUN_SERIAL TRUE
    ENVIRONMENT "PSModulePath=$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/Modules")
  add_test(NAME installer_path_tests COMMAND powershell -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_SOURCE_DIR}/tests/installer_path_tests.ps1" -Repo "${CMAKE_SOURCE_DIR}"
    -Proxy $<TARGET_FILE:openvr_api> -Original $<TARGET_FILE:fixture_original> -Checker $<TARGET_FILE:ftp_check>)
  set_tests_properties(installer_path_tests PROPERTIES RUN_SERIAL TRUE
    ENVIRONMENT "PSModulePath=$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/Modules")
  add_test(NAME launcher_tests COMMAND powershell -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_SOURCE_DIR}/tests/launcher_tests.ps1")
  set_tests_properties(launcher_tests PROPERTIES
    ENVIRONMENT "PSModulePath=$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/Modules")
  foreach(host IN ITEMS alyx other fault)
    add_executable(proxy_host_${host} tests/proxy_host.cpp)
    if(host STREQUAL "other")
      set(host_name other_game)
    else()
      set(host_name hlvr)
    endif()
    set_target_properties(proxy_host_${host} PROPERTIES OUTPUT_NAME ${host_name}
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fixtures/${host}")
    add_dependencies(proxy_host_${host} openvr_api fixture_original fixture_vrclient)
    add_custom_command(TARGET proxy_host_${host} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:openvr_api> $<TARGET_FILE_DIR:proxy_host_${host}>
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fixture_original> $<TARGET_FILE_DIR:proxy_host_${host}>
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fixture_vrclient> $<TARGET_FILE_DIR:proxy_host_${host}>)
    add_test(NAME proxy_${host} COMMAND proxy_host_${host} ${host})
  endforeach()
endif()
