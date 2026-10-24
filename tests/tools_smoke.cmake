# End-to-end check of mkfs.nanoer and nanoer. Run by ctest.
file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
set(IMG "${WORKDIR}/test.img")

function(run expect_success)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(expect_success AND NOT rc EQUAL 0)
    message(FATAL_ERROR "failed (${rc}): ${ARGN}\n${out}${err}")
  elseif(NOT expect_success AND rc EQUAL 0)
    message(FATAL_ERROR "should have failed: ${ARGN}\n${out}${err}")
  endif()
  set(OUTPUT "${out}${err}" PARENT_SCOPE)
endfunction()

run(TRUE "${MKFS}" --size 512M --label smoke --chunk-size 64K --codec lz4 --level 0 "${IMG}")
run(TRUE "${NANOER}" info "${IMG}")
foreach(expected "label:           smoke" "chunk size:      64.0 KiB" "new data:        lz4 level 0"
                 "slot 0: valid, generation 2" "slot 1: valid, generation 1")
  string(FIND "${OUTPUT}" "${expected}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "nanoer info output lacks '${expected}':\n${OUTPUT}")
  endif()
endforeach()

# Refuses to overwrite a filesystem without --force, and to clobber an existing file.
run(FALSE "${MKFS}" "${IMG}")
run(FALSE "${MKFS}" --size 512M "${IMG}")
run(TRUE "${MKFS}" --force "${IMG}")

# Bad options and a too-small image fail cleanly, leaving nothing behind.
run(FALSE "${MKFS}" --size 512M --chunk-size 100K "${WORKDIR}/bad.img")
run(FALSE "${MKFS}" --size 1M "${WORKDIR}/small.img")
if(EXISTS "${WORKDIR}/small.img")
  message(FATAL_ERROR "a failed mkfs left an image behind")
endif()

run(TRUE "${NANOER}" bench --max-size 1M "${CMAKE_CURRENT_LIST_DIR}")
run(FALSE "${NANOER}" info "${WORKDIR}/missing.img")
