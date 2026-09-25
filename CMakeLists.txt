# Фикс политики совместимости для CMake 4.x
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

cmake_minimum_required(VERSION 3.15)

# Отключаем SSL-проверку отзыва сертификатов
set(CMAKE_TLS_VERIFY OFF CACHE BOOL "" FORCE)

# Отключаем сборку тестов и некритичных опций
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CPR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)

project(mysplib CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# 1. CPR (C++ Requests)
FetchContent_Declare(
    cpr
    GIT_REPOSITORY https://github.com/libcpr/cpr.git
    GIT_TAG        1.10.4
)
FetchContent_MakeAvailable(cpr)

# 2. nlohmann_json
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(json)

# 3. Автоматическая загрузка miniaudio.h
FetchContent_Declare(
    miniaudio
    URL https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(miniaudio)

# Исполняемый файл приложения (WIN32 под WinMain)
add_executable(NativeSoundApp WIN32 main.cpp)

# Флаг для MSVC: принудительная кодировка UTF-8 для исходников и строк
if(MSVC)
    target_compile_options(NativeSoundApp PRIVATE /utf-8)
endif()

target_include_directories(NativeSoundApp PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${miniaudio_SOURCE_DIR}
)

target_link_libraries(NativeSoundApp PRIVATE
    cpr::cpr
    nlohmann_json::nlohmann_json
    Comctl32.lib
    Winmm.lib
)