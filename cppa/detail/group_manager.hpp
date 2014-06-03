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


#ifndef CPPA_DETAIL_GROUP_MANAGER_HPP
#define CPPA_DETAIL_GROUP_MANAGER_HPP

#include <map>
#include <mutex>
#include <thread>

#include "cppa/abstract_group.hpp"
#include "cppa/util/shared_spinlock.hpp"

#include "cppa/detail/singleton_mixin.hpp"

namespace cppa {
namespace detail {

class group_manager : public singleton_mixin<group_manager> {

    friend class singleton_mixin<group_manager>;

 public:

    ~group_manager();

    group get(const std::string& module_name,
              const std::string& group_identifier);

    group anonymous();

    void add_module(abstract_group::unique_module_ptr);

    abstract_group::module_ptr get_module(const std::string& module_name);

 private:

    typedef std::map<std::string, abstract_group::unique_module_ptr> modules_map;

    modules_map m_mmap;
    std::mutex m_mmap_mtx;

    group_manager();

};

} // namespace detail
} // namespace cppa

#endif // CPPA_DETAIL_GROUP_MANAGER_HPP
