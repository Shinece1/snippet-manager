cmake_minimum_required(VERSION 3.20)

project(snippet_manager LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Dependency options
#
# By default the sources in third_party/ are used, so the project builds
# without network access or a package manager.  If a dependency directory is
# missing, it is downloaded into the build tree instead.  Every location can be
# overridden explicitly:
#
#   cmake -S . -B build \
#         -DSNIPGUI_CIMGUI_DIR=C:/deps/cimgui \
#         -DSNIPGUI_IMGUI_DIR=C:/deps/imgui \
#         -DSNIPGUI_SQLITE_DIR=C:/deps/sqlite-amalgamation-3460100 \
#         -DSNIPGUI_SDL2_DIR=C:/deps/SDL2-2.30.9
# ---------------------------------------------------------------------------
set(SNIPGUI_DEPS_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH
    "Where downloaded dependencies are extracted")
set(SNIPGUI_CIMGUI_DIR "" CACHE PATH "Local cimgui source directory")
set(SNIPGUI_IMGUI_DIR "" CACHE PATH "Local Dear ImGui source directory")
set(SNIPGUI_SQLITE_DIR "" CACHE PATH
    "Local SQLite amalgamation directory (contains sqlite3.c)")
set(SNIPGUI_SDL2_DIR "" CACHE PATH "Local SDL2 source directory")
option(SNIPGUI_FETCH_SDL2 "Download and build SDL2 when no system SDL2 is found" ON)

# The repository carries a small, stripped copy of its C/C++ dependencies under
# third_party/ so a checkout builds without network access.  Pass any of the
# SNIPGUI_*_DIR variables to use a different copy.
set(SNIPGUI_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party" CACHE PATH
    "Directory containing the vendored dependencies")
if(NOT SNIPGUI_CIMGUI_DIR AND
   EXISTS "${SNIPGUI_THIRD_PARTY_DIR}/cimgui/cimgui.cpp")
    set(SNIPGUI_CIMGUI_DIR "${SNIPGUI_THIRD_PARTY_DIR}/cimgui")
endif()
if(NOT SNIPGUI_IMGUI_DIR AND
   EXISTS "${SNIPGUI_CIMGUI_DIR}/imgui/imgui.h")
    set(SNIPGUI_IMGUI_DIR "${SNIPGUI_CIMGUI_DIR}/imgui")
endif()
if(NOT SNIPGUI_SQLITE_DIR AND
   EXISTS "${SNIPGUI_THIRD_PARTY_DIR}/sqlite/sqlite3.c")
    set(SNIPGUI_SQLITE_DIR "${SNIPGUI_THIRD_PARTY_DIR}/sqlite")
endif()
if(NOT SNIPGUI_SDL2_DIR AND WIN32 AND
   EXISTS "${SNIPGUI_THIRD_PARTY_DIR}/SDL2/CMakeLists.txt")
    set(SNIPGUI_SDL2_DIR "${SNIPGUI_THIRD_PARTY_DIR}/SDL2")
endif()

# Pinned so a build is reproducible even if the upstream default branch moves.
set(SNIPGUI_CIMGUI_URL
    "https://codeload.github.com/cimgui/cimgui/zip/c4461a7eadbd17b6a6a8124a8abbf499aba6318d")
set(SNIPGUI_IMGUI_URL
    "https://codeload.github.com/ocornut/imgui/zip/6d32174508fb8c518e7a832cc1d571fadd5cba76")
set(SNIPGUI_SQLITE_URL
    "https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip")
set(SNIPGUI_SDL2_URL
    "https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz")

get_filename_component(SNIPGUI_DEPS_DIR "${SNIPGUI_DEPS_DIR}" ABSOLUTE)

# Download an archive once and locate the single top-level directory that
# contains `marker`.  Sets SNIPGUI_<NAME>_SOURCE_DIR in the parent scope.
function(snipgui_fetch_dependency NAME URL MARKER)
    if(DEFINED SNIPGUI_${NAME}_DIR AND NOT "${SNIPGUI_${NAME}_DIR}" STREQUAL "")
        set(dir "${SNIPGUI_${NAME}_DIR}")
    else()
        set(root "${SNIPGUI_DEPS_DIR}/${NAME}")
        set(stamp "${root}/.download-complete")
        if(NOT EXISTS "${stamp}")
            file(MAKE_DIRECTORY "${root}")
            set(archive "${SNIPGUI_DEPS_DIR}/${NAME}.download")
            message(STATUS "Downloading ${NAME} from ${URL}")
            file(DOWNLOAD "${URL}" "${archive}"
                 STATUS download_status
                 SHOW_PROGRESS
                 TLS_VERIFY ON)
            list(GET download_status 0 download_code)
            list(GET download_status 1 download_message)
            if(NOT download_code EQUAL 0)
                file(REMOVE "${archive}")
                message(FATAL_ERROR
                    "Could not download ${NAME}: ${download_message}\n"
                    "Check the network/proxy, or point SNIPGUI_${NAME}_DIR at a "
                    "local copy and configure again.")
            endif()
            file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${root}")
            file(REMOVE "${archive}")
            file(WRITE "${stamp}" "")
        endif()

        file(GLOB matches "${root}/*/${MARKER}")
        list(LENGTH matches match_count)
        if(match_count EQUAL 0)
            message(FATAL_ERROR
                "The ${NAME} archive did not contain ${MARKER} below ${root}.")
        endif()
        list(GET matches 0 marker_path)
        get_filename_component(dir "${marker_path}" DIRECTORY)
    endif()

    if(NOT EXISTS "${dir}/${MARKER}")
        message(FATAL_ERROR
            "SNIPGUI_${NAME}_DIR=${dir} does not contain ${MARKER}.")
    endif()
    set(SNIPGUI_${NAME}_SOURCE_DIR "${dir}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# cimgui + Dear ImGui
# ---------------------------------------------------------------------------
snipgui_fetch_dependency(CIMGUI "${SNIPGUI_CIMGUI_URL}" "cimgui.cpp")
snipgui_fetch_dependency(IMGUI "${SNIPGUI_IMGUI_URL}" "imgui.h")

# cimgui.cpp includes "./imgui/imgui.h" relative to its own file.  The vendored
# copy and a recursive git checkout already have that layout.  A plain zip
# download does not, so stage those files together in the build directory.
if(EXISTS "${SNIPGUI_CIMGUI_SOURCE_DIR}/imgui/imgui.h")
    set(SNIPGUI_CIMGUI_STAGE "${SNIPGUI_CIMGUI_SOURCE_DIR}")
else()
    set(SNIPGUI_CIMGUI_STAGE "${CMAKE_BINARY_DIR}/cimgui_stage")
    file(MAKE_DIRECTORY "${SNIPGUI_CIMGUI_STAGE}")
    file(COPY "${SNIPGUI_CIMGUI_SOURCE_DIR}/cimgui.cpp"
              "${SNIPGUI_CIMGUI_SOURCE_DIR}/cimgui.h"
         DESTINATION "${SNIPGUI_CIMGUI_STAGE}")
    file(COPY "${SNIPGUI_IMGUI_SOURCE_DIR}/"
         DESTINATION "${SNIPGUI_CIMGUI_STAGE}/imgui")
endif()

add_library(snippet_imgui STATIC
    "${SNIPGUI_CIMGUI_STAGE}/cimgui.cpp"
    "${SNIPGUI_IMGUI_SOURCE_DIR}/imgui.cpp"
    "${SNIPGUI_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
    "${SNIPGUI_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
    "${SNIPGUI_IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
    "${SNIPGUI_IMGUI_SOURCE_DIR}/imgui_demo.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/imgui_backend_sdl2.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/imgui_backend_opengl3.cpp"
)
target_include_directories(snippet_imgui PUBLIC
    "${SNIPGUI_CIMGUI_STAGE}"
    "${SNIPGUI_IMGUI_SOURCE_DIR}"
    "${SNIPGUI_IMGUI_SOURCE_DIR}/backends"
)
target_compile_definitions(snippet_imgui PUBLIC
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS=1
    CIMGUI_NO_EXPORT
)
if(WIN32)
    target_link_libraries(snippet_imgui PUBLIC imm32)
endif()

# ---------------------------------------------------------------------------
# SQLite amalgamation (always with FTS5)
# ---------------------------------------------------------------------------
snipgui_fetch_dependency(SQLITE "${SNIPGUI_SQLITE_URL}" "sqlite3.c")
add_library(snippet_sqlite STATIC "${SNIPGUI_SQLITE_SOURCE_DIR}/sqlite3.c")
target_include_directories(snippet_sqlite PUBLIC
    "${SNIPGUI_SQLITE_SOURCE_DIR}")
target_compile_definitions(snippet_sqlite PUBLIC
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_FTS3
    SQLITE_ENABLE_FTS3_PARENTHESIS
    SQLITE_ENABLE_JSON1
    SQLITE_THREADSAFE=1
)

# ---------------------------------------------------------------------------
# SDL2: local source -> system package -> download source
# ---------------------------------------------------------------------------
set(SNIPGUI_SDL2_TARGET "")

if(SNIPGUI_SDL2_DIR)
    set(SNIPGUI_SDL2_SOURCE_DIR "${SNIPGUI_SDL2_DIR}")
endif()

if(NOT SNIPGUI_SDL2_SOURCE_DIR)
    find_package(SDL2 CONFIG QUIET)
endif()

if(TARGET SDL2::SDL2-static)
    set(SNIPGUI_SDL2_TARGET SDL2::SDL2-static)
elseif(TARGET SDL2::SDL2)
    set(SNIPGUI_SDL2_TARGET SDL2::SDL2)
elseif(NOT SNIPGUI_SDL2_SOURCE_DIR)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2_PC QUIET sdl2)
    endif()
    if(SDL2_PC_FOUND)
        find_library(SDL2_PC_LIBRARY NAMES SDL2
                     PATHS ${SDL2_PC_LIBRARY_DIRS} NO_DEFAULT_PATH)
        if(NOT SDL2_PC_LIBRARY)
            find_library(SDL2_PC_LIBRARY NAMES SDL2)
        endif()
        if(SDL2_PC_LIBRARY)
            add_library(SDL2::SDL2 UNKNOWN IMPORTED)
            set_target_properties(SDL2::SDL2 PROPERTIES
                IMPORTED_LOCATION "${SDL2_PC_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${SDL2_PC_INCLUDE_DIRS}")
            set(SNIPGUI_SDL2_TARGET SDL2::SDL2)
        endif()
    endif()
endif()

if(NOT SNIPGUI_SDL2_TARGET)
    # On Unix, prefer a system SDL2 (distributions ship it); fall back to the
    # vendored source when it is not available.
    if(NOT SNIPGUI_SDL2_SOURCE_DIR AND
       EXISTS "${SNIPGUI_THIRD_PARTY_DIR}/SDL2/CMakeLists.txt")
        set(SNIPGUI_SDL2_SOURCE_DIR "${SNIPGUI_THIRD_PARTY_DIR}/SDL2")
    endif()

    if(NOT SNIPGUI_SDL2_SOURCE_DIR)
        if(NOT SNIPGUI_FETCH_SDL2)
            message(FATAL_ERROR
                "SDL2 was not found and SNIPGUI_FETCH_SDL2 is OFF. Install "
                "SDL2 development files or pass SNIPGUI_SDL2_DIR.")
        endif()
        snipgui_fetch_dependency(SDL2 "${SNIPGUI_SDL2_URL}" "include/SDL.h")
    endif()

    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)

    add_subdirectory("${SNIPGUI_SDL2_SOURCE_DIR}"
                     "${CMAKE_BINARY_DIR}/sdl2_build"
                     EXCLUDE_FROM_ALL)

    if(TARGET SDL2::SDL2-static)
        set(SNIPGUI_SDL2_TARGET SDL2::SDL2-static)
    elseif(TARGET SDL2::SDL2)
        set(SNIPGUI_SDL2_TARGET SDL2::SDL2)
    elseif(TARGET SDL2-static)
        set(SNIPGUI_SDL2_TARGET SDL2-static)
    else()
        message(FATAL_ERROR "SDL2 was built but no SDL2 target was created.")
    endif()
endif()

# The SDL2 backend includes <SDL.h>, so the ImGui target needs SDL2's include
# directories as well as the application target.
target_link_libraries(snippet_imgui PUBLIC ${SNIPGUI_SDL2_TARGET})

# ---------------------------------------------------------------------------
# OpenGL
# ---------------------------------------------------------------------------
set(OpenGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)

# ---------------------------------------------------------------------------
# Application targets
#
# snippet_manager.exe  : console build, keeps terminal error output
# SnippetManager.exe   : GUI build on Windows, no console; stderr goes to
#                        snippet_manager.log, fatal errors use a MessageBox
# ---------------------------------------------------------------------------
set(SNIPGUI_APP_SOURCES
    src/main.c
    src/snippet_db.c
)

function(snipgui_configure_app_target target)
    target_include_directories(${target} PRIVATE src)
    target_link_libraries(${target} PRIVATE
        snippet_imgui
        snippet_sqlite
        ${SNIPGUI_SDL2_TARGET}
        OpenGL::GL
    )
    set_target_properties(${target} PROPERTIES LINKER_LANGUAGE CXX)

    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()

    if(MINGW)
        # Single self-contained .exe: no libgcc/libstdc++/winpthread DLLs.
        target_link_options(${target} PRIVATE
            -static -static-libgcc -static-libstdc++)
    endif()

    if(APPLE)
        target_compile_definitions(${target} PRIVATE GL_SILENCE_DEPRECATION)
    endif()
endfunction()

add_executable(snippet_manager ${SNIPGUI_APP_SOURCES})
snipgui_configure_app_target(snippet_manager)
set_target_properties(snippet_manager PROPERTIES OUTPUT_NAME "snippet_manager")

if(WIN32)
    add_executable(snippet_manager_gui ${SNIPGUI_APP_SOURCES})
    snipgui_configure_app_target(snippet_manager_gui)
    target_compile_definitions(snippet_manager_gui PRIVATE SNIPGUI_GUI_APP)
    set_target_properties(snippet_manager_gui PROPERTIES
        WIN32_EXECUTABLE TRUE
        OUTPUT_NAME "SnippetManager"
    )
endif()

if(MSVC)
    target_compile_options(snippet_imgui PRIVATE /utf-8)
else()
    target_compile_options(snippet_imgui PRIVATE -Wall -Wextra)
endif()

# If SDL2 was found as a shared library, place its DLL next to each executable
# so the programs can be launched from the build directory directly.
if(WIN32 AND TARGET ${SNIPGUI_SDL2_TARGET})
    get_target_property(SNIPGUI_SDL2_TYPE ${SNIPGUI_SDL2_TARGET} TYPE)
    if(SNIPGUI_SDL2_TYPE STREQUAL "SHARED_LIBRARY")
        foreach(SNIPGUI_EXE snippet_manager snippet_manager_gui)
            if(TARGET ${SNIPGUI_EXE})
                add_custom_command(TARGET ${SNIPGUI_EXE} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "$<TARGET_FILE:${SNIPGUI_SDL2_TARGET}>"
                            "$<TARGET_FILE_DIR:${SNIPGUI_EXE}>"
                    COMMENT "Copying SDL2 runtime next to ${SNIPGUI_EXE}")
            endif()
        endforeach()
    endif()
endif()
