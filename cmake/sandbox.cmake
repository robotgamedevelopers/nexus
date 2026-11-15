# The two programs Claude's sandbox needs on Linux and WSL2 (docs/secrets.md, "The token wall"), built here so no machine
# needs a package installed for them: bubblewrap (github.com/containers/bubblewrap, LGPL v2 or later), which puts each
# command the AI runs in its own namespaces, and socat (www.dest-unreach.org/socat, GPL v2), which carries the sandbox's
# network proxy into them. bubblewrap needs libcap (git.kernel.org's libcap2, BSD or GPL v2), linked in statically.
# Each is pinned to a release by its SHA-256 and compiled from its own sources unmodified, with no root and nothing
# installed: only a C compiler, make, and the kernel's headers (Ubuntu's linux-libc-dev, which any C toolchain has).
#
# The results land in <build>/sandbox/: bwrap/bwrap and socat/socat. nexus-server tells its drivers where
# (NEXUS_SANDBOX_BIN), and the Claude driver puts them on Claude's PATH (drivers/claude/src/tools.ts). On Ubuntu 24.04
# and later, whose AppArmor lets a program make the user namespaces bubblewrap needs only with a profile, the driver
# loads one for this bwrap, once, through passwordless sudo (tools.ts appArmorProfile).

include(ExternalProject)

set(nexus_sandbox_dir ${CMAKE_BINARY_DIR}/sandbox)
file(MAKE_DIRECTORY ${nexus_sandbox_dir}/bwrap ${nexus_sandbox_dir}/socat)
# A build directory made before this change has the old link to /usr/bin/bwrap; nothing uses it now.
file(REMOVE_RECURSE ${nexus_sandbox_dir}/system-bwrap)

# libcap 2.78: only its static library, which its own Makefile builds without installing.
ExternalProject_Add(nexus_libcap
    URL https://mirrors.edge.kernel.org/pub/linux/libs/security/linux-privs/libcap2/libcap-2.78.tar.xz
    URL_HASH SHA256=0d621e562fd932ccf67b9660fb018e468a683d7b827541df27813228c996bb11
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    CONFIGURE_COMMAND ""
    BUILD_IN_SOURCE TRUE
    BUILD_COMMAND make -C libcap CC=${CMAKE_C_COMPILER} BUILD_CC=${CMAKE_C_COMPILER} USE_GPERF=no libcap.a
    BUILD_BYPRODUCTS <SOURCE_DIR>/libcap/libcap.a
    INSTALL_COMMAND ""
    EXCLUDE_FROM_ALL TRUE)
ExternalProject_Get_Property(nexus_libcap SOURCE_DIR)
set(nexus_libcap_dir ${SOURCE_DIR})

# bubblewrap 0.13.1: its six sources, with the flag and the one-line config.h its meson.build gives them.
file(WRITE ${CMAKE_BINARY_DIR}/sandbox-config/config.h "#define PACKAGE_STRING \"bubblewrap 0.13.1\"\n")
ExternalProject_Add(nexus_bwrap
    URL https://github.com/containers/bubblewrap/releases/download/v0.13.1/bubblewrap-0.13.1.tar.xz
    URL_HASH SHA256=0e85c8702248b8dd64144f594147553650cdd0e4c951690345faace79c217c4d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    DEPENDS nexus_libcap
    CONFIGURE_COMMAND ""
    BUILD_IN_SOURCE TRUE
    BUILD_COMMAND ${CMAKE_C_COMPILER} -O2 -D_GNU_SOURCE -I${CMAKE_BINARY_DIR}/sandbox-config -I.
        -I${nexus_libcap_dir}/libcap/include
        bubblewrap.c bind-mount.c network.c utils.c chroot_realpath.c safe_openat.c
        ${nexus_libcap_dir}/libcap/libcap.a -o ${nexus_sandbox_dir}/bwrap/bwrap
    BUILD_BYPRODUCTS ${nexus_sandbox_dir}/bwrap/bwrap
    # Each program keeps its licence beside it (README.md, "What it builds on").
    INSTALL_COMMAND ${CMAKE_COMMAND} -E copy <SOURCE_DIR>/COPYING ${nexus_sandbox_dir}/bwrap/COPYING
        COMMAND ${CMAKE_COMMAND} -E copy ${nexus_libcap_dir}/License ${nexus_sandbox_dir}/bwrap/libcap-License
    EXCLUDE_FROM_ALL TRUE)

# socat 1.8.1.3: its own configure, without the libraries the proxy does not use (OpenSSL, readline, libwrap), so it
# needs none installed. Its site serves no valid certificate; the hash pins it (the same as Homebrew's formula).
ExternalProject_Add(nexus_socat
    URL http://www.dest-unreach.org/socat/download/socat-1.8.1.3.tar.gz
    URL_HASH SHA256=06602ffd591e98c75b3dc1d66f0f19136cc666b0b2d95caad987d6ab2cb28097
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    CONFIGURE_COMMAND <SOURCE_DIR>/configure --quiet CC=${CMAKE_C_COMPILER} --disable-openssl --disable-readline
        --disable-libwrap --disable-fips
    BUILD_COMMAND make socat
    BUILD_BYPRODUCTS ${nexus_sandbox_dir}/socat/socat
    INSTALL_COMMAND ${CMAKE_COMMAND} -E copy <BINARY_DIR>/socat ${nexus_sandbox_dir}/socat/socat
        COMMAND ${CMAKE_COMMAND} -E copy <SOURCE_DIR>/COPYING ${nexus_sandbox_dir}/socat/COPYING
    EXCLUDE_FROM_ALL TRUE)

add_custom_target(nexus_sandbox_tools DEPENDS nexus_bwrap nexus_socat)
