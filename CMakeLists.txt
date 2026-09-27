cmake_minimum_required(VERSION 3.10)
project(AntiCheatPlugin_EasyAntiCheat LANGUAGES CXX)

# Set C++ standard to C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force 32-bit build when using MSVC
if (MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This project must be built as Win32 (x86). Use: cmake -A Win32")
endif()

# Create the DLL
add_library(plugin_eac SHARED plugin_eac.cpp)

set_target_properties(plugin_eac PROPERTIES
    OUTPUT_NAME "easyanticheat"
)

if (WIN32)
    target_compile_definitions(plugin_eac PRIVATE -DPLUGIN_EXPORTS)
endif()

# EOS deployment credentials, supplied by the builder (see README).
# Left undefined, plugin_eac.cpp falls back to placeholders and Initialize()
# refuses to create a platform rather than failing obscurely at runtime.
foreach(cred
    EAC_EOS_PRODUCT_ID
    EAC_EOS_SANDBOX_ID
    EAC_EOS_DEPLOYMENT_ID
    EAC_EOS_CLIENT_ID
    EAC_EOS_CLIENT_SECRET)
    set(${cred} "" CACHE STRING "EOS credential: ${cred}")
    if (NOT ${cred} STREQUAL "")
        target_compile_definitions(plugin_eac PRIVATE ${cred}="${${cred}}")
    else()
        message(WARNING "${cred} is not set - the built plugin will refuse to initialize.")
    endif()
endforeach()
