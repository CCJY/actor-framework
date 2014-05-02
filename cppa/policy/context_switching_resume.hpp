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
 * Copyright (C) 2011-2013                                                    *
 * Dominik Charousset <dominik.charousset@haw-hamburg.de>                     *
 *                                                                            *
 * This file is part of libcppa.                                              *
 * libcppa is free software: you can redistribute it and/or modify it under   *
 * the terms of the GNU Lesser General Public License as published by the     *
 * Free Software Foundation; either version 2.1 of the License,               *
 * or (at your option) any later version.                                     *
 *                                                                            *
 * libcppa is distributed in the hope that it will be useful,                 *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                       *
 * See the GNU Lesser General Public License for more details.                *
 *                                                                            *
 * You should have received a copy of the GNU Lesser General Public License   *
 * along with libcppa. If not, see <http://www.gnu.org/licenses/>.            *
\******************************************************************************/

#ifndef CPPA_CONTEXT_SWITCHING_ACTOR_HPP
#define CPPA_CONTEXT_SWITCHING_ACTOR_HPP

#include "cppa/config.hpp"
#include "cppa/logging.hpp"
#include "cppa/resumable.hpp"
#include "cppa/mailbox_element.hpp"

#include "cppa/detail/cs_thread.hpp"

#include "cppa/policy/resume_policy.hpp"

#include "cppa/detail/yield_interface.hpp"

namespace cppa {
class local_actor;
}

namespace cppa {
namespace policy {

/**
 * @brief Context-switching actor implementation.
 * @extends scheduled_actor
 */
class context_switching_resume {

 public:

    // required by detail::cs_thread
    static void trampoline(void* _this);

    // Base must be a mailbox-based actor
    template<class Base, class Derived>
    struct mixin : Base, resumable {

        template<typename... Ts>
        mixin(Ts&&... args)
            : Base(std::forward<Ts>(args)...)
            , m_cs_thread(context_switching_resume::trampoline,
                      static_cast<blocking_actor*>(this)) { }

        void attach_to_scheduler() override {
            this->ref();
        }

        void detach_from_scheduler() override {
            this->deref();
        }

        resumable::resume_result resume(detail::cs_thread* from,
                                        execution_unit* host) override {
            CPPA_REQUIRE(from != nullptr);
            CPPA_LOG_TRACE("");
            this->m_host = host;
            using namespace detail;
            for (;;) {
                switch (call(&m_cs_thread, from)) {
                    case yield_state::done: {
                        return resumable::done;
                    }
                    case yield_state::ready: {
                        // should not happen, since it is simply a waste
                        // of time (switching back-and-forth for no reason)
                        CPPA_LOG_WARNING("context-switching actor returned "
                                         "with yield_state::ready");
                        break;
                    }
                    case yield_state::blocked: {
                        if (static_cast<Derived*>(this)->mailbox().try_block()) {
                            return resumable::resume_later;
                        }
                        break;
                    }
                    case yield_state::invalid: {
                        // must not happen
                        CPPA_CRITICAL("illegal state");
                    }
                }
            }
        }

        detail::cs_thread m_cs_thread;

    };

    template<class Actor>
    void await_ready(Actor* self) {
        CPPA_LOG_TRACE("");
        while (!self->has_next_message()) {
            // will call mailbox().try_block() in resume()
            detail::yield(detail::yield_state::blocked);
        }
    }

 private:

    // members
    detail::cs_thread m_cs_thread;

};

} // namespace policy
} // namespace cppa

#endif // CPPA_CONTEXT_SWITCHING_ACTOR_HPP
