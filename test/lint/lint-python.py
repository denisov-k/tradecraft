#!/usr/bin/env python3
#
# Copyright (c) 2022 The Bitcoin Core developers
# Copyright (c) 2010-2024 The Freicoin Developers
#
# This program is free software: you can redistribute it and/or modify it under
# the terms of version 3 of the GNU Affero General Public License as published
# by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public License for more
# details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

"""
Check for specified mypy warnings in python files.
"""

import os
from pathlib import Path
import subprocess

from importlib.metadata import metadata, PackageNotFoundError

# Customize mypy cache dir via environment variable
cache_dir = Path(__file__).parent.parent / ".mypy_cache"
os.environ["MYPY_CACHE_DIR"] = str(cache_dir)

DEPS = ['lief', 'mypy', 'pyzmq']

# Only .py files in test/functional and contrib/devtools have type annotations
# enforced.
MYPY_FILES_ARGS = ['git', 'ls-files', 'test/functional/*.py', 'contrib/devtools/*.py']


def check_dependencies():
    for dep in DEPS:
        try:
            metadata(dep)
        except PackageNotFoundError:
            print(f"Skipping Python linting since {dep} is not installed.")
            exit(0)


def main():
    check_dependencies()

    mypy_files = subprocess.check_output(MYPY_FILES_ARGS).decode("utf-8").splitlines()
    mypy_args = ['mypy', '--show-error-codes'] + mypy_files

    try:
        subprocess.check_call(mypy_args)
    except subprocess.CalledProcessError:
        exit(1)


if __name__ == "__main__":
    main()
