cmake_minimum_required(VERSION 3.20)
project(BoxLink VERSION 0.9.0 LANGUAGES CXX RC)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(WARNING "Becky! 2 は 32bit のため、-A Win32 で構成してください")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# VC++ ランタイムを静的リンクし、配布先で再頒布パッケージを不要にする
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(json)

add_library(BoxLink SHARED
  src/plugin.cpp
  src/share_dialog.cpp
  src/settings_dialog.cpp
  src/box_client.cpp
  src/oauth_listener.cpp
  src/http.cpp
  src/config.cpp
  src/token_store.cpp
  src/util.cpp
  src/BoxLink.rc
  src/BoxLink.def
  sdk/BeckyAPI.cpp)

target_include_directories(BoxLink PRIVATE src sdk)
# SDK は ANSI (LPCTSTR = char) 前提のため UNICODE は定義しない。UI は W 版 API を明示的に使う
target_compile_definitions(BoxLink PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(BoxLink PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8 /W3 /EHsc>)
target_link_libraries(BoxLink PRIVATE
  nlohmann_json::nlohmann_json
  winhttp ws2_32 crypt32 bcrypt comctl32 shell32)
