# Verify that every code section is mapped inside an executable LOAD segment.
#
# Companion to check_readonly_relocs.cmake, which catches the write side of the
# same class of mistake.  This one catches the execute side: code that the
# linker placed in a segment without the E flag.  On the Switch that is an
# immediate "Instruction Abort" the first time the address is branched to, and
# the faulting PC is the code symbol itself, so the report looks like a mystery.
#
# libco hit this on aarch64: settings.h puts its context-switch trampoline in a
# `.text#` orphan section, and ld groups orphan `.text*` sections into the
# read-only LOAD segment that sits next to .rodata.
#
# Usage: cmake -DREADELF=<path> -DELF=<path> -P check_exec_sections.cmake

if(NOT EXISTS "${READELF}")
    message(FATAL_ERROR "check_exec_sections: readelf not found: ${READELF}")
endif()
if(NOT EXISTS "${ELF}")
    message(FATAL_ERROR "check_exec_sections: ELF not found: ${ELF}")
endif()

set(work_dir "${CMAKE_BINARY_DIR}/check_exec_sections")
file(MAKE_DIRECTORY "${work_dir}")
execute_process(
    COMMAND "${READELF}" -lW "${ELF}"
    OUTPUT_FILE "${work_dir}/program_headers.txt"
    ERROR_VARIABLE phdr_error
    RESULT_VARIABLE phdr_result)
execute_process(
    COMMAND "${READELF}" -SW "${ELF}"
    OUTPUT_FILE "${work_dir}/sections.txt"
    ERROR_VARIABLE sec_error
    RESULT_VARIABLE sec_result)
if(NOT phdr_result EQUAL 0 OR NOT sec_result EQUAL 0)
    message(FATAL_ERROR "check_exec_sections: readelf failed\n${phdr_error}${sec_error}")
endif()

# LOAD columns: Offset VirtAddr PhysAddr FileSiz MemSiz Flg Align
file(STRINGS "${work_dir}/program_headers.txt" load_lines REGEX "^[ \t]+LOAD[ \t]")
set(exec_ranges "")
foreach(load_line IN LISTS load_lines)
    string(REGEX MATCH
        "^[ \t]+LOAD[ \t]+0x[0-9a-f]+[ \t]+0x([0-9a-f]+)[ \t]+0x[0-9a-f]+[ \t]+0x([0-9a-f]+)[ \t]+0x[0-9a-f]+[ \t]+([RWE ]+)"
        segment_fields
        "${load_line}")
    if(NOT segment_fields)
        message(FATAL_ERROR "check_exec_sections: cannot parse LOAD line: ${load_line}")
    endif()
    math(EXPR segment_vaddr "0x${CMAKE_MATCH_1}")
    math(EXPR segment_filesz "0x${CMAKE_MATCH_2}")
    math(EXPR segment_end "${segment_vaddr} + ${segment_filesz}")
    string(FIND "${CMAKE_MATCH_3}" "E" segment_exec_index)
    if(NOT segment_exec_index EQUAL -1)
        list(APPEND exec_ranges "${segment_vaddr}:${segment_end}")
    endif()
endforeach()

if(NOT exec_ranges)
    message(FATAL_ERROR "check_exec_sections: no executable LOAD segment found in ${ELF}")
endif()

# Section columns: [Nr] Name Type Address Off Size ES Flg Lk Inf Al
# readelf prints Address/Off/Size as bare hex without an 0x prefix, and the
# first column is "[ 12]" / "[100]" with variable padding.
file(STRINGS "${work_dir}/sections.txt" section_lines REGEX "^[ \t]*\\[[ 0-9]+\\]")
# Any .text* variant counts as code, including orphans such as libco's `.text#`
# (settings.h appends '#' so the GNU assembler does not warn about the section
# attribute).  Those are exactly the ones that get dumped into the read-only
# segment, so they must not be filtered out by a stricter pattern.
set(code_pattern "^\\.text[^ ]*$|^\\.init$|^\\.fini$|^\\.plt$")
set(offenders "")
set(checked 0)
foreach(section_line IN LISTS section_lines)
    string(REGEX MATCH
        "\\] +([^ ]+) +[^ ]+ +([0-9a-f]+) +[0-9a-f]+ +([0-9a-f]+)"
        section_fields
        "${section_line}")
    if(NOT section_fields)
        message(FATAL_ERROR "check_exec_sections: cannot parse section line: ${section_line}")
    endif()
    set(section_name "${CMAKE_MATCH_1}")
    math(EXPR section_addr "0x${CMAKE_MATCH_2}")
    math(EXPR section_size "0x${CMAKE_MATCH_3}")
    if(section_size EQUAL 0)
        continue()
    endif()
    if(NOT section_name MATCHES "${code_pattern}")
        continue()
    endif()
    math(EXPR section_end "${section_addr} + ${section_size}")
    math(EXPR checked "${checked} + 1")
    set(section_executable FALSE)
    foreach(exec_range IN LISTS exec_ranges)
        string(REPLACE ":" ";" exec_bounds "${exec_range}")
        list(GET exec_bounds 0 exec_start)
        list(GET exec_bounds 1 exec_stop)
        if(section_addr GREATER_EQUAL exec_start AND section_end LESS_EQUAL exec_stop)
            set(section_executable TRUE)
            break()
        endif()
    endforeach()
    if(NOT section_executable)
        list(APPEND offenders
            "  ${section_name} at 0x${section_addr} size 0x${section_size} is not inside any executable LOAD segment")
    endif()
endforeach()

if(checked EQUAL 0)
    # A check that inspects nothing is worse than no check: it would pass forever.
    message(FATAL_ERROR
        "check_exec_sections: found no code sections in ${ELF}; the section parser is "
        "broken or the output format changed.")
endif()

if(offenders)
    list(LENGTH offenders offender_count)
    list(SUBLIST offenders 0 20 offenders_shown)
    string(JOIN "\n" offender_list ${offenders_shown})
    if(offender_count GREATER 20)
        math(EXPR remaining "${offender_count} - 20")
        set(offender_list "${offender_list}\n  ... and ${remaining} more")
    endif()
    message(FATAL_ERROR
        "check_exec_sections: ${offender_count} code sections are not executable. Branching "
        "to one of them raises an Instruction Abort. Place such code in a real .text "
        "subsection (see third-party/libco/aarch64.c) instead of an orphan section.\n"
        "  ${offender_list}")
endif()

message(STATUS "check_exec_sections: ${checked} code sections, all inside executable segments")