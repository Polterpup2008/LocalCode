# Shared warning configuration applied through the localcode_warnings interface target.
add_library(localcode_warnings INTERFACE)
add_library(LocalCode::warnings ALIAS localcode_warnings)

if(MSVC)
    target_compile_options(localcode_warnings INTERFACE
        /W4
        /wd4100  # unreferenced formal parameter (common in interface impls)
        /wd4267  # size_t -> smaller type, pervasive with Win32 APIs
        /wd4244)
    if(LOCALCODE_WARNINGS_AS_ERRORS)
        target_compile_options(localcode_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(localcode_warnings INTERFACE
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
        -Wno-missing-field-initializers)
    if(LOCALCODE_WARNINGS_AS_ERRORS)
        target_compile_options(localcode_warnings INTERFACE -Werror)
    endif()
endif()
