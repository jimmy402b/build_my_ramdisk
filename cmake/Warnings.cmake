function(myrd_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /EHsc
            /utf-8
        )
    endif()
endfunction()
