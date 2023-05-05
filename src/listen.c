/* ncdc - NCurses Direct Connect client

  Copyright (c) 2011-2022 Yoran Heling

  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files (the
  "Software"), to deal in the Software without restriction, including
  without limitation the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software, and to
  permit persons to whom the Software is furnished to do so, subject to
  the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/

#include "ncdc.h"
#include "listen.h"


#if INTERFACE

#define LBT_UDP 1
#define LBT_TCP 2
#define LBT_IP4 4
#define LBT_IP6 8

#define LBT_UDP4 (LBT_UDP|LBT_IP4)
#define LBT_UDP6 (LBT_UDP|LBT_IP6)
#define LBT_TCP4 (LBT_TCP|LBT_IP4)
#define LBT_TCP6 (LBT_TCP|LBT_IP6)

#define LBT_STR(x) ((x) == LBT_TCP4 ? "TCP4" : (x) == LBT_TCP6 ? "TCP6" : (x) == LBT_UDP4 ? "UDP4" : "UDP6")

// port + ip4 are "cached" for convenience.
struct listen_bind_t {
  guint16 type; // LBT_*
  guint16 port;
  struct in_addr ip4;
  struct in6_addr ip6;
  int src; // glib event source
  int sock;
  GSList *hubs; // hubs that use this bind
};

struct listen_hub_bind_t {
  guint64 hubid;
  // A hub is always active in either IPv4 or IPv6, both is currently not supported.
  listen_bind_t *tcp, *udp;
};

#endif


GList      *listen_binds     = NULL; // List of currently used binds
GHashTable *listen_hub_binds = NULL; // Map of &hubid to listen_hub_bind

// The port to use when active_port hasn't been set. Initialized to a random
// value on startup. Note that the same port is still used for all hubs that
// have the port set to "random". This obviously isn't very "random", but
// avoids a change to the port every time that listen_refresh() is called.
// Also note that this isn't necessarily a "random *free* port" such as what
// the OS would give if you actually specify port=0, so if a port happens to be
// chosen that is already in use, you'll get an error. (This isn't very nice...
// Especially if the port is being used for e.g. an outgoing connection, which
// isn't very uncommon for high ports).
static guint16 random_tcp_port, random_udp_port;


// Public interface to fetch current listen configuration

gboolean listen_hub_active(guint64 hub) {
  listen_hub_bind_t *b = g_hash_table_lookup(listen_hub_binds, &hub);
  return b && b->tcp;
}

// These all returns 0 if passive or disabled
guint16 listen_hub_tcp(guint64 hub) {
  listen_hub_bind_t *b = g_hash_table_lookup(listen_hub_binds, &hub);
  return b && b->tcp ? b->tcp->port : 0;
}

guint16 listen_hub_udp(guint64 hub) {
  listen_hub_bind_t *b = g_hash_table_lookup(listen_hub_binds, &hub);
  return b && b->udp ? b->udp->port : 0;
}


const char *listen_bind_ipport(listen_bind_t *b) {
  static char buf[100];
  g_snprintf(buf, 100, b->type & LBT_IP4 ? "%s:%d" : "[%s]:%d",
    b->type & LBT_IP4 ? ip4_unpack(b->ip4) : ip6_unpack(b->ip6), (int)b->port);
  return buf;
}



void listen_global_init() {
  listen_hub_binds = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, g_free);
  random_tcp_port = g_random_int_range(1025, 65535);
  random_udp_port = g_random_int_range(1025, 65535);
}


// Closes all listen sockets and clears *listen_binds and *listen_hub_binds.
static void listen_stop() {
  g_debug("listen: Stopping.");
  g_hash_table_remove_all(listen_hub_binds);
  GList *n, *b = listen_binds;
  while(b) {
    n = b->next;
    listen_bind_t *lb = b->data;
    if(lb->src)
      g_source_remove(lb->src);
    if(lb->sock)
      close(lb->sock);
    g_slist_free(lb->hubs);
    g_free(lb);
    g_list_free_1(b);
    b = n;
  }
  listen_binds = NULL;
}


static gboolean listen_tcp_handle(gpointer dat) {
  listen_bind_t *b = dat;

  int c;
  char addr_str[100];
  if(b->type & LBT_IP4) {
    struct sockaddr_in a = {};
    socklen_t len = sizeof(a);
    c = accept(b->sock, (struct sockaddr *)&a, &len);
    g_snprintf(addr_str, 100, "%s:%d", ip4_unpack(a.sin_addr), ntohs(a.sin_port));
  } else {
    struct sockaddr_in6 a = {};
    socklen_t len = sizeof(a);
    c = accept(b->sock, (struct sockaddr *)&a, &len);
    g_snprintf(addr_str, 100, "[%s]:%d", ip6_unpack(a.sin6_addr), ntohs(a.sin6_port));
  }

  // handle error
  if(c < 0) {
    if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
      return TRUE;
    ui_mf(uit_main_tab, 0, "TCP accept error on %s: %s. Switching to passive mode.",
      listen_bind_ipport(b), g_strerror(errno));
    listen_stop();
    hub_global_nfochange();
    return FALSE;
  }

  // Create connection
  fcntl(c, F_SETFL, fcntl(c, F_GETFL, 0)|O_NONBLOCK);
  cc_incoming(cc_create(NULL), b->port, c, addr_str, b->type & LBT_IP4 ? FALSE : TRUE);
  return TRUE;
}


static gboolean listen_udp_handle(gpointer dat) {
  static char buf[5000]; // can be static, this function is only called in the main thread.
  listen_bind_t *b = dat;

  int r;
  char addr_str[100];
  if(b->type & LBT_IP4) {
    struct sockaddr_in a = {};
    socklen_t len = sizeof(a);
    r = recvfrom(b->sock, buf, sizeof(buf)-1, 0, (struct sockaddr *)&a, &len);
    g_snprintf(addr_str, 100, "%s:%d", ip4_unpack(a.sin_addr), ntohs(a.sin_port));
  } else {
    struct sockaddr_in6 a = {};
    socklen_t len = sizeof(a);
    r = recvfrom(b->sock, buf, sizeof(buf)-1, 0, (struct sockaddr *)&a, &len);
    g_snprintf(addr_str, 100, "[%s]:%d", ip6_unpack(a.sin6_addr), ntohs(a.sin6_port));
  }

  // handle error
  if(r < 0) {
    if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
      return TRUE;
    ui_mf(uit_main_tab, 0, "UDP read error on %s: %s. Switching to passive mode.",
      listen_bind_ipport(b), g_strerror(errno));
    listen_stop();
    hub_global_nfochange();
    return FALSE;
  }

  // Since all incoming messages must be search results, just pass the messages to search.c
  buf[r] = 0;
  if(!search_handle_udp(addr_str, buf, r)) {
    // The message may habe been encrypted with SUDP, so this error reporting
    // may not be too useful.
    g_message("UDP:%s: Invalid message: %s", addr_str, buf);
  }
  return TRUE;
}


#define bind_hub_add(lb, h) do {\
    if((h)->tcp != lb && (h)->udp != lb)\
      (lb)->hubs = g_slist_prepend((lb)->hubs, h);\
    if((lb)->type & LBT_TCP)\
      (h)->tcp = lb;\
    else\
      (h)->udp = lb;\
  } while(0)


static void bind_add(listen_hub_bind_t *b, int type, char *ip, guint16 port) {
  if(!port)
    port = type & LBT_UDP ? random_udp_port : random_tcp_port;

  listen_bind_t lb = {};
  lb.type = type;
  lb.port = port;
  if(type & LBT_IP4)
    lb.ip4 = var_parse_ip4(ip);
  else
    lb.ip6 = var_parse_ip6(ip);
  g_debug("Listen: Adding %s %s", LBT_STR(type), listen_bind_ipport(&lb));

  // First: look if we can re-use an existing bind and look for any unresolvable conflicts.
  GList *c;
  for(c=listen_binds; c; c=c->next) {
    listen_bind_t *i = c->data;
    // Same? Just re-use.
    if(type == i->type && i->port == port && (type & LBT_IP4
          ? ip4_cmp(i->ip4, lb.ip4) == 0 || ip4_isany(i->ip4)
          : ip6_cmp(i->ip6, lb.ip6) == 0 || ip6_isany(i->ip6))) {
      g_debug("Listen: Re-using!");
      bind_hub_add(i, b);
      return;
    }
  }

  // Create and add bind item
  listen_bind_t *nlb = g_new0(listen_bind_t, 1);
  *nlb = lb;
  bind_hub_add(nlb, b);

  // Look for existing binds that should be merged.
  GList *n;
  if(nlb->type & LBT_IP4 ? ip4_isany(nlb->ip4) : ip6_isany(nlb->ip6)) {
    for(c=listen_binds; c; c=n) {
      n = c->next;
      listen_bind_t *i = c->data;
      if(i->port != nlb->port || i->type != nlb->type)
        continue;
      g_debug("Listen: Merging!");
      // Move over all hubs to *nlb
      GSList *in;
      for(in=i->hubs; in; in=in->next)
        bind_hub_add(nlb, (listen_hub_bind_t *)in->data);
      g_slist_free(i->hubs);
      // And remove this bind
      g_free(i);
      listen_binds = g_list_delete_link(listen_binds, c);
    }
  }

  listen_binds = g_list_prepend(listen_binds, nlb);
}


static void bind_create(listen_bind_t *b) {
  g_debug("Listen: binding %s %s", LBT_STR(b->type), listen_bind_ipport(b));
  int err = 0;

  // Create socket
  int sock = socket(b->type & LBT_IP4 ? AF_INET : AF_INET6, b->type & LBT_UDP ? SOCK_DGRAM : SOCK_STREAM, 0);
  g_return_if_fail(sock > 0);

  // Make sure that, if this is an IPv6 '::' socket, it does not bind to IPv4.
  // If it would, then this or a subsequent bind() may fail because we may also
  // create a separate socket for the same port on IPv4.
  int one = 1;
#ifdef IPV6_V6ONLY
  if(b->type & LBT_IP6)
    setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (void *)&one, sizeof(one));
#endif

  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void *)&one, sizeof(one));
  fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0)|O_NONBLOCK);

  // Bind
  int r = b->type & LBT_IP4
    ? bind(sock, ip4_sockaddr(b->ip4, b->port), sizeof(struct sockaddr_in))
    : bind(sock, ip6_sockaddr(b->ip6, b->port), sizeof(struct sockaddr_in6));
  if(r < 0)
    err = errno;

  // listen
  if(!err && b->type & LBT_TCP && listen(sock, 5) < 0)
    err = errno;

  // Bind or listen failed? Abandon ship! (This may be a bit extreme, but at
  // least it avoids any other problems that may arise from a partially
  // activated configuration).
  if(err) {
    ui_mf(uit_main_tab, UIP_MED, "Error binding to %s %s, %s. Switching to passive mode.",
      LBT_STR(b->type), listen_bind_ipport(b), g_strerror(err));
    close(sock);
    listen_stop();
    return;
  }
  b->sock = sock;

  // Start accepting incoming connections or handling incoming messages
  GSource *src = fdsrc_new(sock, G_IO_IN);
  g_source_set_callback((GSource *)src, b->type & LBT_UDP ? listen_udp_handle : listen_tcp_handle, b, NULL);
  b->src = g_source_attach((GSource *)src, NULL);
  g_source_unref((GSource *)src);
}


// Should be called every time a hub is opened/closed or an active_ config
// variable has changed.
void listen_refresh() {
  listen_stop();
  g_debug("listen: Refreshing");

  // Walk through the list of hubs and get their config
  GHashTableIter i;
  hub_t *h = NULL;
  g_hash_table_iter_init(&i, hubs);
  while(g_hash_table_iter_next(&i, NULL, (gpointer *)&h)) {
    // We only look at hubs on which we are active
    if(!var_get_bool(h->id, VAR_active) || !hub_ip(h))
      continue;
    // Add to listen_hub_binds
    listen_hub_bind_t *b = g_new0(listen_hub_bind_t, 1);
    b->hubid = h->id;
    g_hash_table_insert(listen_hub_binds, &b->hubid, b);
    // And add the required binds for this hub (Due to the conflict resolution in binds_add(), this is O(n^2))
    // Note: bind_add() can call listen_stop() on error, detect this on whether listen_hub_binds is empty or not.
    bind_add(b, net_is_ipv6(h->net) ? LBT_TCP6 : LBT_TCP4, var_get(b->hubid, VAR_local_address), var_get_int(b->hubid, VAR_active_port));
    if(!g_hash_table_size(listen_hub_binds))
      break;
    bind_add(b, net_is_ipv6(h->net) ? LBT_UDP6 : LBT_UDP4, var_get(b->hubid, VAR_local_address), var_get_int(b->hubid, VAR_active_udp_port));
    if(!g_hash_table_size(listen_hub_binds))
      break;
  }

  // Now walk through *listen_binds and actually create the listen sockets
  GList *l;
  for(l=listen_binds; l; listen_binds ? (l=l->next) : (l=NULL))
    bind_create((listen_bind_t *)l->data);
}

