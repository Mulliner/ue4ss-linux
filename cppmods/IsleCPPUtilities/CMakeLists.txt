cmake_minimum_required(VERSION 3.22)
set(TARGET IsleCPPUtilities)
project(${TARGET})

# No ImGui link and no GUI guard in the parent: this mod has no UI of its own.
# It runs headless on a dedicated server, which is the only place it is useful.
set(${TARGET}_Sources
        "${CMAKE_CURRENT_SOURCE_DIR}/src/dllmain.cpp"
        )

add_library(${TARGET} SHARED ${${TARGET}_Sources})
target_link_libraries(${TARGET} PUBLIC UE4SS)
