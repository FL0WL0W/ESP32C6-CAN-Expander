set(LIBRARIES_DIR	    ${CMAKE_SOURCE_DIR}/libs)
set(EmbeddedIOServicesHardwareAbstraction_Path ${LIBRARIES_DIR}/EmbeddedIOServices/HardwareAbstractions/Esp32Idf)
set(ATTinyHardwareAbstraction_Path ${LIBRARIES_DIR}/EmbeddedIOServices/HardwareAbstractions/ATTiny427Expander)

idf_component_register( SRCS "JustAFile.cpp"
                        INCLUDE_DIRS "."
                        PRIV_REQUIRES driver esp_http_server)

include(FetchContent)

FetchContent_Declare(
    EFIGenie
    GIT_REPOSITORY https://github.com/FL0WL0W/EFIGenie
    GIT_TAG        main
    GIT_PROGRESS   TRUE
    GIT_SHALLOW    TRUE
    SOURCE_DIR  ${LIBRARIES_DIR}/EFIGenie
)

FetchContent_MakeAvailable(EFIGenie)

target_link_libraries(${COMPONENT_LIB} PUBLIC EFIGenie)

file(GLOB_RECURSE EmbeddedIOServiceHardwareAbstraction_SRCS ${EmbeddedIOServicesHardwareAbstraction_Path}/src/*.cpp)
file(GLOB_RECURSE EmbeddedIOServiceHardwareAbstraction_SRCS_Templates ${EmbeddedIOServicesHardwareAbstraction_Path}/src/*_template.cpp)
IF(EmbeddedIOServiceHardwareAbstraction_SRCS_Templates)
        list(REMOVE_ITEM EmbeddedIOServiceHardwareAbstraction_SRCS ${EmbeddedIOServiceHardwareAbstraction_SRCS_Templates})
ENDIF()

file(GLOB_RECURSE ATTinyHardwareAbstraction_SRCS ${ATTinyHardwareAbstraction_Path}/src/*.cpp)
file(GLOB_RECURSE ATTinyHardwareAbstraction_SRCS_Templates ${ATTinyHardwareAbstraction_Path}/src/*_template.cpp)
IF(ATTinyHardwareAbstraction_SRCS_Templates)
        list(REMOVE_ITEM ATTinyHardwareAbstraction_SRCS ${ATTinyHardwareAbstraction_SRCS_Templates})
ENDIF()

target_sources(${COMPONENT_LIB} PUBLIC "${EmbeddedIOServiceHardwareAbstraction_SRCS}")
target_include_directories(${COMPONENT_LIB} PUBLIC "${EmbeddedIOServicesHardwareAbstraction_Path}/include")
target_sources(${COMPONENT_LIB} PUBLIC "${ATTinyHardwareAbstraction_SRCS}")
target_include_directories(${COMPONENT_LIB} PUBLIC "${ATTinyHardwareAbstraction_Path}/include")