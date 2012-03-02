// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */



#ifndef CEPH_MESSENGER_H
#define CEPH_MESSENGER_H

#include <map>
using namespace std;

#include "Message.h"
#include "Dispatcher.h"
#include "common/Mutex.h"
#include "common/Cond.h"
#include "include/Context.h"
#include "include/types.h"
#include "include/ceph_features.h"

#include <errno.h>
#include <sstream>

class MDS;
class Timer;


class Messenger {
public:
  struct Policy {
    bool lossy;
    bool server;
    Throttle *throttler;

    uint64_t features_supported;
    uint64_t features_required;

    Policy()
      : lossy(false), server(false), throttler(NULL),
	features_supported(CEPH_FEATURES_SUPPORTED_DEFAULT),
	features_required(0) {}
    Policy(bool l, bool s, uint64_t sup, uint64_t req)
      : lossy(l), server(s), throttler(NULL),
	features_supported(sup | CEPH_FEATURES_SUPPORTED_DEFAULT),
	features_required(req) {}

    static Policy stateful_server(uint64_t sup, uint64_t req) {
      return Policy(false, true, sup, req);
    }
    static Policy stateless_server(uint64_t sup, uint64_t req) {
      return Policy(true, true, sup, req);
    }
    static Policy lossless_peer(uint64_t sup, uint64_t req) {
      return Policy(false, false, sup, req);
    }
    static Policy client(uint64_t sup, uint64_t req) {
      return Policy(false, false, sup, req);
    }
  };


private:
  list<Dispatcher*> dispatchers;

protected:
  /// the "name" of the local daemon. eg client.99
  entity_name_t _my_name;
  int default_send_priority;
  /// set to true once the Messenger has started, and set to false on shutdown
  bool started;

 public:
  CephContext *cct;
  Messenger(CephContext *cct_, entity_name_t w)
    : default_send_priority(CEPH_MSG_PRIO_DEFAULT), started(false),
      cct(cct_)
  {
    _my_name = w;
  }
  virtual ~Messenger() {}

  virtual void destroy() {
  }

  // accessors
  entity_name_t get_myname() { return _my_name; }
  virtual entity_addr_t get_myaddr() = 0;
  virtual void set_ip(entity_addr_t &addr) = 0;
  entity_inst_t get_myinst() { return entity_inst_t(get_myname(), get_myaddr()); }
  
  /**
   * Set the name of the local entity. The name is reported to others and
   * can be changed while the system is running, but doing so at incorrect
   * times may have bad results.
   *
   * @param m The name to set.
   */
  void set_myname(const entity_name_t m) { _my_name = m; }

  /**
   * Set the default send priority
   * This is an init-time function and must be called *before* calling
   * start().
   *
   * @param p The cluster protocol to use. Defined externally.
   */
  void set_default_send_priority(int p) {
    assert(!started);
    default_send_priority = p;
  }
  int get_default_send_priority() { return default_send_priority; }
  
  // hrmpf.
  virtual int get_dispatch_queue_len() { return 0; };

  /**
   * Add a new Dispatcher to the front of the list. If you add
   * a Dispatcher which is already included, it will get a duplicate
   * entry. This will reduce efficiency but not break anything.
   *
   * @param d The Dispatcher to insert into the list.
   */
  void add_dispatcher_head(Dispatcher *d) { 
    bool first = dispatchers.empty();
    dispatchers.push_front(d);
    if (first)
      ready();
  }
  /**
   * Add a new Dispatcher to the end of the list. If you add
   * a Dispatcher which is already included, it will get a duplicate
   * entry. This will reduce efficiency but not break anything.
   *
   * @param d The Dispatcher to insert into the list.
   */
  void add_dispatcher_tail(Dispatcher *d) { 
    bool first = dispatchers.empty();
    dispatchers.push_back(d);
    if (first)
      ready();
  }

  virtual void ready() { }
  bool is_ready() { return !dispatchers.empty(); }

  // dispatch incoming messages
  void ms_deliver_dispatch(Message *m) {
    m->set_dispatch_stamp(ceph_clock_now(cct));
    for (list<Dispatcher*>::iterator p = dispatchers.begin();
	 p != dispatchers.end();
	 p++)
      if ((*p)->ms_dispatch(m))
	return;
    std::ostringstream oss;
    oss << "ms_deliver_dispatch: fatal error: unhandled message "
	<< m << " " << *m << " from " << m->get_source_inst();
    dout_emergency(oss.str());
    assert(0);
  }
  void ms_deliver_handle_connect(Connection *con) {
    for (list<Dispatcher*>::iterator p = dispatchers.begin();
	 p != dispatchers.end();
	 p++)
      (*p)->ms_handle_connect(con);
  }
  void ms_deliver_handle_reset(Connection *con) {
    for (list<Dispatcher*>::iterator p = dispatchers.begin();
	 p != dispatchers.end();
	 p++)
      if ((*p)->ms_handle_reset(con))
	return;
  }
  void ms_deliver_handle_remote_reset(Connection *con) {
    for (list<Dispatcher*>::iterator p = dispatchers.begin();
	 p != dispatchers.end();
	 p++)
      (*p)->ms_handle_remote_reset(con);
  }

  AuthAuthorizer *ms_deliver_get_authorizer(int peer_type, bool force_new) {
    AuthAuthorizer *a = 0;
    for (list<Dispatcher*>::iterator p = dispatchers.begin();
	 p != dispatchers.end();
	 p++)
      if ((*p)->ms_get_authorizer(peer_type, &a, force_new))
	return a;
    return NULL;
  }
  bool ms_deliver_verify_authorizer(Connection *con, int peer_type,
				    int protocol, bufferlist& authorizer, bufferlist& authorizer_reply,
				    bool& isvalid) {
    for (list<Dispatcher*>::iterator p = dispatchers.begin();
	 p != dispatchers.end();
	 p++)
      if ((*p)->ms_verify_authorizer(con, peer_type, protocol, authorizer, authorizer_reply, isvalid))
	return true;
    return false;
  }

  // setup
  /**
   * Perform any resource allocation, thread startup, etc
   * that is required before attempting to connect to other
   * Messengers or transmit messages.
   * Once this function completes, started shall be set to true.
   *
   * @return 0 on success; -errno on failure.
   */
  virtual int start() { started = true; return 0; }

  // shutdown
  /**
   * Block until the Messenger has finished shutting down (according
   * to the shutdown() function).
   * It is valid to call this after calling shutdown(), but it must
   * be called before deleting the Messenger.
   */
  virtual void wait() = 0;
  /**
   * Initiate a shutdown of the Messenger.
   *
   * @return 0 on success, -errno otherwise.
   */
  virtual int shutdown() { started = false; return 0; }
  virtual void suicide() = 0;

  // send message
  virtual void prepare_dest(const entity_inst_t& inst) {}
  virtual int send_message(Message *m, const entity_inst_t& dest) = 0;
  virtual int send_message(Message *m, Connection *con) = 0;
  virtual int lazy_send_message(Message *m, const entity_inst_t& dest) {
    return send_message(m, dest);
  }
  virtual int lazy_send_message(Message *m, Connection *con) = 0;
  virtual int send_keepalive(const entity_inst_t& dest) = 0;
  virtual int send_keepalive(Connection *con) = 0;

  virtual void mark_down(const entity_addr_t& a) = 0;
  virtual void mark_down(Connection *con) = 0;
  virtual void mark_down_on_empty(Connection *con) = 0;
  virtual void mark_disposable(Connection *con) = 0;
  virtual void mark_down_all() = 0;

  virtual Connection *get_connection(const entity_inst_t& dest) = 0;

  virtual int rebind(int avoid_port) { return -EOPNOTSUPP; }
};





#endif
