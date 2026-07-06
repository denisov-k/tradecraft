# Unit tests

The sources in this directory are unit test cases. Boost includes a
unit testing framework, and since Freicoin already uses Boost, it makes
sense to simply use this framework rather than require developers to
configure some other framework (we want as few impediments to creating
unit tests as possible).

The build system is set up to compile an executable called `test_freicoin`
that runs all of the unit tests. The main source file for the test library is found in
`util/setup_common.cpp`.

The examples in this document assume the build directory is named
`build`. You'll need to adapt them if you named it differently.

### Compiling/running unit tests

Unit tests will be automatically compiled if dependencies were met
during the generation of the Bitcoin Core build system
and tests weren't explicitly disabled.

The unit tests can be run with `ctest --test-dir build`, which includes unit
tests from subtrees.

<<<<<<< v29.0
Run `test_bitcoin --list_content` for the full list of tests.

To run the unit tests manually, launch `build/bin/test_bitcoin`. To recompile
after a test file was modified, run `cmake --build build` and then run the test again. If you
modify a non-test file, use `cmake --build build --target test_bitcoin` to recompile only what's needed
=======
To run the unit tests manually, launch `src/test/test_freicoin`. To recompile
after a test file was modified, run `make` and then run the test again. If you
modify a non-test file, use `make -C src/test` to recompile only what's needed
>>>>>>> tc-28.1
to run the unit tests.

To add more unit tests, add `BOOST_AUTO_TEST_CASE` functions to the existing
.cpp files in the `test/` directory or add new .cpp files that
implement new `BOOST_AUTO_TEST_SUITE` sections.

<<<<<<< v29.0
To run the GUI unit tests manually, launch `build/bin/test_bitcoin-qt`
=======
To run the GUI unit tests manually, launch `src/qt/test/test_freicoin-qt`
>>>>>>> tc-28.1

To add more GUI unit tests, add them to the `src/qt/test/` directory and
the `src/qt/test/test_main.cpp` file.

### Running individual tests

<<<<<<< v29.0
The `test_bitcoin` runner accepts command line arguments from the Boost
framework. To see the list of arguments that may be passed, run:

```
test_bitcoin --help
=======
`test_freicoin` accepts the command line arguments from the boost framework.
For example, to run just the `getarg_tests` suite of tests:

```bash
test_freicoin --log_level=all --run_test=getarg_tests
>>>>>>> tc-28.1
```

For example, to run only the tests in the `getarg_tests` file, with full logging:

<<<<<<< v29.0
```bash
build/bin/test_bitcoin --log_level=all --run_test=getarg_tests
```

or

```bash
build/bin/test_bitcoin -l all -t getarg_tests
```

or to run only the doubledash test in `getarg_tests`

```bash
build/bin/test_bitcoin --run_test=getarg_tests/doubledash
```

The `--log_level=` (or `-l`) argument controls the verbosity of the test output.

The `test_bitcoin` runner also accepts some of the command line arguments accepted by
`bitcoind`. Use `--` to separate these sets of arguments:

```bash
build/bin/test_bitcoin --log_level=all --run_test=getarg_tests -- -printtoconsole=1
=======
`test_freicoin` also accepts some of the command line arguments accepted by
`freicoind`. Use `--` to separate these sets of arguments:

```bash
test_freicoin --log_level=all --run_test=getarg_tests -- -printtoconsole=1
>>>>>>> tc-28.1
```

The `-printtoconsole=1` after the two dashes sends debug logging, which
normally goes only to `debug.log` within the data directory, to the
standard terminal output as well.

<<<<<<< v29.0
Running `test_bitcoin` creates a temporary working (data) directory with a randomly
generated pathname within `test_common bitcoin/`, which in turn is within
=======
... or to run just the doubledash test:

```bash
test_freicoin --run_test=getarg_tests/doubledash
```

`test_freicoin` creates a temporary working (data) directory with a randomly
generated pathname within `test_common_Freicoin/`, which in turn is within
>>>>>>> tc-28.1
the system's temporary directory (see
[`temp_directory_path`](https://en.cppreference.com/w/cpp/filesystem/temp_directory_path)).
This data directory looks like a simplified form of the standard `freicoind` data
directory. Its content will vary depending on the test, but it will always
have a `debug.log` file, for example.

The location of the temporary data directory can be specified with the
`-testdatadir` option. This can make debugging easier. The directory
path used is the argument path appended with
<<<<<<< v29.0
`/test_common bitcoin/<test-name>/datadir`.
=======
`/test_common_Freicoin/<test-name>/datadir`.
>>>>>>> tc-28.1
The directory path is created if necessary.
Specifying this argument also causes the data directory
not to be removed after the last test. This is useful for looking at
what the test wrote to `debug.log` after it completes, for example.
(The directory is removed at the start of the next test run,
so no leftover state is used.)

```bash
<<<<<<< v29.0
$ build/bin/test_bitcoin --run_test=getarg_tests/doubledash -- -testdatadir=/somewhere/mydatadir
Test directory (will not be deleted): "/somewhere/mydatadir/test_common bitcoin/getarg_tests/doubledash/datadir"
Running 1 test case...

*** No errors detected
$ ls -l '/somewhere/mydatadir/test_common bitcoin/getarg_tests/doubledash/datadir'
=======
$ test_freicoin --run_test=getarg_tests/doubledash -- -testdatadir=/somewhere/mydatadir
Test directory (will not be deleted): "/somewhere/mydatadir/test_common_Freicoin/getarg_tests/doubledash/datadir
Running 1 test case...

*** No errors detected
$ ls -l '/somewhere/mydatadir/test_common_Freicoin/getarg_tests/doubledash/datadir'
>>>>>>> tc-28.1
total 8
drwxrwxr-x 2 admin admin 4096 Nov 27 22:45 blocks
-rw-rw-r-- 1 admin admin 1003 Nov 27 22:45 debug.log
```

If you run an entire test suite, such as `--run_test=getarg_tests`, or all the test suites
(by not specifying `--run_test`), a separate directory
will be created for each individual test.

<<<<<<< v29.0
=======
Run `test_freicoin --help` for the full list of tests.

>>>>>>> tc-28.1
### Adding test cases

To add a new unit test file to our test suite, you need
to add the file to either `src/test/CMakeLists.txt` or
`src/wallet/test/CMakeLists.txt` for wallet-related tests. The pattern is to create
one test file for each class or source file for which you want to create
unit tests. The file naming convention is `<source_filename>_tests.cpp`
and such files should wrap their tests in a test suite
called `<source_filename>_tests`. For an example of this pattern,
see `uint256_tests.cpp`.

### Logging and debugging in unit tests

`ctest --test-dir build` will write to the log file `build/Testing/Temporary/LastTest.log`. You can
additionally use the `--output-on-failure` option to display logs of the failed tests automatically
on failure. For running individual tests verbosely, refer to the section
[above](#running-individual-tests).

To write to logs from unit tests you need to use specific message methods
provided by Boost. The simplest is `BOOST_TEST_MESSAGE`.

For debugging you can launch the `test_freicoin` executable with `gdb` or `lldb` and
start debugging, just like you would with any other program:

```bash
<<<<<<< v29.0
gdb build/bin/test_bitcoin
=======
gdb src/test/test_freicoin
>>>>>>> tc-28.1
```

#### Segmentation faults

If you hit a segmentation fault during a test run, you can diagnose where the fault
<<<<<<< v29.0
is happening by running `gdb ./build/bin/test_bitcoin` and then using the `bt` command
=======
is happening by running `gdb ./src/test/test_freicoin` and then using the `bt` command
>>>>>>> tc-28.1
within gdb.

Another tool that can be used to resolve segmentation faults is
[valgrind](https://valgrind.org/).

If for whatever reason you want to produce a core dump file for this fault, you can do
that as well. By default, the boost test runner will intercept system errors and not
produce a core file. To bypass this, add `--catch_system_errors=no` to the
`test_freicoin` arguments and ensure that your ulimits are set properly (e.g. `ulimit -c
unlimited`).

Running the tests and hitting a segmentation fault should now produce a file called `core`
(on Linux platforms, the file name will likely depend on the contents of
`/proc/sys/kernel/core_pattern`).

You can then explore the core dump using
```bash
<<<<<<< v29.0
gdb build/bin/test_bitcoin core
=======
gdb src/test/test_freicoin
>>>>>>> tc-28.1

(gdb) bt  # produce a backtrace for where a segfault occurred
```
