# PhotoCraft's RAW decoder (Rust) as a static library behind a C ABI.
#
# engine/raw-photocraft is the crate; its dependency photocraft-raw is the
# pinned, verified and overlaid copy engine/setup-photocraft-raw.{sh,ps1}
# stage in <repo>/build/deps/photocraft-raw (pin and overlay:
# engine/third_party/photocraft-raw). Nothing is fetched here: this file
# verifies the stage and runs `cargo rustc --locked`, so the crates are the
# ones in engine/raw-photocraft/Cargo.lock.
#
# Defines the imported target spk_photocraft_raw and, in
# <build>/engine/third-party/photocraft-raw, the licence texts the binary
# then carries (PhotoCraft's, and one file for the Rust crates linked in).
#
# -DSPEKTRALAB_PHOTOCRAFT_RAW=OFF builds without it: io/raw_photocraft.cpp
# then reports every decode as unavailable and the host keeps LibRaw only.
option(SPEKTRALAB_PHOTOCRAFT_RAW "Build and link the PhotoCraft RAW decoder (Rust; engine/raw-photocraft)" ON)
if(NOT SPEKTRALAB_PHOTOCRAFT_RAW)
    return()
endif()

set(photocraft_third "${CMAKE_CURRENT_LIST_DIR}/../third_party/photocraft-raw")
set(photocraft_crate "${CMAKE_CURRENT_LIST_DIR}/../raw-photocraft")
# Not SPEKTRALAB_DEPS_DIR: Cargo.toml names this path, relative to the crate.
set(photocraft_stage "${SPEKTRALAB_REPO_ROOT}/build/deps/photocraft-raw")
set(photocraft_setup "run engine/setup-photocraft-raw.ps1 (Windows) or engine/setup-photocraft-raw.sh (POSIX), or configure with -DSPEKTRALAB_PHOTOCRAFT_RAW=OFF")

file(STRINGS "${photocraft_third}/PIN" photocraft_pin REGEX "^commit=[0-9a-f]+")
string(REGEX REPLACE "^commit=([0-9a-f]+).*" "\\1" photocraft_commit "${photocraft_pin}")
string(LENGTH "${photocraft_commit}" photocraft_commit_length)
if(NOT photocraft_commit_length EQUAL 40)
    message(FATAL_ERROR "engine/third_party/photocraft-raw/PIN does not name a commit")
endif()
if(NOT EXISTS "${photocraft_stage}/.spk-pin" OR NOT EXISTS "${photocraft_stage}/src/lib.rs")
    message(FATAL_ERROR "PhotoCraft RAW needs the staged photocraft-raw source in ${photocraft_stage}; ${photocraft_setup}")
endif()
file(STRINGS "${photocraft_stage}/.spk-pin" photocraft_staged_pin REGEX "^commit=")
if(NOT photocraft_staged_pin STREQUAL "commit=${photocraft_commit}")
    message(FATAL_ERROR "${photocraft_stage} is not PhotoCraft ${photocraft_commit}; ${photocraft_setup}")
endif()
# The stage must carry exactly the tracked overlay, so an edit to the overlay
# that was not re-staged cannot build the old source. Line endings aside: the
# stage is written with LF, a Windows checkout of the overlay may have CRLF.
foreach(path IN ITEMS Cargo.toml src/develop.rs src/lib.rs)
    file(READ "${photocraft_stage}/${path}" photocraft_staged_text)
    file(READ "${photocraft_third}/files/${path}" photocraft_overlay_text)
    string(REPLACE "\r" "" photocraft_staged_text "${photocraft_staged_text}")
    string(REPLACE "\r" "" photocraft_overlay_text "${photocraft_overlay_text}")
    if(NOT photocraft_staged_text STREQUAL photocraft_overlay_text)
        message(FATAL_ERROR "${photocraft_stage}/${path} is not the tracked overlay; ${photocraft_setup}")
    endif()
endforeach()
if(NOT EXISTS "${photocraft_crate}/Cargo.lock")
    message(FATAL_ERROR "engine/raw-photocraft/Cargo.lock is missing; it is tracked, and engine/tools/pin_photocraft_lock.py regenerates it")
endif()

find_program(SPEKTRALAB_CARGO cargo HINTS "$ENV{CARGO_HOME}/bin" "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin")
if(NOT SPEKTRALAB_CARGO)
    message(FATAL_ERROR "PhotoCraft RAW needs cargo (https://rustup.rs; engine/raw-photocraft/rust-toolchain.toml pins the toolchain), or configure with -DSPEKTRALAB_PHOTOCRAFT_RAW=OFF")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The Rust target that matches the C++ toolchain. A static library needs no
# linker, so the Windows targets cross-build from Linux with rustup's target
# alone.
set(photocraft_cargo_target "")
set(photocraft_library_name "libspk_photocraft_raw.a")
if(WIN32 AND MSVC)
    set(photocraft_cargo_target "x86_64-pc-windows-msvc")
    set(photocraft_library_name "spk_photocraft_raw.lib")
elseif(WIN32)
    set(photocraft_cargo_target "x86_64-pc-windows-gnu")
elseif(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "PhotoCraft RAW: no Rust target is set up for cross-compiling to ${CMAKE_SYSTEM_NAME}; configure with -DSPEKTRALAB_PHOTOCRAFT_RAW=OFF")
endif()
set(photocraft_target_dir "${CMAKE_CURRENT_BINARY_DIR}/photocraft-raw")
set(photocraft_target_args "")
set(photocraft_output_dir "${photocraft_target_dir}/release")
if(photocraft_cargo_target)
    set(photocraft_target_args --target "${photocraft_cargo_target}")
    set(photocraft_output_dir "${photocraft_target_dir}/${photocraft_cargo_target}/release")
endif()
set(photocraft_library "${photocraft_output_dir}/${photocraft_library_name}")
set(photocraft_notice_dir "${CMAKE_CURRENT_BINARY_DIR}/third-party/photocraft-raw")
set(photocraft_notices "${photocraft_notice_dir}/Rust-crates-NOTICES.txt")

file(GLOB_RECURSE photocraft_sources CONFIGURE_DEPENDS
    "${photocraft_crate}/src/*.rs" "${photocraft_stage}/src/*.rs")
# cargo is started in the crate directory so rustup applies its
# rust-toolchain.toml. `cargo rustc -- --print native-static-libs` builds the
# library exactly as `cargo build` would and has rustc name the system
# libraries a C or C++ link needs; the list below is checked against that.
add_custom_command(
    OUTPUT "${photocraft_library}" "${photocraft_notices}"
    COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${photocraft_target_dir}"
            "${SPEKTRALAB_CARGO}" rustc --locked --release --lib ${photocraft_target_args}
            -- --print native-static-libs
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${photocraft_notice_dir}"
    COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${photocraft_target_dir}"
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../tools/rust_crate_notices.py"
            --cargo "${SPEKTRALAB_CARGO}" --crate "${photocraft_crate}" --target "${photocraft_cargo_target}"
            --output "${photocraft_notices}"
    WORKING_DIRECTORY "${photocraft_crate}"
    DEPENDS ${photocraft_sources} "${photocraft_crate}/Cargo.toml" "${photocraft_crate}/Cargo.lock"
            "${photocraft_crate}/rust-toolchain.toml" "${photocraft_stage}/Cargo.toml" "${photocraft_stage}/.spk-pin"
            "${CMAKE_CURRENT_LIST_DIR}/../tools/rust_crate_notices.py"
    COMMENT "PhotoCraft RAW (cargo, ${photocraft_commit})"
    VERBATIM USES_TERMINAL)
add_custom_target(spk_photocraft_raw_build DEPENDS "${photocraft_library}" "${photocraft_notices}")

add_library(spk_photocraft_raw STATIC IMPORTED GLOBAL)
set_target_properties(spk_photocraft_raw PROPERTIES IMPORTED_LOCATION "${photocraft_library}")
add_dependencies(spk_photocraft_raw spk_photocraft_raw_build)
# What Rust's standard library needs from the system.
if(WIN32)
    target_link_libraries(spk_photocraft_raw INTERFACE ws2_32 userenv ntdll advapi32 bcrypt)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(spk_photocraft_raw INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# PhotoCraft's own licence texts, staged beside its source by the setup script.
file(MAKE_DIRECTORY "${photocraft_notice_dir}")
configure_file("${photocraft_stage}/LICENSE-MIT" "${photocraft_notice_dir}/PhotoCraft-LICENSE-MIT.txt" COPYONLY)
configure_file("${photocraft_stage}/LICENSE-APACHE" "${photocraft_notice_dir}/PhotoCraft-LICENSE-APACHE.txt" COPYONLY)
configure_file("${photocraft_stage}/NOTICE" "${photocraft_notice_dir}/PhotoCraft-NOTICE.txt" COPYONLY)

set(SPEKTRALAB_PHOTOCRAFT_CRATE "${photocraft_crate}" CACHE INTERNAL "")
set(SPEKTRALAB_PHOTOCRAFT_TARGET_DIR "${photocraft_target_dir}" CACHE INTERNAL "")
set(SPEKTRALAB_PHOTOCRAFT_PIN "${photocraft_third}/PIN" CACHE INTERNAL "")
