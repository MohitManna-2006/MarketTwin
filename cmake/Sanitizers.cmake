# Optional ASan + UBSan instrumentation for MarketTwin-owned targets.

function(markettwin_enable_sanitizers target)
  if(NOT MARKETTWIN_ENABLE_SANITIZERS)
    return()
  endif()

  if(MSVC)
    message(FATAL_ERROR "MARKETTWIN_ENABLE_SANITIZERS is supported for Clang and GCC only.")
  endif()

  target_compile_options(
    ${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
  )
  target_link_options(${target} PRIVATE -fsanitize=address,undefined)
endfunction()
