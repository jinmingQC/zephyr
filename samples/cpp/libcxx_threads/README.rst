.. SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
.. SPDX-License-Identifier: Apache-2.0

libc++ threads
##############

Four C++17 threads wait on a condition variable, increment a shared counter
under a mutex, and join. A successful run prints::

   libc++ threads: 4000 increments

See `../../../lib/cpp/libcxx/README.rst`_ for source acquisition, compiler
selection, resource limits, tests, and the scope of this proof of concept.
