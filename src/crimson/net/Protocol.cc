// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "Protocol.h"

#include "auth/Auth.h"

#include "crimson/common/log.h"
#include "crimson/net/Errors.h"
#include "crimson/net/Socket.h"
#include "crimson/net/SocketConnection.h"
#include "msg/Message.h"

namespace {
  seastar::logger& logger() {
    return ceph::get_logger(ceph_subsys_ms);
  }
}

namespace ceph::net {

Protocol::Protocol(proto_t type,
                   Dispatcher& dispatcher,
                   SocketConnection& conn)
  : proto_type(type),
    dispatcher(dispatcher),
    conn(conn),
    auth_meta{seastar::make_lw_shared<AuthConnectionMeta>()}
{}

Protocol::~Protocol()
{
  ceph_assert(pending_dispatch.is_closed());
  assert(!exit_open);
}

bool Protocol::is_connected() const
{
  return write_state == write_state_t::open;
}

seastar::future<> Protocol::close()
{
  if (closed) {
    // already closing
    assert(close_ready.valid());
    return close_ready.get_future();
  }

  // unregister_conn() drops a reference, so hold another until completion
  auto cleanup = [conn_ref = conn.shared_from_this(), this] {
      logger().debug("{} closed!", conn);
    };

  trigger_close();

  // close_ready become valid only after state is state_t::closing
  assert(!close_ready.valid());

  if (socket) {
    socket->shutdown();
    close_ready = pending_dispatch.close().finally([this] {
      return socket->close();
    }).finally(std::move(cleanup));
  } else {
    close_ready = pending_dispatch.close().finally(std::move(cleanup));
  }

  closed = true;
  set_write_state(write_state_t::drop);

  return close_ready.get_future();
}

seastar::future<> Protocol::send(MessageRef msg)
{
  if (write_state != write_state_t::drop) {
    conn.out_q.push_back(std::move(msg));
    write_event();
  }
  return seastar::now();
}

seastar::future<> Protocol::keepalive()
{
  if (!need_keepalive) {
    need_keepalive = true;
    write_event();
  }
  return seastar::now();
}

void Protocol::notify_keepalive_ack(utime_t _keepalive_ack)
{
  logger().trace("{} got keepalive ack {}", conn, _keepalive_ack);
  keepalive_ack = _keepalive_ack;
  write_event();
}

void Protocol::notify_ack()
{
  if (!conn.policy.lossy) {
    ++ack_left;
    write_event();
  }
}

void Protocol::requeue_sent()
{
  assert(write_state != write_state_t::open);
  if (conn.sent.empty()) {
    return;
  }

  conn.out_seq -= conn.sent.size();
  logger().debug("{} requeue {} items, revert out_seq to {}",
                 conn, conn.sent.size(), conn.out_seq);
  for (MessageRef& msg : conn.sent) {
    msg->clear_payload();
    msg->set_seq(0);
  }
  conn.out_q.insert(conn.out_q.begin(),
                    std::make_move_iterator(conn.sent.begin()),
                    std::make_move_iterator(conn.sent.end()));
  conn.sent.clear();
  write_event();
}

void Protocol::requeue_up_to(seq_num_t seq)
{
  assert(write_state != write_state_t::open);
  if (conn.sent.empty() && conn.out_q.empty()) {
    logger().debug("{} nothing to requeue, reset out_seq from {} to seq {}",
                   conn, conn.out_seq, seq);
    conn.out_seq = seq;
    return;
  }
  logger().debug("{} discarding sent items by seq {} (sent_len={}, out_seq={})",
                 conn, seq, conn.sent.size(), conn.out_seq);
  while (!conn.sent.empty()) {
    auto cur_seq = conn.sent.front()->get_seq();
    if (cur_seq == 0 || cur_seq > seq) {
      break;
    } else {
      conn.sent.pop_front();
    }
  }
  requeue_sent();
}

void Protocol::reset_write()
{
  assert(write_state != write_state_t::open);
  conn.out_seq = 0;
  conn.out_q.clear();
  conn.sent.clear();
  need_keepalive = false;
  keepalive_ack = std::nullopt;
  ack_left = 0;
}

void Protocol::ack_writes(seq_num_t seq)
{
  if (conn.policy.lossy) {  // lossy connections don't keep sent messages
    return;
  }
  while (!conn.sent.empty() && conn.sent.front()->get_seq() <= seq) {
    logger().trace("{} got ack seq {} >= {}, pop {}",
                   conn, seq, conn.sent.front()->get_seq(), conn.sent.front());
    conn.sent.pop_front();
  }
}

seastar::future<> Protocol::do_write_dispatch_sweep()
{
  return seastar::repeat([this] {
    switch (write_state) {
     case write_state_t::open: {
      size_t num_msgs = conn.out_q.size();
      // we must have something to write...
      ceph_assert(is_queued());
      conn.pending_q.clear();
      conn.pending_q.swap(conn.out_q);
      if (!conn.policy.lossy) {
        conn.sent.insert(conn.sent.end(),
                         conn.pending_q.begin(),
                         conn.pending_q.end());
      }
      auto acked = ack_left;
      assert(acked == 0 || conn.in_seq > 0);
      // sweep all pending writes with the concrete Protocol
      return socket->write(do_sweep_messages(
          conn.pending_q, num_msgs, need_keepalive, keepalive_ack, acked > 0)
      ).then([this, prv_keepalive_ack=keepalive_ack, acked] {
        need_keepalive = false;
        if (keepalive_ack == prv_keepalive_ack) {
          keepalive_ack = std::nullopt;
        }
        assert(ack_left >= acked);
        ack_left -= acked;
        if (!is_queued()) {
          // good, we have nothing pending to send now.
          return socket->flush().then([this] {
            if (!is_queued()) {
              // still nothing pending to send after flush,
              // the dispatching can ONLY stop now
              ceph_assert(write_dispatching);
              write_dispatching = false;
              if (unlikely(exit_open.has_value())) {
                exit_open->set_value();
                exit_open = std::nullopt;
                logger().info("{} write_event: nothing queued at {},"
                              " set exit_open",
                              conn, get_state_name(write_state));
              }
              return seastar::make_ready_future<stop_t>(stop_t::yes);
            } else {
              // something is pending to send during flushing
              return seastar::make_ready_future<stop_t>(stop_t::no);
            }
          });
        } else {
          // messages were enqueued during socket write
          return seastar::make_ready_future<stop_t>(stop_t::no);
        }
      });
     }
     case write_state_t::delay:
      // delay dispatching writes until open
      if (exit_open) {
        exit_open->set_value();
        exit_open = std::nullopt;
        logger().info("{} write_event: delay and set exit_open ...", conn);
      } else {
        logger().info("{} write_event: delay ...", conn);
      }
      return state_changed.get_shared_future()
      .then([] { return stop_t::no; });
     case write_state_t::drop:
      ceph_assert(write_dispatching);
      write_dispatching = false;
      if (exit_open) {
        exit_open->set_value();
        exit_open = std::nullopt;
        logger().info("{} write_event: dropped and set exit_open", conn);
      } else {
        logger().info("{} write_event: dropped", conn);
      }
      return seastar::make_ready_future<stop_t>(stop_t::yes);
     default:
      ceph_assert(false);
    }
  }).handle_exception_type([this] (const std::system_error& e) {
    if (e.code() != error::broken_pipe &&
        e.code() != error::connection_reset &&
        e.code() != error::negotiation_failure) {
      logger().error("{} write_event(): unexpected error at {} -- {}",
                     conn, get_state_name(write_state), e);
      ceph_abort();
    }
    socket->shutdown();
    if (write_state == write_state_t::open) {
      logger().info("{} write_event(): fault at {}, going to delay -- {}",
                    conn, get_state_name(write_state), e);
      write_state = write_state_t::delay;
    } else {
      logger().info("{} write_event(): fault at {} -- {}",
                    conn, get_state_name(write_state), e);
    }
    return do_write_dispatch_sweep();
  });
}

void Protocol::write_event()
{
  notify_write();
  if (write_dispatching) {
    // already dispatching
    return;
  }
  write_dispatching = true;
  switch (write_state) {
   case write_state_t::open:
     [[fallthrough]];
   case write_state_t::delay:
    (void) seastar::with_gate(pending_dispatch, [this] {
      return do_write_dispatch_sweep(
      ).handle_exception([this] (std::exception_ptr eptr) {
        logger().error("{} do_write_dispatch_sweep(): unexpected exception {}",
                       conn, eptr);
        ceph_abort();
      });
    });
    return;
   case write_state_t::drop:
    write_dispatching = false;
    return;
   default:
    ceph_assert(false);
  }
}

} // namespace ceph::net
