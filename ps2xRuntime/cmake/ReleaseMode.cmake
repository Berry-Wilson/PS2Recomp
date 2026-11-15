include(CheckIPOSupported)

check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)

function(EnableFastReleaseMode TargetName)
    message("> Enabling optimization for: ${TargetName}")
    if(MSVC)
        target_compile_options(${TargetName} PRIVATE
            $<$<CONFIG:Release>:
                /O2 # speed
                /Ob2 # inline aggressively
                /Oi # intrinsics
                /GL # whole program opt
                /Gy # function-level linking
                /Gw # global data in COMDAT
                /GF # string pooling
                /Zc:inline # remove unreferenced inline
                /fp:fast # fast math (graphics friendly)
                /DNDEBUG
                /arch:AVX2 # Advanced Vector Extensions 2
                /GS- # Disable Buffer Security Check (faster)
                /Qspectre- # Disable Spectre mitigations (faster)
            >
        )

        if(TARGET ${TargetName})
            target_link_options(${TargetName} PRIVATE
                $<$<CONFIG:Release>:
                    /LTCG # link-time code generation
                    /OPT:REF # remove unreferenced
                    /OPT:ICF # fold identical COMDATs
                >
            )
        endif()
    elseif(NOT PS2X_IS_ARM_TARGET AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        # The same CPU level as MSVC's /arch:AVX2; the translated code uses SSE4.1 and AVX
        # intrinsics, which clang and GCC only allow with the target feature on. No contraction of
        # a * b + c into one rounding, so float results match a plain build.
        target_compile_options(${TargetName} PRIVATE
            $<$<CONFIG:Release>:
                -mavx2
                -mfma
                -ffp-contract=off
                -fno-math-errno
            >
        )
    endif()

    if(MINGW)
        # The C++ library and its unwinder go into the program, so it doesn't need the toolchain's DLLs.
        target_link_options(${TargetName} PRIVATE -static-libstdc++ -static-libgcc)
    endif()

    if(IPO_SUPPORTED)
        set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    else()
        message(WARNING "Interprocedural optimization not supported: ${ipo_error}")
    endif()
endfunction()