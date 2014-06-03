/******************************************************************************\
 *           ___        __                                                    *
 *          /\_ \    __/\ \                                                   *
 *          \//\ \  /\_\ \ \____    ___   _____   _____      __               *
 *            \ \ \ \/\ \ \ '__`\  /'___\/\ '__`\/\ '__`\  /'__`\             *
 *             \_\ \_\ \ \ \ \L\ \/\ \__/\ \ \L\ \ \ \L\ \/\ \L\.\_           *
 *             /\____\\ \_\ \_,__/\ \____\\ \ ,__/\ \ ,__/\ \__/.\_\          *
 *             \/____/ \/_/\/___/  \/____/ \ \ \/  \ \ \/  \/__/\/_/          *
 *                                          \ \_\   \ \_\                     *
 *                                           \/_/    \/_/                     *
 *                                                                            *
 * Copyright (C) 2011 - 2014                                                  *
 * Dominik Charousset <dominik.charousset (at) haw-hamburg.de>                *
 *                                                                            *
 * Distributed under the Boost Software License, Version 1.0. See             *
 * accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt  *
\******************************************************************************/


#ifndef CPPA_UTIL_ALGORITHM_HPP
#define CPPA_UTIL_ALGORITHM_HPP

#include <cmath>       // fabs
#include <string>
#include <vector>
#include <limits>
#include <sstream>
#include <algorithm>
#include <type_traits>

#include "cppa/util/type_traits.hpp"

namespace cppa {
namespace util {

std::vector<std::string> split(const std::string& str,
                               char delim = ' ',
                               bool keep_empties = true);

template<typename Iterator>
typename std::enable_if<is_forward_iterator<Iterator>::value,std::string>::type
join(Iterator begin, Iterator end, const std::string& glue = "") {
    bool first = true;
    std::ostringstream oss;
    for ( ; begin != end; ++begin) {
        if (first) first = false;
        else oss << glue;
        oss << *begin;
    }
    return oss.str();
}

template<typename Container>
typename std::enable_if<is_iterable<Container>::value,std::string>::type
join(const Container& c, const std::string& glue = "") {
    return join(c.begin(), c.end(), glue);
}

// end of recursion
inline void splice(std::string&, const std::string&) { }

template<typename T, typename... Ts>
void splice(std::string& str, const std::string& glue, T&& arg, Ts&&... args) {
    str += glue;
    str += std::forward<T>(arg);
    splice(str, glue, std::forward<Ts>(args)...);
}


/**
 * @brief Compares two values by using @p operator== unless two floating
 *        point numbers are compared. In the latter case, the function
 *        performs an epsilon comparison.
 */
template<typename T, typename U>
typename std::enable_if<
    !std::is_floating_point<T>::value && !std::is_floating_point<U>::value,
    bool
>::type
safe_equal(const T& lhs, const U& rhs) {
    return lhs == rhs;
}

template<typename T, typename U>
typename std::enable_if<
    std::is_floating_point<T>::value || std::is_floating_point<U>::value,
    bool
>::type
safe_equal(const T& lhs, const U& rhs) {
    typedef decltype(lhs - rhs) res_type;
    return std::fabs(lhs - rhs) <= std::numeric_limits<res_type>::epsilon();
}

} // namespace util
} // namespace cppa

#endif // CPPA_UTIL_ALGORITHM_HPP
