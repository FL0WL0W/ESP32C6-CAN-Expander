idf_component_register(PRIV_REQUIRES esp_driver_gpio esp_driver_gptimer esp_adc)

include(ExternalProject)

set(EMBEDDEDIOSERVICES_INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/embeddedioservices_install)

externalproject_add(embeddedioservices_proj
    GIT_REPOSITORY https://github.com/FL0WL0W/EmbeddedIOServices
    GIT_TAG main

    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>

    USES_TERMINAL_DOWNLOAD TRUE
    USES_TERMINAL_CONFIGURE TRUE
    USES_TERMINAL_BUILD TRUE

    INSTALL_DIR ${EMBEDDEDIOSERVICES_INSTALL_DIR}
    BUILD_BYPRODUCTS "${EMBEDDEDIOSERVICES_INSTALL_DIR}/lib/libEmbeddedIOServices.a"
)

add_prebuilt_library(embeddedioservices_lib "${EFIGENIE_INSTALL_DIR}/lib/libEmbeddedIOServices.a" PRIV_REQUIRES cxx)
add_dependencies(embeddedioservices_lib efigenie_proj)

set(EmbeddedIOServicesHardwareAbstraction_Path ${CMAKE_CURRENT_BINARY_DIR}/embeddedioservices_proj-prefix/embeddedioservices_proj/HardwareAbstractions/Esp32Idf)
file(GLOB_RECURSE EmbeddedIOServiceHardwareAbstraction_SRCS ${EmbeddedIOServicesHardwareAbstraction_Path}/src/*.cpp)
file(GLOB_RECURSE EmbeddedIOServiceHardwareAbstraction_SRCS_Templates ${EmbeddedIOServicesHardwareAbstraction_Path}/src/*_template.cpp)
IF(EmbeddedIOServiceHardwareAbstraction_SRCS_Templates)
        list(REMOVE_ITEM EmbeddedIOServiceHardwareAbstraction_SRCS ${EmbeddedIOServiceHardwareAbstraction_SRCS_Templates})
ENDIF()

target_sources(${COMPONENT_LIB} PUBLIC "${EmbeddedIOServiceHardwareAbstraction_SRCS}")
target_include_directories(${COMPONENT_LIB} INTERFACE "${EMBEDDEDIOSERVICES_INSTALL_DIR}/include")
target_include_directories(${COMPONENT_LIB} INTERFACE "${EmbeddedIOServicesHardwareAbstraction_Path}/include")