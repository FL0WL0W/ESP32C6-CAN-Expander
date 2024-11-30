set(LIBRARIES_DIR	${CMAKE_SOURCE_DIR}/libs)
set(UPDI_Path           ${LIBRARIES_DIR}/UPDI)

idf_component_register( SRCS "ATTiny_UPDI.cpp"
                        INCLUDE_DIRS "."
                        PRIV_REQUIRES driver)

include(FetchContent)

FetchContent_Declare(
    UPDI
    GIT_REPOSITORY https://github.com/FL0WL0W/UPDI
    GIT_TAG        main
    GIT_PROGRESS   TRUE
    GIT_SHALLOW    TRUE
    SOURCE_DIR     ${UPDI_Path}
)
FetchContent_MakeAvailable(UPDI)


file(GLOB_RECURSE UPDI_SRCS ${UPDI_Path}/src/*.c)
file(GLOB_RECURSE UPDI_SRCS_Templates ${UPDI_Path}/src/*_template.c)
IF(UPDI_SRCS_Templates)
        list(REMOVE_ITEM UPDI_SRCS ${UPDI_SRCS_Templates})
ENDIF()

target_sources(${COMPONENT_LIB} PUBLIC "${UPDI_SRCS}")
target_include_directories(${COMPONENT_LIB} PUBLIC "${UPDI_Path}/include")