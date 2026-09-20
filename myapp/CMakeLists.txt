cmake_minimum_required(VERSION 3.16)
project(myapp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-O3 -march=native)

# ─── Dependencies ────────────────────────────────────────────────────────────
find_package(Drogon REQUIRED)
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

# ─── webview (header-only + platform libs) ───────────────────────────────────
# We fetch webview from GitHub so users don't need to install it manually.
include(FetchContent)
FetchContent_Declare(
    webview
    GIT_REPOSITORY https://github.com/webview/webview.git
    GIT_TAG        0.12.0
)
FetchContent_MakeAvailable(webview)

# ─── Sources ─────────────────────────────────────────────────────────────────
set(SOURCES
    main.cc
    controllers/ChatController.cc
    controllers/SearchController.cc
    services/OllamaService.cc
    services/DuckDuckGoService.cc
    services/McpService.cc
    services/WebScraperService.cc
    services/RagService.cc
)

# ─── macOS app bundle ────────────────────────────────────────────────────────
if(APPLE)
    # Create the icon if the .icns doesn't exist yet (from the generate script)
    set(ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/AppIcon.icns")

    set(MACOSX_BUNDLE_EXECUTABLE_NAME ${PROJECT_NAME})
    set(MACOSX_BUNDLE_BUNDLE_NAME "Local Global")
    set(MACOSX_BUNDLE_GUI_IDENTIFIER "com.localglobal.app")
    set(MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0")
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.0")
    set(MACOSX_BUNDLE_ICON_FILE "AppIcon")

    add_executable(${PROJECT_NAME} MACOSX_BUNDLE ${SOURCES})

    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
        OUTPUT_NAME "Local Global"
    )

    # Embed the icon in the bundle
    if(EXISTS "${ICON_FILE}")
        target_sources(${PROJECT_NAME} PRIVATE "${ICON_FILE}")
        set_source_files_properties("${ICON_FILE}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources"
        )
    endif()
else()
    add_executable(${PROJECT_NAME} ${SOURCES})
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Drogon::Drogon
    Threads::Threads
    webview::core
    CURL::libcurl
)

# ─── Copy frontend/ into the right place ─────────────────────────────────────
if(APPLE)
    # For .app bundle: copy into Contents/Resources/frontend
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/frontend"
            "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/Resources/frontend"
        COMMENT "Copying frontend assets into app bundle"
    )
    # Also copy mcp_servers.json if it exists
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/mcp_servers.json")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_CURRENT_SOURCE_DIR}/mcp_servers.json"
                "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/Resources/mcp_servers.json"
            COMMENT "Copying mcp_servers.json into app bundle"
        )
    endif()

    # Copy TTS server script into app bundle (model is downloaded on first run)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/tts_server.py"
            "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/Resources/tts_server.py"
        COMMENT "Copying tts_server.py into app bundle"
    )
    # NOTE: Kokoro TTS model files are NOT bundled — they are automatically
    # downloaded to ~/Library/Application Support/Local Global/models/kokoro/
    # on first launch.
else()
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/frontend"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/frontend"
        COMMENT "Copying frontend assets to build directory"
    )
endif()
