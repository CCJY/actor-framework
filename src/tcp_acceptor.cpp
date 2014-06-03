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

#include <ios>
#include <cstring>
#include <errno.h>
#include <iostream>

#include "cppa/config.hpp"
#include "cppa/logging.hpp"
#include "cppa/exception.hpp"

#include "cppa/io/stream.hpp"
#include "cppa/io/tcp_acceptor.hpp"
#include "cppa/io/tcp_io_stream.hpp"

#include "cppa/detail/fd_util.hpp"

#ifdef CPPA_WINDOWS
#   include <winsock2.h>
#   include <ws2tcpip.h>
#else
#   include <netdb.h>
#   include <unistd.h>
#   include <arpa/inet.h>
#   include <sys/types.h>
#   include <sys/socket.h>
#   include <netinet/in.h>
#   include <netinet/tcp.h>
#endif

namespace cppa {
namespace io {

using namespace ::cppa::detail::fd_util;

namespace {

struct socket_guard {

    bool m_released;
    native_socket_type m_socket;

 public:

    socket_guard(native_socket_type sfd) : m_released(false), m_socket(sfd) { }

    ~socket_guard() {
        if (!m_released) closesocket(m_socket);
    }

    void release() {
        m_released = true;
    }

};

bool accept_impl(stream_ptr_pair& result,
                 native_socket_type fd,
                 bool nonblocking) {
    sockaddr addr;
    memset(&addr, 0, sizeof(addr));
    socklen_t addrlen = sizeof(addr);
    auto sfd = ::accept(fd, &addr, &addrlen);
    if (sfd == invalid_socket) {
        auto err = last_socket_error();
        if (nonblocking && would_block_or_temporarily_unavailable(err)) {
            // ok, try again
            return false;
        }
        throw_io_failure("accept failed");
    }
    stream_ptr ptr(tcp_io_stream::from_sockfd(sfd));
    result.first = ptr;
    result.second = ptr;
    return true;
}

} // namespace <anonymous>

tcp_acceptor::tcp_acceptor(native_socket_type fd, bool nonblocking)
: m_fd(fd), m_is_nonblocking(nonblocking) { }

std::unique_ptr<acceptor> tcp_acceptor::create(std::uint16_t port,
                                                const char* addr) {
    CPPA_LOGM_TRACE("tcp_acceptor", CPPA_ARG(port) << ", addr = "
                                     << (addr ? addr : "nullptr"));
#   ifdef CPPA_WINDOWS
    // ensure that TCP has been initialized via WSAStartup
    cppa::get_middleman();
#   endif
    native_socket_type sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == invalid_socket) {
        throw network_error("could not create server socket");
    }
    // sguard closes the socket in case of exception
    socket_guard sguard(sockfd);
    int on = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<setsockopt_ptr>(&on), sizeof(on)) < 0) {
        throw_io_failure("unable to set SO_REUSEADDR");
    }
    struct sockaddr_in serv_addr;
    memset((char*) &serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    if (! addr) {
        serv_addr.sin_addr.s_addr = INADDR_ANY;
    }
    else if (::inet_pton(AF_INET, addr, &serv_addr.sin_addr) <= 0) {
        throw network_error("invalid IPv4 address");
    }
    serv_addr.sin_port = htons(port);
    if (bind(sockfd, (sockaddr*) &serv_addr, sizeof(serv_addr)) < 0) {
        throw bind_failure(errno);
    }
    if (listen(sockfd, 10) != 0) {
        throw network_error("listen() failed");
    }
    // ok, no exceptions so far
    sguard.release();
    CPPA_LOGM_DEBUG("tcp_acceptor", "sockfd = " << sockfd);
    return from_sockfd(sockfd);
}

std::unique_ptr<acceptor> tcp_acceptor::from_sockfd(native_socket_type fd) {
    // sguard closes the socket in case of exception
    socket_guard sguard(fd);
    // default mode is nonblocking
    nonblocking(fd, true);
    // ok, no exceptions
    sguard.release();
    return std::unique_ptr<tcp_acceptor>(new tcp_acceptor(fd, true));
}


tcp_acceptor::~tcp_acceptor() {
    closesocket(m_fd);
}

native_socket_type tcp_acceptor::file_handle() const {
    return m_fd;
}

stream_ptr_pair tcp_acceptor::accept_connection() {
    if (m_is_nonblocking) {
        nonblocking(m_fd, false);
        m_is_nonblocking = false;
    }
    stream_ptr_pair result;
    accept_impl(result, m_fd, m_is_nonblocking);
    return result;
}

optional<stream_ptr_pair> tcp_acceptor::try_accept_connection() {
    if (!m_is_nonblocking) {
        nonblocking(m_fd, true);
        m_is_nonblocking = true;
    }
    stream_ptr_pair result;
    if (accept_impl(result, m_fd, m_is_nonblocking)) {
        return result;
    }
    return none;
}

} // namespace util
} // namespace cppa

