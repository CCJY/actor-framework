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


#include <utility>

#include "cppa/local_actor.hpp"
#include "cppa/response_promise.hpp"

#include "cppa/detail/raw_access.hpp"

using std::move;

namespace cppa {

response_promise::response_promise(const actor_addr& from,
                                   const actor_addr& to,
                                   const message_id& id)
: m_from(from), m_to(to), m_id(id) {
    CPPA_REQUIRE(id.is_response() || !id.valid());
}

void response_promise::deliver(any_tuple msg) {
    if (m_to) {
        auto to = detail::raw_access::get(m_to);
        auto from = detail::raw_access::get(m_from);
        to->enqueue({m_from, to, m_id}, move(msg), from->m_host);
        m_to = invalid_actor_addr;
    }
}

} // namespace cppa
