# cmake/LintChangelog.cmake: the changelog's entries, as the CTest test lint.changelog
# (cmake -DFP_SOURCE_DIR=<root> -P <this>).
#
# A pull request that changes what a user can see adds docs/changelog/<name>.md and never edits CHANGELOG.md, whose
# Unreleased section every open pull request would otherwise change at the same lines; scripts/changelog.py folds the
# entries in on main (docs/changelog/README.md). This test holds the entries to what `scripts/changelog.py check`
# holds them to, as far as CMake's string functions go, so that a machine with no Python checks them too: the name
# (lower-case letters, digits and hyphens), one `### ` heading, a **Added**, **Fixed** or **Changed** list, no `# ` or
# `## ` heading, no tab; and CHANGELOG.md has its `## Unreleased` heading. `check` also holds the lists' items, the
# extension's line, the columns and the last newline.
if(NOT FP_SOURCE_DIR)
  message(FATAL_ERROR "LintChangelog.cmake: pass -DFP_SOURCE_DIR=<project root>")
endif()
set(_bad "")
file(GLOB_RECURSE _files RELATIVE ${FP_SOURCE_DIR} ${FP_SOURCE_DIR}/docs/changelog/*)
list(SORT _files)
foreach(_rel IN LISTS _files)
  if(_rel STREQUAL "docs/changelog/README.md")
    continue()
  endif()
  if(NOT _rel MATCHES "^docs/changelog/[a-z0-9-]+\\.md$")
    list(APPEND _bad "${_rel}: not an entry's name (docs/changelog/<name>.md, lower-case letters, digits and hyphens)")
    continue()
  endif()
  file(READ ${FP_SOURCE_DIR}/${_rel} _text)
  string(REPLACE "\r" "" _text "${_text}")   # a Windows checkout may hold CR LF: the rules are on the lines alone
  string(REGEX MATCHALL "(^|\n)### " _headings "${_text}")
  list(LENGTH _headings _n)
  if(_n EQUAL 0)
    list(APPEND _bad "${_rel}: no `### ` heading (an entry starts with `### <title>`)")
  elseif(_n GREATER 1)
    list(APPEND _bad "${_rel}: ${_n} `### ` headings (one pull request, one entry)")
  endif()
  if(NOT _text MATCHES "(^|\n)\\*\\*(Added|Fixed|Changed)\\*\\*(\n|$)")
    list(APPEND _bad "${_rel}: no **Added**, **Fixed** or **Changed** list")
  endif()
  if(_text MATCHES "(^|\n)##?( |\n|$)")
    list(APPEND _bad "${_rel}: a `# ` or `## ` heading (an entry has one `### ` heading: fold puts it under a day)")
  endif()
  string(FIND "${_text}" "\t" _tab)
  if(NOT _tab EQUAL -1)
    list(APPEND _bad "${_rel}: a tab (indent with spaces)")
  endif()
endforeach()
if(NOT EXISTS ${FP_SOURCE_DIR}/CHANGELOG.md)
  list(APPEND _bad "CHANGELOG.md: missing")
else()
  file(READ ${FP_SOURCE_DIR}/CHANGELOG.md _log)
  string(REPLACE "\r" "" _log "${_log}")     # CR LF, as above
  if(NOT _log MATCHES "(^|\n)## Unreleased[ \t]*(\n|$)")
    list(APPEND _bad "CHANGELOG.md: no `## Unreleased` heading (the entries are folded in under it)")
  endif()
endif()
if(_bad)
  list(JOIN _bad "\n  " _msg)
  message(FATAL_ERROR "lint.changelog: an entry is docs/changelog/<name>.md, a `### ` heading and its **Added**, "
                      "**Fixed** or **Changed** lists (scripts/changelog.py check says more):\n  ${_msg}")
endif()
