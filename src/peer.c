/*
 * Peers
 */

#include <c-macro.h>
#include <c-rbtree.h>
#include <grp.h>
#include <pwd.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include "bus.h"
#include "dbus/address.h"
#include "dbus/message.h"
#include "dbus/protocol.h"
#include "dbus/socket.h"
#include "driver.h"
#include "match.h"
#include "name.h"
#include "peer.h"
#include "reply.h"
#include "util/dispatch.h"
#include "util/error.h"
#include "util/fdlist.h"
#include "util/metrics.h"
#include "util/user.h"

static int peer_dispatch_connection(Peer *peer, uint32_t events) {
        int r;

        r = connection_dispatch(&peer->connection, events);
        if (r)
                return error_fold(r);

        for (;;) {
                _c_cleanup_(message_unrefp) Message *m = NULL;

                r = connection_dequeue(&peer->connection, &m);
                if (r || !m) {
                        if (r == CONNECTION_E_EOF)
                                return PEER_E_EOF;

                        return error_fold(r);
                }

                metrics_sample_start(&peer->bus->metrics);
                r = driver_dispatch(peer, m);
                metrics_sample_end(&peer->bus->metrics);
                if (r) {
                        if (r == DRIVER_E_PROTOCOL_VIOLATION)
                                return PEER_E_PROTOCOL_VIOLATION;

                        return error_fold(r);
                }
        }

        return 0;
}

static int peer_dispatch(DispatchFile *file, uint32_t mask) {
        Peer *peer = c_container_of(file, Peer, connection.socket_file);
        static const uint32_t interest[] = { EPOLLIN | EPOLLHUP, EPOLLOUT };
        size_t i;
        int r;

        /*
         * Usually, we would just call peer_dispatch_connection(peer, mask)
         * here. However, a very common scenario is to dispatch D-Bus driver
         * calls. Those calls fetch an incoming message from a peer, handle it
         * and then immediately queue a reply. In those cases we want EPOLLOUT
         * to be handled late.
         * Hence, rather than dispatching the connection in one go, we rather
         * split it into two:
         *
         *     peer_dispatch_connection(peer, EPOLLIN | EPOLLHUP);
         *     peer_dispatch_connection(peer, EPOLLOUT);
         *
         * This makes sure to first handle all the incoming messages, then the
         * outgointg messages.
         *
         * Note that it is not enough to simply delay the call to
         * connection_dispatch(EPOLLOUT). The socket API requires you to loop
         * over connection_dequeue() after *ANY* call to the dispatcher. This
         * is, because the dequeue function is considered to be the event
         * handler, and as such the only function that performs forward
         * progress on the socket.
         *
         * Furthermore, note that we must ignore @mask but rather query
         * dispatch_file_events(), since the connection handler might select or
         * deselect events we want to handle.
         *
         * Lastly, the connection API explicitly allows splitting the events.
         * There is no requirement to provide them in-order.
         */
        for (i = 0; i < C_ARRAY_SIZE(interest); ++i) {
                if (dispatch_file_events(file) & interest[i]) {
                        r = peer_dispatch_connection(peer, mask & interest[i]);
                        if (r)
                                break;
                }
        }

        if (r) {
                if (r == PEER_E_EOF) {
                        r = driver_goodbye(peer, false);
                        if (r)
                                return error_fold(r);

                        connection_shutdown(&peer->connection);
                } else if (r == PEER_E_PROTOCOL_VIOLATION) {
                        connection_close(&peer->connection);

                        r = driver_goodbye(peer, false);
                        if (r)
                                return error_fold(r);
                } else {
                        return error_fold(r);
                }

                if (!connection_is_running(&peer->connection))
                        peer_free(peer);
        }

        /* Careful: @peer might be deallocated here */

        return 0;
}

static int peer_get_peersec(int fd, char **labelp, size_t *lenp) {
        _c_cleanup_(c_freep) char *label = NULL;
        char *l;
        socklen_t len = 1023;
        int r;

        label = malloc(len + 1);
        if (!label)
                return error_origin(-ENOMEM);

        for (;;) {
                r = getsockopt(fd, SOL_SOCKET, SO_PEERSEC, label, &len);
                if (r >= 0) {
                        label[len] = '\0';
                        *lenp = len;
                        *labelp = label;
                        label = NULL;
                        break;
                } else if (errno == ENOPROTOOPT) {
                        *lenp = 0;
                        *labelp = NULL;
                        break;
                } else if (errno != ERANGE)
                        return -errno;

                l = realloc(label, len + 1);
                if (!l)
                        return error_origin(-ENOMEM);

                label = l;
        }

        return 0;
}

static int peer_get_peergroups(int fd, uid_t uid, gid_t gid, gid_t **gidsp, size_t *n_gidsp) {
        _c_cleanup_(c_freep) gid_t *gids = NULL;
        struct passwd *passwd;
        int r, n_gids = 64;
        void *tmp;

        #ifdef SO_PEERGROUPS
        {
                socklen_t socklen = n_gids * sizeof(*gids);

                gids = malloc(sizeof(gid) + socklen);
                if (!gids)
                        return error_origin(-ENOMEM);
                gids[0] = gid;

                r = getsockopt(fd, SOL_SOCKET, SO_PEERGROUPS, gids + 1, &socklen);
                if (r < 0 && errno == ERANGE) {
                        tmp = realloc(gids, sizeof(gid) + socklen);
                        if (!tmp)
                                return error_origin(-ENOMEM);
                        gids = tmp;
                        gids[0] = gid;

                        r = getsockopt(fd, SOL_SOCKET, SO_PEERGROUPS, gids + 1, &socklen);
                }
                if (r < 0 && errno != ENOPROTOOPT) {
                        return error_origin(-errno);
                } else if (r >= 0) {
                        *gidsp = gids;
                        gids = NULL;
                        *n_gidsp = 1 + socklen / sizeof(*gids);
                        return 0;
                }
        }
        #endif

        {
                static bool warned;

                if (!warned) {
                        warned = true;
                        fprintf(stderr, "Falling back to resolving auxillary groups using nss, "
                                        "this is racy and may cause deadlocks. Update to a kernel with "
                                        "SO_PEERGROUPS support.\n");
                }
        }

        passwd = getpwuid(uid);
        if (!passwd)
                return error_origin(-errno);

        do {
                int n_gids_previous = n_gids;

                tmp = realloc(gids, sizeof(*gids) * n_gids);
                if (!tmp)
                        return error_origin(-ENOMEM);

                gids = tmp;
                r = getgrouplist(passwd->pw_name, passwd->pw_gid, gids, &n_gids);
                if (r == -1 && n_gids <= n_gids_previous)
                        return error_origin(-ENOTRECOVERABLE);
        } while (r == -1);

        *gidsp = gids;
        gids = NULL;
        *n_gidsp = n_gids;
        return 0;
}

static int peer_compare(CRBTree *tree, void *k, CRBNode *rb) {
        Peer *peer = c_container_of(rb, Peer, registry_node);
        uint64_t id = *(uint64_t*)k;

        if (id < peer->id)
                return -1;
        if (id > peer->id)
                return 1;

        return 0;
}

/**
 * peer_new() - XXX
 */
int peer_new_with_fd(Peer **peerp,
                     Bus *bus,
                     PolicyRegistry *policy,
                     const char guid[],
                     DispatchContext *dispatcher,
                     int fd) {
        _c_cleanup_(peer_freep) Peer *peer = NULL;
        _c_cleanup_(user_unrefp) User *user = NULL;
        _c_cleanup_(c_freep) gid_t *gids = NULL;
        _c_cleanup_(c_freep) char *seclabel = NULL;
        CRBNode **slot, *parent;
        size_t n_seclabel, n_gids = 0;
        struct ucred ucred;
        socklen_t socklen = sizeof(ucred);
        int r;

        r = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &socklen);
        if (r < 0)
                return error_origin(-errno);

        r = user_registry_ref_user(&bus->users, &user, ucred.uid);
        if (r < 0)
                return error_fold(r);

        r = peer_get_peersec(fd, &seclabel, &n_seclabel);
        if (r < 0)
                return error_trace(r);

        if (policy_registry_needs_groups(policy)) {
                r = peer_get_peergroups(fd, ucred.uid, ucred.gid, &gids, &n_gids);
                if (r)
                        return error_trace(r);
        }

        peer = calloc(1, sizeof(*peer));
        if (!peer)
                return error_origin(-ENOMEM);

        peer->bus = bus;
        peer->connection = (Connection)CONNECTION_NULL(peer->connection);
        peer->registry_node = (CRBNode)C_RBNODE_INIT(peer->registry_node);
        peer->user = user;
        user = NULL;
        peer->pid = ucred.pid;
        peer->seclabel = seclabel;
        seclabel = NULL;
        peer->n_seclabel = n_seclabel;
        peer->charges[0] = (UserCharge)USER_CHARGE_INIT;
        peer->charges[1] = (UserCharge)USER_CHARGE_INIT;
        peer->charges[2] = (UserCharge)USER_CHARGE_INIT;
        peer->policy = (PeerPolicy)PEER_POLICY_INIT;
        peer->owned_names = (NameOwner)NAME_OWNER_INIT;
        peer->matches = (MatchRegistry)MATCH_REGISTRY_INIT(peer->matches);
        peer->owned_matches = (MatchOwner)MATCH_OWNER_INIT;
        peer->replies_outgoing = (ReplyRegistry)REPLY_REGISTRY_INIT;
        peer->owned_replies = (ReplyOwner)REPLY_OWNER_INIT(peer->owned_replies);

        r = user_charge(user, &peer->charges[0], NULL, USER_SLOT_BYTES, sizeof(Peer));
        r = r ?: user_charge(user, &peer->charges[1], NULL, USER_SLOT_FDS, 1);
        r = r ?: user_charge(user, &peer->charges[2], NULL, USER_SLOT_OBJECTS, 1);
        if (r) {
                if (r == USER_E_QUOTA)
                        return PEER_E_QUOTA;

                return error_fold(r);
        }

        r = peer_policy_instantiate(&peer->policy, policy, ucred.uid, gids, n_gids);
        if (r) {
                if (r == POLICY_E_ACCESS_DENIED)
                        return PEER_E_CONNECTION_REFUSED;

                return error_fold(r);
        }

        r = connection_init_server(&peer->connection,
                                   dispatcher,
                                   peer_dispatch,
                                   peer->user,
                                   guid,
                                   fd);
        if (r < 0)
                return error_fold(r);

        peer->id = bus->peers.ids++;
        slot = c_rbtree_find_slot(&bus->peers.peer_tree, peer_compare, &peer->id, &parent);
        assert(slot); /* peer->id is guaranteed to be unique */
        c_rbtree_add(&bus->peers.peer_tree, parent, slot, &peer->registry_node);

        *peerp = peer;
        peer = NULL;
        return 0;
}

/**
 * peer_free() - XXX
 */
Peer *peer_free(Peer *peer) {
        int fd;

        if (!peer)
                return NULL;

        assert(!peer->registered);

        c_rbtree_remove_init(&peer->bus->peers.peer_tree, &peer->registry_node);

        fd = peer->connection.socket.fd;

        reply_owner_deinit(&peer->owned_replies);
        reply_registry_deinit(&peer->replies_outgoing);
        match_owner_deinit(&peer->owned_matches);
        match_registry_deinit(&peer->matches);
        name_owner_deinit(&peer->owned_names);
        peer_policy_deinit(&peer->policy);
        connection_deinit(&peer->connection);
        user_unref(peer->user);
        user_charge_deinit(&peer->charges[2]);
        user_charge_deinit(&peer->charges[1]);
        user_charge_deinit(&peer->charges[0]);
        free(peer->seclabel);
        free(peer);

        close(fd);

        return NULL;
}

int peer_spawn(Peer *peer) {
        return error_fold(connection_open(&peer->connection));
}

void peer_register(Peer *peer) {
        assert(!peer->registered);
        assert(!peer->monitor);

        peer->registered = true;
}

void peer_unregister(Peer *peer) {
        assert(peer->registered);
        assert(!peer->monitor);

        peer->registered = false;
}

bool peer_is_privileged(Peer *peer) {
        if (peer->user->uid == 0)
                return true;

        if (peer->user->uid == peer->bus->user->uid)
                return true;

        return false;
}

int peer_request_name(Peer *peer, const char *name, uint32_t flags, NameChange *change) {
        int r;

        if (!strcmp(name, "org.freedesktop.DBus"))
                return PEER_E_NAME_RESERVED;

        if (name[0] == ':')
                return PEER_E_NAME_UNIQUE;

        /* XXX: refuse invalid names */

        r = peer_policy_check_own(&peer->policy, name);
        if (r) {
                if (r == POLICY_E_ACCESS_DENIED)
                        return PEER_E_NAME_REFUSED;

                return error_fold(r);
        }

        r = name_registry_request_name(&peer->bus->names,
                                       &peer->owned_names,
                                       peer->user,
                                       name,
                                       flags,
                                       change);
        if (r == NAME_E_QUOTA)
                return PEER_E_QUOTA;
        else if (r == NAME_E_ALREADY_OWNER)
                return PEER_E_NAME_ALREADY_OWNER;
        else if (r == NAME_E_IN_QUEUE)
                return PEER_E_NAME_IN_QUEUE;
        else if (r == NAME_E_EXISTS)
                return PEER_E_NAME_EXISTS;
        else if (r)
                return error_fold(r);

        return 0;
}

int peer_release_name(Peer *peer, const char *name, NameChange *change) {
        int r;

        if (!strcmp(name, "org.freedesktop.DBus"))
                return PEER_E_NAME_RESERVED;

        if (name[0] == ':')
                return PEER_E_NAME_UNIQUE;

        /* XXX: refuse invalid names */

        r = name_registry_release_name(&peer->bus->names, &peer->owned_names, name, change);
        if (r == NAME_E_NOT_FOUND)
                return PEER_E_NAME_NOT_FOUND;
        else if (r == NAME_E_NOT_OWNER)
                return PEER_E_NAME_NOT_OWNER;
        else if (r)
                return error_fold(r);

        return 0;
}

void peer_release_name_ownership(Peer *peer, NameOwnership *ownership, NameChange *change) {
        name_ownership_release(ownership, change);
}

static int peer_link_match(Peer *peer, MatchRule *rule, bool monitor) {
        Address addr;
        Peer *sender;
        int r;

        if (!rule->keys.sender) {
                match_rule_link(rule, &peer->bus->wildcard_matches, monitor);
        } else if (strcmp(rule->keys.sender, "org.freedesktop.DBus") == 0) {
                match_rule_link(rule, &peer->bus->driver_matches, monitor);
        } else {
                address_from_string(&addr, rule->keys.sender);
                switch (addr.type) {
                case ADDRESS_TYPE_ID: {
                        sender = peer_registry_find_peer(&peer->bus->peers, addr.id);
                        if (sender) {
                                match_rule_link(rule, &sender->matches, monitor);
                        } else if (addr.id >= peer->bus->peers.ids) {
                                /*
                                 * This peer does not yet exist, but it could
                                 * appear, keep it with the wildcards. It will
                                 * stay there even if the peer later appears.
                                 * This works and is meant for compatibility.
                                 * It does not perform nicely, but there is
                                 * also no reason to ever guess the ID of a
                                 * forthcoming peer.
                                 */
                                rule->keys.filter.sender = addr.id;
                                match_rule_link(rule, &peer->bus->wildcard_matches, monitor);
                        } else {
                                /*
                                 * The peer has already disconnected and will
                                 * never reappear, since the ID allocator is
                                 * already beyond the ID.
                                 * We can simply skip linking the rule, since
                                 * it can never have an effect. It stays linked
                                 * in its owner, though, so we don't lose
                                 * track.
                                 */
                        }
                        break;
                }
                case ADDRESS_TYPE_NAME:
                case ADDRESS_TYPE_OTHER: {
                        /*
                         * XXX: dbus-daemon rejects any match on invalid names.
                         *      However, we cannot do this here as our caller
                         *      does not expect this. This needs some further
                         *      restructuring.
                         */
                        _c_cleanup_(name_unrefp) Name *name = NULL;

                        r = name_registry_ref_name(&peer->bus->names, &name, rule->keys.sender);
                        if (r)
                                return error_fold(r);

                        match_rule_link(rule, &name->matches, monitor);
                        name_ref(name); /* this reference must be explicitly released */
                        break;
                }
                default:
                        return error_origin(-ENOTRECOVERABLE);
                }
        }

        return 0;
}

int peer_add_match(Peer *peer, const char *rule_string, bool force_eavesdrop) {
        _c_cleanup_(match_rule_user_unrefp) MatchRule *rule = NULL;
        int r;

        r = match_owner_ref_rule(&peer->owned_matches, &rule, peer->user, rule_string);
        if (r) {
                if (r == MATCH_E_QUOTA)
                        return PEER_E_QUOTA;
                else if (r == MATCH_E_INVALID)
                        return PEER_E_MATCH_INVALID;
                else
                        return error_fold(r);
        }

        if (force_eavesdrop)
                rule->keys.eavesdrop = true;

        r = peer_link_match(peer, rule, false);
        if (r)
                return error_trace(r);

        rule = NULL;

        return 0;
}

int peer_remove_match(Peer *peer, const char *rule_string) {
        _c_cleanup_(name_unrefp) Name *name = NULL;
        MatchRule *rule;
        int r;

        r = match_owner_find_rule(&peer->owned_matches, &rule, rule_string);
        if (r == MATCH_E_INVALID)
                return PEER_E_MATCH_INVALID;
        else if (r)
                return error_fold(r);
        else if (!rule)
                return PEER_E_MATCH_NOT_FOUND;

        if (rule->keys.sender && *rule->keys.sender != ':' && strcmp(rule->keys.sender, "org.freedesktop.DBus") != 0)
                name = c_container_of(rule->registry, Name, matches);

        match_rule_user_unref(rule);

        return 0;
}

int peer_become_monitor(Peer *peer, MatchOwner *owned_matches) {
        MatchRule *rule;
        size_t n_matches = 0;
        int r, poison = 0;

        assert(!peer->registered);
        assert(!peer->monitor);
        assert(c_rbtree_is_empty(&peer->owned_matches.rule_tree));

        /* only fatal errors may occur after this point */
        peer->owned_matches = *owned_matches;
        *owned_matches = (MatchOwner)MATCH_OWNER_INIT;

        c_rbtree_for_each_entry(rule, &peer->owned_matches.rule_tree, owner_node) {

                rule->keys.eavesdrop = true;
                rule->owner = &peer->owned_matches;

                r = peer_link_match(peer, rule, true);
                if (r && !poison)
                        poison = error_trace(r);

                ++n_matches;
        }

        if (poison)
                /* a fatal error occured, the peer was modified, but still consistent */
                return poison;

        peer->monitor = true;

        return 0;
}

void peer_flush_matches(Peer *peer) {
        CRBNode *node;

        while ((node = peer->owned_matches.rule_tree.root)) {
                _c_cleanup_(name_unrefp) Name *name = NULL;
                MatchRule *rule = c_container_of(node, MatchRule, owner_node);

                if (rule->keys.sender && *rule->keys.sender != ':' && strcmp(rule->keys.sender, "org.freedesktop.DBus") != 0)
                        name = c_container_of(rule->registry, Name, matches);

                match_rule_user_unref(rule);
        }
}

int peer_queue_call(PeerPolicy *sender_policy, NameSet *sender_names, MatchRegistry *sender_matches, ReplyOwner *sender_replies, User *sender_user, uint64_t sender_id, Peer *receiver, Message *message) {
        _c_cleanup_(reply_slot_freep) ReplySlot *slot = NULL;
        NameSet receiver_names = NAME_SET_INIT_FROM_OWNER(&receiver->owned_names);
        int r;

        if (sender_replies &&
            (message->header->type == DBUS_MESSAGE_TYPE_METHOD_CALL) &&
            !(message->header->flags & DBUS_HEADER_FLAG_NO_REPLY_EXPECTED)) {
                r = reply_slot_new(&slot, &receiver->replies_outgoing, sender_replies,
                                   receiver->user, sender_user, sender_id, message_read_serial(message));
                if (r == REPLY_E_EXISTS)
                        return PEER_E_EXPECTED_REPLY_EXISTS;
                else if (r == REPLY_E_QUOTA)
                        return PEER_E_QUOTA;
                else if (r)
                        return error_fold(r);
        }

        r = peer_policy_check_receive(&receiver->policy, sender_names,
                                      message->metadata.fields.interface, message->metadata.fields.member,
                                      message->metadata.fields.path, message->header->type);
        if (r) {
                if (r == POLICY_E_ACCESS_DENIED)
                        return PEER_E_RECEIVE_DENIED;

                return error_fold(r);
        }

        r = peer_policy_check_send(sender_policy, &receiver_names,
                                   message->metadata.fields.interface, message->metadata.fields.member,
                                   message->metadata.fields.path, message->header->type);
        if (r) {
                if (r == POLICY_E_ACCESS_DENIED)
                        return PEER_E_SEND_DENIED;

                return error_fold(r);
        }

        r = connection_queue(&receiver->connection, sender_user, 0, message);
        if (r) {
                if (CONNECTION_E_QUOTA)
                        return PEER_E_QUOTA;
                else
                        return error_fold(r);
        }

        /* for eavesdropping */
        r = peer_broadcast(sender_policy, sender_names, sender_matches, sender_id, receiver, receiver->bus, NULL, message);
        if (r)
                return error_trace(r);

        slot = NULL;
        return 0;
}

int peer_queue_reply(Peer *sender, const char *destination, uint32_t reply_serial, Message *message) {
        _c_cleanup_(reply_slot_freep) ReplySlot *slot = NULL;
        NameSet sender_names = NAME_SET_INIT_FROM_OWNER(&sender->owned_names);
        Peer *receiver;
        Address addr;
        int r;

        address_from_string(&addr, destination);
        if (addr.type != ADDRESS_TYPE_ID)
                return PEER_E_UNEXPECTED_REPLY;

        slot = reply_slot_get_by_id(&sender->replies_outgoing, addr.id, reply_serial);
        if (!slot)
                return PEER_E_UNEXPECTED_REPLY;

        receiver = c_container_of(slot->owner, Peer, owned_replies);

        r = connection_queue(&receiver->connection, NULL, 0, message);
        if (r) {
                if (r == CONNECTION_E_QUOTA)
                        connection_shutdown(&receiver->connection);
                else
                        return error_fold(r);
        }

        /* for eavesdropping */
        r = peer_broadcast(&sender->policy, &sender_names, &sender->matches, sender->id, receiver, sender->bus, NULL, message);
        if (r)
                return error_trace(r);


        return 0;
}

static int peer_broadcast_to_matches(PeerPolicy *sender_policy, NameSet *sender_names, MatchRegistry *matches, MatchFilter *filter, uint64_t transaction_id, Message *message) {
        MatchRule *rule;
        int r;

        for (rule = match_rule_next_match(matches, NULL, filter); rule; rule = match_rule_next_match(matches, rule, filter)) {
                Peer *receiver = c_container_of(rule->owner, Peer, owned_matches);
                NameSet receiver_names = NAME_SET_INIT_FROM_OWNER(&receiver->owned_names);

                /* exclude the destination from broadcasts */
                if (filter->destination == receiver->id)
                        continue;

                if (sender_policy) {
                        r = peer_policy_check_send(sender_policy, &receiver_names,
                                                   message->metadata.fields.interface, message->metadata.fields.member,
                                                   message->metadata.fields.path, message->header->type);
                        if (r) {
                                if (r == POLICY_E_ACCESS_DENIED)
                                        continue;

                                return error_fold(r);
                        }
                }

                r = peer_policy_check_receive(&receiver->policy, sender_names,
                                              message->metadata.fields.interface, message->metadata.fields.member,
                                              message->metadata.fields.path, message->header->type);
                if (r) {
                        if (r == POLICY_E_ACCESS_DENIED)
                                continue;

                        return error_fold(r);
                }

                r = connection_queue(&receiver->connection, NULL, transaction_id, message);
                if (r) {
                        if (r == CONNECTION_E_QUOTA)
                                connection_shutdown(&receiver->connection);
                        else
                                return error_fold(r);
                }
        }

        return 0;
}

int peer_broadcast(PeerPolicy *sender_policy, NameSet *sender_names, MatchRegistry *sender_matches, uint64_t sender_id, Peer *destination, Bus *bus, MatchFilter *filter, Message *message) {
        MatchFilter fallback_filter = MATCH_FILTER_INIT;
        int r;

        if (!filter) {
                filter = &fallback_filter;

                filter->type = message->metadata.header.type;
                filter->sender = sender_id;
                filter->destination = destination ? destination->id : ADDRESS_ID_INVALID;
                filter->interface = message->metadata.fields.interface;
                filter->member = message->metadata.fields.member,
                filter->path = message->metadata.fields.path;

                for (size_t i = 0; i < 64; ++i) {
                        if (message->metadata.args[i].element == 's') {
                                filter->args[i] = message->metadata.args[i].value;
                                filter->argpaths[i] = message->metadata.args[i].value;
                        } else if (message->metadata.args[i].element == 'o') {
                                filter->argpaths[i] = message->metadata.args[i].value;
                        }
                }
        }

        /* start a new transaction, to avoid duplicates */
        ++bus->transaction_ids;

        r = peer_broadcast_to_matches(sender_policy, sender_names, &bus->wildcard_matches, filter, bus->transaction_ids, message);
        if (r)
                return error_trace(r);

        if (sender_matches) {
                r = peer_broadcast_to_matches(sender_policy, sender_names, sender_matches, filter, bus->transaction_ids, message);
                if (r)
                        return error_trace(r);
        }

        if (sender_names) {
                NameOwner *owner;
                NameOwnership *ownership;
                NameSnapshot *snapshot;

                switch (sender_names->type) {
                case NAME_SET_TYPE_OWNER:
                        owner = sender_names->owner;

                        c_rbtree_for_each_entry(ownership, &owner->ownership_tree, owner_node) {
                                if (!name_ownership_is_primary(ownership))
                                        continue;

                                r = peer_broadcast_to_matches(sender_policy, sender_names, &ownership->name->matches, filter, bus->transaction_ids, message);
                                if (r)
                                        return error_trace(r);
                        }
                        break;
                case NAME_SET_TYPE_SNAPSHOT:
                        snapshot = sender_names->snapshot;

                        for (size_t i = 0; i < snapshot->n_names; ++i) {
                                r = peer_broadcast_to_matches(sender_policy, sender_names, &snapshot->names[i]->matches, filter, bus->transaction_ids, message);
                                if (r)
                                        return error_trace(r);
                        }
                        break;
                default:
                        return error_origin(-ENOTRECOVERABLE);
                }
        } else {
                /* sent from the driver */
                r = peer_broadcast_to_matches(NULL, NULL, &bus->driver_matches, filter, bus->transaction_ids, message);
                if (r)
                        return error_trace(r);
        }

        return 0;
}

void peer_registry_init(PeerRegistry *registry) {
        *registry = (PeerRegistry)PEER_REGISTRY_INIT;
}

void peer_registry_deinit(PeerRegistry *registry) {
        assert(c_rbtree_is_empty(&registry->peer_tree));
        registry->ids = 0;
}

void peer_registry_flush(PeerRegistry *registry) {
        Peer *peer, *safe;
        int r;

        c_rbtree_for_each_entry_unlink(peer, safe, &registry->peer_tree, registry_node) {
                r = driver_goodbye(peer, true);
                assert(!r); /* can not fail in silent mode */
                peer_free(peer);
        }
}

Peer *peer_registry_find_peer(PeerRegistry *registry, uint64_t id) {
        Peer *peer;

        peer = c_rbtree_find_entry(&registry->peer_tree, peer_compare, &id, Peer, registry_node);

        return peer && peer->registered ? peer : NULL;
}
