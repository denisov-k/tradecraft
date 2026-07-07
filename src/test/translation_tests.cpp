// Copyright (c) 2023 The Bitcoin Core developers
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

#include <tinyformat.h>
#include <util/translation.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(translation_tests)

static TranslateFn translate{[](const char * str) {  return strprintf("t(%s)", str);  }};

// Custom translation function _t(), similar to _() but internal to this test.
consteval auto _t(util::TranslatedLiteral str)
{
    str.translate_fn = &translate;
    return str;
}

BOOST_AUTO_TEST_CASE(translation_namedparams)
{
    bilingual_str arg{"original", "translated"};
    bilingual_str result{strprintf(_t("original [%s]"), arg)};
    BOOST_CHECK_EQUAL(result.original, "original [original]");
    BOOST_CHECK_EQUAL(result.translated, "t(original [translated])");

    util::TranslatedLiteral arg2{"original", &translate};
    bilingual_str result2{strprintf(_t("original [%s]"), arg2)};
    BOOST_CHECK_EQUAL(result2.original, "original [original]");
    BOOST_CHECK_EQUAL(result2.translated, "t(original [t(original)])");
}

BOOST_AUTO_TEST_SUITE_END()
