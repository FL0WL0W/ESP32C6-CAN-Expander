idf_component_register()

include(ExternalProject)

set(EFIGENIE_INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/efigenie_install)

externalproject_add(efigenie_proj
    GIT_REPOSITORY https://github.com/FL0WL0W/EFIGenie
    GIT_TAG main

    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>

    USES_TERMINAL_DOWNLOAD TRUE
    USES_TERMINAL_CONFIGURE TRUE
    USES_TERMINAL_BUILD TRUE

    INSTALL_DIR ${EFIGENIE_INSTALL_DIR}
    BUILD_BYPRODUCTS "${EFIGENIE_INSTALL_DIR}/lib/libOperationArchitecture.a"
    BUILD_BYPRODUCTS "${EFIGENIE_INSTALL_DIR}/lib/libEmbeddedIOServices.a"
    BUILD_BYPRODUCTS "${EFIGENIE_INSTALL_DIR}/lib/libEmbeddedIOOperations.a"
    BUILD_BYPRODUCTS "${EFIGENIE_INSTALL_DIR}/lib/libReluctorOperations.a"
    BUILD_BYPRODUCTS "${EFIGENIE_INSTALL_DIR}/lib/libEFIGenie.a"
)

add_prebuilt_library(operationarchitecture_lib "${EFIGENIE_INSTALL_DIR}/lib/libOperationArchitecture.a" PRIV_REQUIRES cxx)
add_dependencies(operationarchitecture_lib efigenie_proj)

add_prebuilt_library(embeddedioservices_lib "${EFIGENIE_INSTALL_DIR}/lib/libEmbeddedIOServices.a" PRIV_REQUIRES cxx)
add_dependencies(embeddedioservices_lib efigenie_proj)

add_prebuilt_library(embeddediooperations_lib "${EFIGENIE_INSTALL_DIR}/lib/libEmbeddedIOOperations.a" PRIV_REQUIRES cxx)
add_dependencies(embeddediooperations_lib efigenie_proj)
target_link_libraries(embeddediooperations_lib INTERFACE operationarchitecture_lib)
target_link_libraries(embeddediooperations_lib INTERFACE embeddedioservices_lib)

add_prebuilt_library(reluctoroperations_lib "${EFIGENIE_INSTALL_DIR}/lib/libReluctorOperations.a" PRIV_REQUIRES cxx)
add_dependencies(reluctoroperations_lib efigenie_proj)
target_link_libraries(reluctoroperations_lib INTERFACE operationarchitecture_lib)
target_link_libraries(reluctoroperations_lib INTERFACE embeddedioservices_lib)
target_link_libraries(reluctoroperations_lib INTERFACE embeddediooperations_lib)

add_prebuilt_library(efigenie_lib "${EFIGENIE_INSTALL_DIR}/lib/libEFIGenie.a" PRIV_REQUIRES cxx)
add_dependencies(efigenie_lib efigenie_proj)
target_link_libraries(efigenie_lib INTERFACE operationarchitecture_lib)
target_link_libraries(efigenie_lib INTERFACE embeddedioservices_lib)
target_link_libraries(efigenie_lib INTERFACE embeddediooperations_lib)
target_link_libraries(efigenie_lib INTERFACE reluctoroperations_lib)
target_link_libraries(${COMPONENT_LIB} INTERFACE efigenie_lib)

set(EmbeddedIOServicesHardwareAbstraction_Path ${CMAKE_CURRENT_BINARY_DIR}/efigenie_proj-prefix/src/efigenie_proj-build/_deps/embeddedioservices-src/HardwareAbstractions/Esp32Idf)
file(GLOB_RECURSE EmbeddedIOServiceHardwareAbstraction_SRCS ${EmbeddedIOServicesHardwareAbstraction_Path}/src/*.cpp)
file(GLOB_RECURSE EmbeddedIOServiceHardwareAbstraction_SRCS_Templates ${EmbeddedIOServicesHardwareAbstraction_Path}/src/*_template.cpp)
IF(EmbeddedIOServiceHardwareAbstraction_SRCS_Templates)
        list(REMOVE_ITEM EmbeddedIOServiceHardwareAbstraction_SRCS ${EmbeddedIOServiceHardwareAbstraction_SRCS_Templates})
ENDIF()

target_sources(${COMPONENT_LIB} PUBLIC "${EmbeddedIOServiceHardwareAbstraction_SRCS}")
target_include_directories(${COMPONENT_LIB} INTERFACE "${EmbeddedIOServicesHardwareAbstraction_Path}/include")
target_include_directories(${COMPONENT_LIB} INTERFACE "${EFIGENIE_INSTALL_DIR}/include")