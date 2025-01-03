if(CMAKE_COMPILER_IS_GNUCXX
   OR ${CMAKE_C_COMPILER_ID} MATCHES Clang
   OR ${CMAKE_C_COMPILER_ID} MATCHES GNU
   OR ${CMAKE_C_COMPILER_ID} MATCHES Intel)
    if(ENABLE_LTO)
        set(LTO_FLAGS "-flto")
        set(LTO_LINK_FLAGS "-fuse-ld=gold")
    else()
        set(LTO_FLAGS "")
        set(LTO_LINK_FLAGS "")
    endif()
    set(CMAKE_C_FLAGS_DEBUG "-Og -g3")
    set(CMAKE_CXX_FLAGS_DEBUG "-Og -g3")

    set(CMAKE_C_FLAGS_RELEASE "-O2 -g1 -fstack-protector-strong ${LTO_FLAGS}")
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g1 -fstack-protector-strong ${LTO_FLAGS}")

    set(CMAKE_C_FLAGS_RELWITHDEBINFO
        "-O2 -g3 -fno-omit-frame-pointer -fstack-protector-strong ${LTO_FLAGS}")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO
        "-O2 -g3 -fno-omit-frame-pointer -fstack-protector-strong ${LTO_FLAGS}")

    set(FORCE_FRAME_POINTER "-fno-omit-frame-pointer")
    # Ensure each library is fully linked
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--no-undefined")

    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${LTO_LINK_FLAGS}")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${LTO_LINK_FLAGS}")
else(MSVC)
    if(ENABLE_LTO)
        set(LTO_FLAGS "/GL")
        set(LTO_LINK_FLAGS "/LTCG")
    else()
        set(LTO_FLAGS "")
        set(LTO_LINK_FLAGS "")
    endif()
    set(CMAKE_C_FLAGS_DEBUG "/MP /Od /Zi /MDd /D_DEBUG")
    set(CMAKE_CXX_FLAGS_DEBUG "/MP /Od /Zi /MDd /D_DEBUG")

    set(CMAKE_C_FLAGS_RELEASE "/MP /O2 /MD /D NDEBUG ${LTO_FLAGS}")
    set(CMAKE_CXX_FLAGS_RELEASE "/MP /O2 /MD /D NDEBUG ${LTO_FLAGS}")

    set(CMAKE_C_FLAGS_RELWITHDEBINFO "/MP /O2 /Zi /MD /D NDEBUG ${LTO_FLAGS}")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "/MP /O2 /Zi /MD /D NDEBUG ${LTO_FLAGS}")

    set(FORCE_FRAME_POINTER "/Oy-")

    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${LTO_LINK_FLAGS}")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${LTO_LINK_FLAGS}")
    set(CMAKE_STATIC_LINKER_FLAGS "${CMAKE_STATIC_LINKER_FLAGS} ${LTO_LINK_FLAGS}")
endif()
