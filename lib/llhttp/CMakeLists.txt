aux_source_directory(. llhttp_src)
set(DIR_SRCS ${llhttp_src})
include_directories(.)
add_library( llhttp
        STATIC
        ${DIR_SRCS})
# Static: a DLL linking it must not export llhttp_* (obs-airplay).
target_compile_definitions( llhttp PUBLIC LLHTTP_STATIC )
