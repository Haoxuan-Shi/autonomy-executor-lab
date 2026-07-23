cmake_minimum_required(VERSION 3.20)
project(autonomy_executor_lab VERSION 0.1.0 LANGUAGES CXX)

option(AEL_BUILD_TESTS "Build autonomy-executor-lab tests" ON)

add_library(ael src/executor.cpp)
add_library(ael::ael ALIAS ael)
target_include_directories(ael
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(ael PUBLIC cxx_std_17)
if(MSVC)
  target_compile_options(ael PRIVATE /W4 /permissive-)
else()
  target_compile_options(ael PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

add_executable(ael-sim app/main.cpp)
target_link_libraries(ael-sim PRIVATE ael::ael)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS ael ael-sim EXPORT aelTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT aelTargets NAMESPACE ael:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ael)
configure_package_config_file(
  cmake/aelConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/aelConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ael)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/aelConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/aelConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/aelConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ael)

if(AEL_BUILD_TESTS)
  enable_testing()
  add_executable(ael-tests tests/test_executor.cpp)
  target_link_libraries(ael-tests PRIVATE ael::ael)
  add_test(NAME ael-tests COMMAND ael-tests)
  add_executable(ael-interface-smoke tests/install_consumer/main.cpp)
  target_link_libraries(ael-interface-smoke PRIVATE ael::ael)
  add_test(NAME ael-interface-smoke COMMAND ael-interface-smoke)
endif()
