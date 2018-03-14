#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/hashtable.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter/x_tables.h>
#include <net/netns/hash.h>
#include <net/netfilter/nf_nat.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_tuple.h>
#include <net/netfilter/nf_conntrack_core.h>
#include <net/netfilter/nf_conntrack_ecache.h>

#define HASH_2(x, y) ((x + y) / 2 * (x + y + 1) + y)

#define HASHTABLE_BUCKET_BITS 10

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)

static inline int nf_ct_netns_get(struct net *net, u8 nfproto) { return 0; }

static inline void nf_ct_netns_put(struct net *net, u8 nfproto) {}

static inline struct net_device *xt_in(const struct xt_action_param *par) {
  return par->in;
}

static inline struct net_device *xt_out(const struct xt_action_param *par) {
  return par->out;
}

static inline unsigned int xt_hooknum(const struct xt_action_param *par) {
  return par->hooknum;
}

#endif

struct nat_mapping_original_tuple {
  struct nf_conntrack_tuple tuple;
  struct net *net;
  struct nf_conntrack_zone *zone;

  struct list_head node;
};

struct nat_mapping {
  uint16_t port;     /* external UDP port */
  __be32 int_addr;   /* internal source ip address */
  uint16_t int_port; /* internal source port */
  int ifindex;       /* external interface index*/
  int refer_count;   /* how many references linked to this mapping
                      * aka. length of original_tuples */

  struct list_head original_tuples;

  struct hlist_node node_by_ext_port;
  struct hlist_node node_by_original_src;

};

struct nf_ct_net_event {
  struct net *net;
  u8 family;
  struct nf_ct_event_notifier ct_event_notifier;
  int refer_count;

  struct list_head node;
};

static LIST_HEAD(nf_ct_net_event_list);

static DEFINE_MUTEX(nf_ct_net_event_lock);

static DEFINE_HASHTABLE(mapping_table_by_ext_port, HASHTABLE_BUCKET_BITS);
static DEFINE_HASHTABLE(mapping_table_by_original_src, HASHTABLE_BUCKET_BITS);

static DEFINE_SPINLOCK(fullconenat_lock);

/* this is needed to avoid dead lock stuck in ct_event_cb */
static atomic_t mapping_check_busy;

static char tuple_tmp_string[512];
static char* nf_ct_stringify_tuple(const struct nf_conntrack_tuple *t) {
  snprintf(tuple_tmp_string, sizeof(tuple_tmp_string), "%pI4:%hu -> %pI4:%hu",
         &t->src.u3.ip, be16_to_cpu(t->src.u.all),
         &t->dst.u3.ip, be16_to_cpu(t->dst.u.all));
  return tuple_tmp_string;
}

static struct nat_mapping* allocate_mapping(const struct net *net, const uint16_t port, const __be32 int_addr, const uint16_t int_port, const int ifindex) {
  struct nat_mapping *p_new;
  u32 hash_src;

  p_new = kmalloc(sizeof(struct nat_mapping), GFP_ATOMIC);
  if (p_new == NULL) {
    pr_debug("xt_FULLCONENAT: ERROR: kmalloc() for new nat_mapping failed.\n");
    return NULL;
  }
  p_new->port = port;
  p_new->int_addr = int_addr;
  p_new->int_port = int_port;
  p_new->ifindex = ifindex;
  p_new->refer_count = 0;
  (p_new->original_tuples).next = &(p_new->original_tuples);
  (p_new->original_tuples).prev = &(p_new->original_tuples);

  hash_src = HASH_2(int_addr, (u32)int_port);

  hash_add(mapping_table_by_ext_port, &p_new->node_by_ext_port, port);
  hash_add(mapping_table_by_original_src, &p_new->node_by_original_src, hash_src);

  pr_debug("xt_FULLCONENAT: new mapping allocated for %pI4:%d ==> %d\n", 
    &p_new->int_addr, p_new->int_port, p_new->port);

  return p_new;
}

static void add_original_tuple_to_mapping(struct nat_mapping *mapping, const struct nf_conntrack_tuple* original_tuple, struct net *net, struct nf_conntrack_zone *zone) {
  struct nat_mapping_original_tuple *item = kmalloc(sizeof(struct nat_mapping_original_tuple), GFP_ATOMIC);
  if (item == NULL) {
    pr_debug("xt_FULLCONENAT: ERROR: kmalloc() for nat_mapping_original_tuple failed.\n");
    return;
  }
  memcpy(&item->tuple, original_tuple, sizeof(struct nf_conntrack_tuple));
  item->net = net;
  item->zone = zone;
  list_add(&item->node, &mapping->original_tuples);
  (mapping->refer_count)++;
}

static struct nat_mapping* get_mapping_by_ext_port(const uint16_t port, const int ifindex) {
  struct nat_mapping *p_current;

  hash_for_each_possible(mapping_table_by_ext_port, p_current, node_by_ext_port, port) {
    if (p_current->port == port && p_current->ifindex == ifindex) {
      return p_current;
    }
  }

  return NULL;
}

static struct nat_mapping* get_mapping_by_original_src(const __be32 src_ip, const uint16_t src_port, const int ifindex) {
  struct nat_mapping *p_current;
  u32 hash_src = HASH_2(src_ip, (u32)src_port);

  hash_for_each_possible(mapping_table_by_original_src, p_current, node_by_original_src, hash_src) {
    if (p_current->int_addr == src_ip && p_current->int_port == src_port && p_current->ifindex == ifindex) {
      return p_current;
    }
  }

  return NULL;
}

static struct nat_mapping* get_mapping_by_original_src_no_ifindex(const __be32 src_ip, const uint16_t src_port) {
  struct nat_mapping *p_current;
  u32 hash_src = HASH_2(src_ip, (u32)src_port);

  hash_for_each_possible(mapping_table_by_original_src, p_current, node_by_original_src, hash_src) {
    if (p_current->int_addr == src_ip && p_current->int_port == src_port) {
      return p_current;
    }
  }

  return NULL;
}

static void kill_mapping(struct nat_mapping *mapping) {
  struct list_head *iter, *tmp;
  struct nat_mapping_original_tuple *original_tuple_item;

  if (mapping == NULL) {
    return;
  }

  list_for_each_safe(iter, tmp, &mapping->original_tuples) {
    original_tuple_item = list_entry(iter, struct nat_mapping_original_tuple, node);
    list_del(&original_tuple_item->node);
    kfree(original_tuple_item);
  }

  hash_del(&mapping->node_by_ext_port);
  hash_del(&mapping->node_by_original_src);
  kfree(mapping);
}

static void destroy_mappings(void) {
  struct nat_mapping *p_current;
  struct hlist_node *tmp;
  int i;

  spin_lock(&fullconenat_lock);

  hash_for_each_safe(mapping_table_by_ext_port, i, tmp, p_current, node_by_ext_port) {
    kill_mapping(p_current);
  }

  spin_unlock(&fullconenat_lock);
}

/* check if a mapping is valid.
 * possibly delete and free an invalid mapping.
 * the mapping should not be used anymore after check_mapping() returns 0. */
static int check_mapping(struct nat_mapping* mapping) {
  struct list_head *iter, *tmp;
  struct nat_mapping_original_tuple *original_tuple_item;

  if (mapping == NULL) {
    return 0;
  }

  if (mapping->port == 0 || mapping->int_addr == 0 || mapping->int_port == 0 || mapping->ifindex == -1) {
    return 0;
  }

  list_for_each_safe(iter, tmp, &mapping->original_tuples) {
    original_tuple_item = list_entry(iter, struct nat_mapping_original_tuple, node);

    atomic_set(&mapping_check_busy, 1);
    if (nf_conntrack_find_get(original_tuple_item->net, 
      original_tuple_item->zone, &original_tuple_item->tuple) == NULL) {
      atomic_set(&mapping_check_busy, 0);

      pr_debug("xt_FULLCONENAT: check_mapping(): tuple %s expired. free this tuple.\n", nf_ct_stringify_tuple(&original_tuple_item->tuple));

      list_del(&original_tuple_item->node);
      kfree(original_tuple_item);
      (mapping->refer_count)--;
    }
    atomic_set(&mapping_check_busy, 0);
  }

  /* for dying/unconfirmed conntracks, an IPCT_DESTROY event may NOT be fired.
   * so we manually kill one of those conntracks once we acquire one. */
  pr_debug("xt_FULLCONENAT: check_mapping() refer_count for mapping at ext_port %d is now %d\n", mapping->port, mapping->refer_count);
  if (mapping->refer_count <= 0) {
    pr_debug("xt_FULLCONENAT: check_mapping(): kill dying/unconfirmed mapping at ext port %d\n", mapping->port);
    kill_mapping(mapping);
    return 0;
  } else {
    return 1;
  }
}

/* conntrack destroy event callback function */
static int ct_event_cb(unsigned int events, struct nf_ct_event *item) {
  struct nf_conn *ct;
  struct net *net;
  struct nf_conntrack_tuple *ct_tuple, *ct_tuple_origin;
  struct nat_mapping *mapping;
  uint8_t protonum;
  __be32 ip;
  uint16_t port;

  struct list_head *iter, *tmp;
  struct nat_mapping_original_tuple *original_tuple_item;

  ct = item->ct;
  /* we handle only conntrack destroy events */
  if (ct == NULL || !(events & (1 << IPCT_DESTROY))) {
    return 0;
  }

  net = nf_ct_net(ct);

  ct_tuple_origin = &(ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple);

  ct_tuple = &(ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple);

  protonum = (ct_tuple->dst).protonum;
  if (protonum != IPPROTO_UDP) {
    return 0;
  }

  if (atomic_read(&mapping_check_busy) > 0) {
    /* if this event is triggered by nf_conntrack_find_get() called by check_mapping(),
    * we just do nothing and let check_mapping() do the rest
    * to avoid a dead spinlock. */
    return 0;
  }

  spin_lock(&fullconenat_lock);

  /* we dont know the conntrack direction for now so we try in both ways. */
  ip = (ct_tuple->src).u3.ip;
  port = be16_to_cpu((ct_tuple->src).u.udp.port);
  mapping = get_mapping_by_original_src_no_ifindex(ip, port);
  if (mapping == NULL) {
    ct_tuple = &(ct->tuplehash[IP_CT_DIR_REPLY].tuple);
    ip = (ct_tuple->src).u3.ip;
    port = be16_to_cpu((ct_tuple->src).u.udp.port);
    mapping = get_mapping_by_original_src_no_ifindex(ip, port);
    if (mapping != NULL) {
      pr_debug("xt_FULLCONENAT: ct_event_cb(): IPCT_DESTROY event for INBOUND conntrack at ext port %d\n", mapping->port);
    }
  } else {
    pr_debug("xt_FULLCONENAT: ct_event_cb(): IPCT_DESTROY event for OUTBOUND conntrack at ext port %d\n", mapping->port);
  }

  if (mapping == NULL) {
    goto out;
  }

  list_for_each_safe(iter, tmp, &mapping->original_tuples) {
    original_tuple_item = list_entry(iter, struct nat_mapping_original_tuple, node);

    if (nf_ct_tuple_equal(&original_tuple_item->tuple, ct_tuple_origin)) {
      pr_debug("xt_FULLCONENAT: ct_event_cb(): tuple %s expired. free this tuple.\n", nf_ct_stringify_tuple(ct_tuple_origin));
      list_del(&original_tuple_item->node);
      kfree(original_tuple_item);
      (mapping->refer_count)--;
    }
  }

  /* then kill it if needed*/
  pr_debug("xt_FULLCONENAT: ct_event_cb(): refer_count for mapping at ext_port %d is now %d\n", mapping->port, mapping->refer_count);
  if (mapping->refer_count <= 0) {
    pr_debug("xt_FULLCONENAT: ct_event_cb(): kill expired mapping at ext port %d\n", mapping->port);
    kill_mapping(mapping);
  }

out:
  spin_unlock(&fullconenat_lock);

  return 0;
}

static __be32 get_device_ip(const struct net_device* dev) {
  struct in_device* in_dev;
  struct in_ifaddr* if_info;

  in_dev = dev->ip_ptr;
  if (in_dev == NULL) {
    return 0;
  }
  if_info = in_dev->ifa_list;
  if (if_info) {
    return if_info->ifa_local;
  } else {
    return 0;
  }
}

static uint16_t find_appropriate_port(struct net *net, struct nf_conntrack_zone *zone, const uint16_t original_port, const int ifindex, const struct nf_nat_ipv4_range *range) {
  uint16_t min, start, selected, range_size, i;
  struct nat_mapping* mapping = NULL;

  if (range->flags & NF_NAT_RANGE_PROTO_SPECIFIED) {
    min = be16_to_cpu((range->min).udp.port);
    range_size = be16_to_cpu((range->max).udp.port) - min + 1;
  } else {
    /* minimum port is 1024. same behavior as default linux NAT. */
    min = 1024;
    range_size = 65535 - min + 1;
  }

  if ((range->flags & NF_NAT_RANGE_PROTO_RANDOM)
    || (range->flags & NF_NAT_RANGE_PROTO_RANDOM_FULLY)) {
    /* for now we do the same thing for both --random and --random-fully */

    /* select a random starting point */
    start = (uint16_t)(prandom_u32() % (u32)range_size);
  } else {

    if ((original_port >= min && original_port <= min + range_size - 1)
      || !(range->flags & NF_NAT_RANGE_PROTO_SPECIFIED)) {
      /* 1. try to preserve the port if it's available */
      mapping = get_mapping_by_ext_port(original_port, ifindex);
      if (mapping == NULL || !(check_mapping(mapping))) {
        return original_port;
      }
    }

    /* otherwise, we start from zero */
    start = 0;
  }

  for (i = 0; i < range_size; i++) {
    /* 2. try to find an available port */
    selected = min + ((start + i) % range_size);
    mapping = get_mapping_by_ext_port(selected, ifindex);
    if (mapping == NULL || !(check_mapping(mapping))) {
      return selected;
    }
  }

  /* 3. at least we tried. override a previous mapping. */
  selected = min + start;
  mapping = get_mapping_by_ext_port(selected, ifindex);
  kill_mapping(mapping);

  return selected;
}

static unsigned int fullconenat_tg(struct sk_buff *skb, const struct xt_action_param *par)
{
  const struct nf_nat_ipv4_multi_range_compat *mr;
  const struct nf_nat_ipv4_range *range;

  struct nf_conn *ct;
  enum ip_conntrack_info ctinfo;
  struct nf_conntrack_tuple *ct_tuple, *ct_tuple_origin;
  struct net *net;
  struct nf_conntrack_zone *zone;

  struct nat_mapping *mapping, *src_mapping;
  unsigned int ret;
  struct nf_nat_range newrange;

  __be32 new_ip, ip;
  uint16_t port, original_port, want_port;
  uint8_t protonum;
  int ifindex;

  ip = 0;
  original_port = 0;

  mr = par->targinfo;
  range = &mr->range[0];

  mapping = NULL;
  ret = XT_CONTINUE;

  ct = nf_ct_get(skb, &ctinfo);
  net = nf_ct_net(ct);
  zone = nf_ct_zone(ct);

  memset(&newrange.min_addr, 0, sizeof(newrange.min_addr));
  memset(&newrange.max_addr, 0, sizeof(newrange.max_addr));
  newrange.flags       = mr->range[0].flags | NF_NAT_RANGE_MAP_IPS;
  newrange.min_proto   = mr->range[0].min;
  newrange.max_proto   = mr->range[0].max;

  if (xt_hooknum(par) == NF_INET_PRE_ROUTING) {
    /* inbound packets */
    ifindex = xt_in(par)->ifindex;
    ct_tuple_origin = &(ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple);

    protonum = (ct_tuple_origin->dst).protonum;
    if (protonum != IPPROTO_UDP) {
      return ret;
    }
    ip = (ct_tuple_origin->src).u3.ip;
    port = be16_to_cpu((ct_tuple_origin->dst).u.udp.port);

    spin_lock(&fullconenat_lock);

    /* find an active mapping based on the inbound port */
    mapping = get_mapping_by_ext_port(port, ifindex);
    if (mapping == NULL) {
      spin_unlock(&fullconenat_lock);
      return ret;
    }
    if (check_mapping(mapping)) {
      newrange.flags = NF_NAT_RANGE_MAP_IPS | NF_NAT_RANGE_PROTO_SPECIFIED;
      newrange.min_addr.ip = mapping->int_addr;
      newrange.max_addr.ip = mapping->int_addr;
      newrange.min_proto.udp.port = cpu_to_be16(mapping->int_port);
      newrange.max_proto = newrange.min_proto;

      pr_debug("xt_FULLCONENAT: inbound NAT %s ==> %pI4:%d\n", nf_ct_stringify_tuple(ct_tuple_origin), &mapping->int_addr, mapping->int_port);

      ret = nf_nat_setup_info(ct, &newrange, HOOK2MANIP(xt_hooknum(par)));

      if (ret == NF_ACCEPT) {
        add_original_tuple_to_mapping(mapping, ct_tuple_origin, net, zone);
        pr_debug("xt_FULLCONENAT: fullconenat_tg(): INBOUND: refer_count for mapping at ext_port %d is now %d\n", mapping->port, mapping->refer_count);
      }
    }
    spin_unlock(&fullconenat_lock);
    return ret;


  } else if (xt_hooknum(par) == NF_INET_POST_ROUTING) {
    /* outbound packets */
    ifindex = xt_out(par)->ifindex;

    ct_tuple_origin = &(ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple);
    protonum = (ct_tuple_origin->dst).protonum;

    spin_lock(&fullconenat_lock);

    if (protonum == IPPROTO_UDP) {
      ip = (ct_tuple_origin->src).u3.ip;
      original_port = be16_to_cpu((ct_tuple_origin->src).u.udp.port);

      src_mapping = get_mapping_by_original_src(ip, original_port, ifindex);
      if (src_mapping != NULL && check_mapping(src_mapping)) {

        /* outbound nat: if a previously established mapping is active,
         * we will reuse that mapping. */

        newrange.flags = NF_NAT_RANGE_MAP_IPS | NF_NAT_RANGE_PROTO_SPECIFIED;
        newrange.min_proto.udp.port = cpu_to_be16(src_mapping->port);
        newrange.max_proto = newrange.min_proto;

      } else {
        want_port = find_appropriate_port(net, zone, original_port, ifindex, range);

        newrange.flags = NF_NAT_RANGE_MAP_IPS | NF_NAT_RANGE_PROTO_SPECIFIED;
        newrange.min_proto.udp.port = cpu_to_be16(want_port);
        newrange.max_proto = newrange.min_proto;

      }
    }

    new_ip = get_device_ip(skb->dev);
    newrange.min_addr.ip = new_ip;
    newrange.max_addr.ip = new_ip;

    ret = nf_nat_setup_info(ct, &newrange, HOOK2MANIP(xt_hooknum(par)));

    if (protonum != IPPROTO_UDP || ret != NF_ACCEPT) {
      spin_unlock(&fullconenat_lock);
      return ret;
    }

    /* the reply tuple contains the mapped port. */
    ct_tuple = &(ct->tuplehash[IP_CT_DIR_REPLY].tuple);

    port = be16_to_cpu((ct_tuple->dst).u.udp.port);

    pr_debug("xt_FULLCONENAT: outbound NAT %s ==> %d\n", nf_ct_stringify_tuple(ct_tuple_origin), port);

    /* save the mapping information into our mapping table */
    mapping = get_mapping_by_ext_port(port, ifindex);
    if (mapping == NULL || !check_mapping(mapping)) {
      mapping = allocate_mapping(net, port, ip, original_port, ifindex);
    }
    if (mapping != NULL) {
      add_original_tuple_to_mapping(mapping, ct_tuple_origin, net, zone);
      pr_debug("xt_FULLCONENAT: fullconenat_tg(): OUTBOUND: refer_count for mapping at ext_port %d is now %d\n", mapping->port, mapping->refer_count);
    }

    spin_unlock(&fullconenat_lock);
    return ret;
  }

  return ret;
}

static int fullconenat_tg_check(const struct xt_tgchk_param *par)
{
  struct nf_ct_net_event *net_event;
  struct list_head* iter;

  struct net *net = par->net;

  mutex_lock(&nf_ct_net_event_lock);

  list_for_each(iter, &nf_ct_net_event_list) {
    net_event = list_entry(iter, struct nf_ct_net_event, node);
    if (net_event->net == net) {
      (net_event->refer_count)++;
      pr_debug("xt_FULLCONENAT: refer_count for net addr %p is now %d\n", (void*) (net_event->net), net_event->refer_count);
      goto out;
    }
  }

  net_event = kmalloc(sizeof(struct nf_ct_net_event), GFP_KERNEL);
  if (net_event == NULL) {
    pr_debug("xt_FULLCONENAT: ERROR: kmalloc() for net_event failed.\n");
    goto out;
  }
  net_event->net = net;
  net_event->family = par->family;
  (net_event->ct_event_notifier).fcn = ct_event_cb;
  net_event->refer_count = 1;
  list_add(&net_event->node, &nf_ct_net_event_list);

  nf_ct_netns_get(net_event->net, net_event->family);
  nf_conntrack_register_notifier(net_event->net, &(net_event->ct_event_notifier));

  pr_debug("xt_FULLCONENAT: refer_count for net addr %p is now %d\n", (void*) (net_event->net), net_event->refer_count);
  pr_debug("xt_FULLCONENAT: ct_event_notifier registered for net addr %p\n", (void*) (net_event->net));

out:
  mutex_unlock(&nf_ct_net_event_lock);

  return 0;
}

static void fullconenat_tg_destroy(const struct xt_tgdtor_param *par)
{
  struct nf_ct_net_event *net_event;
  struct list_head *iter, *tmp_iter;

  struct net *net = par->net;

  mutex_lock(&nf_ct_net_event_lock);

  list_for_each_safe(iter, tmp_iter, &nf_ct_net_event_list) {
    net_event = list_entry(iter, struct nf_ct_net_event, node);
    if (net_event->net == net) {
      (net_event->refer_count)--;
      pr_debug("xt_FULLCONENAT: refer_count for net addr %p is now %d\n", (void*) (net_event->net), net_event->refer_count);

      if (net_event->refer_count <= 0) {
        nf_conntrack_unregister_notifier(net_event->net, &(net_event->ct_event_notifier));
        nf_ct_netns_put(net_event->net, net_event->family);

        pr_debug("xt_FULLCONENAT: unregistered ct_net_event for net addr %p\n", (void*) (net_event->net));
        list_del(&net_event->node);
        kfree(net_event);
      }
    }
  }

  mutex_unlock(&nf_ct_net_event_lock);
}

static struct xt_target tg_reg[] __read_mostly = {
 {
  .name       = "FULLCONENAT",
  .family     = NFPROTO_IPV4,
  .revision   = 0,
  .target     = fullconenat_tg,
  .targetsize = sizeof(struct nf_nat_ipv4_multi_range_compat),
  .table      = "nat",
  .hooks      = (1 << NF_INET_PRE_ROUTING) |
                (1 << NF_INET_POST_ROUTING),
  .checkentry = fullconenat_tg_check,
  .destroy    = fullconenat_tg_destroy,
  .me         = THIS_MODULE,
 },
};

static int __init fullconenat_tg_init(void)
{
  return xt_register_targets(tg_reg, ARRAY_SIZE(tg_reg));
}

static void fullconenat_tg_exit(void)
{
  xt_unregister_targets(tg_reg, ARRAY_SIZE(tg_reg));

  destroy_mappings();
}

module_init(fullconenat_tg_init);
module_exit(fullconenat_tg_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Xtables: implementation of RFC3489 full cone NAT");
MODULE_AUTHOR("Chion Tang <tech@chionlab.moe>");
MODULE_ALIAS("ipt_FULLCONENAT");
