/*
 * Copyright (c) 2015-2017 Intel Corporation. All rights reserved.
 * Copyright (c) 2017, Cisco Systems, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <netinet/in.h>
#include <inttypes.h>

#if HAVE_GETIFADDRS
#include <net/if.h>
#include <ifaddrs.h>
#endif

#include <ofi_util.h>


enum {
	UTIL_NO_ENTRY = -1,
	UTIL_DEFAULT_AV_SIZE = 1024,
};

static int fi_get_src_sockaddr(const struct sockaddr *dest_addr, size_t dest_addrlen,
			       struct sockaddr **src_addr, size_t *src_addrlen)
{
	socklen_t len; /* needed for OS compatability */
	int sock, ret;

	sock = socket(dest_addr->sa_family, SOCK_DGRAM, 0);
	if (sock < 0)
		return -errno;

	ret = connect(sock, dest_addr, dest_addrlen);
	if (ret)
		goto out;

	*src_addr = calloc(dest_addrlen, 1);
	if (!*src_addr) {
		ret = -FI_ENOMEM;
		goto out;
	}

	len = (socklen_t) dest_addrlen;
	ret = getsockname(sock, *src_addr, &len);
	if (ret) {
		ret = -errno;
		goto out;
	}
	*src_addrlen = len;

	switch ((*src_addr)->sa_family) {
	case AF_INET:
		((struct sockaddr_in *) (*src_addr))->sin_port = 0;
		break;
	case AF_INET6:
		((struct sockaddr_in6 *) (*src_addr))->sin6_port = 0;
		break;
	default:
		ret = -FI_ENOSYS;
		break;
	}

out:
	ofi_close_socket(sock);
	return ret;

}

void ofi_getnodename(uint16_t sa_family, char *buf, int buflen)
{
	int ret;
	struct addrinfo ai, *rai = NULL;
	struct ifaddrs *ifaddrs, *ifa;

	assert(buf && buflen > 0);
	ret = gethostname(buf, buflen);
	buf[buflen - 1] = '\0';
	if (ret == 0) {
		memset(&ai, 0, sizeof(ai));
		ai.ai_family = sa_family  ? sa_family : AF_INET;
		ret = getaddrinfo(buf, NULL, &ai, &rai);
		if (!ret) {
			freeaddrinfo(rai);
			return;
		}
	}

#if HAVE_GETIFADDRS
	ret = ofi_getifaddrs(&ifaddrs);
	if (!ret) {
		for (ifa = ifaddrs; ifa != NULL; ifa = ifa->ifa_next) {
			if (ifa->ifa_addr == NULL || !(ifa->ifa_flags & IFF_UP))
				continue;

			if (sa_family) {
				if (ifa->ifa_addr->sa_family != sa_family)
					continue;
			} else if ((ifa->ifa_addr->sa_family != AF_INET) &&
				   (ifa->ifa_addr->sa_family != AF_INET6)) {
				continue;
			}

			ret = getnameinfo(ifa->ifa_addr, ofi_sizeofaddr(ifa->ifa_addr),
				  	  buf, buflen, NULL, 0, NI_NUMERICHOST);
			buf[buflen - 1] = '\0';
			if (ret == 0) {
				freeifaddrs(ifaddrs);
				return;
			}
		}
		freeifaddrs(ifaddrs);
	}
#endif
	/* no reasonable address found, use ipv4 loopback */
	strncpy(buf, "127.0.0.1", buflen);
	buf[buflen - 1] = '\0';
}

int ofi_get_src_addr(uint32_t addr_format,
		    const void *dest_addr, size_t dest_addrlen,
		    void **src_addr, size_t *src_addrlen)
{
	switch (addr_format) {
	case FI_SOCKADDR:
	case FI_SOCKADDR_IN:
	case FI_SOCKADDR_IN6:
		return fi_get_src_sockaddr(dest_addr, dest_addrlen,
					   (struct sockaddr **) src_addr,
					   src_addrlen);
	default:
		return -FI_ENOSYS;
	}
}

static int fi_get_sockaddr(int sa_family, uint64_t flags,
			   const char *node, const char *service,
			   struct sockaddr **addr, size_t *addrlen)
{
	struct addrinfo hints, *ai;
	int ret;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = sa_family;
	hints.ai_socktype = SOCK_STREAM;
	if (flags & FI_SOURCE)
		hints.ai_flags = AI_PASSIVE;

	ret = getaddrinfo(node, service, &hints, &ai);
	if (ret)
		return -FI_ENODATA;

	*addr = mem_dup(ai->ai_addr, ai->ai_addrlen);
	if (!*addr) {
		ret = -FI_ENOMEM;
		goto out;
	}

	*addrlen = ai->ai_addrlen;
out:
	freeaddrinfo(ai);
	return ret;
}

void ofi_get_str_addr(const char *node, const char *service,
		      char **addr, size_t *addrlen)
{
	if (!node || !strstr(node, "://"))
		return;

	*addr = strdup(node);
	*addrlen = strlen(node) + 1;
}

int ofi_get_addr(uint32_t addr_format, uint64_t flags,
		const char *node, const char *service,
		void **addr, size_t *addrlen)
{
	switch (addr_format) {
	case FI_SOCKADDR:
		return fi_get_sockaddr(0, flags, node, service,
				       (struct sockaddr **) addr, addrlen);
	case FI_SOCKADDR_IN:
		return fi_get_sockaddr(AF_INET, flags, node, service,
				       (struct sockaddr **) addr, addrlen);
	case FI_SOCKADDR_IN6:
		return fi_get_sockaddr(AF_INET6, flags, node, service,
				       (struct sockaddr **) addr, addrlen);
	case FI_ADDR_STR:
		ofi_get_str_addr(node, service, (char **) addr, addrlen);
		return 0;
	default:
		return -FI_ENOSYS;
	}
}

static void *util_av_get_data(struct util_av *av, int index)
{
	return (char *) av->data + (index * av->addrlen);
}

void *ofi_av_get_addr(struct util_av *av, int index)
{
	FI_DBG(av->prov, FI_LOG_AV, "get[%d]:%s\n", index,
		ofi_hex_str(util_av_get_data(av, index), av->addrlen));
	return util_av_get_data(av, index);
}

static void util_av_set_data(struct util_av *av, int index,
			     const void *data, size_t len)
{
	FI_DBG(av->prov, FI_LOG_AV, "set[%d]:%s\n", index,
		ofi_hex_str(data, len));
	memcpy(util_av_get_data(av, index), data, len);
}

int ofi_verify_av_insert(struct util_av *av, uint64_t flags)
{
	if ((av->flags & FI_EVENT) && !av->eq) {
		FI_WARN(av->prov, FI_LOG_AV, "no EQ bound to AV\n");
		return -FI_ENOEQ;
	}

	if (flags & ~(FI_MORE)) {
		FI_WARN(av->prov, FI_LOG_AV, "unsupported flags\n");
		return -FI_ENOEQ;
	}

	return 0;
}

/*
 * Must hold AV lock
 */
static int util_av_hash_insert(struct util_av_hash *hash, int slot,
			       int index, int *table_slot)
{
	int entry, i;

	if (slot < 0 || slot >= hash->slots)
		return -FI_EINVAL;

	if (hash->table[slot].index == UTIL_NO_ENTRY) {
		hash->table[slot].index = index;
		if (table_slot)
			*table_slot = slot;
		return 0;
	}

	if (hash->free_list == UTIL_NO_ENTRY)
		return -FI_ENOSPC;

	entry = hash->free_list;
	hash->free_list = hash->table[hash->free_list].next;

	for (i = slot; hash->table[i].next != UTIL_NO_ENTRY; )
		i = hash->table[i].next;

	hash->table[i].next = entry;
	if (table_slot)
		*table_slot = i;
	hash->table[entry].index = index;
	hash->table[entry].next = UTIL_NO_ENTRY;
	return 0;
}

/* Caller must hold `av::lock` */
static inline
int util_av_lookup_index(struct util_av *av, const void *addr,
			 int slot, int *table_slot)
{
	int i, ret = -FI_ENODATA;

	if (av->hash.table[slot].index == UTIL_NO_ENTRY) {
		FI_DBG(av->prov, FI_LOG_AV, "no entry at slot (%d)\n", slot);
		goto out;
	}

	for (i = slot; i != UTIL_NO_ENTRY; i = av->hash.table[i].next) {
		if (!memcmp(ofi_av_get_addr(av, av->hash.table[i].index), addr,
			    av->addrlen)) {
			ret = av->hash.table[i].index;
			if (table_slot)
				*table_slot = i;
			FI_DBG(av->prov, FI_LOG_AV, "entry at index (%d)\n", ret);
			break;
		}
	}
out:
	FI_DBG(av->prov, FI_LOG_AV, "%d\n", ret);
	return ret;
}

/*
 * Must hold AV lock
 */
int ofi_av_insert_addr(struct util_av *av, const void *addr, int slot, int *index)
{
	int ret;

	if (OFI_UNLIKELY(av->free_list == UTIL_NO_ENTRY)) {
		FI_WARN(av->prov, FI_LOG_AV, "AV is full\n");
		return -FI_ENOSPC;
	}

	if (av->flags & OFI_AV_HASH) {
		int table_slot;

		if (OFI_UNLIKELY(slot < 0 || slot >= av->hash.slots)) {
			FI_WARN(av->prov, FI_LOG_AV, "invalid slot (%d)\n", slot);
			return -FI_EINVAL;
		}
		ret = util_av_lookup_index(av, addr, slot, &table_slot);
		if (ret != -FI_ENODATA) {
			*index = ret;
			ofi_atomic_inc32(&av->hash.table[table_slot].use_cnt);
			return 0;
		}
		ret = util_av_hash_insert(&av->hash, slot, av->free_list,
					  &table_slot);
		if (ret) {
			FI_WARN(av->prov, FI_LOG_AV,
				"failed to insert addr into hash table\n");
			return ret;
		}
		ofi_atomic_inc32(&av->hash.table[table_slot].use_cnt);
	}

	*index = av->free_list;
	av->free_list = *(int *) util_av_get_data(av, av->free_list);
	util_av_set_data(av, *index, addr, av->addrlen);
	return 0;
}

static inline int
util_av_hash_lookup_table_slot(struct util_av_hash *hash, int slot, int index)
{
	int i;

	if (hash->table[slot].index == index) {
		return slot;
	} else {
		for (i = slot; hash->table[i].index != index; )
			i = hash->table[i].next;
		return i;
	}
}

/*
 * Must hold AV lock
 */
static void util_av_hash_remove(struct util_av_hash *hash, int slot, int index)
{
	int table_slot, slot_next;

	if (OFI_UNLIKELY(slot < 0 || slot >= hash->slots))
		return;

	table_slot = util_av_hash_lookup_table_slot(hash, slot, index);
	if (table_slot == slot) {
		if (hash->table[table_slot].next == UTIL_NO_ENTRY) {
			hash->table[table_slot].index = UTIL_NO_ENTRY;
			return;
		}
	}

	slot_next = hash->table[slot].next;
	hash->table[slot] = hash->table[slot_next];

	hash->table[slot_next].next = hash->free_list;
	hash->free_list = slot_next;
}

/*
 * Must hold AV lock
 */
int ofi_av_remove_addr(struct util_av *av, int slot, int index)
{
	int *entry, *next, i;
	int ret = 0;

	if (OFI_UNLIKELY(index < 0 || (size_t)index > av->count)) {
		FI_WARN(av->prov, FI_LOG_AV, "index out of range\n");
		return -FI_EINVAL;
	}

	if (av->flags & OFI_AV_HASH) {
		int table_slot;

		if (OFI_UNLIKELY(slot < 0 || slot >= av->hash.slots)) {
			FI_WARN(av->prov, FI_LOG_AV, "invalid slot (%d)\n", slot);
			return -FI_EINVAL;
		}

		table_slot = util_av_hash_lookup_table_slot(&av->hash, slot, index);
		if (ofi_atomic_dec32(&av->hash.table[table_slot].use_cnt))
			return FI_SUCCESS;
	}

	if (av->flags & OFI_AV_HASH)
		util_av_hash_remove(&av->hash, slot, index);

	entry = util_av_get_data(av, index);
	if (av->free_list == UTIL_NO_ENTRY || index < av->free_list) {
		*entry = av->free_list;
		av->free_list = index;
	} else {
		i = av->free_list;
		for (next = util_av_get_data(av, i); index > *next;) {
			i = *next;
			next = util_av_get_data(av, i);
		}
		util_av_set_data(av, index, next, sizeof index);
		*next = index;
	}

	return ret;
}

int ofi_av_lookup_index(struct util_av *av, const void *addr, int slot)
{
	int ret;

	if (OFI_UNLIKELY(slot < 0 || slot >= av->hash.slots)) {
		FI_WARN(av->prov, FI_LOG_AV, "invalid slot (%d)\n", slot);
		return -FI_EINVAL;
	}

	fastlock_acquire(&av->lock);
	ret = util_av_lookup_index(av, addr, slot, NULL);
	fastlock_release(&av->lock);
	return ret;
}

int ofi_av_bind(struct fid *av_fid, struct fid *eq_fid, uint64_t flags)
{
	struct util_av *av;
	struct util_eq *eq;

	av = container_of(av_fid, struct util_av, av_fid.fid);
	if (eq_fid->fclass != FI_CLASS_EQ) {
		FI_WARN(av->prov, FI_LOG_AV, "invalid fid class\n");
		return -FI_EINVAL;
	}

	if (flags) {
		FI_WARN(av->prov, FI_LOG_AV, "invalid flags\n");
		return -FI_EINVAL;
	}

	eq = container_of(eq_fid, struct util_eq, eq_fid.fid);
	av->eq = eq;
	ofi_atomic_inc32(&eq->ref);
	return 0;
}

static void util_av_close(struct util_av *av)
{
	/* TODO: unmap data? */
	free(av->data);
}

int ofi_av_close_lightweight(struct util_av *av)
{
	if (ofi_atomic_get32(&av->ref)) {
		FI_WARN(av->prov, FI_LOG_AV, "AV is busy\n");
		return -FI_EBUSY;
	}

	if (av->eq)
		ofi_atomic_dec32(&av->eq->ref);

	ofi_atomic_dec32(&av->domain->ref);
	fastlock_destroy(&av->lock);

	return 0;
}

int ofi_av_close(struct util_av *av)
{
	int ret = ofi_av_close_lightweight(av);
	if (ret)
		return ret;
	util_av_close(av);
	return 0;
}

static void util_av_hash_init(struct util_av_hash *hash)
{
	int i;

	for (i = 0; i < hash->slots; i++) {
		hash->table[i].index = UTIL_NO_ENTRY;
		hash->table[i].next = UTIL_NO_ENTRY;
		ofi_atomic_initialize32(&hash->table[i].use_cnt, 0);
	}

	hash->free_list = hash->slots;
	for (i = hash->slots; i < hash->total_count; i++) {
		hash->table[i].index = UTIL_NO_ENTRY;
		hash->table[i].next = i + 1;
		ofi_atomic_initialize32(&hash->table[i].use_cnt, 0);
	}
	hash->table[hash->total_count - 1].next = UTIL_NO_ENTRY;
}

static int util_verify_av_util_attr(struct util_domain *domain,
				    const struct util_av_attr *util_attr)
{
	if (util_attr->flags & ~(OFI_AV_HASH)) {
		FI_WARN(domain->prov, FI_LOG_AV, "invalid internal flags\n");
		return -FI_EINVAL;
	}

	if (util_attr->addrlen < sizeof(int)) {
		FI_WARN(domain->prov, FI_LOG_AV, "unsupported address size\n");
		return -FI_ENOSYS;
	}

	return 0;
}

static int util_av_init(struct util_av *av, const struct fi_av_attr *attr,
			const struct util_av_attr *util_attr)
{
	int *entry, i, ret = 0;
	size_t max_count;

	ret = util_verify_av_util_attr(av->domain, util_attr);
	if (ret)
		return ret;

	if (attr->count) {
		max_count = attr->count;
	} else {
		if (fi_param_get_size_t(NULL, "universe_size", &max_count))
			max_count = UTIL_DEFAULT_AV_SIZE;
	}

	av->count = max_count ? max_count : UTIL_DEFAULT_AV_SIZE;
	av->count = roundup_power_of_two(av->count);
	av->addrlen = util_attr->addrlen;
	av->flags = util_attr->flags | attr->flags;

	FI_INFO(av->prov, FI_LOG_AV, "AV size %zu\n", av->count);

	/* TODO: Handle FI_READ */
	/* TODO: Handle mmap - shared AV */

	if (util_attr->flags & OFI_AV_HASH) {
		av->hash.slots = av->count;
		if (util_attr->overhead)
			av->hash.total_count = av->count + util_attr->overhead;
		else
			av->hash.total_count = av->count * 2;
		FI_INFO(av->prov, FI_LOG_AV,
		       "OFI_AV_HASH requested, hash size %u\n", av->hash.total_count);
	}

	av->data = malloc((av->count * util_attr->addrlen) +
			  (av->hash.total_count * sizeof(*av->hash.table)));
	if (!av->data)
		return -FI_ENOMEM;

	for (i = 0; i < (int)av->count - 1; i++) {
		entry = util_av_get_data(av, i);
		*entry = i + 1;
	}
	entry = util_av_get_data(av, av->count - 1);
	*entry = UTIL_NO_ENTRY;

	if (util_attr->flags & OFI_AV_HASH) {
		av->hash.table = util_av_get_data(av, av->count);
		util_av_hash_init(&av->hash);
	}

	return ret;
}

static int util_verify_av_attr(struct util_domain *domain,
			       const struct fi_av_attr *attr)
{
	switch (attr->type) {
	case FI_AV_MAP:
	case FI_AV_TABLE:
		if ((domain->av_type != FI_AV_UNSPEC) &&
		    (attr->type != domain->av_type)) {
			FI_INFO(domain->prov, FI_LOG_AV, "Invalid AV type\n");
		   	return -FI_EINVAL;
		}
		break;
	default:
		FI_WARN(domain->prov, FI_LOG_AV, "invalid av type\n");
		return -FI_EINVAL;
	}

	if (attr->name) {
		FI_WARN(domain->prov, FI_LOG_AV, "Shared AV is unsupported\n");
		return -FI_ENOSYS;
	}

	if (attr->flags & ~(FI_EVENT | FI_READ | FI_SYMMETRIC)) {
		FI_WARN(domain->prov, FI_LOG_AV, "invalid flags\n");
		return -FI_EINVAL;
	}

	return 0;
}

int ofi_av_init_lightweight(struct util_domain *domain, const struct fi_av_attr *attr,
			    struct util_av *av, void *context)
{
	int ret;

	ret = util_verify_av_attr(domain, attr);
	if (ret)
		return ret;

	av->prov = domain->prov;
	ofi_atomic_initialize32(&av->ref, 0);
	fastlock_init(&av->lock);
	av->av_fid.fid.fclass = FI_CLASS_AV;
	/*
	 * ops set by provider
	 * av->av_fid.fid.ops = &prov_av_fi_ops;
	 * av->av_fid.ops = &prov_av_ops;
	 */
	av->context = context;
	av->domain = domain;
	dlist_init(&av->ep_list);
	ofi_atomic_inc32(&domain->ref);
	return 0;
}

int ofi_av_init(struct util_domain *domain, const struct fi_av_attr *attr,
		const struct util_av_attr *util_attr,
		struct util_av *av, void *context)
{
	int ret = ofi_av_init_lightweight(domain, attr, av, context);
	if (ret)
		return ret;

	ret = util_av_init(av, attr, util_attr);
	if (ret)
		return ret;
	return ret;
}


/*************************************************************************
 *
 * AV for IP addressing
 *
 *************************************************************************/

static int ip_av_slot(struct util_av *av, const struct sockaddr *sa)
{
	uint32_t host;
	uint16_t port;

	if (!sa)
		return UTIL_NO_ENTRY;

	switch (((struct sockaddr *) sa)->sa_family) {
	case AF_INET:
		host = (uint16_t) ntohl(((struct sockaddr_in *) sa)->
					sin_addr.s_addr);
		port = ntohs(((struct sockaddr_in *) sa)->sin_port);
		break;
	case AF_INET6:
		host = (uint16_t) ((struct sockaddr_in6 *) sa)->
					sin6_addr.s6_addr[15];
		port = ntohs(((struct sockaddr_in6 *) sa)->sin6_port);
		break;
	default:
		assert(0);
		return UTIL_NO_ENTRY;
	}

	/* TODO: Find a good hash function */
	FI_DBG(av->prov, FI_LOG_AV, "slot %d\n",
		((host << 16) | port) % av->hash.slots);
	return ((host << 16) | port) % av->hash.slots;
}

int ip_av_get_index(struct util_av *av, const void *addr)
{
	return ofi_av_lookup_index(av, addr, ip_av_slot(av, addr));
}

void ofi_av_write_event(struct util_av *av, uint64_t data,
			int err, void *context)
{
	struct fi_eq_err_entry entry = { 0 };
	size_t size;
	ssize_t ret;
	uint64_t flags;

	entry.fid = &av->av_fid.fid;
	entry.context = context;
	entry.data = data;

	if (err) {
		FI_INFO(av->prov, FI_LOG_AV, "writing error entry to EQ\n");
		entry.err = err;
		size = sizeof(struct fi_eq_err_entry);
		flags = UTIL_FLAG_ERROR;
	} else {
		FI_DBG(av->prov, FI_LOG_AV, "writing entry to EQ\n");
		size = sizeof(struct fi_eq_entry);
		flags = 0;
	}

	ret = fi_eq_write(&av->eq->eq_fid, FI_AV_COMPLETE, &entry,
			  size, flags);
	if (ret != size)
		FI_WARN(av->prov, FI_LOG_AV, "error writing to EQ\n");
}

static int ip_av_valid_addr(struct util_av *av, const void *addr)
{
	const struct sockaddr_in *sin = addr;
	const struct sockaddr_in6 *sin6 = addr;

	switch (sin->sin_family) {
	case AF_INET:
		return sin->sin_port && sin->sin_addr.s_addr;
	case AF_INET6:
		return sin6->sin6_port &&
		      memcmp(&in6addr_any, &sin6->sin6_addr, sizeof(in6addr_any));
	default:
		return 0;
	}
}

static int ip_av_insert_addr(struct util_av *av, const void *addr,
			     fi_addr_t *fi_addr, void *context)
{
	int ret, index = -1;

	if (ip_av_valid_addr(av, addr)) {
		fastlock_acquire(&av->lock);
		ret = ofi_av_insert_addr(av, addr, ip_av_slot(av, addr), &index);
		fastlock_release(&av->lock);
	} else {
		ret = -FI_EADDRNOTAVAIL;
		FI_WARN(av->prov, FI_LOG_AV, "invalid address\n");
	}

	if (fi_addr)
		*fi_addr = !ret ? index : FI_ADDR_NOTAVAIL;

	ofi_straddr_dbg(av->prov, FI_LOG_AV, "av_insert addr", addr);
	if (fi_addr)
		FI_DBG(av->prov, FI_LOG_AV, "av_insert fi_addr: %" PRIu64 "\n",
		       *fi_addr);

	return ret;
}

int ofi_ip_av_insertv(struct util_av *av, const void *addr, size_t addrlen,
		      size_t count, fi_addr_t *fi_addr, void *context)
{
	int ret, success_cnt = 0;
	size_t i;

	FI_DBG(av->prov, FI_LOG_AV, "inserting %zu addresses\n", count);
	for (i = 0; i < count; i++) {
		ret = ip_av_insert_addr(av, (const char *) addr + i * addrlen,
					fi_addr ? &fi_addr[i] : NULL, context);
		if (!ret)
			success_cnt++;
		else if (av->eq)
			ofi_av_write_event(av, i, -ret, context);
	}

	FI_DBG(av->prov, FI_LOG_AV, "%d addresses successful\n", success_cnt);
	if (av->eq) {
		ofi_av_write_event(av, success_cnt, 0, context);
		ret = 0;
	} else {
		ret = success_cnt;
	}
	return ret;
}

int ofi_ip_av_insert(struct fid_av *av_fid, const void *addr, size_t count,
		     fi_addr_t *fi_addr, uint64_t flags, void *context)
{
	struct util_av *av;
	int ret;

	av = container_of(av_fid, struct util_av, av_fid);
	ret = ofi_verify_av_insert(av, flags);
	if (ret)
		return ret;

	return ofi_ip_av_insertv(av, addr, ofi_sizeofaddr(addr),
				 count, fi_addr, context);
}

static int ip_av_insertsvc(struct fid_av *av, const char *node,
			   const char *service, fi_addr_t *fi_addr,
			   uint64_t flags, void *context)
{
	return fi_av_insertsym(av, node, 1, service, 1, fi_addr, flags, context);
}

/* Caller should free *addr */
static int
ip_av_ip4sym_getaddr(struct util_av *av, struct in_addr ip, size_t ipcnt,
		     uint16_t port, size_t portcnt, void **addr, size_t *addrlen)
{
	struct sockaddr_in *sin;
	int count = ipcnt * portcnt;
	size_t i, p, k;

	*addrlen = sizeof(*sin);
	sin = calloc(count, *addrlen);
	if (!sin)
		return -FI_ENOMEM;

	for (i = 0, k = 0; i < ipcnt; i++) {
		for (p = 0; p < portcnt; p++, k++) {
			sin[k].sin_family = AF_INET;
			/* TODO: should we skip addresses x.x.x.0 and x.x.x.255? */
			sin[k].sin_addr.s_addr = htonl(ntohl(ip.s_addr) + i);
			sin[k].sin_port = htons(port + p);
		}
	}
	*addr = sin;
	return count;
}

/* Caller should free *addr */
static int
ip_av_ip6sym_getaddr(struct util_av *av, struct in6_addr ip, size_t ipcnt,
		     uint16_t port, size_t portcnt, void **addr, size_t *addrlen)
{
	struct sockaddr_in6 *sin6, sin6_temp;
	int j, count = ipcnt * portcnt;
	size_t i, p, k;

	*addrlen = sizeof(*sin6);
	sin6 = calloc(count, *addrlen);
	if (!sin6)
		return -FI_ENOMEM;

	sin6_temp.sin6_addr = ip;

	for (i = 0, k = 0; i < ipcnt; i++) {
		for (p = 0; p < portcnt; p++, k++) {
			sin6[k].sin6_family = AF_INET6;
			sin6[k].sin6_addr = sin6_temp.sin6_addr;
			sin6[k].sin6_port = htons(port + p);
		}
		/* TODO: should we skip addresses x::0 and x::255? */
		for (j = 15; j >= 0; j--) {
			if (++sin6_temp.sin6_addr.s6_addr[j] < 255)
				break;
		}
	}
	*addr = sin6;
	return count;
}

/* Caller should free *addr */
static int ip_av_nodesym_getaddr(struct util_av *av, const char *node,
				 size_t nodecnt, const char *service,
				 size_t svccnt, void **addr, size_t *addrlen)
{
	struct addrinfo hints, *ai;
	void *addr_temp;
	char name[FI_NAME_MAX];
	char svc[FI_NAME_MAX];
	size_t name_len, n, s;
	int ret, name_index, svc_index, count = nodecnt * svccnt;

	memset(&hints, 0, sizeof hints);

	hints.ai_socktype = SOCK_DGRAM;
	switch (av->domain->addr_format) {
	case FI_SOCKADDR_IN:
		hints.ai_family = AF_INET;
		*addrlen = sizeof(struct sockaddr_in);
		break;
	case FI_SOCKADDR_IN6:
		hints.ai_family = AF_INET6;
		*addrlen = sizeof(struct sockaddr_in6);
		break;
	default:
		FI_INFO(av->prov, FI_LOG_AV, "Unknown address format!\n");
		return -FI_EINVAL;
	}

	*addr = calloc(nodecnt * svccnt, *addrlen);
	if (!*addr)
		return -FI_ENOMEM;

	addr_temp = *addr;

	for (name_len = strlen(node); isdigit(node[name_len - 1]); )
		name_len--;

	memcpy(name, node, name_len);
	name_index = atoi(node + name_len);
	svc_index = atoi(service);

	for (n = 0; n < nodecnt; n++) {
		if (nodecnt == 1) {
			strncpy(name, node, sizeof(name) - 1);
			name[FI_NAME_MAX - 1] = '\0';
		} else {
			snprintf(name + name_len, sizeof(name) - name_len - 1,
				 "%zu", name_index + n);
		}

		for (s = 0; s < svccnt; s++) {
			if (svccnt == 1) {
				strncpy(svc, service, sizeof(svc) - 1);
				svc[FI_NAME_MAX - 1] = '\0';
			} else {
				snprintf(svc, sizeof(svc) - 1,
					 "%zu", svc_index + s);
			}
			FI_INFO(av->prov, FI_LOG_AV, "resolving %s:%s for AV "
				"insert\n", node, service);

			ret = getaddrinfo(node, service, &hints, &ai);
			if (ret)
				goto err;

			memcpy(addr_temp, ai->ai_addr, *addrlen);
			addr_temp = (char *)addr_temp + *addrlen;
			freeaddrinfo(ai);
		}
	}
	return count;
err:
	freeaddrinfo(ai);
	free(addr);
	return ret;
}

/* Caller should free *addr */
int ofi_ip_av_sym_getaddr(struct util_av *av, const char *node,
			  size_t nodecnt, const char *service,
			  size_t svccnt, void **addr, size_t *addrlen)
{
	struct in6_addr ip6;
	struct in_addr ip4;
	int ret;

	if (strlen(node) >= FI_NAME_MAX || strlen(service) >= FI_NAME_MAX) {
		FI_WARN(av->prov, FI_LOG_AV,
			"node or service name is too long\n");
		return -FI_ENOSYS;
	}

	ret = inet_pton(AF_INET, node, &ip4);
	if (ret == 1) {
		FI_INFO(av->prov, FI_LOG_AV, "insert symmetric IPv4\n");
		return ip_av_ip4sym_getaddr(av, ip4, nodecnt,
					  (uint16_t) strtol(service, NULL, 0),
					  svccnt, addr, addrlen);
	}

	ret = inet_pton(AF_INET6, node, &ip6);
	if (ret == 1) {
		FI_INFO(av->prov, FI_LOG_AV, "insert symmetric IPv6\n");
		return ip_av_ip6sym_getaddr(av, ip6, nodecnt,
					  (uint16_t) strtol(service, NULL, 0),
					  svccnt, addr, addrlen);
	}

	FI_INFO(av->prov, FI_LOG_AV, "insert symmetric host names\n");
	return ip_av_nodesym_getaddr(av, node, nodecnt, service,
				     svccnt, addr, addrlen);
}

static int ip_av_insertsym(struct fid_av *av_fid, const char *node,
			   size_t nodecnt, const char *service, size_t svccnt,
			   fi_addr_t *fi_addr, uint64_t flags, void *context)
{
	struct util_av *av;
	void *addr;
	size_t addrlen;
	int ret, count;

	av = container_of(av_fid, struct util_av, av_fid);
	ret = ofi_verify_av_insert(av, flags);
	if (ret)
		return ret;

	count = ofi_ip_av_sym_getaddr(av, node, nodecnt, service,
				      svccnt, &addr, &addrlen);
	if (count <= 0)
		return count;

	ret = ofi_ip_av_insertv(av, addr, addrlen, count,
				fi_addr, context);
	free(addr);
	return ret;
}

int ofi_ip_av_remove(struct fid_av *av_fid, fi_addr_t *fi_addr,
		     size_t count, uint64_t flags)
{
	struct util_av *av;
	int i, slot, index, ret;

	av = container_of(av_fid, struct util_av, av_fid);
	if (flags) {
		FI_WARN(av->prov, FI_LOG_AV, "invalid flags\n");
		return -FI_EINVAL;
	}

	/*
	 * It's more efficient to remove addresses from high to low index.
	 * We assume that addresses are removed in the same order that they were
	 * added -- i.e. fi_addr passed in here was also passed into insert.
	 * Thus, we walk through the array backwards.
	 */
	for (i = count - 1; i >= 0; i--) {
		index = (int) fi_addr[i];
		slot = ip_av_slot(av, ip_av_get_addr(av, index));
		fastlock_acquire(&av->lock);
		ret = ofi_av_remove_addr(av, slot, index);
		fastlock_release(&av->lock);
		if (ret) {
			FI_WARN(av->prov, FI_LOG_AV,
				"removal of fi_addr %d failed\n", index);
		}
	}
	return 0;
}

int ofi_ip_av_lookup(struct fid_av *av_fid, fi_addr_t fi_addr,
		     void *addr, size_t *addrlen)
{
	struct util_av *av;
	int index;

	av = container_of(av_fid, struct util_av, av_fid);
	index = (int) fi_addr;
	if (index < 0 || (size_t)index > av->count) {
		FI_WARN(av->prov, FI_LOG_AV, "unknown address\n");
		return -FI_EINVAL;
	}

	memcpy(addr, ip_av_get_addr(av, index),
	       MIN(*addrlen, av->addrlen));
	*addrlen = av->addrlen;
	return 0;
}

const char *
ofi_ip_av_straddr(struct fid_av *av, const void *addr, char *buf, size_t *len)
{
	return ofi_straddr(buf, len, FI_SOCKADDR, addr);
}

static struct fi_ops_av ip_av_ops = {
	.size = sizeof(struct fi_ops_av),
	.insert = ofi_ip_av_insert,
	.insertsvc = ip_av_insertsvc,
	.insertsym = ip_av_insertsym,
	.remove = ofi_ip_av_remove,
	.lookup = ofi_ip_av_lookup,
	.straddr = ofi_ip_av_straddr,
};

static int ip_av_close(struct fid *av_fid)
{
	struct util_av *av;
	int ret;

	av = container_of(av_fid, struct util_av, av_fid.fid);
	ret = ofi_av_close(av);
	if (ret)
		return ret;
	free(av);
	return 0;
}

static struct fi_ops ip_av_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = ip_av_close,
	.bind = ofi_av_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

int ip_av_create_flags(struct fid_domain *domain_fid, struct fi_av_attr *attr,
		       struct fid_av **av, void *context, int flags)
{
	struct util_domain *domain;
	struct util_av_attr util_attr;
	struct util_av *util_av;
	int ret;

	domain = container_of(domain_fid, struct util_domain, domain_fid);
	if (domain->addr_format == FI_SOCKADDR_IN)
		util_attr.addrlen = sizeof(struct sockaddr_in);
	else
		util_attr.addrlen = sizeof(struct sockaddr_in6);

	util_attr.overhead = attr->count >> 1;
	util_attr.flags = flags;

	if (attr->type == FI_AV_UNSPEC)
		attr->type = FI_AV_MAP;

	util_av = calloc(1, sizeof(*util_av));
	if (!util_av)
		return -FI_ENOMEM;

	ret = ofi_av_init(domain, attr, &util_attr, util_av, context);
	if (ret) {
		free(util_av);
		return ret;
	}

	*av = &util_av->av_fid;
	(*av)->fid.ops = &ip_av_fi_ops;
	(*av)->ops = &ip_av_ops;
	return 0;
}

int ip_av_create(struct fid_domain *domain_fid, struct fi_av_attr *attr,
		 struct fid_av **av, void *context)
{
	struct util_domain *domain = container_of(domain_fid, struct util_domain,
						  domain_fid);

	return ip_av_create_flags(domain_fid, attr, av, context,
				  (domain->info_domain_caps & FI_SOURCE) ?
				  OFI_AV_HASH : 0);
}
