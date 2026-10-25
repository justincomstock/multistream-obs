cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

find_package(obs-frontend-api REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)

find_package(Qt6 REQUIRED COMPONENTS Widgets Core Network)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Network)
if(OS_WINDOWS)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE winhttp advapi32)
elseif(OS_MACOS)
  # http.cpp: the system's libcurl (macOS SDK).
  find_package(CURL REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE CURL::libcurl)
  # games.cpp: the windows on screen and the app each belongs to.
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "$<LINK_LIBRARY:FRAMEWORK,CoreGraphics.framework>"
                                                      "$<LINK_LIBRARY:FRAMEWORK,CoreFoundation.framework>")
endif()
target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
)
# No moc: the plugin's own classes don't declare signals or slots, and the
# vendored obs-browser header (deps/browser-panel.hpp) must not be moc'ed
# (its QCefWidget lives in obs-browser).
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/deps")

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/dock.cpp
    src/bridge.cpp
    src/games.cpp
    src/http.cpp
    src/obs-state.cpp
    src/relay-output.cpp
    src/vertical/audio-mix.cpp
    src/vertical/canvas.cpp
    src/vertical/docks.cpp
    src/vertical/items.cpp
    src/vertical/migrate.cpp
    src/vertical/record.cpp
    src/vertical/preview.cpp
    src/vertical/theme.cpp
    src/vertical/websocket.cpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
