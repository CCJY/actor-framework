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


#ifndef CPPA_DETAIL_BEHAVIOR_STACK_HPP
#define CPPA_DETAIL_BEHAVIOR_STACK_HPP

#include <vector>
#include <memory>
#include <utility>
#include <algorithm>

#include "cppa/optional.hpp"
#include "cppa/config.hpp"
#include "cppa/behavior.hpp"
#include "cppa/message_id.hpp"
#include "cppa/mailbox_element.hpp"

namespace cppa {
namespace detail {

struct behavior_stack_mover;

class behavior_stack
{

    friend struct behavior_stack_mover;

    behavior_stack(const behavior_stack&) = delete;
    behavior_stack& operator=(const behavior_stack&) = delete;

    typedef std::pair<behavior, message_id> element_type;

 public:

    behavior_stack() = default;

    // @pre expected_response.valid()
    optional<behavior&> sync_handler(message_id expected_response);

    // erases the last asynchronous message handler
    void pop_async_back();

    void clear();

    // erases the synchronous response handler associated with @p rid
    void erase(message_id rid) {
        erase_if([=](const element_type& e) { return e.second == rid; });
    }

    inline bool empty() const { return m_elements.empty(); }

    inline behavior& back() {
        CPPA_REQUIRE(!empty());
        return m_elements.back().first;
    }

    inline message_id back_id() {
        CPPA_REQUIRE(!empty());
        return m_elements.back().second;
    }

    inline void push_back(behavior&& what,
                          message_id response_id = message_id::invalid) {
        m_elements.emplace_back(std::move(what), response_id);
    }

    inline void cleanup() {
        m_erased_elements.clear();
    }

 private:

    std::vector<element_type> m_elements;
    std::vector<behavior> m_erased_elements;

    // note: checks wheter i points to m_elements.end() before calling erase()
    inline void erase_at(std::vector<element_type>::iterator i) {
        if (i != m_elements.end()) {
            m_erased_elements.emplace_back(std::move(i->first));
            m_elements.erase(i);
        }
    }

    inline void rerase_at(std::vector<element_type>::reverse_iterator i) {
        // base iterator points to the element *after* the correct element
        if (i != m_elements.rend()) erase_at(i.base() - 1);
    }

    template<typename UnaryPredicate>
    inline void erase_if(UnaryPredicate p) {
        erase_at(std::find_if(m_elements.begin(), m_elements.end(), p));
    }

    template<typename UnaryPredicate>
    inline void rerase_if(UnaryPredicate p) {
        rerase_at(std::find_if(m_elements.rbegin(), m_elements.rend(), p));
    }

};

} // namespace detail
} // namespace cppa

#endif // CPPA_DETAIL_BEHAVIOR_STACK_HPP
