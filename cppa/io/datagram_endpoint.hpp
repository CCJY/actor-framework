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

#ifndef CPPA_IO_DATAGRAM_ENDPOINT_HPP
#define CPPA_IO_DATAGRAM_ENDPOINT_HPP

#include <memory>
#include <sys/socket.h>

//#include "cppa/io/network.hpp"

namespace cppa {
namespace io {

namespace network {
    struct datagram_endpoint_data {
        sockaddr_storage addr; // todo: free this?
        socklen_t addrlen;
    };
}

using datagram_endpoint = std::shared_ptr<network::datagram_endpoint_data>;

} // namespace io
} // namespace cppa

#endif // CPPA_IO_DATAGRAM_ENDPOINT_HPP
