/* -*- C++ -*-; c-basic-offset: 4; indent-tabs-mode: nil */
/*
 * Implementation for Processor class.
 *
 * Copyright (c) 2014 Cisco Systems, Inc. and others.  All rights reserved.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v1.0 which accompanies this distribution,
 * and is available at http://www.eclipse.org/legal/epl-v10.html
 */

/* This must be included before anything else */
#if HAVE_CONFIG_H
#  include <config.h>
#endif


#include <time.h>
#include <uv.h>
#include <limits>

#include <boost/tuple/tuple.hpp>
#include <boost/foreach.hpp>
#include <boost/unordered_set.hpp>
#include "opflex/engine/internal/OpflexPEHandler.h"
#include "opflex/engine/internal/ProcessorMessage.h"
#include "opflex/engine/Processor.h"
#include "LockGuard.h"
#include "opflex/logging/internal/logging.hpp"

namespace opflex {
namespace engine {

using boost::shared_ptr;
using boost::unordered_set;
using std::vector;
using std::pair;
using std::make_pair;
using modb::ObjectStore;
using modb::ClassInfo;
using modb::PropertyInfo;
using modb::class_id_t;
using modb::prop_id_t;
using modb::reference_t;
using modb::URI;
using modb::mointernal::StoreClient;
using modb::mointernal::ObjectInstance;
using modb::hash_value;
using ofcore::OFConstants;

using namespace internal;

static const uint64_t LOCAL_REFRESH_RATE = 1000*60*30;
static const uint64_t DEFAULT_PROC_DELAY = 250;
static const uint64_t DEFAULT_RETRY_DELAY = 1000*10;
static const uint64_t TOMBSTONE_DELAY = 1000*60*5;
static const uint64_t FIRST_XID = (uint64_t)1 << 63;

size_t hash_value(pair<class_id_t, URI> const& p);

Processor::Processor(ObjectStore* store_)
    : AbstractObjectListener(store_),
      serializer(store_, this),
      pool(*this), nextXid(FIRST_XID),
      processingDelay(DEFAULT_PROC_DELAY),
      retryDelay(DEFAULT_RETRY_DELAY),
      proc_active(false) {
    uv_mutex_init(&item_mutex);
}

Processor::~Processor() {
    stop();
    uv_mutex_destroy(&item_mutex);
}

// get the current time in milliseconds since something
inline uint64_t now(uv_loop_t* loop) {
    return uv_now(loop);
}

Processor::change_expiration::change_expiration(uint64_t new_exp_)
    : new_exp(new_exp_) {}

void Processor::change_expiration::operator()(Processor::item& i) {
    i.expiration = new_exp;
}

Processor::change_last_xid::change_last_xid(uint64_t new_last_xid_)
    : new_last_xid(new_last_xid_) {}

void Processor::change_last_xid::operator()(Processor::item& i) {
    i.last_xid = new_last_xid;
}

// check whether the object state index has work for us
bool Processor::hasWork(/* out */ obj_state_by_exp::iterator& it) {
    if (obj_state.size() == 0) return false;
    obj_state_by_exp& exp_index = obj_state.get<expiration_tag>();
    it = exp_index.begin();
    if (it->expiration == 0 || now(&proc_loop) >= it->expiration) return true;
    return false;
}

// add a reference if it doesn't already exist
void Processor::addRef(obj_state_by_exp::iterator& it,
                       const reference_t& up) {
    if (it->details->urirefs.find(up) == it->details->urirefs.end()) {
        obj_state_by_uri& uri_index = obj_state.get<uri_tag>();
        obj_state_by_uri::iterator uit = uri_index.find(up.second);
        if (uit != uri_index.end() && uit->details->state == DELETED) {
            // If there's a tombstone object in place, remove it and
            // re-add a fresh one
            uri_index.erase(uit);
            uit = uri_index.end();
        }
        if (uit == uri_index.end()) {
            obj_state.insert(item(up.second, up.first,
                                  0, LOCAL_REFRESH_RATE,
                                  UNRESOLVED, false));
            // XXX - TODO create the resolver object as well
            uit = uri_index.find(up.second);
        }
        uit->details->refcount += 1;
        LOG(DEBUG2) << "addref " << uit->uri.toString()
                    << " (from " << it->uri.toString() << ")"
                    << " " << uit->details->refcount
                    << " state " << uit->details->state;

        it->details->urirefs.insert(up);
    }
}

// remove a reference if it already exists.  If refcount is zero,
// schedule the reference for collection
void Processor::removeRef(obj_state_by_exp::iterator& it,
                          const reference_t& up) {
    if (it->details->urirefs.find(up) != it->details->urirefs.end()) {
        obj_state_by_uri& uri_index = obj_state.get<uri_tag>();
        obj_state_by_uri::iterator uit = uri_index.find(up.second);
        if (uit != uri_index.end()) {
            uit->details->refcount -= 1;
            LOG(DEBUG2) << "removeref " << uit->uri.toString()
                        << " (from " << it->uri.toString() << ")"
                        << " " << uit->details->refcount
                        << " state " << uit->details->state;
            if (uit->details->refcount <= 0) {
                uint64_t nexp = now(&proc_loop)+processingDelay;
                uri_index.modify(uit, Processor::change_expiration(nexp));
            }
        }
        it->details->urirefs.erase(up);
    }
}

size_t Processor::getRefCount(const URI& uri) {
    util::LockGuard guard(&item_mutex);
    obj_state_by_uri& uri_index = obj_state.get<uri_tag>();
    obj_state_by_uri::iterator uit = uri_index.find(uri);
    if (uit != uri_index.end()) {
        return uit->details->refcount;
    }
    return 0;
}

bool Processor::isObjNew(const URI& uri) {
    util::LockGuard guard(&item_mutex);
    obj_state_by_uri& uri_index = obj_state.get<uri_tag>();
    obj_state_by_uri::iterator uit = uri_index.find(uri);
    if (uit != uri_index.end()) {
        return uit->details->state == NEW;
    }
    return true;
}

// check if the object has a zero refcount and it has no remote
// ancestor that has a zero refcount.
bool Processor::isOrphan(const item& item) {
    // simplest case: refcount is nonzero or item is local
    if (item.details->local || item.details->refcount > 0)
        return false;

    try {
        std::pair<URI, prop_id_t> parent(URI::ROOT, 0);
        if (client->getParent(item.details->class_id, item.uri, parent)) {
            obj_state_by_uri& uri_index = obj_state.get<uri_tag>();
            obj_state_by_uri::iterator uit = uri_index.find(parent.first);
            // parent missing
            if (uit == uri_index.end())
                return true;

            // the parent is local, so there can be no remote parent with
            // a nonzero refcount
            if (uit->details->local)
                return true;

            return isOrphan(*uit);
        }
    } catch (const std::out_of_range& e) {}
    return true;
}

// Check if an object is the highest-rank ancestor for objects that
// are synced to the server.  We don't bother syncing child objects
// since those will get synced when we sync the parent.
bool Processor::isParentSyncObject(const item& item) {
    try {
        const ClassInfo& ci = store->getClassInfo(item.details->class_id);
        std::pair<URI, prop_id_t> parent(URI::ROOT, 0);
        if (client->getParent(item.details->class_id, item.uri, parent)) {
            const ClassInfo& parent_ci = store->getPropClassInfo(parent.second);

            // The parent object will be synchronized
            if (ci.getType() == parent_ci.getType()) return false;

            return true;
        }
    } catch (const std::out_of_range& e) {}
    // possibly an unrooted object; sync anyway since it won't be
    // garbage-collected
    return true;
}

void Processor::sendToRole(const item& i, uint64_t& newexp,
                           OpflexMessage* req,
                           ofcore::OFConstants::OpflexRole role) {
    uint64_t xid = req->getReqXid();
    size_t pending = pool.sendToRole(req, role);
    i.details->pending_reqs = pending;

    obj_state_by_uri& uri_index = obj_state.get<uri_tag>();
    obj_state_by_uri::iterator uit = uri_index.find(i.uri);
    uri_index.modify(uit, change_last_xid(xid));

    if (pending > 0)
        newexp = now(&proc_loop) + retryDelay;
}

bool Processor::resolveObj(ClassInfo::class_type_t type, const item& i,
                           uint64_t& newexp, bool checkTime) {
    uint64_t curTime = now(&proc_loop);
    bool shouldRefresh =
        (i.details->resolve_time == 0) ||
        (curTime > (i.details->resolve_time + i.details->refresh_rate/2));
    bool shouldRetry =
        (i.details->pending_reqs != 0) &&
        (curTime > (i.details->resolve_time + retryDelay/2));

    if (checkTime && !shouldRefresh && !shouldRetry)
        return false;

    switch (type) {
    case ClassInfo::POLICY:
        {
            LOG(DEBUG) << "Resolving policy " << i.uri;
            i.details->resolve_time = curTime;
            vector<reference_t> refs;
            refs.push_back(make_pair(i.details->class_id, i.uri));
            PolicyResolveReq* req =
                new PolicyResolveReq(this, nextXid++, refs);
            sendToRole(i, newexp, req, OFConstants::POLICY_REPOSITORY);
            return true;
        }
        break;
    case ClassInfo::REMOTE_ENDPOINT:
        {
            LOG(DEBUG) << "Resolving remote endpoint " << i.uri;
            i.details->resolve_time = curTime;
            vector<reference_t> refs;
            refs.push_back(make_pair(i.details->class_id, i.uri));
            EndpointResolveReq* req =
                new EndpointResolveReq(this, nextXid++, refs);
            sendToRole(i, newexp, req, OFConstants::ENDPOINT_REGISTRY);
            return true;
        }
        break;
    default:
        // do nothing
        return false;
        break;
    }
}

bool Processor::declareObj(ClassInfo::class_type_t type, const item& i,
                           uint64_t& newexp) {
    uint64_t curTime = now(&proc_loop);
    switch (type) {
    case ClassInfo::LOCAL_ENDPOINT:
        if (isParentSyncObject(i)) {
            LOG(DEBUG) << "Declaring local endpoint " << i.uri;
            i.details->resolve_time = curTime;
            vector<reference_t> refs;
            refs.push_back(make_pair(i.details->class_id, i.uri));
            EndpointDeclareReq* req =
                new EndpointDeclareReq(this, nextXid++, refs);
            sendToRole(i, newexp, req, OFConstants::ENDPOINT_REGISTRY);
        }
        return true;
        break;
    case ClassInfo::OBSERVABLE:
        if (isParentSyncObject(i)) {
            LOG(DEBUG3) << "Declaring local observable " << i.uri;
            i.details->resolve_time = curTime;
            vector<reference_t> refs;
            refs.push_back(make_pair(i.details->class_id, i.uri));
            StateReportReq* req = new StateReportReq(this, nextXid++, refs);
            sendToRole(i, newexp, req, OFConstants::OBSERVER);
        }
        return true;
        break;
    default:
        // do nothing
        return false;
        break;
    }
}

// Process the item.  This is where we do most of the actual work of
// syncing the managed object over opflex
void Processor::processItem(obj_state_by_exp::iterator& it) {
    StoreClient::notif_t notifs;

    util::LockGuard guard(&item_mutex);
    ItemState curState = it->details->state;
    size_t curRefCount = it->details->refcount;
    bool local = it->details->local;

    obj_state_by_exp& exp_index = obj_state.get<expiration_tag>();
    uint64_t newexp = std::numeric_limits<uint64_t>::max();
    if (it->details->refresh_rate > 0) {
        if (it->details->pending_reqs > 0)
            newexp = now(&proc_loop) + retryDelay;
        else
            newexp = now(&proc_loop) + it->details->refresh_rate;
    }

    ItemState newState;

    switch (curState) {
    case NEW:
    case UPDATED:
        newState = IN_SYNC;
        break;
    case PENDING_DELETE:
        if (local)
            newState = IN_SYNC;
        else
            newState = REMOTE;
    case DELETED:
        LOG(DEBUG) << "Purging state for " << it->uri.toString();
        exp_index.erase(it);
        return;
    default:
        newState = curState;
        break;
    }

    const ClassInfo& ci = store->getClassInfo(it->details->class_id);
    shared_ptr<const ObjectInstance> oi;
    if (!client->get(it->details->class_id, it->uri, oi)) {
        // item removed
        switch (curState) {
        case UNRESOLVED:
            break;
        default:
            newState = DELETED;
            break;
        }
    }

    LOG(DEBUG2) << "Processing " << (local ? "local" : "nonlocal")
               << " item " << it->uri.toString()
               << " of class " << ci.getName()
               << " and type " << ci.getType()
               << " in state " << curState;

    // Check whether this item needs to be garbage collected
    if (oi && isOrphan(*it)) {
        switch (curState) {
        case NEW:
        case REMOTE:
            {
                // requeue new items so if there are any pending references
                // we won't remove them right away
                LOG(DEBUG2) << "Queuing delete for orphan " << it->uri.toString();
                newState = PENDING_DELETE;
                obj_state_by_exp& exp_index = obj_state.get<expiration_tag>();
                exp_index.modify(it,
                                 change_expiration(now(&proc_loop)+
                                                   processingDelay));
                break;
            }
        default:
            {
                // Remove object from store and dispatch a notification
                LOG(DEBUG) << "Removing orphan object " << it->uri.toString();
                client->remove(it->details->class_id,
                               it->uri,
                               false, &notifs);
                client->queueNotification(it->details->class_id, it->uri,
                                          notifs);
                oi.reset();
                newState = DELETED;
                break;
            }
        }
    }

    // check for references to other objects and update the reference
    // count.  Create a new state object or clear the object as
    // needed.
    boost::unordered_set<reference_t> visited;
    if (oi && ci.getType() != ClassInfo::REVERSE_RELATIONSHIP) {
        BOOST_FOREACH(const ClassInfo::property_map_t::value_type& p,
                      ci.getProperties()) {
            if (p.second.getType() == PropertyInfo::REFERENCE) {
                if (p.second.getCardinality() == PropertyInfo::SCALAR) {
                    if (oi->isSet(p.first,
                                  PropertyInfo::REFERENCE,
                                  PropertyInfo::SCALAR)) {
                        reference_t u = oi->getReference(p.first);
                        visited.insert(u);
                        addRef(it, u);
                    }
                } else {
                    size_t c = oi->getReferenceSize(p.first);
                    for (size_t i = 0; i < c; ++i) {
                        reference_t u = oi->getReference(p.first, i);
                        visited.insert(u);
                        addRef(it, u);
                    }
                }
            }
        }
    }
    boost::unordered_set<reference_t> existing(it->details->urirefs);
    BOOST_FOREACH(const reference_t& up, existing) {
        if (visited.find(up) == visited.end()) {
            removeRef(it, up);
        }
    }

    if (curRefCount > 0) {
        resolveObj(ci.getType(), *it, newexp);
        newState = RESOLVED;
    } else if (oi) {
        if (declareObj(ci.getType(), *it, newexp))
            newState = IN_SYNC;
    } else if (newState == DELETED) {
        client->removeChildren(it->details->class_id,
                               it->uri,
                               &notifs);

        switch (ci.getType()) {
        case ClassInfo::POLICY:
            if (it->details->resolve_time > 0) {
                LOG(DEBUG) << "Unresolving " << it->uri.toString();
                vector<reference_t> refs;
                refs.push_back(make_pair(it->details->class_id, it->uri));
                PolicyUnresolveReq* req =
                    new PolicyUnresolveReq(this, nextXid++, refs);
                pool.sendToRole(req, OFConstants::POLICY_REPOSITORY);
            }
            break;
        case ClassInfo::REMOTE_ENDPOINT:
            if (it->details->resolve_time > 0) {
                LOG(DEBUG) << "Unresolving " << it->uri.toString();
                vector<reference_t> refs;
                refs.push_back(make_pair(it->details->class_id, it->uri));
                EndpointUnresolveReq* req =
                    new EndpointUnresolveReq(this, nextXid++, refs);
                pool.sendToRole(req, OFConstants::ENDPOINT_REGISTRY);
            }
            break;
        case ClassInfo::LOCAL_ENDPOINT:
            {
                LOG(DEBUG) << "Undeclaring " << it->uri.toString();
                vector<reference_t> refs;
                refs.push_back(make_pair(it->details->class_id, it->uri));
                EndpointUndeclareReq* req =
                    new EndpointUndeclareReq(this, nextXid++, refs);
                pool.sendToRole(req, OFConstants::ENDPOINT_REGISTRY);
            }
            break;
        default:
            // do nothing
            break;
        }

        LOG(DEBUG) << "Creating tombstone for " << it->uri.toString();
        newexp = now(&proc_loop) + TOMBSTONE_DELAY;
    }

    it->details->state = newState;
    exp_index.modify(it, Processor::change_expiration(newexp));

    guard.release();

    if (notifs.size() > 0)
        client->deliverNotifications(notifs);
}

void Processor::doProcess() {
    obj_state_by_exp::iterator it;
    while (proc_active) {
        {
            util::LockGuard guard(&item_mutex);
            if (!hasWork(it))
                break;
        }
        processItem(it);
    }
}

void Processor::proc_async_cb(uv_async_t* handle) {
    Processor* processor = (Processor*)handle->data;
    processor->doProcess();
}

void Processor::connect_async_cb(uv_async_t* handle) {
    Processor* processor = (Processor*)handle->data;
    processor->handleNewConnections();
}

static void register_listeners(void* processor, const modb::ClassInfo& ci) {
    Processor* p = (Processor*)processor;
    p->listen(ci.getId());
}

void Processor::proc_thread_func(void* processor_) {
    Processor* processor = (Processor*)processor_;
    uv_run(&processor->proc_loop, UV_RUN_DEFAULT);
}

void Processor::timer_callback(uv_timer_t* handle) {
    Processor* processor = (Processor*)handle->data;
    processor->doProcess();
}

void Processor::cleanup_async_cb(uv_async_t* handle) {
    Processor* processor = (Processor*)handle->data;
    uv_timer_stop(&processor->proc_timer);
    uv_close((uv_handle_t*)&processor->proc_timer, NULL);
    uv_close((uv_handle_t*)&processor->proc_async, NULL);
    uv_close((uv_handle_t*)&processor->connect_async, NULL);
    uv_close((uv_handle_t*)handle, NULL);
}

void Processor::start() {
    if (proc_active) return;
    proc_active = true;

    LOG(DEBUG) << "Starting OpFlex Processor";

    client = &store->getStoreClient("_SYSTEM_");
    store->forEachClass(&register_listeners, this);

    uv_loop_init(&proc_loop);
    uv_timer_init(&proc_loop, &proc_timer);
    cleanup_async.data = this;
    uv_async_init(&proc_loop, &cleanup_async, cleanup_async_cb);
    proc_async.data = this;
    uv_async_init(&proc_loop, &proc_async, proc_async_cb);
    connect_async.data = this;
    uv_async_init(&proc_loop, &connect_async, connect_async_cb);
    proc_timer.data = this;
    uv_timer_start(&proc_timer, &timer_callback,
                   processingDelay, processingDelay);
    uv_thread_create(&proc_thread, proc_thread_func, this);

    pool.start();
}

void Processor::stop() {
    if (!proc_active) return;

    LOG(DEBUG) << "Stopping OpFlex Processor";
    proc_active = false;

    unlisten();

    uv_async_send(&cleanup_async);
    uv_thread_join(&proc_thread);
    uv_loop_close(&proc_loop);

    pool.stop();
}

void Processor::objectUpdated(modb::class_id_t class_id,
                              const modb::URI& uri) {
    doObjectUpdated(class_id, uri, false);
}

void Processor::remoteObjectUpdated(modb::class_id_t class_id,
                                    const modb::URI& uri) {
    doObjectUpdated(class_id, uri, true);
}

// if remote is true, then the object is coming from the server,
// otherwise the object is coming from the listener, which could mean
// either local or remote.
void Processor::doObjectUpdated(modb::class_id_t class_id,
                                const modb::URI& uri,
                                bool remote) {
    util::LockGuard guard(&item_mutex);
    obj_state_by_uri& uri_index = obj_state.get<uri_tag>();
    obj_state_by_uri::iterator uit = uri_index.find(uri);

    uint64_t curtime = now(&proc_loop);
    uint64_t nexp = 0;
    if (uit != uri_index.end() && uit->details->state == DELETED) {
        // If there's a tombstone object in place, remove it and
        // re-add a fresh one with the "local" bit set correctly
        remote = !uit->details->local;
        uri_index.erase(uit);
        uit = uri_index.end();
    }
    if (!remote) nexp = curtime+processingDelay;
    if (uit == uri_index.end()) {
        obj_state.insert(item(uri, class_id,
                              nexp, LOCAL_REFRESH_RATE,
                              remote ? REMOTE : NEW, remote == false));
    } else if (uit->details->local) {
        uit->details->state = UPDATED;
        uri_index.modify(uit, change_expiration(nexp));
        uri_index.modify(uit, change_last_xid(0));
    } else {
        uri_index.modify(uit, change_expiration(curtime));
    }
    uv_async_send(&proc_async);
}

void Processor::setOpflexIdentity(const std::string& name,
                                  const std::string& domain) {
    pool.setOpflexIdentity(name, domain);
}

void Processor::setOpflexIdentity(const std::string& name,
                                  const std::string& domain,
                                  const std::string& location) {
    pool.setOpflexIdentity(name, domain, location);
}

void Processor::enableSSL(const std::string& caStorePath,
                          bool verifyPeers) {
    pool.enableSSL(caStorePath, verifyPeers);
}

void Processor::addPeer(const std::string& hostname,
                        int port) {
    pool.addPeer(hostname, port);
}

void
Processor::registerPeerStatusListener(ofcore::PeerStatusListener* listener) {
    pool.registerPeerStatusListener(listener);
}

OpflexHandler* Processor::newHandler(OpflexConnection* conn) {
    return new OpflexPEHandler(conn, this);
}

void Processor::handleNewConnections() {
    util::LockGuard guard(&item_mutex);
    BOOST_FOREACH(const item& i, obj_state) {
        uint64_t newexp = 0;
        const ClassInfo& ci = store->getClassInfo(i.details->class_id);
        if (i.details->state == IN_SYNC) {
            declareObj(ci.getType(), i, newexp);
        }
        if (i.details->state == RESOLVED) {
            resolveObj(ci.getType(), i, newexp, false);
        }
    }
}

void Processor::connectionReady(OpflexConnection* conn) {
    uv_async_send(&connect_async);
}

void Processor::responseReceived(uint64_t reqId) {
    util::LockGuard guard(&item_mutex);
    obj_state_by_xid& xid_index = obj_state.get<xid_tag>();
    obj_state_by_xid::iterator xi0,xi1;
    boost::tuples::tie(xi0,xi1)=xid_index.equal_range(reqId);

    unordered_set<URI> items;
    while (xi0 != xi1) {
        items.insert(xi0->uri);
        xi0++;
    }

    obj_state_by_uri& uri_index = obj_state.get<uri_tag>();

    BOOST_FOREACH(const URI& uri, items) {
        obj_state_by_uri::iterator uit = uri_index.find(uri);
        if (uit == uri_index.end()) continue;

        if (uit->details->pending_reqs > 0)
            uit->details->pending_reqs -= 1;

        if (uit->details->pending_reqs == 0) {
            // All peers responded to the message
            uri_index.modify(uit,
                             change_expiration(uit->details->resolve_time +
                                               uit->details->refresh_rate));
        }
    }
}

} /* namespace engine */
} /* namespace opflex */
