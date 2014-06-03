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


#ifndef CPPA_SCHEDULER_HPP
#define CPPA_SCHEDULER_HPP

#include <chrono>
#include <memory>
#include <thread>
#include <cstdint>
#include <functional>
#include <type_traits>

#include "cppa/atom.hpp"
#include "cppa/actor.hpp"
#include "cppa/channel.hpp"
#include "cppa/any_tuple.hpp"
#include "cppa/cow_tuple.hpp"
#include "cppa/attachable.hpp"
#include "cppa/scoped_actor.hpp"
#include "cppa/spawn_options.hpp"
#include "cppa/execution_unit.hpp"
#include "cppa/message_header.hpp"

#include "cppa/util/duration.hpp"
#include "cppa/util/producer_consumer_list.hpp"

namespace cppa {

class resumable;

namespace detail { class singleton_manager; }

namespace scheduler {

class coordinator;


/**
 * @brief A work-stealing scheduling worker.
 *
 * The work-stealing implementation of libcppa minimizes access to the
 * synchronized queue. The reasoning behind this design decision is that
 * it has been shown that stealing actually is very rare for workloads [1].
 * Hence, implementations should focus on the performance in
 * the non-stealing case. For this reason, each worker has an exposed
 * job queue that can be accessed by the central scheduler instance as
 * well as other workers, but it also has a private job list it is
 * currently working on. To account for the load balancing aspect, each
 * worker makes sure that at least one job is left in its exposed queue
 * to allow other workers to steal it.
 *
 * [1] http://dl.acm.org/citation.cfm?doid=2398857.2384639
 */
class worker : public execution_unit {

    friend class coordinator;

 public:

    worker() = default;

    worker(worker&&);

    worker& operator=(worker&&);

    worker(const worker&) = delete;

    worker& operator=(const worker&) = delete;

    typedef resumable* job_ptr;

    typedef util::producer_consumer_list<resumable> job_queue;

    /**
     * @brief Attempt to steal an element from the exposed job queue.
     */
    job_ptr try_steal();

    /**
     * @brief Enqueues a new job to the worker's queue from an external
     *        source, i.e., from any other thread.
     */
    void external_enqueue(job_ptr);

    /**
     * @brief Enqueues a new job to the worker's queue from an internal
     *        source, i.e., a job that is currently executed by
     *        this worker.
     * @warning Must not be called from other threads.
     */
    void exec_later(job_ptr) override;

 private:

    void start(size_t id, coordinator* parent); // called from coordinator

    void run(); // work loop

    job_ptr raid(); // go on a raid in quest for a shiny new job

    // this queue is exposed to others, i.e., other workers
    // may attempt to steal jobs from it and the central scheduling
    // unit can push new jobs to the queue
    job_queue m_exposed_queue;

    // internal job stack
    std::vector<job_ptr> m_job_list;

    // the worker's thread
    std::thread m_this_thread;

    // the worker's ID received from scheduler
    size_t m_id;

    // the ID of the last victim we stole from
    size_t m_last_victim;

    coordinator* m_parent;

};

/**
 * @brief Central scheduling interface.
 */
class coordinator {

    friend class detail::singleton_manager;

 public:

    class shutdown_helper;

    /**
     * @brief Returns a handle to the central printing actor.
     */
    actor printer() const;

    /**
     * @brief Puts @p what into the queue of a randomly chosen worker.
     */
    void enqueue(resumable* what);

    template<typename Duration, typename... Data>
    void delayed_send(message_header hdr,
                      const Duration& rel_time,
                      any_tuple data           ) {
        auto tup = make_any_tuple(atom("SEND"),
                                  util::duration{rel_time},
                                  std::move(hdr),
                                  std::move(data));
        m_timer->enqueue(message_header{}, std::move(tup), nullptr);
    }

    template<typename Duration, typename... Data>
    void delayed_reply(message_header hdr,
                       const Duration& rel_time,
                       any_tuple data           ) {
        CPPA_REQUIRE(hdr.id.valid() && hdr.id.is_response());
        auto tup = make_any_tuple(atom("SEND"),
                                  util::duration{rel_time},
                                  std::move(hdr),
                                  std::move(data));
        m_timer->enqueue(message_header{}, std::move(tup), nullptr);
    }

    inline size_t num_workers() const {
        return static_cast<unsigned>(m_workers.size());
    }

    inline worker& worker_by_id(size_t id) {
        return m_workers[id];
    }

 private:

    static coordinator* create_singleton();

    coordinator();

    inline void dispose() { delete this; }

    void initialize();

    void destroy();

    intrusive_ptr<blocking_actor> m_timer;
    scoped_actor m_printer;

    std::thread m_timer_thread;
    std::thread m_printer_thread;

    // ID of the worker receiving the next enqueue
    std::atomic<size_t> m_next_worker;

    // vector of size std::thread::hardware_concurrency()
    std::vector<worker> m_workers;

};

} // namespace scheduler

} // namespace cppa

#endif // CPPA_SCHEDULER_HPP
