// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2011-2024 The Freicoin Developers
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of version 3 of the GNU Affero General Public License as published
// by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public License for more
// details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <util/check.h>

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <clientversion.h>
#include <tinyformat.h>

#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <string>
#include <string_view>

std::string StrFormatInternalBug(std::string_view msg, const std::source_location& loc)
{
    return strprintf("Internal bug detected: %s\n%s:%d (%s)\n"
                     "%s %s\n"
                     "Please report this issue here: %s\n",
                     msg, loc.file_name(), loc.line(), loc.function_name(),
                     CLIENT_NAME, FormatFullVersion(), CLIENT_BUGREPORT);
}

NonFatalCheckError::NonFatalCheckError(std::string_view msg, const std::source_location& loc)
    : std::runtime_error{StrFormatInternalBug(msg, loc)}
{
}

bool g_detail_test_only_CheckFailuresAreExceptionsNotAborts{false};

void assertion_fail(const std::source_location& loc, std::string_view assertion)
{
    if (g_detail_test_only_CheckFailuresAreExceptionsNotAborts) {
        throw NonFatalCheckError{assertion, loc};
    }
    auto str = strprintf("%s:%s %s: Assertion `%s' failed.\n", loc.file_name(), loc.line(), loc.function_name(), assertion);
    fwrite(str.data(), 1, str.size(), stderr);
    std::abort();
}

std::atomic<bool> g_enable_dynamic_fuzz_determinism{false};
