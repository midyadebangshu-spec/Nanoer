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
                 "state:           clean"
                 "slot 0: valid, generation 2" "slot 1: valid, generation 1")
  string(FIND "${OUTPUT}" "${expected}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "nanoer info output lacks '${expected}':\n${OUTPUT}")
  endif()
endforeach()

# fsck: a new filesystem is clean, under both names; damage is found and
# repaired, and the exit codes follow e2fsck.
function(run_rc expected_rc)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL expected_rc)
    message(FATAL_ERROR "expected exit code ${expected_rc}, got ${rc}: ${ARGN}\n${out}${err}")
  endif()
  set(OUTPUT "${out}${err}" PARENT_SCOPE)
endfunction()
run_rc(0 "${NANOER}" fsck --deep "${IMG}")
string(FIND "${OUTPUT}" ": clean" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "nanoer fsck did not report a clean filesystem:\n${OUTPUT}")
endif()
run_rc(0 "${FSCK}" -a -C 0 "${IMG}")  # as fsck(8) runs it at boot
run_rc(0 "${NANOER}" fsck --help)
run_rc(8 "${NANOER}" fsck --bogus "${IMG}")
run_rc(8 "${NANOER}" fsck "${WORKDIR}/missing.img")
# Spoil superblock slot 1 (at 4 KiB): a check reports it, a repair rewrites it.
execute_process(COMMAND python3 -c
  "import sys; f=open(sys.argv[1],'r+b'); f.seek(4200); b=f.read(1); f.seek(4200); f.write(bytes([b[0]^0xff]))"
  "${IMG}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "could not damage the image")
endif()
run_rc(0 "${NANOER}" fsck "${IMG}")  # a spoiled spare slot is only a note
string(FIND "${OUTPUT}" "slot 1 is not valid" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "nanoer fsck did not notice the damaged superblock:\n${OUTPUT}")
endif()
run_rc(0 "${NANOER}" fsck --repair "${IMG}")
run_rc(0 "${NANOER}" info "${IMG}")
string(FIND "${OUTPUT}" "slot 1: valid" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "fsck --repair did not rewrite the damaged superblock:\n${OUTPUT}")
endif()

# gc: compacts an unmounted filesystem (nothing to do on a new one), and the
# result checks clean.
run(TRUE "${NANOER}" gc "${IMG}")
string(FIND "${OUTPUT}" "segments emptied" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "nanoer gc did not report:\n${OUTPUT}")
endif()
run(FALSE "${NANOER}" gc --max-full 200 "${IMG}")
run_rc(0 "${NANOER}" fsck --deep "${IMG}")

# --no-dedup is recorded as a preference.
run(TRUE "${MKFS}" --size 512M --no-dedup "${WORKDIR}/nodedup.img")
run(TRUE "${NANOER}" info "${WORKDIR}/nodedup.img")
string(FIND "${OUTPUT}" "compat no-dedup" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "nanoer info does not show the no-dedup preference:\n${OUTPUT}")
endif()

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
