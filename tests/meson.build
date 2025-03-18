# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is part of libcxlmi.
#

cunit_dep = dependency('cunit', required: true)

executable(
    'api-simple-tests',
    ['api-simple-tests.c'],
    dependencies: libcxlmi_dep,
    include_directories: [inc]
)

executable(
    'unit-tests',
    ['unit_tests.c'],
    dependencies: [libcxlmi_dep, cunit_dep],
    include_directories: [inc],
)

