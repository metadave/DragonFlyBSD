/*
 * Copyright (c) 2007 The DragonFly Project.  All rights reserved.
 * 
 * This code is derived from software contributed to The DragonFly Project
 * by Sepherosa Ziehau <sepherosa@gmail.com>
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * 
 * $DragonFly: src/sys/net/dummynet/ip_dummynet_glue.c,v 1.2 2007/11/17 08:07:27 sephe Exp $
 */

#include <sys/param.h>
#include <sys/mbuf.h>
#include <sys/msgport.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/route.h>
#include <net/ethernet.h>
#include <net/netisr.h>
#include <net/netmsg2.h>

#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>

#include <net/dummynet/ip_dummynet.h>

static void	ip_dn_ether_output(struct netmsg *);
static void	ip_dn_ether_demux(struct netmsg *);
static void	ip_dn_ip_input(struct netmsg *);
static void	ip_dn_ip_output(struct netmsg *);

static void	ip_dn_freepkt_dispatch(struct netmsg *);
static void	ip_dn_dispatch(struct netmsg *);

static void	ip_dn_freepkt(struct dn_pkt *);

ip_dn_io_t	*ip_dn_io_ptr;
int		ip_dn_cpu = 0;

void
ip_dn_queue(struct mbuf *m)
{
	struct netmsg_packet *nmp;
	lwkt_port_t port;

	KASSERT(m->m_type != MT_TAG, ("mbuf contains old style tag!\n"));
	M_ASSERTPKTHDR(m);
	KASSERT(m->m_pkthdr.fw_flags & DUMMYNET_MBUF_TAGGED,
		("mbuf is not tagged for dummynet!\n"));

	nmp = &m->m_hdr.mh_netmsg;
	netmsg_init(&nmp->nm_netmsg, &netisr_apanic_rport, 0,
		    ip_dn_dispatch);
	nmp->nm_packet = m;

	port = cpu_portfn(ip_dn_cpu);
	lwkt_sendmsg(port, &nmp->nm_netmsg.nm_lmsg);
}

void
ip_dn_packet_free(struct dn_pkt *pkt)
{
	struct netmsg_packet *nmp;
	lwkt_port_t port;
	struct mbuf *m = pkt->dn_m;

	KASSERT(m->m_type != MT_TAG, ("mbuf contains old style tag!\n"));
	M_ASSERTPKTHDR(m);
	KASSERT(m->m_pkthdr.fw_flags & DUMMYNET_MBUF_TAGGED,
		("mbuf is not tagged for dummynet!\n"));

	if (pkt->cpuid == mycpuid) {
		ip_dn_freepkt(pkt);
		return;
	}

	nmp = &m->m_hdr.mh_netmsg;
	netmsg_init(&nmp->nm_netmsg, &netisr_apanic_rport, 0,
		    ip_dn_freepkt_dispatch);
	nmp->nm_packet = m;

	port = cpu_portfn(pkt->cpuid);
	lwkt_sendmsg(port, &nmp->nm_netmsg.nm_lmsg);
}

void
ip_dn_packet_redispatch(struct dn_pkt *pkt)
{
	static const netisr_fn_t dispatches[DN_TO_MAX] = {
	[DN_TO_IP_OUT]		= ip_dn_ip_output,
	[DN_TO_IP_IN]		= ip_dn_ip_input,
	[DN_TO_ETH_DEMUX]	= ip_dn_ether_demux,
	[DN_TO_ETH_OUT]		= ip_dn_ether_output
	};

	struct netmsg_packet *nmp;
	struct mbuf *m;
	netisr_fn_t dispatch;
	lwkt_port_t port;
	int dir;

	dir = (pkt->dn_flags & DN_FLAGS_DIR_MASK);
	KASSERT(dir < DN_TO_MAX,
		("unknown dummynet redispatch dir %d\n", dir));

	dispatch = dispatches[dir];
	KASSERT(dispatch != NULL,
		("unsupported dummynet redispatch dir %d\n", dir));

	m = pkt->dn_m;
	KASSERT(m->m_type != MT_TAG, ("mbuf contains old style tag!\n"));
	M_ASSERTPKTHDR(m);
	KASSERT(m->m_pkthdr.fw_flags & DUMMYNET_MBUF_TAGGED,
		("mbuf is not tagged for dummynet!\n"));

	nmp = &m->m_hdr.mh_netmsg;
	netmsg_init(&nmp->nm_netmsg, &netisr_apanic_rport, 0, dispatch);
	nmp->nm_packet = m;

	port = cpu_portfn(pkt->cpuid);
	lwkt_sendmsg(port, &nmp->nm_netmsg.nm_lmsg);
}

static void
ip_dn_freepkt(struct dn_pkt *pkt)
{
	struct rtentry *rt = pkt->ro.ro_rt;

	/* Unreference route entry */
	if (rt != NULL) {
		if (rt->rt_refcnt <= 0) {	/* XXX assert? */
			kprintf("-- warning, refcnt now %ld, decreasing\n",
				rt->rt_refcnt);
		}
		RTFREE(rt);
	}

	/* Unreference packet private data */
	if (pkt->dn_unref_priv)
		pkt->dn_unref_priv(pkt->dn_priv);

	/* Free the parent mbuf, this will free 'pkt' as well */
	m_freem(pkt->dn_m);
}

static void
ip_dn_freepkt_dispatch(struct netmsg *nmsg)
{
	struct netmsg_packet *nmp;
	struct mbuf *m;
	struct m_tag *mtag;
	struct dn_pkt *pkt;

	nmp = (struct netmsg_packet *)nmsg;
	m = nmp->nm_packet;
	M_ASSERTPKTHDR(m);
	KASSERT(m->m_pkthdr.fw_flags & DUMMYNET_MBUF_TAGGED,
		("mbuf is not tagged for dummynet!\n"));

	mtag = m_tag_find(m, PACKET_TAG_DUMMYNET, NULL);
	KKASSERT(mtag != NULL);

	pkt = m_tag_data(mtag);
	KASSERT(pkt->cpuid == mycpuid,
		("%s: dummynet packet was delivered to wrong cpu! "
		 "target cpuid %d, mycpuid %d\n", __func__,
		 pkt->cpuid, mycpuid));

	ip_dn_freepkt(pkt);
}

static void
ip_dn_dispatch(struct netmsg *nmsg)
{
	struct netmsg_packet *nmp;
	struct mbuf *m;
	struct m_tag *mtag;
	struct dn_pkt *pkt;

	KASSERT(ip_dn_cpu == mycpuid,
		("%s: dummynet packet was delivered to wrong cpu! "
		 "dummynet cpuid %d, mycpuid %d\n", __func__,
		 ip_dn_cpu, mycpuid));

	nmp = (struct netmsg_packet *)nmsg;
	m = nmp->nm_packet;
	M_ASSERTPKTHDR(m);
	KASSERT(m->m_pkthdr.fw_flags & DUMMYNET_MBUF_TAGGED,
		("mbuf is not tagged for dummynet!\n"));

	if (DUMMYNET_LOADED) {
		if (ip_dn_io_ptr(m) == 0)
			return;
	}

	/*
	 * ip_dn_io_ptr() failed or dummynet(4) is not loaded
	 */
	mtag = m_tag_find(m, PACKET_TAG_DUMMYNET, NULL);
	KKASSERT(mtag != NULL);

	pkt = m_tag_data(mtag);
	ip_dn_packet_free(pkt);
}

static void
ip_dn_ip_output(struct netmsg *nmsg)
{
	struct netmsg_packet *nmp;
	struct mbuf *m;
	struct m_tag *mtag;
	struct dn_pkt *pkt;
	struct rtentry *rt;
	ip_dn_unref_priv_t unref_priv;
	void *priv;

	nmp = (struct netmsg_packet *)nmsg;
	m = nmp->nm_packet;
	M_ASSERTPKTHDR(m);
	KASSERT(m->m_pkthdr.fw_flags & DUMMYNET_MBUF_TAGGED,
		("mbuf is not tagged for dummynet!\n"));

	mtag = m_tag_find(m, PACKET_TAG_DUMMYNET, NULL);
	KKASSERT(mtag != NULL);

	pkt = m_tag_data(mtag);
	KASSERT(pkt->cpuid == mycpuid,
		("%s: dummynet packet was delivered to wrong cpu! "
		 "target cpuid %d, mycpuid %d\n", __func__,
		 pkt->cpuid, mycpuid));
	KASSERT((pkt->dn_flags & DN_FLAGS_DIR_MASK) == DN_TO_IP_OUT,
		("wrong direction %d, should be %d\n",
		 (pkt->dn_flags & DN_FLAGS_DIR_MASK), DN_TO_IP_OUT));

	priv = pkt->dn_priv;
	unref_priv = pkt->dn_unref_priv;
	rt = pkt->ro.ro_rt;

	if (rt != NULL && !(rt->rt_flags & RTF_UP)) {
		/*
		 * Recorded rtentry is gone, when the packet
		 * was on delay line.
		 */
		ip_dn_freepkt(pkt);
		return;
	}

	ip_output(pkt->dn_m, NULL, NULL, 0, NULL, NULL);

	if (rt != NULL) {
		if (rt->rt_refcnt <= 0) {	/* XXX assert? */
			kprintf("-- warning, refcnt now %ld, decreasing\n",
				rt->rt_refcnt);
		}
		RTFREE(rt);
	}
	if (unref_priv)
		unref_priv(priv);
}

static void
ip_dn_ip_input(struct netmsg *nmsg)
{
	struct netmsg_packet *nmp;
	struct mbuf *m;
	struct m_tag *mtag;
	struct dn_pkt *pkt;
	ip_dn_unref_priv_t unref_priv;
	void *priv;

	nmp = (struct netmsg_packet *)nmsg;
	m = nmp->nm_packet;
	M_ASSERTPKTHDR(m);
	KASSERT(m->m_pkthdr.fw_flags & DUMMYNET_MBUF_TAGGED,
		("mbuf is not tagged for dummynet!\n"));

	mtag = m_tag_find(m, PACKET_TAG_DUMMYNET, NULL);
	KKASSERT(mtag != NULL);

	pkt = m_tag_data(mtag);
	KASSERT(pkt->cpuid == mycpuid,
		("%s: dummynet packet was delivered to wrong cpu! "
		 "target cpuid %d, mycpuid %d\n", __func__,
		 pkt->cpuid, mycpuid));
	KASSERT(pkt->ro.ro_rt == NULL,
		("route entry is not NULL for ip_input\n"));
	KASSERT((pkt->dn_flags & DN_FLAGS_DIR_MASK) == DN_TO_IP_IN,
		("wrong direction %d, should be %d\n",
		 (pkt->dn_flags & DN_FLAGS_DIR_MASK), DN_TO_IP_IN));

	priv = pkt->dn_priv;
	unref_priv = pkt->dn_unref_priv;

	ip_input(m);

	if (unref_priv)
		unref_priv(priv);
}

static void
ip_dn_ether_demux(struct netmsg *nmsg)
{
	struct netmsg_packet *nmp;
	struct mbuf *m;
	struct m_tag *mtag;
	struct dn_pkt *pkt;
	struct ether_header *eh;
	ip_dn_unref_priv_t unref_priv;
	void *priv;

	nmp = (struct netmsg_packet *)nmsg;
	m = nmp->nm_packet;
	M_ASSERTPKTHDR(m);
	KASSERT(m->m_pkthdr.fw_flags & DUMMYNET_MBUF_TAGGED,
		("mbuf is not tagged for dummynet!\n"));

	mtag = m_tag_find(m, PACKET_TAG_DUMMYNET, NULL);
	KKASSERT(mtag != NULL);

	pkt = m_tag_data(mtag);
	KASSERT(pkt->cpuid == mycpuid,
		("%s: dummynet packet was delivered to wrong cpu! "
		 "target cpuid %d, mycpuid %d\n", __func__,
		 pkt->cpuid, mycpuid));
	KASSERT(pkt->ro.ro_rt == NULL,
		("route entry is not NULL for ether_demux\n"));
	KASSERT((pkt->dn_flags & DN_FLAGS_DIR_MASK) == DN_TO_ETH_DEMUX,
		("wrong direction %d, should be %d\n",
		 (pkt->dn_flags & DN_FLAGS_DIR_MASK), DN_TO_ETH_DEMUX));

	priv = pkt->dn_priv;
	unref_priv = pkt->dn_unref_priv;

	if (m->m_len < ETHER_HDR_LEN &&
	    (m = m_pullup(m, ETHER_HDR_LEN)) == NULL) {
		kprintf("%s: pullup fail, dropping pkt\n", __func__);
		goto back;
	}

	/*
	 * Same as ether_input, make eh be a pointer into the mbuf
	 */
	eh = mtod(m, struct ether_header *);
	m_adj(m, ETHER_HDR_LEN);
	ether_demux(NULL, eh, m);
back:
	if (unref_priv)
		unref_priv(priv);
}

static void
ip_dn_ether_output(struct netmsg *nmsg)
{
	struct netmsg_packet *nmp;
	struct mbuf *m;
	struct m_tag *mtag;
	struct dn_pkt *pkt;
	ip_dn_unref_priv_t unref_priv;
	void *priv;

	nmp = (struct netmsg_packet *)nmsg;
	m = nmp->nm_packet;
	M_ASSERTPKTHDR(m);
	KASSERT(m->m_pkthdr.fw_flags & DUMMYNET_MBUF_TAGGED,
		("mbuf is not tagged for dummynet!\n"));

	mtag = m_tag_find(m, PACKET_TAG_DUMMYNET, NULL);
	KKASSERT(mtag != NULL);

	pkt = m_tag_data(mtag);
	KASSERT(pkt->cpuid == mycpuid,
		("%s: dummynet packet was delivered to wrong cpu! "
		 "target cpuid %d, mycpuid %d\n", __func__,
		 pkt->cpuid, mycpuid));
	KASSERT(pkt->ro.ro_rt == NULL,
		("route entry is not NULL for ether_output_frame\n"));
	KASSERT((pkt->dn_flags & DN_FLAGS_DIR_MASK) == DN_TO_ETH_OUT,
		("wrong direction %d, should be %d\n",
		 (pkt->dn_flags & DN_FLAGS_DIR_MASK), DN_TO_ETH_OUT));

	priv = pkt->dn_priv;
	unref_priv = pkt->dn_unref_priv;

	ether_output_frame(pkt->ifp, m);

	if (unref_priv)
		unref_priv(priv);
}
