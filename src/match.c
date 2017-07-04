/*
 * D-Bus Match Rules
 */

#include <c-list.h>
#include <c-macro.h>
#include <c-rbtree.h>
#include <c-string.h>
#include "dbus/address.h"
#include "dbus/protocol.h"
#include "match.h"
#include "util/error.h"

static int match_rules_compare(CRBTree *tree, void *k, CRBNode *rb) {
        MatchRule *rule = c_container_of(rb, MatchRule, owner_node);
        MatchRuleKeys *key1 = k, *key2 = &rule->keys;
        int r;

        if ((r = c_string_compare(key1->sender, key2->sender)) ||
            (r = c_string_compare(key1->destination, key2->destination)) ||
            (r = c_string_compare(key1->filter.interface, key2->filter.interface)) ||
            (r = c_string_compare(key1->filter.member, key2->filter.member)) ||
            (r = c_string_compare(key1->filter.path, key2->filter.path)) ||
            (r = c_string_compare(key1->path_namespace, key2->path_namespace)) ||
            (r = c_string_compare(key1->arg0namespace, key2->arg0namespace)))
                return r;

        if (key1->filter.type > key2->filter.type)
                return 1;
        if (key1->filter.type < key2->filter.type)
                return -1;

        if (key1->eavesdrop > key2->eavesdrop)
                return 1;
        if (key1->eavesdrop < key2->eavesdrop)
                return -1;

        for (unsigned int i = 0; i < C_ARRAY_SIZE(key1->filter.args); i ++) {
                if ((r = c_string_compare(key1->filter.args[i], key2->filter.args[i])))
                        return r;

                if ((r = c_string_compare(key1->filter.argpaths[i], key2->filter.argpaths[i])))
                        return r;
        }

        return 0;
}

static bool match_string_prefix(const char *string, const char *prefix, char delimiter, bool delimiter_included) {
        char *tail;

        if (string == prefix)
                return true;

        if (!string || !prefix)
                return false;

        tail = c_string_prefix(string, prefix);
        if (!tail)
                return false;

        if (delimiter_included) {
                if (tail == string || (*tail != '\0' && *(tail - 1) != delimiter))
                        return false;
        } else {
                if (*tail != '\0' && *tail != delimiter)
                        return false;
        }

        return true;
}

static bool match_rule_keys_match_filter(MatchRuleKeys *keys, MatchFilter *filter) {
        if (keys->filter.type != DBUS_MESSAGE_TYPE_INVALID && keys->filter.type != filter->type)
                return false;

        if (keys->filter.destination != ADDRESS_ID_INVALID && keys->filter.destination != filter->destination)
                return false;

        if (keys->filter.sender != ADDRESS_ID_INVALID && keys->filter.sender != filter->sender)
                return false;

        if (keys->filter.interface && !c_string_equal(keys->filter.interface, filter->interface))
                return false;

        if (keys->filter.member && !c_string_equal(keys->filter.member, filter->member))
                return false;

        if (keys->filter.path && !c_string_equal(keys->filter.path, filter->path))
                return false;

        if (keys->path_namespace && !match_string_prefix(keys->path_namespace, filter->path, '/', false))
                return false;

        /* XXX: verify that arg0 is a (potentially single-label) bus name */
        if (keys->arg0namespace && !match_string_prefix(keys->arg0namespace, filter->args[0], '.', false))
                return false;

        for (unsigned int i = 0; i < C_ARRAY_SIZE(filter->args); i ++) {
                if (keys->filter.args[i] && !c_string_equal(keys->filter.args[i], filter->args[i]))
                        return false;

                if (keys->filter.argpaths[i]) {
                        if (!match_string_prefix(filter->argpaths[i], keys->filter.argpaths[i], '/', true) &&
                            !match_string_prefix(keys->filter.argpaths[i], filter->argpaths[i], '/', true))
                                return false;
                }
        }

        return true;
}

static bool match_key_equal(const char *key1, const char *key2, size_t n_key2) {
        if (strlen(key1) != n_key2)
                return false;

        return (strncmp(key1, key2, n_key2) == 0);
}

static int match_rule_keys_assign(MatchRuleKeys *keys, const char *key, size_t n_key, const char *value) {
        Address addr;

        if (match_key_equal("type", key, n_key)) {
                if (keys->filter.type != DBUS_MESSAGE_TYPE_INVALID)
                        return MATCH_E_INVALID;

                if (strcmp(value, "signal") == 0)
                        keys->filter.type = DBUS_MESSAGE_TYPE_SIGNAL;
                else if (strcmp(value, "method_call") == 0)
                        keys->filter.type = DBUS_MESSAGE_TYPE_METHOD_CALL;
                else if (strcmp(value, "method_return") == 0)
                        keys->filter.type = DBUS_MESSAGE_TYPE_METHOD_RETURN;
                else if (strcmp(value, "error") == 0)
                        keys->filter.type = DBUS_MESSAGE_TYPE_ERROR;
                else
                        return MATCH_E_INVALID;
        } else if (match_key_equal("sender", key, n_key)) {
                if (keys->sender)
                        return MATCH_E_INVALID;
                keys->sender = value;
        } else if (match_key_equal("destination", key, n_key)) {
                if (keys->destination)
                        return MATCH_E_INVALID;
                keys->destination = value;

                address_from_string(&addr, value);
                if (addr.type == ADDRESS_TYPE_ID)
                        keys->filter.destination = addr.id;
                else
                        keys->filter.destination = ADDRESS_ID_INVALID;
        } else if (match_key_equal("interface", key, n_key)) {
                if (keys->filter.interface)
                        return MATCH_E_INVALID;
                keys->filter.interface = value;
        } else if (match_key_equal("member", key, n_key)) {
                if (keys->filter.member)
                        return MATCH_E_INVALID;
                keys->filter.member = value;
        } else if (match_key_equal("path", key, n_key)) {
                if (keys->filter.path || keys->path_namespace)
                        return MATCH_E_INVALID;
                keys->filter.path = value;
        } else if (match_key_equal("path_namespace", key, n_key)) {
                if (keys->path_namespace || keys->filter.path)
                        return MATCH_E_INVALID;
                keys->path_namespace = value;
        } else if (match_key_equal("eavesdrop", key, n_key)) {
                if (strcmp(value, "true") == 0)
                        keys->eavesdrop = true;
                else if (strcmp(value, "false") == 0)
                        keys->eavesdrop = false;
                else
                        return MATCH_E_INVALID;
        } else if (match_key_equal("arg0namespace", key, n_key)) {
                if (keys->arg0namespace || keys->filter.args[0] || keys->filter.argpaths[0])
                        return MATCH_E_INVALID;
                keys->arg0namespace = value;
        } else if (n_key >= strlen("arg") && match_key_equal("arg", key, strlen("arg"))) {
                unsigned int i = 0;

                key += strlen("arg");
                n_key -= strlen("arg");

                for (unsigned int j = 0; j < 2 && n_key; ++j, ++key, --n_key) {
                        if (*key < '0' || *key > '9')
                                break;

                        i = i * 10 + *key - '0';
                }

                if (i == 0 && keys->arg0namespace)
                        return MATCH_E_INVALID;
                if (i > 63)
                        return MATCH_E_INVALID;

                if (keys->filter.args[i] || keys->filter.argpaths[i])
                        return MATCH_E_INVALID;

                if (match_key_equal("", key, n_key)) {
                        keys->filter.args[i] = value;
                } else if (match_key_equal("path", key, n_key)) {
                        keys->filter.argpaths[i] = value;
                } else
                        return MATCH_E_INVALID;
        } else {
                return MATCH_E_INVALID;
        }

        return 0;
}

/*
 * Takes a null-termianted stream of characters, removes any quoting, breaks them up at commas and returns them one character at a time.
 */
static char match_string_value_pop(const char **match, bool *quoted) {
        /*
         * Within single quotes (apostrophe), a backslash represents itself, and an apostrophe ends the quoted section. Outside single quotes, \'
         * (backslash, apostrophe) represents an apostrophe, and any backslash not followed by an apostrophe represents itself.
         */
        while (**match == '\'') {
                (*match) ++;
                *quoted = !*quoted;
        }

        switch (**match) {
        case '\0':
                return '\0';
        case ',':
                (*match) ++;

                if (*quoted)
                        return ',';
                else
                        return '\0';
        case '\\':
                (*match) ++;

                if (!(*quoted) && **match == '\'') {
                        (*match) ++;
                        return '\'';
                } else {
                        return '\\';
                }
        default:
                return *((*match) ++);
        }
}

static int match_rule_key_read(const char **keyp, size_t *n_keyp, const char **match) {
        const char *key;
        size_t n_key = 0;

        /* skip any leading whitespace and stray equal signs */
        *match += strspn(*match, " \t\n\r=");
        if (!**match)
                return MATCH_E_EOF;

        /* skip over the key, recording its length */
        n_key = strcspn(*match, " \t\n\r=");
        key = *match;
        *match += n_key;
        if (!**match)
                return MATCH_E_INVALID;

        /* drop trailing whitespace */
        *match += strspn(*match, " \t\n\r");

        /* skip over the equals sign between the key and the value */
        if (**match != '=')
                return MATCH_E_INVALID;
        else
                ++*match;

        *keyp = key;
        *n_keyp = n_key;
        return 0;
}

static int match_rule_keys_parse(MatchRuleKeys *keys, char *buffer, size_t n_buffer, const char *rule_string) {
        size_t i = 0;
        int r = 0;

        keys->filter.type = DBUS_MESSAGE_TYPE_INVALID;
        keys->filter.destination = ADDRESS_ID_INVALID;
        keys->filter.sender = ADDRESS_ID_INVALID;

        while (i < n_buffer) {
                const char *key, *value;
                size_t n_key;
                bool quoted = false;
                char c;

                r = match_rule_key_read(&key, &n_key, &rule_string);
                if (r) {
                        if (r == MATCH_E_EOF)
                                break;
                        else
                                return error_trace(r);
                }

                value = buffer + i;

                do {
                        c = match_string_value_pop(&rule_string, &quoted);
                        buffer[i++] = c;
                } while (c);

                if (quoted)
                        return MATCH_E_INVALID;

                r = match_rule_keys_assign(keys, key, n_key, value);
                if (r)
                        return error_trace(r);
        }

        if (r != MATCH_E_EOF)
                /* this should not be possible */
                return error_origin(-ENOTRECOVERABLE);

        return 0;
}

static MatchRule *match_rule_free(MatchRule *rule) {
        if (!rule)
                return NULL;

        assert(!rule->n_user_refs);

        user_charge_deinit(&rule->charge[1]);
        user_charge_deinit(&rule->charge[0]);
        c_rbtree_remove_init(&rule->owner->rule_tree, &rule->owner_node);
        match_rule_unlink(rule);
        free(rule);

        return NULL;
}

C_DEFINE_CLEANUP(MatchRule *, match_rule_free);

static int match_rule_new(MatchRule **rulep, MatchOwner *owner, User *user, size_t n) {
        _c_cleanup_(match_rule_freep) MatchRule *rule = NULL;
        int r;

        rule = calloc(1, sizeof(*rule) + n);
        if (!rule)
                return error_origin(-ENOMEM);

        *rule = (MatchRule)MATCH_RULE_NULL(*rule);
        rule->owner = owner;

        r = user_charge(user, &rule->charge[0], NULL, USER_SLOT_BYTES, sizeof(*rule) + n);
        r = r ?: user_charge(user, &rule->charge[1], NULL, USER_SLOT_MATCHES, 1);
        if (r)
                return (r == USER_E_QUOTA) ? MATCH_E_QUOTA : error_fold(r);

        *rulep = rule;
        rule = NULL;
        return 0;
}

MatchRule *match_rule_user_ref(MatchRule *rule) {
        if (!rule)
                return NULL;

        assert(rule->n_user_refs > 0);

        ++rule->n_user_refs;

        return rule;
}

MatchRule *match_rule_user_unref(MatchRule *rule) {
        if (!rule)
                return NULL;

        assert(rule->n_user_refs > 0);

        --rule->n_user_refs;

        if (rule->n_user_refs == 0)
                match_rule_free(rule);

        return NULL;
}

void match_rule_link(MatchRule *rule, MatchRegistry *registry, bool monitor) {
        if (c_list_is_linked(&rule->registry_link)) {
                assert(rule->registry == registry);
                return;
        }

        rule->registry = registry;
        if (monitor)
                c_list_link_tail(&registry->monitor_list, &rule->registry_link);
        else if (rule->keys.eavesdrop)
                c_list_link_tail(&registry->eavesdrop_list, &rule->registry_link);
        else
                c_list_link_tail(&registry->rule_list, &rule->registry_link);
}

void match_rule_unlink(MatchRule *rule) {
        if (!rule->registry)
                return;

        c_list_unlink_init(&rule->registry_link);
        rule->registry = NULL;
}

int match_rule_get(MatchRule **rulep, MatchOwner *owner, const char *rule_string) {
        char buffer[strlen(rule_string) + 1];
        MatchRuleKeys keys = {};
        MatchRule *rule;
        int r;

        r = match_rule_keys_parse(&keys, buffer, sizeof(buffer), rule_string);
        if (r)
                return error_trace(r);

        rule = c_rbtree_find_entry(&owner->rule_tree, match_rules_compare, &keys, MatchRule, owner_node);
        if (!rule)
                return MATCH_E_NOT_FOUND;

        *rulep = rule;
        return 0;
}

static MatchRule *match_rule_next(MatchRegistry *registry, MatchRule *rule, bool unicast) {
        if (!rule) {
                rule = c_list_first_entry(&registry->eavesdrop_list, MatchRule, registry_link);
                if (rule)
                        return rule;

                if (unicast)
                        return NULL;

                return c_list_first_entry(&registry->rule_list, MatchRule, registry_link);
        } else if (rule->keys.eavesdrop) {
                if (rule != c_list_last_entry(&registry->eavesdrop_list, MatchRule, registry_link))
                        return c_list_entry(rule->registry_link.next, MatchRule, registry_link);

                if (unicast)
                        return NULL;

                return c_list_first_entry(&registry->rule_list, MatchRule, registry_link);
        } else {
                if (rule != c_list_last_entry(&registry->rule_list, MatchRule, registry_link))
                        return c_list_entry(rule->registry_link.next, MatchRule, registry_link);
        }

        return NULL;
}

MatchRule *match_rule_next_match(MatchRegistry *registry, MatchRule *rule, MatchFilter *filter) {
        bool unicast = filter->destination != ADDRESS_ID_INVALID;

        for (rule = match_rule_next(registry, rule, unicast); rule; rule = match_rule_next(registry, rule, unicast))
                if (match_rule_keys_match_filter(&rule->keys, filter))
                        return rule;

        return NULL;
}

MatchRule *match_rule_next_monitor_match(MatchRegistry *registry, MatchRule *rule, MatchFilter *filter) {
        if (rule)
                rule = c_list_entry(rule->registry_link.next, MatchRule, registry_link);
        else
                rule = c_list_first_entry(&registry->monitor_list, MatchRule, registry_link);

        for (; rule != c_list_last_entry(&registry->monitor_list, MatchRule, registry_link);
               rule = c_list_entry(rule->registry_link.next, MatchRule, registry_link))
                if (match_rule_keys_match_filter(&rule->keys, filter))
                        return rule;

        return NULL;
}

/**
 * match_owner_init() - XXX
 */
void match_owner_init(MatchOwner *owner) {
        *owner = (MatchOwner)MATCH_OWNER_INIT;
}

/**
 * match_owner_deinit() - XXX
 */
void match_owner_deinit(MatchOwner *owner) {
        assert(c_rbtree_is_empty(&owner->rule_tree));
}

/**
 * match_owner_ref_rule() - XXX
 */
int match_owner_ref_rule(MatchOwner *owner, MatchRule **rulep, User *user, const char *rule_string) {
        _c_cleanup_(match_rule_user_unrefp) MatchRule *rule = NULL;
        CRBNode **slot, *parent;
        size_t n_buffer;
        int r;

        /* the buffer needs at most the size of the string */
        n_buffer = strlen(rule_string) + 1;

        r = match_rule_new(&rule, owner, user, n_buffer);
        if (r)
                return error_trace(r);

        ++rule->n_user_refs;

        r = match_rule_keys_parse(&rule->keys, rule->buffer, n_buffer, rule_string);
        if (r)
                return error_trace(r);

        slot = c_rbtree_find_slot(&owner->rule_tree, match_rules_compare, &rule->keys, &parent);
        if (!slot) {
                /* one already exists, take a ref on that instead and drop the one we created */
                if (rulep)
                        *rulep = match_rule_user_ref(c_container_of(parent, MatchRule, owner_node));
        } else {
                /* link the new rule into the rbtree */
                c_rbtree_add(&owner->rule_tree, parent, slot, &rule->owner_node);
                if (rulep)
                        *rulep = rule;
                rule = NULL;
        }

        return 0;
}

/**
 * match_registry_init() - XXX
 */
void match_registry_init(MatchRegistry *registry) {
        *registry = (MatchRegistry)MATCH_REGISTRY_INIT(*registry);
}

/**
 * match_registry_deinit() - XXX
 */
void match_registry_deinit(MatchRegistry *registry) {
        assert(c_list_is_empty(&registry->rule_list));
        assert(c_list_is_empty(&registry->eavesdrop_list));
        assert(c_list_is_empty(&registry->monitor_list));
}
