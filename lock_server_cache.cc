// the caching lock server implementation

#include "lock_server_cache.h"
#include <sstream>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "lang/verify.h"
#include "handle.h"
#include "tprintf.h"
#include "lock.h"
#include "zdebug.h"

lock_server_cache::lock_server_cache()
{
    pthread_mutex_init(&server_mutex, NULL);
}


int lock_server_cache::acquire(lock_protocol::lockid_t lid, std::string id, 
                               int &)
{
    map<lock_protocol::lockid_t, CacheLock>::iterator it;
    int ret = lock_protocol::RETRY;
    bool need_revoke = false;
    string holder_id;
    {
        ScopedLock _(&server_mutex);
        it = locks.find(lid);
        if (it == locks.end()) {
            CacheLock &c = locks[lid];
            c.client = id;
            c.state = CacheLock::LOCKED;
            ret = lock_protocol::OK;
        } else {
            CacheLock::State st = it->state;
            switch(it->state) {
                case CacheLock::FREE:
                    it->state = CacheLock::LOCKED;
                    it->client = id;
                    ret = lock_protocol::OK;
                    if (it->queue.size()) {
                        ERR("how can free lock has a queue");
                    }
                    break;
                case CacheLock::LOCKED:
                    it->state = CacheLock::LOCKED_AND_WAIT;
                    it->queue.push_back(id);
                    holder_id = it->client;
                    need_revoke = true;
                    ret = lock_protocol::RETRY;
                    break;
                case CacheLock::LOCKED_AND_WAIT:
                    // add to wait queue
                    it->queue.push_back(id);
                    ret = lock_protocol::RETRY;
                    break;
                case CacheLock::ORDERED:
                    if (!id.compare(it->front())) {
                        // he comes to get what he deserves
                        it->client = id;
                        string f = it->queue.pop_front();
                        if (f.compare(id)) {
                            ERR("front changed, huh?");
                        }
                        if (it->queue.size()) {
                            it->state = CacheLock::LOCKED_AND_WAIT;
                        } else {
                            it->state = CacheLock::LOCKED;
                        }
                    } else {
                        it->queue.push_back(id);
                        ret = lock_protocol::RETRY;
                    }
                default:
                    ret = lock_protocol::RETRY;
                    break;
            }
        }
    }
    if (need_revoke) {
        handle holder(holder_id);
        rpcc * cl = holder.safebind();
        if (cl) {
            int r;
            int rs = cl->call(rlock_protocol::revoke, lid, r);
            if (rs != rlock_protocol::OK) {
                ERR("call revoke failed");
                // TODO : should change the lock state back
            }
            ret = lock_protocol::RETRY;
        } else {
            ERR("cannot safe bind");
        }
    }
    return ret;
}

int 
lock_server_cache::release(lock_protocol::lockid_t lid, std::string id, 
         int &r)
{
    ScopedLock _(&server_mutex);
    map<lock_protocol::lockid_t, Lock>::iterator it;
    it = locks.find(lid);
    if (it != locks.end()) {
        switch(it->state) {
            case FREE:
                ERR("who are releasing a free lock????, %s", id.c_str());
                break;

            case CacheLock::LOCKED:
                it->state = CacheLock::FREE;
                it->client = "";
                if (it->queue.size()) {
                    ERR("nani ? queue size not zero");
                }
                break;

            case CacheLock::LOCKED_AND_WAIT:
                it->state = CacheLock::ORDERED;
                it->client = "";
                string addr = it->queue.front();
                handle h(addr);
                rpcc * cl = h.safebind();
                if (cl) {
                    int r;
                    int rs = cl->call(rlock_protocol::retry, lid, r);
                    if (rs != rlock_protocol::OK) {
                        ERR("retry failed");
                    }
                } else {
                    ERR("safe bind failed");
                }
                break;

            case CacheLock::ORDERED:
                ERR("who are releasing an ordered lock?? %s", id.c_str());
                break;

            default:
                break;
        }
    } else {
        ERR("releasing an non-existing lock");
    }
    return lock_protocol::OK;
}

lock_protocol::status
lock_server_cache::stat(lock_protocol::lockid_t lid, int &r)
{
  tprintf("stat request\n");
  r = nacquire;
  return lock_protocol::OK;
}
