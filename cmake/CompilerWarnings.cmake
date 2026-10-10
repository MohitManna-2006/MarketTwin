# Warning flags for MarketTwin-owned targets. Do not apply this to FetchContent dependencies.

function(markettwin_set_warnings target)
  set(msvc_warnings /W4 /permissive- /w14640)
  set(clang_warnings
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wshadow
      -Wnon-virtual-dtor
      -Wold-style-cast
      -Wcast-align
      -Wunused
      -Woverloaded-virtual
      -Wnull-dereference
      -Wdouble-promotion
      -Wformat=2
      -Wimplicit-fallthrough
  )
  set(gcc_warnings ${clang_warnings} -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast)

  if(MARKETTWIN_WARNINGS_AS_ERRORS)
    list(APPEND msvc_warnings /WX)
    list(APPEND clang_warnings -Werror)
    list(APPEND gcc_warnings -Werror)
  endif()

  target_compile_options(
    ${target}
    PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:${msvc_warnings}>"
            "$<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:${clang_warnings}>"
            "$<$<CXX_COMPILER_ID:GNU>:${gcc_warnings}>"
  )
endfunction()
