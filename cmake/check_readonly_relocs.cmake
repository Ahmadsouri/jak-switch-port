# Fails the link when the freshly linked module contains dynamic relocations that
# target a read-only LOAD segment.
#
# libnx's __nx_dynamic() applies R_AARCH64_RELATIVE entries exactly as the ELF
# describes them, without ever making the destination writable.  A provably
# constant static that the compiler decides to emit into .rodata therefore makes
# the NRO take a Data Abort (store) inside __nx_dynamic, before main() runs.
# Catching that here beats pushing a 15 MB binary to an SD card to find out.
#
# Usage: cmake -DREADELF=<path> -DELF=<path> -P check_readonly_relocs.cmake

if(NOT EXISTS "${READELF}")
    message(FATAL_ERROR "check_readonly_relocs: readelf not found: ${READELF}")
endif()
if(NOT EXISTS "${ELF}")
    message(FATAL_ERROR "check_readonly_relocs: ELF not found: ${ELF}")
endif()

# readelf's output goes to files rather than variables: CMake's regex engine is
# not multiline, and file(STRINGS) hands back one line per element.
set(work_dir "${CMAKE_BINARY_DIR}/check_readonly_relocs")
file(MAKE_DIRECTORY "${work_dir}")
execute_process(
    COMMAND "${READELF}" -lW "${ELF}"
    OUTPUT_FILE "${work_dir}/program_headers.txt"
    ERROR_VARIABLE phdr_error
    RESULT_VARIABLE phdr_result)
execute_process(
    COMMAND "${READELF}" -rW "${ELF}"
    OUTPUT_FILE "${work_dir}/relocations.txt"
    ERROR_VARIABLE reloc_error
    RESULT_VARIABLE reloc_result)
if(NOT phdr_result EQUAL 0 OR NOT reloc_result EQUAL 0)
    message(FATAL_ERROR "check_readonly_relocs: readelf failed\n${phdr_error}${reloc_error}")
endif()

# LOAD columns: Offset VirtAddr PhysAddr FileSiz MemSiz Flg Align
file(STRINGS "${work_dir}/program_headers.txt" load_lines REGEX "^[ \t]+LOAD[ \t]")
set(segments "")
foreach(load_line IN LISTS load_lines)
    string(REGEX MATCH
        "^([ \t]+)LOAD[ \t]+0x[0-9a-f]+[ \t]+0x([0-9a-f]+)[ \t]+0x[0-9a-f]+[ \t]+0x([0-9a-f]+)[ \t]+0x([0-9a-f]+)[ \t]+([RWE ]+)"
        segment_fields
        "${load_line}")
    if(NOT segment_fields)
        message(FATAL_ERROR "check_readonly_relocs: cannot parse LOAD line: ${load_line}")
    endif()
    math(EXPR segment_vaddr "0x${CMAKE_MATCH_2}")
    math(EXPR segment_filesz "0x${CMAKE_MATCH_4}")
    set(segment_flags "${CMAKE_MATCH_5}")
    string(FIND "${segment_flags}" "W" segment_writable_index)
    math(EXPR segment_end "${segment_vaddr} + ${segment_filesz}")
    list(APPEND segments "${segment_vaddr}:${segment_end}:${segment_writable_index}")
endforeach()

if(NOT segments)
    message(FATAL_ERROR "check_readonly_relocs: no LOAD segments found in ${ELF}")
endif()

# Relocation columns: Offset Info Type Sym.Value Sym.Name+Addend
file(STRINGS "${work_dir}/relocations.txt" reloc_lines REGEX "^[0-9a-f]+[ \t]+[0-9a-f]+[ \t]+R_")
set(offenders "")
set(checked 0)
foreach(reloc_line IN LISTS reloc_lines)
    string(REGEX MATCH
        "^([0-9a-f]+)[ \t]+([0-9a-f]+)[ \t]+([A-Za-z0-9_]+)"
        reloc_fields
        "${reloc_line}")
    if(NOT reloc_fields)
        continue()
    endif()
    if(CMAKE_MATCH_3 STREQUAL "R_AARCH64_NONE")
        continue()
    endif()
    math(EXPR reloc_offset "0x${CMAKE_MATCH_1}")
    math(EXPR checked "${checked} + 1")
    foreach(segment IN LISTS segments)
        string(REPLACE ":" ";" segment_parts "${segment}")
        list(GET segment_parts 0 segment_vaddr)
        list(GET segment_parts 1 segment_end)
        list(GET segment_parts 2 segment_writable_index)
        if(reloc_offset GREATER_EQUAL segment_vaddr AND reloc_offset LESS segment_end)
            if(segment_writable_index EQUAL -1)
                list(APPEND offenders "${reloc_offset} ${CMAKE_MATCH_3}")
            endif()
            break()
        endif()
    endforeach()
endforeach()

file(REMOVE_RECURSE "${work_dir}")

if(offenders)
    list(LENGTH offenders offender_count)
    set(shown_count 20)
    if(offender_count LESS shown_count)
        set(shown_count ${offender_count})
    endif()
    list(SUBLIST offenders 0 ${shown_count} offenders_shown)
    string(JOIN "\n  " offender_list ${offenders_shown})
    if(offender_count GREATER shown_count)
        math(EXPR remaining "${offender_count} - ${shown_count}")
        set(offender_list "${offender_list}\n  ... and ${remaining} more")
    endif()
    message(FATAL_ERROR
        "check_readonly_relocs: ${offender_count} of ${checked} relocations target a "
        "read-only LOAD segment. libnx's __nx_dynamic() will Data Abort on the first "
        "one, before main() runs. Move those objects out of read-only sections "
        "(e.g. __attribute__((section(\".data\"))) on the variables the relocations "
        "refer to).\n  ${offender_list}")
endif()

message(STATUS "check_readonly_relocs: ${checked} relocations, none in read-only segments")