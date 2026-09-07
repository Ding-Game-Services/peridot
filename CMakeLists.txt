cmake_minimum_required(VERSION 3.20)
project(peridot VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Sources ──────────────────────────────────────────────────────────────────
set(PERIDOT_SOURCES
    src/bus.cpp
    src/cpu.cpp
    src/ppu.cpp
    src/apu.cpp
    src/gameboy.cpp
    src/core_api.cpp
)

set(SDK_SOURCES
    sdk/src/ding_audio.cpp
    sdk/src/ding_savestate.cpp
    sdk/src/ding_md5.cpp
)

set(ALL_SOURCES ${PERIDOT_SOURCES} ${SDK_SOURCES})

# ─── Include paths ────────────────────────────────────────────────────────────
set(PERIDOT_INCLUDES
    include
    sdk/include
)

# ─── Emscripten / WASM build ──────────────────────────────────────────────────
if(EMSCRIPTEN)
    # Every function declared in ding_core.h (plus Peridot's one platform
    # extension, ding_gb_set_printer_callback) must appear here so the
    # Emscripten linker doesn't dead-strip it.
    set(EXPORTED_FUNCTIONS
        "_ding_init"
        "_ding_destroy"
        "_ding_reset"
        "_ding_load_rom"
        "_ding_load_disc"
        "_ding_load_bios"
        "_ding_swap_disc"
        "_ding_run_frame"
        "_ding_get_core_info"
        "_ding_get_video_info"
        "_ding_get_audio_info"
        "_ding_get_rom_identity"
        "_ding_get_savestate_info"
        "_ding_get_memory_region_count"
        "_ding_get_memory_region"
        "_ding_get_bios_count"
        "_ding_get_bios_descriptor"
        "_ding_get_input_descriptor_count"
        "_ding_get_input_descriptor"
        "_ding_get_framebuffer"
        "_ding_get_current_dimensions"
        "_ding_get_audio_sample_count"
        "_ding_read_audio_samples"
        "_ding_set_button"
        "_ding_set_axis"
        "_ding_save_state"
        "_ding_load_state"
        "_ding_diag_cpu_state"
        "_ding_diag_video_state"
        "_ding_has_error"
        "_ding_diag_last_error"
        "_ding_is_disc_swap_pending"
        "_ding_set_region"
        "_ding_get_sram"
        "_ding_get_sram_size"
        "_ding_sram_has"
        "_ding_sram_dirty"
        "_ding_sram_clear_dirty"
        "_ding_write8"
        "_ding_gb_set_printer_callback"
        "_ding_gb_set_palette"
        "_ding_gb_reblit"
        "_ding_gb_get_if"
        "_ding_gb_get_ie"
        "_ding_gb_get_mbc_type"
        "_ding_gb_get_ram_bank"
        "_ding_gb_get_rom_bank"
        "_ding_gb_get_rom_banks"
        "_ding_gb_get_ram_en"
        "_ding_gb_get_mbc1_mode"
        "_ding_gb_get_cpu_regs"
        "_ding_gb_get_apu_diag"
        "_malloc"
        "_free"
    )

    # Convert to the JSON-array string Emscripten expects.
    string(JOIN "," EXPORTED_FUNCTIONS_CSV ${EXPORTED_FUNCTIONS})
    set(EXPORTED_FUNCTIONS_STR "[${EXPORTED_FUNCTIONS_CSV}]")

    add_executable(peridot ${ALL_SOURCES})
    target_include_directories(peridot PRIVATE ${PERIDOT_INCLUDES})
    target_compile_options(peridot PRIVATE -O2)
    target_link_options(peridot PRIVATE
        -sWASM=1
        -sALLOW_MEMORY_GROWTH=1
        -sALLOW_TABLE_GROWTH=1
        -sNO_EXIT_RUNTIME=1
        -sMODULARIZE=1
        -sEXPORT_NAME=PeridotCore
        "-sEXPORTED_FUNCTIONS=${EXPORTED_FUNCTIONS_STR}"
        "-sEXPORTED_RUNTIME_METHODS=[ccall,cwrap,getValue,setValue,HEAPU8,HEAPU16,addFunction,removeFunction,UTF8ToString]"
        --no-entry
    )
    # Output: peridot.js + peridot.wasm (Emscripten convention)
    set_target_properties(peridot PROPERTIES SUFFIX ".js")

# ─── Native build (.dll / .so) ────────────────────────────────────────────────
else()
    add_library(peridot SHARED ${ALL_SOURCES})
    target_include_directories(peridot PRIVATE ${PERIDOT_INCLUDES})

    if(MSVC)
        # /O2 and /RTC1 (added by CMake's Debug config) are mutually exclusive.
        # Use a generator expression so /O2 only fires in Release/RelWithDebInfo.
        target_compile_options(peridot PRIVATE
            /W3
            $<$<NOT:$<CONFIG:Debug>>:/O2>
        )
        # Export all extern "C" symbols automatically on Windows
        set_target_properties(peridot PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    else()
        target_compile_options(peridot PRIVATE -Wall -Wextra
            $<$<NOT:$<CONFIG:Debug>>:-O2>
        )
    endif()

    # ── Headless test harness (native only) ───────────────────────────────────
    # harness/main.cpp is optional — the target is declared here so the build
    # definition is complete whenever one gets added, same convention as
    # genesis-core.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/harness/main.cpp")
        add_executable(peridot-harness
            harness/main.cpp
            ${PERIDOT_SOURCES}   # statically compiled into harness — no .so dependency
            ${SDK_SOURCES}
        )
        target_include_directories(peridot-harness PRIVATE ${PERIDOT_INCLUDES})
        if(MSVC)
            target_compile_options(peridot-harness PRIVATE /W3
                $<$<NOT:$<CONFIG:Debug>>:/O2>)
        else()
            target_compile_options(peridot-harness PRIVATE -Wall -Wextra
                $<$<NOT:$<CONFIG:Debug>>:-O2>)
        endif()
    endif()
endif()
