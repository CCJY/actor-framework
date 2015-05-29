/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright (C) 2011 - 2015                                                  *
 * Dominik Charousset <dominik.charousset (at) haw-hamburg.de>                *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#include "caf/io/basp_broker.hpp"

#include "caf/exception.hpp"
#include "caf/make_counted.hpp"
#include "caf/event_based_actor.hpp"
#include "caf/binary_serializer.hpp"
#include "caf/binary_deserializer.hpp"
#include "caf/forwarding_actor_proxy.hpp"

#include "caf/detail/singletons.hpp"
#include "caf/detail/actor_registry.hpp"
#include "caf/detail/sync_request_bouncer.hpp"

#include "caf/io/basp.hpp"
#include "caf/io/middleman.hpp"
#include "caf/io/unpublish.hpp"

#include "caf/io/network/interfaces.hpp"

using std::string;

namespace caf {
namespace io {

using detail::singletons;

class connection_slave : public event_based_actor {
 public:
  connection_slave(actor master)
      : m_backend(middleman::instance()->backend()),
        m_master(master) {
    // nop
  }

  ~connection_slave();

  behavior make_behavior() {
    return {
      [=](std::vector<std::pair<std::string, std::string>>& addresses,
          uint16_t port, node_id& src, node_id& target) {
        while (!addresses.empty() && addresses.back().first != "ipv4") {
          // TODO: add support for IPv6
          CAF_LOG_INFO("skipping unsupported IPv6 address: "
                       << addresses.back().second);
          addresses.pop_back();
        }
        if (addresses.empty()) {
          // failure; tell basp_broker to send direct_conn_response
          send(m_master, error_atom{}, std::move(src), std::move(target));
          return;
        }
        try {
          CAF_LOG_DEBUG("connecting to " << addresses.back().second);
          auto hdl = m_backend.new_tcp_scribe(addresses.back().second, port);
          addresses.pop_back();
          // we got a connection, await handshake
          send(m_master, get_atom{}, hdl, int64_t{0}, this,
               std::set<std::string>{});
          become(
            keep_behavior,
            [=](ok_atom ok, int64_t, actor_addr) {
              CAF_LOG_DEBUG("handshake complete, making broker send response");
              send(m_master, ok, std::move(src), std::move(target));
              unbecome();
            },
            [=](error_atom, int64_t, const std::string&) {
              CAF_LOG_DEBUG("handshake failed, trying next address");
              send(this, std::move(addresses), port, std::move(src),
                   std::move(target));
              unbecome();
            }
          );
        }
        catch (...) {
          CAF_LOG_INFO("failed to connect to " << addresses.back().second);
          // try next address
          addresses.pop_back();
          send(this, std::move(addresses), port, std::move(src),
               std::move(target));
        }
      },
      on(atom("INIT")) >> [=] {
        auto res = m_backend.new_tcp_doorman(0);
        send(m_master, res.first, res.second);
      }
    };
  }

 private:
  network::multiplexer& m_backend;
  actor m_master;
};

connection_slave::~connection_slave() {
  // nop
}


basp_broker::payload_writer::~payload_writer() {
  // nop
}

template <class F>
class functor_payload_writer : public basp_broker::payload_writer {
 public:
  functor_payload_writer(F fun) : m_fun(fun) {
    // nop
  }

  functor_payload_writer(const functor_payload_writer&) = default;

  functor_payload_writer& operator=(const functor_payload_writer&) = default;

  void write(binary_serializer& sink) override {
    m_fun(sink);
  }

 private:
  F m_fun;
};

template <class F>
functor_payload_writer<F> make_payload_writer(F fun) {
  return {fun};
}

basp_broker::basp_broker(middleman& pref) : broker(pref), m_namespace(*this) {
  m_meta_msg = uniform_typeid<message>();
  m_meta_id_type = uniform_typeid<node_id>();
  CAF_LOG_DEBUG("BASP broker started: " << to_string(node()));
}

basp_broker::~basp_broker() {
  CAF_LOG_TRACE("");
}

behavior basp_broker::make_behavior() {
  trap_exit(true);
  if (m_slave == invalid_actor) {
    // wait until slave opened a local port for us
    m_slave = spawn<connection_slave, detached + hidden + linked>(this);
    send(m_slave, atom("INIT"));
    return {
      [=](accept_handle hdl, uint16_t default_port) {
        assign_tcp_doorman(hdl);
        m_default_port = default_port;
        become(make_behavior()); // this time for real
      }
    };
  }
  return {
    [=](const exit_msg& msg) {
      send_exit(m_slave, msg.reason);
      quit(msg.reason);
    },
    // received from underlying broker implementation
    [=](new_data_msg& msg) {
      CAF_LOG_TRACE("handle = " << msg.handle.id());
      CAF_ASSERT(m_ctx.count(msg.handle) > 0);
      new_data(m_ctx[msg.handle], msg.buf);
    },
    // received from underlying broker implementation
    [=](const new_connection_msg& msg) {
      CAF_LOG_TRACE("handle = " << msg.handle.id());
      CAF_ASSERT(m_ctx.count(msg.handle) == 0);
      auto& ctx = m_ctx[msg.handle];
      ctx.hdl = msg.handle;
      ctx.handshake_data = none;
      ctx.state = await_client_handshake;
      auto& ptr = m_acceptors[msg.source].first;
      init_handshake_as_server(ctx, ptr ? ptr->address() : invalid_actor_addr);
    },
    // received from underlying broker implementation
    [=](const connection_closed_msg& msg) {
      CAF_LOG_TRACE(CAF_MARG(msg.handle, id));
      auto j = m_ctx.find(msg.handle);
      if (j != m_ctx.end()) {
        auto hd = j->second.handshake_data;
        if (hd) {
          send(hd->client, error_atom::value, hd->request_id,
               "disconnect during handshake");
        }
        m_ctx.erase(j);
      }
      // purge handle from all routes
      std::vector<node_id> lost_connections;
      for (auto& kvp : m_routes) {
        auto& entry = kvp.second;
        if (entry.first.hdl == msg.handle) {
          CAF_LOG_DEBUG("lost direct connection to " << to_string(kvp.first));
          entry.first.hdl.set_invalid();
        }
        auto last = entry.second.end();
        auto i = std::lower_bound(entry.second.begin(), last, msg.handle,
                                  connection_info_less{});
        if (i != last && i->hdl == msg.handle) {
          entry.second.erase(i);
        }
        if (entry.first.invalid() && entry.second.empty()) {
          lost_connections.push_back(kvp.first);
        }
      }
      // remove routes that no longer have any path and kill all proxies
      for (auto& lc : lost_connections) {
        CAF_LOG_DEBUG("no more route to " << to_string(lc));
        fail_pending_requests(lc, exit_reason::remote_link_unreachable);
        m_routes.erase(lc);
        auto proxies = m_namespace.get_all(lc);
        m_namespace.erase(lc);
        for (auto& p : proxies) {
          p->kill_proxy(exit_reason::remote_link_unreachable);
        }
      }
    },
    // received from underlying broker implementation
    [=](const acceptor_closed_msg& msg) {
      CAF_LOG_TRACE("");
      auto i = m_acceptors.find(msg.handle);
      if (i == m_acceptors.end()) {
        CAF_LOG_INFO("accept handle no longer in use");
        return;
      }
      if (m_open_ports.erase(i->second.second) == 0) {
        CAF_LOG_INFO("accept handle was not bound to a port");
      }
      m_acceptors.erase(i);
    },
    // received from proxy instances
    [=](forward_atom, const actor_addr& sender, const actor_addr& receiver,
        message_id mid, const message& msg) {
      CAF_LOG_TRACE("");
      if (dispatch(sender, receiver, mid, msg) == invalid_node_id
          && mid.is_request()) {
        detail::sync_request_bouncer srb{exit_reason::remote_link_unreachable};
        srb(sender, mid);
      }
    },
    [=](delete_atom, const node_id& nid, actor_id aid) {
      CAF_LOG_TRACE(CAF_TSARG(nid) << ", " << CAF_ARG(aid));
      erase_proxy(nid, aid);
    },
    // received from middleman actor
    [=](put_atom, accept_handle hdl, const actor_addr& whom, uint16_t port) {
      CAF_LOG_TRACE(CAF_ARG(hdl.id()) << ", "<< CAF_TSARG(whom)
                    << ", " << CAF_ARG(port));
      if (hdl.invalid()) {
        return;
      }
      if (whom == invalid_actor_addr) {
        if (current_sender() == m_slave) {
          // allow only from slave since this remains open until broker exits
          m_acceptors.emplace(hdl, std::make_pair(nullptr, port));
          m_open_ports.emplace(port, hdl);
        }
        return;
      }
      try {
        assign_tcp_doorman(hdl);
      }
      catch (...) {
        CAF_LOG_DEBUG("failed to assign doorman from handle");
        return;
      }
      add_published_actor(hdl, actor_cast<abstract_actor_ptr>(whom), port);
      parent().notify<hook::actor_published>(whom, port);
    },
    [=](get_atom, connection_handle hdl, int64_t request_id,
        actor client, std::set<std::string>& expected_ifs) {
      CAF_LOG_TRACE(CAF_ARG(hdl.id()) << ", " << CAF_ARG(request_id)
                    << ", " << CAF_TSARG(client));
      try {
        assign_tcp_scribe(hdl);
      }
      catch (std::exception& e) {
        CAF_LOG_DEBUG("failed to assign scribe from handle: " << e.what());
        send(client, error_atom::value, request_id,
             std::string("failed to assign scribe from handle: ") + e.what());
        return;
      }
      auto& ctx = m_ctx[hdl];
      ctx.hdl = hdl;
      // PODs are not movable, so passing expected_ifs to the ctor  would cause
      // a copy; we avoid this by calling the ctor with an empty set and
      // swap afterwards with expected_ifs
      ctx.handshake_data = client_handshake_data{request_id, client,
                                                 std::set<std::string>()};
      ctx.handshake_data->expected_ifs.swap(expected_ifs);
      init_handshake_as_client(ctx);
    },
    [=](delete_atom, int64_t request_id, const actor_addr& whom, uint16_t port)
    -> message {
      CAF_LOG_TRACE(CAF_ARG(request_id) << ", " << CAF_TSARG(whom)
                    << ", " << CAF_ARG(port));
      if (whom == invalid_actor_addr) {
        return make_message(error_atom::value, request_id,
                            "whom == invalid_actor_addr");
      }
      auto ptr = actor_cast<abstract_actor_ptr>(whom);
      if (port == 0) {
        if (!remove_published_actor(ptr)) {
          return make_message(error_atom::value, request_id,
                              "no mapping found");
        }
      } else {
        if (!remove_published_actor(ptr, port)) {
          return make_message(error_atom::value, request_id,
                              "port not bound to actor");
        }
      }
      return make_message(ok_atom::value, request_id);
    },
    // received from connection slave
    [=](atom_value atm, const node_id& src, const node_id& target) {
      uint64_t op_data = atm == ok_atom{} ? 1 : 0;
      auto writer = make_payload_writer([&](binary_serializer& sink) {
        sink << target;
      });
      dispatch(basp::direct_conn_response, node(), invalid_actor_id,
               src, invalid_actor_id, op_data, &writer);
    },
    // catch-all error handler
    others >> [=] {
      CAF_LOG_ERROR("received unexpected message: "
                   << to_string(current_message()));
    }
  };
}

void basp_broker::new_data(connection_context& ctx, buffer_type& buf) {
  CAF_LOG_TRACE(CAF_TARG(ctx.state, static_cast<int>) << ", "
                CAF_MARG(ctx.hdl, id));
  m_current_context = &ctx;
  connection_state next_state;
  switch (ctx.state) {
    default: {
      binary_deserializer bd{buf.data(), buf.size(), &m_namespace};
      read(bd, ctx.hdr);
      if (!basp::valid(ctx.hdr)) {
        CAF_LOG_INFO("invalid broker message received");
        close(ctx.hdl);
        m_ctx.erase(ctx.hdl);
        return;
      }
      next_state = handle_basp_header(ctx);
      break;
    }
    case await_payload: {
      next_state = handle_basp_header(ctx, &buf);
      break;
    }
  }
  CAF_LOG_DEBUG("transition: " << ctx.state << " -> " << next_state);
  if (next_state == close_connection) {
    close(ctx.hdl);
    m_ctx.erase(ctx.hdl);
    return;
  }
  ctx.state = next_state;
  configure_read(ctx.hdl, receive_policy::exactly(next_state == await_payload
                                                    ? ctx.hdr.payload_len
                                                    : basp::header_size));
}

void basp_broker::local_dispatch(const basp::header& hdr, message&& msg) {
  // TODO: provide hook API to allow ActorShell to
  //       intercept/trace/log each message
  CAF_LOG_TRACE("");
  // look up the source of the messages
  actor_addr src;
  if (hdr.source_node != invalid_node_id
      && hdr.source_actor != invalid_actor_id) {
    if (hdr.source_node != node()) {
      CAF_LOG_DEBUG("source is a proxy");
      src = m_namespace.get_or_put(hdr.source_node,
                                   hdr.source_actor)->address();
    } else {
      CAF_LOG_DEBUG("source is a local actor (so he claims)");
      auto ptr = singletons::get_actor_registry()->get(hdr.source_actor);
      if (ptr) {
        src = ptr->address();
      }
    }
  }
  CAF_LOG_DEBUG_IF(src == invalid_actor_addr, "src == invalid_actor_addr");
  auto dest = singletons::get_actor_registry()->get(hdr.dest_actor);
  CAF_ASSERT(!dest || dest->node() == node());
  // intercept message used for link signaling
  if (dest && src == dest) {
    if (msg.match_elements<atom_value, actor_addr>()) {
      actor_addr other;
      bool is_unlink = true;
      // extract arguments
      msg.apply({
        [&](link_atom, const actor_addr& addr) {
          is_unlink = false;
          other = addr;
        },
        [&](unlink_atom, const actor_addr& addr) {
          other = addr;
        }
      });
      // in both cases, we link a local actor to a proxy
      if (other != invalid_actor_addr) {
        auto iptr = actor_cast<intrusive_ptr<abstract_actor>>(other);
        auto ptr = dynamic_cast<actor_proxy*>(iptr.get());
        if (ptr) {
          if (is_unlink) {
            ptr->local_unlink_from(dest->address());
          } else {
            // it's either an unlink request or a new link
            ptr->local_link_to(dest->address());
          }
          // do not actually send this message as it's been already handled
          return;
        }
      }
    }
  }
  auto mid = message_id::from_integer_value(hdr.operation_data);
  if (!dest) {
    CAF_LOG_DEBUG("received a message for an invalid actor; "
                  "could not find an actor with ID "
                  << hdr.dest_actor);
    parent().notify<hook::invalid_message_received>(hdr.source_node, src,
                                                    hdr.dest_actor, mid, msg);
    return;
  }
  auto dest_addr = dest->address();
  if (mid.is_response() && !m_pending_requests.empty()) {
    // remove from pendings requests
    auto req_id = mid.request_id();
    auto last = m_pending_requests.end();
    auto i = std::find(m_pending_requests.begin(), last,
                       std::tie(hdr.source_node, dest_addr, req_id));
    if (i != last) {
      if (i != (last - 1)) {
        using std::swap;
        swap(*i, m_pending_requests.back());
      }
      m_pending_requests.pop_back();
    }
  }
  parent().notify<hook::message_received>(hdr.source_node, src,
                                          dest_addr, mid, msg);
  CAF_LOG_DEBUG("enqueue message from " << to_string(src) << " to "
                << to_string(dest->address()));
  dest->enqueue(src, mid, std::move(msg), nullptr);
}

void basp_broker::dispatch(connection_handle hdl, uint32_t operation,
                           const node_id& src_node, actor_id src_actor,
                           const node_id& dest_node, actor_id dest_actor,
                           uint64_t op_data, payload_writer* writer) {
  auto& buf = wr_buf(hdl);
  if (writer) {
    // reserve space in the buffer to write the broker message later on
    auto wr_pos = static_cast<ptrdiff_t>(buf.size());
    char placeholder[basp::header_size];
    buf.insert(buf.end(), std::begin(placeholder), std::end(placeholder));
    auto before = buf.size();
    { // lifetime scope of first serializer (write payload)
      binary_serializer bs1{std::back_inserter(buf), &m_namespace};
      writer->write(bs1);
    }
    // write broker message to the reserved space
    binary_serializer bs2{buf.begin() + wr_pos, &m_namespace};
    auto payload_len = static_cast<uint32_t>(buf.size() - before);
    write(bs2, {src_node,    dest_node, src_actor,   dest_actor,
                payload_len, operation, op_data});
  } else {
    binary_serializer bs(std::back_inserter(wr_buf(hdl)), &m_namespace);
    write(bs, {src_node, dest_node, src_actor, dest_actor,
               0, operation, op_data});
  }
  flush(hdl);
}

node_id basp_broker::dispatch(uint32_t operation, const node_id& src_node,
                              actor_id src_actor, const node_id& dest_node,
                              actor_id dest_actor, uint64_t op_data,
                              payload_writer* writer) {
  auto route = get_route(dest_node);
  if (route.invalid()) {
    CAF_LOG_INFO("unable to dispatch message: no route to "
                 << CAF_TSARG(dest_node));
    return invalid_node_id;
  }
  if (route.node != dest_node) {
    CAF_LOG_DEBUG("no direct route to " << to_string(dest_node));
    // TODO: consider using blacklist for failing direct connection attempts.
    if (m_inflight_conn_reqs.find(dest_node) != m_inflight_conn_reqs.end()) {
      CAF_LOG_DEBUG("awaiting response to inflight request");
    } else {
      CAF_LOG_DEBUG("sending complete direct connection request");
      m_inflight_conn_reqs.insert(dest_node);
      using network::protocol;
      std::vector<std::pair<std::string, std::string>> addresses;
      for (auto& addr : network::interfaces::list_addresses(protocol::ipv4)) {
        addresses.emplace_back("ipv4", std::move(addr));
      }
      auto dcr_writer = make_payload_writer([&](binary_serializer& sink) {
        sink.write(node(), m_meta_id_type);
        sink.write(node(), m_meta_id_type);
        sink << m_default_port << static_cast<uint32_t>(addresses.size());
        for (auto& addr : addresses) {
          sink << addr.first << addr.second;
        }
      });
      dispatch(route.hdl, basp::direct_conn_request, node(), invalid_actor_id,
               dest_node, invalid_actor_id, 1, &dcr_writer);
    }
  }
  dispatch(route.hdl, operation, src_node, src_actor, dest_node, dest_actor,
           op_data, writer);
  return route.node;
}

node_id basp_broker::dispatch(const actor_addr& from, const actor_addr& to,
                              message_id mid, const message& msg) {
  CAF_LOG_TRACE(CAF_TSARG(from) << ", " << CAF_MARG(mid, integer_value)
                << ", " << CAF_TSARG(to) << ", " << CAF_TSARG(msg));
  if (to == invalid_actor_addr) {
    return invalid_node_id;
  }
  if (from != invalid_actor_addr && from.node() == node()) {
    // register locally running actors to be able to deserialize them later
    auto reg = detail::singletons::get_actor_registry();
    reg->put(from.id(), actor_cast<abstract_actor_ptr>(from));
  }
  auto writer = make_payload_writer([&](binary_serializer& sink) {
    sink.write(msg, m_meta_msg);
  });
  auto route_node = dispatch(basp::dispatch_message, from.node(), from.id(),
                             to.node(), to.id(), mid.integer_value(), &writer);
  if (route_node == invalid_node_id) {
    parent().notify<hook::message_sending_failed>(from, to, mid, msg);
  } else {
    if (mid.is_request()) {
      // keep track of pendings sync requests for error handling
      m_pending_requests.emplace_back(to.node(), from, mid);
    }
    parent().notify<hook::message_sent>(from, route_node, to, mid, msg);
  }
  return route_node;
}

void basp_broker::read(binary_deserializer& bd, basp::header& msg) {
  bd.read(msg.source_node, m_meta_id_type)
    .read(msg.dest_node, m_meta_id_type)
    .read(msg.source_actor)
    .read(msg.dest_actor)
    .read(msg.payload_len)
    .read(msg.operation)
    .read(msg.operation_data);
}

void basp_broker::write(binary_serializer& bs, const basp::header& msg) {
  bs.write(msg.source_node, m_meta_id_type)
    .write(msg.dest_node, m_meta_id_type)
    .write(msg.source_actor)
    .write(msg.dest_actor)
    .write(msg.payload_len)
    .write(msg.operation)
    .write(msg.operation_data);
}

basp_broker::connection_state
basp_broker::handle_basp_header(connection_context& ctx,
                                const buffer_type* payload) {
  CAF_LOG_TRACE(CAF_TARG(ctx.state, static_cast<int>)
                << ", payload = "
                << (payload ? payload->size() : 0) << " bytes"
                << (payload ? "" : " (nullptr)"));
  auto& hdr = ctx.hdr;
  if (!payload && hdr.payload_len > 0) {
    // receive payload first
    CAF_LOG_DEBUG("await payload");
    return await_payload;
  }
  CAF_LOG_DEBUG("header => "<< CAF_TSARG(hdr.source_node)
                << ", " << CAF_TSARG(hdr.dest_node)
                << ", " << CAF_ARG(hdr.source_actor)
                << ", " << CAF_ARG(hdr.dest_actor)
                << ", " << CAF_ARG(hdr.payload_len)
                << ", " << CAF_ARG(hdr.operation)
                << ", " << CAF_ARG(hdr.operation_data));
  // forward message if not addressed to us; invalid dest_node implies
  // that msg is a server_handshake
  if (hdr.dest_node != invalid_node_id && hdr.dest_node != node()) {
    auto route = get_route(hdr.dest_node);
    if (route.invalid()) {
      CAF_LOG_ERROR("cannot forward message: no route to node "
                    << to_string(hdr.dest_node));
      parent().notify<hook::message_forwarding_failed>(hdr.source_node,
                                                       hdr.dest_node, payload);
      return close_connection;
    }
    CAF_LOG_DEBUG("received message that is not addressed to us -> "
                  << "forward via " << to_string(route.node));
    auto& buf = wr_buf(route.hdl);
    binary_serializer bs{std::back_inserter(buf), &m_namespace};
    write(bs, hdr);
    if (payload) {
      buf.insert(buf.end(), payload->begin(), payload->end());
    }
    flush(route.hdl);
    parent().notify<hook::message_forwarded>(hdr.source_node,
                                             hdr.dest_node, payload);
    return await_header;
  }
  // handle a message that is addressed to us
  switch (hdr.operation) {
    default:
      // must not happen
      throw std::logic_error("invalid operation");
    case basp::dispatch_message: {
      CAF_ASSERT(payload != nullptr);
      binary_deserializer bd{payload->data(), payload->size(), &m_namespace};
      message content;
      bd.read(content, m_meta_msg);
      local_dispatch(ctx.hdr, std::move(content));
      break;
    }
    case basp::announce_proxy_instance: {
      CAF_ASSERT(payload == nullptr);
      // source node has created a proxy for one of our actors
      auto entry = singletons::get_actor_registry()->get_entry(hdr.dest_actor);
      auto nid = hdr.source_node;
      auto aid = hdr.dest_actor;
      if (entry.second != exit_reason::not_exited) {
        send_kill_proxy_instance(nid, aid, entry.second);
      } else {
        auto mm = middleman::instance();
        entry.first->attach_functor([=](uint32_t reason) {
          mm->backend().dispatch([=] {
            CAF_LOG_TRACE(CAF_ARG(reason));
            auto bro = mm->get_named_broker<basp_broker>(atom("_BASP"));
            bro->send_kill_proxy_instance(nid, aid, reason);
          });
        });
      }
      break;
    }
    case basp::kill_proxy_instance: {
      CAF_ASSERT(payload == nullptr);
      // we have a proxy to an actor that has been terminated
      auto ptr = m_namespace.get(hdr.source_node, hdr.source_actor);
      if (ptr) {
        m_namespace.erase(ptr->node(), ptr->id());
        ptr->kill_proxy(static_cast<uint32_t>(hdr.operation_data));
      } else {
        CAF_LOG_DEBUG("received kill proxy twice");
      }
      break;
    }
    case basp::client_handshake: {
      CAF_ASSERT(payload == nullptr);
      if (ctx.remote_id != invalid_node_id) {
        CAF_LOG_INFO("received unexpected client handshake");
        return close_connection;
      }
      ctx.remote_id = hdr.source_node;
      auto i = m_inflight_conn_reqs.find(ctx.remote_id);
      if (i != m_inflight_conn_reqs.end()) {
        CAF_LOG_DEBUG("incoming connection for direct connection request");
        m_inflight_conn_reqs.erase(i);
      }
      if (node() == ctx.remote_id) {
        CAF_LOG_INFO("incoming connection from self");
        return close_connection;
      }
      else if (!try_set_default_route(ctx.remote_id, ctx.hdl)) {
        CAF_LOG_INFO("multiple incoming connections from the same node");
        return close_connection;
      }
      parent().notify<hook::new_connection_established>(ctx.remote_id );
      break;
    }
    case basp::server_handshake: {
      CAF_ASSERT(payload != nullptr);
      if (!ctx.handshake_data) {
        CAF_LOG_INFO("received unexpected server handshake");
        return close_connection;
      }
      if (hdr.operation_data != basp::version) {
        CAF_LOG_INFO("tried to connect to a node with different BASP version");
        return close_connection;
      }
      ctx.remote_id = hdr.source_node;
      binary_deserializer bd{payload->data(), payload->size(), &m_namespace};
      auto remote_aid = bd.read<uint32_t>();
      auto remote_ifs_size = bd.read<uint32_t>();
      std::set<string> remote_ifs;
      for (uint32_t i = 0; i < remote_ifs_size; ++i) {
        auto str = bd.read<string>();
        remote_ifs.insert(std::move(str));
      }
      auto& ifs = ctx.handshake_data->expected_ifs;
      auto hsclient = ctx.handshake_data->client;
      auto hsid = ctx.handshake_data->request_id;
      if (!std::includes(ifs.begin(), ifs.end(),
                         remote_ifs.begin(), remote_ifs.end())) {
        auto tostr = [](const std::set<string>& what) -> string {
          if (what.empty()) {
            return "actor";
          }
          string tmp;
          tmp = "typed_actor<";
          auto i = what.begin();
          auto e = what.end();
          tmp += *i++;
          while (i != e) {
            tmp += ",";
            tmp += *i++;
          }
          tmp += ">";
          return tmp;
        };
        auto iface_str = tostr(remote_ifs);
        auto expected_str = tostr(ifs);
        std::string error_msg;
        if (ifs.empty()) {
          error_msg = "expected remote actor to be a "
                      "dynamically typed actor but found "
                      "a strongly typed actor of type "
                      + iface_str;
        }
        else if (remote_ifs.empty()) {
          error_msg = "expected remote actor to be a "
                      "strongly typed actor of type "
                      + expected_str +
                      " but found a dynamically typed actor";
        } else {
          error_msg = "expected remote actor to be a "
                      "strongly typed actor of type "
                      + expected_str +
                      " but found a strongly typed actor of type "
                      + iface_str;
        }
        // abort with error
        send(hsclient, error_atom::value, hsid, std::move(error_msg));
        return close_connection;
      }
      auto nid = hdr.source_node;
      if (nid == node()) {
        CAF_LOG_INFO("incoming connection from self: drop connection");
        auto res = detail::singletons::get_actor_registry()->get(remote_aid);
        send(hsclient, ok_atom::value, hsid, res->address());
        ctx.handshake_data = none;
        return close_connection;
      }
      if (!try_set_default_route(nid, ctx.hdl)) {
        CAF_LOG_INFO("multiple connections to " << to_string(nid)
                     << " (re-use old one)");
        auto proxy = m_namespace.get_or_put(nid, remote_aid);
        // discard this peer; there's already an open connection
        send(hsclient, ok_atom::value, hsid, proxy->address());
        ctx.handshake_data = none;
        return close_connection;
      }
      // finalize handshake
      dispatch(ctx.hdl, basp::client_handshake,
               node(), invalid_actor_id, nid, invalid_actor_id);
      // prepare to receive messages
      auto proxy = m_namespace.get_or_put(nid, remote_aid);
      ctx.published_actor = proxy;
      send(hsclient, ok_atom::value, hsid, proxy->address());
      ctx.handshake_data = none;
      parent().notify<hook::new_connection_established>(nid);
      break;
    }
    case basp::direct_conn_request: {
      CAF_ASSERT(payload != nullptr);
      node_id request_origin;
      node_id target;
      uint16_t port = 0;
      std::vector<std::pair<std::string, std::string>> addresses;
      {
        binary_deserializer bd{payload->data(), payload->size(), &m_namespace};
        bd.read(request_origin, m_meta_id_type);
        bd.read(target, m_meta_id_type);
        if (hdr.operation_data == 1) {
          bd >> port;
          uint32_t num_addresses;
          bd >> num_addresses;
          for (uint32_t i = 0; i < num_addresses; ++i) {
            std::pair<std::string, std::string> address;
            bd >> address.first >> address.second;
            addresses.push_back(std::move(address));
          }
        }
      }
      if (hdr.operation_data == 0) {
        using network::protocol;
        port = m_default_port;
        for (auto& addr : network::interfaces::list_addresses(protocol::ipv4)) {
          addresses.emplace_back("ipv4", std::move(addr));
        }
        auto writer = make_payload_writer([&](binary_serializer& sink) {
          sink.write(request_origin, m_meta_id_type);
          sink.write(target, m_meta_id_type);
          sink << port << static_cast<uint32_t>(addresses.size());
          for (auto& addr : addresses) {
            sink << addr.first << addr.second;
          }
        });
        // TODO: send direct_conn_response immediately if we already
        //       have a direct connection to the target
        dispatch(basp::direct_conn_request, node(), invalid_actor_id,
                 target, invalid_actor_id, 1, &writer);
      } else if (target != node()) {
        CAF_LOG_ERROR("wrong target in received direct_conn_request");
      } else {
        send(m_slave, std::move(addresses), port,
             request_origin, hdr.source_node);
      }
      break;
    }
    case basp::direct_conn_response: {
      CAF_ASSERT(payload == nullptr);
      // TODO: signalizing stuff, complete handlers, etc.
      //       and fill add this path to the blacklist on failure
      break;
    }
  }
  return await_header;
}

void basp_broker::send_kill_proxy_instance(const node_id& nid, actor_id aid,
                                           uint32_t reason) {
  CAF_LOG_TRACE(CAF_TSARG(nid) << ", " << CAF_ARG(aid) << CAF_ARG(reason));
  auto route_node = dispatch(basp::kill_proxy_instance, node(), aid,
                             nid, invalid_actor_id, reason);
  if (route_node == invalid_node_id) {
    CAF_LOG_INFO("message dropped, no route to node: " << to_string(nid));
  }
}

basp_broker::connection_info basp_broker::get_route(const node_id& dest) {
  connection_info res;
  auto i = m_routes.find(dest);
  if (i != m_routes.end()) {
    auto& entry = i->second;
    res = entry.first;
    CAF_LOG_DEBUG_IF(!res.invalid(),
                     "using default route via " << to_string(res.node));
    if (res.invalid() && !entry.second.empty()) {
      res = *entry.second.begin();
      CAF_LOG_DEBUG("using first auxiliary route via " << to_string(res.node));
    }
  }
  return res;
}

actor_proxy_ptr basp_broker::make_proxy(const node_id& nid, actor_id aid) {
  CAF_LOG_TRACE(CAF_TSARG(nid) << ", "
              << CAF_ARG(aid));
  CAF_ASSERT(m_current_context != nullptr);
  CAF_ASSERT(aid != invalid_actor_id);
  CAF_ASSERT(nid != node());
  // this member function is being called whenever we deserialize a
  // payload received from a remote node; if a remote node N sends
  // us a handle to a third node T, we assume that N has a route to T
  if (nid != m_current_context->remote_id) {
    add_route(nid, m_current_context->hdl);
  }
  // we need to tell remote side we are watching this actor now;
  // use a direct route if possible, i.e., when talking to a third node
  auto route = get_route(nid);
  if (route.invalid()) {
    // this happens if and only if we don't have a path to `nid`
    // and m_current_context->hdl has been blacklisted
    CAF_LOG_INFO("cannot create a proxy instance for an actor "
           "running on a node we don't have a route to");
    return nullptr;
  }
  // create proxy and add functor that will be called if we
  // receive a kill_proxy_instance message
  intrusive_ptr<basp_broker> self = this;
  auto mm = middleman::instance();
  auto res = make_counted<forwarding_actor_proxy>(aid, nid, self);
  res->attach_functor([=](uint32_t) {
    mm->backend().dispatch([=] {
      // using res->id() instead of aid keeps this actor instance alive
      // until the original instance terminates, thus preventing subtle
      // bugs with attachables
      erase_proxy(nid, res->id());
    });
  });
  // tell remote side we are monitoring this actor now
  dispatch(route.hdl, basp::announce_proxy_instance,
           node(), invalid_actor_id, nid, aid);
  parent().notify<hook::new_remote_actor>(res->address());
  return res;
}

void basp_broker::on_exit() {
  CAF_LOG_TRACE("");
  // kill all remaining proxies
  auto proxies = m_namespace.get_all();
  for (auto& proxy : proxies) {
    CAF_ASSERT(proxy != nullptr);
    proxy->kill_proxy(exit_reason::remote_link_unreachable);
  }
  m_slave = invalid_actor;
  m_namespace.clear();
  // remove all remaining state
  m_ctx.clear();
  m_acceptors.clear();
  m_open_ports.clear();
  m_routes.clear();
  m_blacklist.clear();
  m_pending_requests.clear();
  m_inflight_conn_reqs.clear();
  m_pending_conn_resps.clear();
}

void basp_broker::erase_proxy(const node_id& nid, actor_id aid) {
  CAF_LOG_TRACE(CAF_TSARG(nid) << ", " << CAF_ARG(aid));
  m_namespace.erase(nid, aid);
  if (m_namespace.empty()) {
    CAF_LOG_DEBUG("no proxy left, request shutdown of connection");
  }
}

void basp_broker::add_route(const node_id& nid, connection_handle hdl) {
  if (m_blacklist.count(std::make_pair(nid, hdl)) == 0) {
    parent().notify<hook::new_route_added>(m_current_context->remote_id, nid);
    m_routes[nid].second.insert({hdl, m_current_context->remote_id});
    CAF_LOG_DEBUG("added new route: " << to_string(nid) << " -> "
                  << to_string(m_current_context->remote_id));
  }
}

bool basp_broker::try_set_default_route(const node_id& nid,
                                        connection_handle hdl) {
  CAF_ASSERT(!hdl.invalid());
  auto& entry = m_routes[nid];
  if (entry.first.invalid()) {
    CAF_LOG_DEBUG("new default route: " << to_string(nid) << " -> "
                                        << hdl.id());
    entry.first = {hdl, nid};
    return true;
  }
  return false;
}

void basp_broker::init_handshake_as_client(connection_context& ctx) {
  CAF_LOG_TRACE(CAF_ARG(this));
  ctx.state = await_server_handshake;
  configure_read(ctx.hdl, receive_policy::exactly(basp::header_size));
}

void basp_broker::init_handshake_as_server(connection_context& ctx,
                                           actor_addr addr) {
  CAF_LOG_TRACE(CAF_ARG(this));
  CAF_ASSERT(node() != invalid_node_id);
  if (addr != invalid_actor_addr) {
    auto writer = make_payload_writer([&](binary_serializer& sink) {
      sink << addr.id();
      auto sigs = addr.message_types();
      sink << static_cast<uint32_t>(sigs.size());
      for (auto& sig : sigs) {
        sink << sig;
      }
    });
    dispatch(ctx.hdl, basp::server_handshake, node(), addr.id(),
             invalid_node_id, invalid_actor_id, basp::version, &writer);
  } else {
    dispatch(ctx.hdl, basp::server_handshake, node(), invalid_actor_id,
             invalid_node_id, invalid_actor_id, basp::version);
  }
  // prepare for receiving client handshake
  ctx.state = await_client_handshake;
  configure_read(ctx.hdl, receive_policy::exactly(basp::header_size));
}

void basp_broker::add_published_actor(accept_handle hdl,
                                      const abstract_actor_ptr& ptr,
                                      uint16_t port) {
  CAF_LOG_TRACE("");
  if (!ptr) {
    return;
  }
  m_acceptors.emplace(hdl, std::make_pair(ptr, port));
  m_open_ports.emplace(port, hdl);
  ptr->attach_functor([port](abstract_actor* self, uint32_t) {
    unpublish_impl(self->address(), port, false);
  });
  if (ptr->node() == node()) {
    singletons::get_actor_registry()->put(ptr->id(), ptr);
  }
}

bool basp_broker::remove_published_actor(const abstract_actor_ptr& whom) {
  CAF_LOG_TRACE("");
  CAF_ASSERT(whom != nullptr);
  size_t erased_elements = 0;
  auto last = m_acceptors.end();
  auto i = m_acceptors.begin();
  while (i != last) {
    auto& kvp = i->second;
    if (kvp.first == whom) {
      CAF_ASSERT(valid(i->first));
      close(i->first);
      if (m_open_ports.erase(kvp.second) == 0) {
        CAF_LOG_ERROR("inconsistent data: no open port for acceptor!");
      }
      i = m_acceptors.erase(i);
      ++erased_elements;
    }
    else {
      ++i;
    }
  }
  return erased_elements > 0;
}

bool basp_broker::remove_published_actor(const abstract_actor_ptr& whom,
                                         uint16_t port) {
  CAF_LOG_TRACE("");
  CAF_ASSERT(whom != nullptr);
  CAF_ASSERT(port != 0);
  auto i = m_open_ports.find(port);
  if (i == m_open_ports.end()) {
    return false;
  }
  CAF_ASSERT(valid(i->second));
  auto j = m_acceptors.find(i->second);
  if (j->second.first != whom) {
    CAF_LOG_INFO("port has been bound to a different actor");
    return false;
  }
  close(i->second);
  m_open_ports.erase(i);
  if (j == m_acceptors.end()) {
    CAF_LOG_ERROR("inconsistent data: accept handle for port " << port
                  << " not found in m_published_actors");
  } else {
    m_acceptors.erase(j);
  }
  return true;
}

void basp_broker::fail_pending_requests(pending_request_iter first,
                                        pending_request_iter last,
                                        uint32_t reason) {
  CAF_LOG_TRACE(std::distance(first, last) << " elements, " << CAF_ARG(reason));
  if (first == last) {
    return;
  }
  detail::sync_request_bouncer srb{reason};
  auto bounce = [&](const pending_request& req) {
    srb(get<1>(req), get<2>(req));
  };
  std::for_each(first, last, bounce);
  m_pending_requests.erase(first, last);
}

void basp_broker::fail_pending_requests(uint32_t reason) {
  CAF_LOG_TRACE(CAF_ARG(reason));
  fail_pending_requests(m_pending_requests.begin(), m_pending_requests.end(),
                        reason);
}

void basp_broker::fail_pending_requests(const node_id& addr, uint32_t reason) {
  CAF_LOG_TRACE(CAF_TSARG(addr) << ", " << CAF_ARG(reason));
  auto f = [&](const pending_request& req) {
    return get<0>(req) == addr;
  };
  auto last = m_pending_requests.end();
  fail_pending_requests(std::remove_if(m_pending_requests.begin(), last, f),
                        last, reason);
}

} // namespace io
} // namespace caf
