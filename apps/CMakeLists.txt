# Guest ELF Applications Build Configuration
#
# This CMakeLists.txt can be used standalone or included in the main project
# to build guest ELF applications that run on the Linux Compatibility Layer.
#
# Standalone usage:
#   mkdir build && cd build
#   cmake -DCMAKE_TOOLCHAIN_FILE=<path_to_xtensa_toolchain> ..
#   make
#
# Or use the provided build_guest_app.bat script for simpler builds.

cmake_minimum_required(VERSION 3.16)

# Project name
project(guest_apps C)

# Check for Xtensa compiler
if(NOT CMAKE_C_COMPILER MATCHES "xtensa")
    message(WARNING "Not using Xtensa compiler. Guest apps should be cross-compiled for ESP32.")
endif()

# Common compiler flags for guest ELFs
set(GUEST_CFLAGS
    -mlongcalls          # Generate long call sequences
    -fno-common          # Put uninitialized globals in .bss
    -ffunction-sections  # Put each function in its own section
    -fdata-sections      # Put each data item in its own section
    -fPIC                # Position Independent Code
    -Os                  # Optimize for size
)

# Common linker flags for guest ELFs
set(GUEST_LDFLAGS
    -nostartfiles        # Don't use standard startup
    -nostdlib            # Don't link standard library
    -nodefaultlibs       # Don't link default libraries
    -shared              # Create shared object
    -fPIC                # Position Independent Code
    -Wl,--gc-sections    # Garbage collect unused sections
    -Wl,--strip-all      # Strip all symbols
)

# Function to add a guest ELF target
function(add_guest_elf APP_NAME)
    set(APP_SOURCES ${ARGN})

    # Create object library
    add_library(${APP_NAME}_obj OBJECT ${APP_SOURCES})
    target_compile_options(${APP_NAME}_obj PRIVATE ${GUEST_CFLAGS})

    # Create ELF executable
    add_executable(${APP_NAME}.elf $<TARGET_OBJECTS:${APP_NAME}_obj>)
    target_link_options(${APP_NAME}.elf PRIVATE ${GUEST_LDFLAGS} -e main)
    set_target_properties(${APP_NAME}.elf PROPERTIES
        OUTPUT_NAME ${APP_NAME}
        SUFFIX ".elf"
    )

    # Add to list of guest apps
    list(APPEND GUEST_APPS ${APP_NAME}.elf)
    set(GUEST_APPS ${GUEST_APPS} PARENT_SCOPE)
endfunction()

# Build hello_world app
add_guest_elf(hello_world
    hello_world/main.c
)

# Custom target to build all guest apps
add_custom_target(guest_apps ALL
    DEPENDS ${GUEST_APPS}
    COMMENT "Building all guest ELF applications"
)

# Print info about built apps
add_custom_command(TARGET guest_apps POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "Guest ELF applications built:"
    COMMAND ${CMAKE_COMMAND} -E echo "  - hello_world.elf"
)
