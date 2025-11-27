cmake_minimum_required(VERSION 3.16)

# Include the ELF loader component for building guest applications
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Set custom partition table
set(PARTITION_TABLE_CUSTOM_FILENAME "partitions.csv")

project(linux_compat_layer)
