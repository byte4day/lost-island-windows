cmake_minimum_required(VERSION 3.20)
project(LostIsland VERSION 1.0.2 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

file(GLOB SOURCES CONFIGURE_DEPENDS src/*.cpp src/services/*.cpp)
add_executable(LostIsland WIN32 ${SOURCES} res/app.rc)
target_include_directories(LostIsland PRIVATE src src/services)
target_compile_definitions(LostIsland PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  _WIN32_WINNT=0x0A00 LI_VERSION="${PROJECT_VERSION}")
target_link_libraries(LostIsland PRIVATE d2d1 dwrite windowscodecs dwmapi uxtheme shcore
  ole32 oleaut32 uuid runtimeobject winhttp wlanapi powrprof shlwapi comctl32 wbemuuid
  imm32 version dxva2 gdi32 user32 shell32 advapi32)

if(MSVC)
  target_compile_options(LostIsland PRIVATE /W4 /utf-8 /permissive-)
else()
  target_compile_options(LostIsland PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
  target_link_options(LostIsland PRIVATE -municode -static -s)
endif()
