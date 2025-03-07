/*   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; version 2 of the License
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   Copyright (C) 2018 John Crispin <john@phrozen.org>
 */

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include "mtk_offload.h"

#define INVALID	0
#define UNBIND	1
#define BIND	2
#define FIN	3

#define IPV4_HNAPT			0
#define IPV4_HNAT			1

struct mtk_ppe_account_group {
	unsigned int hash;
	unsigned int state;
	unsigned long jiffies;
	unsigned long long bytes;
	unsigned long long packets;
	unsigned int speed_bytes[4];
	unsigned int speed_packets[4];
	void *priv; /* for keepalive callback */
};

static struct mtk_ppe_account_group mtk_ppe_account_group_entry[64];

static u32 mtk_ppe_account_group_alloc(void)
{
	u32 i;
	for (i = 1; i < 64; i++) {
		if (mtk_ppe_account_group_entry[i].state == FOE_STATE_INVALID) {
			mtk_ppe_account_group_entry[i].state = FOE_STATE_FIN; /* mark FIN as in use begin */
			mtk_ppe_account_group_entry[i].bytes = 0;
			mtk_ppe_account_group_entry[i].packets = 0;
			mtk_ppe_account_group_entry[i].jiffies = jiffies;
			return i;
		}
	}
	return 0;
}

static struct mtk_ppe_account_group *mtk_ppe_account_group_get(u32 idx)
{
	if (idx > 0 && idx < 64) {
		return &mtk_ppe_account_group_entry[idx];
	}
	return NULL;
}

static struct timer_list ag_timer;
static void *ag_timer_eth =  NULL;

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)
static void mtk_ppe_account_group_walk(unsigned long ignore)
#else
static void mtk_ppe_account_group_walk(struct timer_list *ignore)
#endif
{
	u32 i;
	unsigned long long bytes, packets;
	struct mtk_ppe_account_group *ag;
	struct mtk_eth *eth = (struct mtk_eth *)ag_timer_eth;
	void (*func)(unsigned int, unsigned long, unsigned long, unsigned int *, unsigned int *);
	for (i = 1; i < 64; i++) {
		ag = mtk_ppe_account_group_get(i);
		if (ag->state == FOE_STATE_BIND) {
			bytes = mtk_r32(eth, 0x2000 + i * 16);
			bytes += ((unsigned long long)mtk_r32(eth, 0x2000 + i * 16 + 4)) << 32;
			packets = mtk_r32(eth, 0x2000 + i * 16 + 8);
			if (bytes > 0 || packets > 0) {
				ag->jiffies = jiffies;
				ag->bytes += bytes;
				ag->packets += packets;
			}
			ag->speed_bytes[(jiffies/HZ) % 4] = (unsigned int)bytes;
			ag->speed_packets[(jiffies/HZ) % 4] = (unsigned int)packets;

			if ((func = ag->priv) != NULL && (((jiffies/HZ) % 2 == 0 && i % 2 == 0) || ((jiffies/HZ) % 2 == 1 && i % 2 == 1)) ) {
				struct mtk_foe_entry *entry = &eth->foe_table[ag->hash];
				if (entry->bfib1.state == BIND && bytes > 0 && packets > 0) {
					bytes = ag->bytes;
					packets = ag->packets;
					func(ag->hash, bytes, packets, ag->speed_bytes, ag->speed_packets);
					ag->bytes = 0;
					ag->packets = 0;
				} else {
					ag->priv = NULL;
				}
			}

			//printk("hnat-walk-ag[%u]: hash=%u bytes=%llu packets=%llu\n", i, ag->hash, bytes, packets);
			if (time_before(ag->jiffies + 15 * HZ, jiffies)) {
				ag->state = FOE_STATE_INVALID;
				//printk("hnat-walk-ag[%u]: hash=%u timeout\n", i, ag->hash);
			}
		} else if (ag->state == FOE_STATE_FIN) {
			if (time_before(ag->jiffies + 15 * HZ, jiffies)) {
				ag->state = FOE_STATE_INVALID;
			}
		}
	}

	mod_timer(&ag_timer, jiffies + HZ * 1);
}

static void mtk_ppe_account_group_walk_stop(void)
{
	u32 i;
	struct mtk_ppe_account_group *ag;
	for (i = 1; i < 64; i++) {
		ag = mtk_ppe_account_group_get(i);
		if (ag->state == FOE_STATE_BIND) {
			ag->state = FOE_STATE_INVALID;
		}
	}
}

static u32
mtk_flow_hash_v4(flow_offload_tuple_t *tuple)
{
	u32 ports = ntohs(tuple->src_port)  << 16 | ntohs(tuple->dst_port);
	u32 src = ntohl(tuple->dst_v4.s_addr);
	u32 dst = ntohl(tuple->src_v4.s_addr);
	u32 hash = (ports & src) | ((~ports) & dst);
	u32 hash_23_0 = hash & 0xffffff;
	u32 hash_31_24 = hash & 0xff000000;

	hash = ports ^ src ^ dst ^ ((hash_23_0 << 8) | (hash_31_24 >> 24));
	hash = ((hash & 0xffff0000) >> 16 ) ^ (hash & 0xfffff);
	hash &= 0xfff;
	hash *= 2;;

	return hash;
}

static int
mtk_foe_prepare_v4(struct mtk_foe_entry *entry,
		   flow_offload_tuple_t *tuple,
		   flow_offload_tuple_t *dest_tuple,
		   flow_offload_hw_path_t *src,
		   flow_offload_hw_path_t *dest)
{
	int is_mcast = !!is_multicast_ether_addr(dest->eth_dest);

	if (tuple->l4proto == IPPROTO_UDP)
		entry->ipv4_hnapt.bfib1.udp = 1;

	entry->ipv4_hnapt.etype = htons(ETH_P_IP);
	entry->ipv4_hnapt.bfib1.pkt_type = IPV4_HNAPT;
	entry->ipv4_hnapt.iblk2.fqos = 0;
	entry->ipv4_hnapt.bfib1.ttl = 1;
	entry->ipv4_hnapt.bfib1.cah = 1;
	entry->ipv4_hnapt.bfib1.ka = 1;
	entry->ipv4_hnapt.iblk2.mcast = is_mcast;
	entry->ipv4_hnapt.iblk2.dscp = 0;
	entry->ipv4_hnapt.iblk2.port_mg = 0x3f;
	entry->ipv4_hnapt.iblk2.port_ag = mtk_ppe_account_group_alloc();
#ifdef CONFIG_NET_RALINK_HW_QOS
	entry->ipv4_hnapt.iblk2.qid = 1;
	entry->ipv4_hnapt.iblk2.fqos = 1;
#endif
#ifdef CONFIG_RALINK
	entry->ipv4_hnapt.iblk2.dp = (dest->dev->netdev_ops->ndo_flow_offload ? 1 : 0);
	if ((dest->flags & FLOW_OFFLOAD_PATH_VLAN) && (dest->vlan_id > 1))
		entry->ipv4_hnapt.iblk2.qid += 8;
#else
	entry->ipv4_hnapt.iblk2.dp = (dest->dev->name[3] - '0') + 1;
#endif

	entry->ipv4_hnapt.sip = ntohl(tuple->src_v4.s_addr);
	entry->ipv4_hnapt.dip = ntohl(tuple->dst_v4.s_addr);
	entry->ipv4_hnapt.sport = ntohs(tuple->src_port);
	entry->ipv4_hnapt.dport = ntohs(tuple->dst_port);

	entry->ipv4_hnapt.new_sip = ntohl(dest_tuple->dst_v4.s_addr);
	entry->ipv4_hnapt.new_dip = ntohl(dest_tuple->src_v4.s_addr);
	entry->ipv4_hnapt.new_sport = ntohs(dest_tuple->dst_port);
	entry->ipv4_hnapt.new_dport = ntohs(dest_tuple->src_port);

	entry->bfib1.state = BIND;

	if (dest->flags & FLOW_OFFLOAD_PATH_PPPOE) {
		entry->bfib1.psn = 1;
		entry->ipv4_hnapt.etype = htons(ETH_P_PPP_SES);
		entry->ipv4_hnapt.pppoe_id = dest->pppoe_sid;
	}

	if (dest->flags & FLOW_OFFLOAD_PATH_VLAN) {
		entry->ipv4_hnapt.vlan1 = dest->vlan_id;
		entry->bfib1.vlan_layer = 1;

		switch (dest->vlan_proto) {
		case htons(ETH_P_8021Q):
			entry->ipv4_hnapt.bfib1.vpm = 1;
			break;
		case htons(ETH_P_8021AD):
			entry->ipv4_hnapt.bfib1.vpm = 2;
			break;
		default:
			return -EINVAL;
		}
	}

	return 0;
}

static void
mtk_foe_set_mac(struct mtk_foe_entry *entry, u8 *smac, u8 *dmac)
{
	entry->ipv4_hnapt.dmac_hi = swab32(*((u32*) dmac));
	entry->ipv4_hnapt.dmac_lo = swab16(*((u16*) &dmac[4]));
	entry->ipv4_hnapt.smac_hi = swab32(*((u32*) smac));
	entry->ipv4_hnapt.smac_lo = swab16(*((u16*) &smac[4]));
}

static int
mtk_check_entry_available(struct mtk_eth *eth, u32 hash)
{
	struct mtk_foe_entry entry = ((struct mtk_foe_entry *)eth->foe_table)[hash];

	return (entry.bfib1.state == BIND || entry.udib1.sta == 1)? 0:1;
}

static void
mtk_foe_write(struct mtk_eth *eth, u32 hash,
	      struct mtk_foe_entry *entry)
{
	struct mtk_foe_entry *table = (struct mtk_foe_entry *)eth->foe_table;

	memcpy(&table[hash], entry, sizeof(*entry));
}

int mtk_flow_offload(struct mtk_eth *eth,
		     flow_offload_type_t type,
		     flow_offload_t *flow,
		     flow_offload_hw_path_t *src,
		     flow_offload_hw_path_t *dest)
{
	flow_offload_tuple_t *otuple = &flow->tuplehash[FLOW_OFFLOAD_DIR_ORIGINAL].tuple;
	flow_offload_tuple_t *rtuple = &flow->tuplehash[FLOW_OFFLOAD_DIR_REPLY].tuple;
	u32 time_stamp = mtk_r32(eth, 0x0010) & (0x7fff);
	u32 ohash, rhash;
	struct mtk_foe_entry orig = {
		.bfib1.time_stamp = time_stamp,
		.bfib1.psn = 0,
	};
	struct mtk_foe_entry reply = {
		.bfib1.time_stamp = time_stamp,
		.bfib1.psn = 0,
	};
	u32 ag_idx;
	struct mtk_ppe_account_group *ag;

	if (otuple->l4proto != IPPROTO_TCP && otuple->l4proto != IPPROTO_UDP)
		return -EINVAL;

	if (type == FLOW_OFFLOAD_DEL) {
		rhash = (unsigned long)flow->timeout;
		ohash = rhash >> 16;
		rhash &= 0xffff;
		flow = NULL;
		rcu_assign_pointer(eth->foe_flow_table[ohash], flow);
		rcu_assign_pointer(eth->foe_flow_table[rhash], flow);
		return 0;
	}

	switch (otuple->l3proto) {
	case AF_INET:
		if (mtk_foe_prepare_v4(&orig, otuple, rtuple, src, dest) ||
		    mtk_foe_prepare_v4(&reply, rtuple, otuple, dest, src))
			return -EINVAL;

		ohash = mtk_flow_hash_v4(otuple);
		rhash = mtk_flow_hash_v4(rtuple);
		break;

	case AF_INET6:
		return -EINVAL;

	default:
		return -EINVAL;
	}

	/* Two-way hash: when hash collision occurs, the hash value will be shifted to the next position. */
	if (!mtk_check_entry_available(eth, ohash)){       
		if (!mtk_check_entry_available(eth, ohash + 1))
			return -EINVAL;
                ohash += 1;
        }
        if (!mtk_check_entry_available(eth, rhash)){
		if (!mtk_check_entry_available(eth, rhash + 1))
                        return -EINVAL;
                rhash += 1;
	}

	if (ohash != ((flow->timeout >> 16) & 0xffff)) {
		if (ohash % 2 == 0) {
			if (!mtk_check_entry_available(eth, ohash + 1)) {
				return -EINVAL;
			} else {
				ohash += 1;
				if (ohash != ((flow->timeout >> 16) & 0xffff)) {
					return -EINVAL;
				}
			}
		} else {
			return -EINVAL;
		}
	}
	if (rhash != ((flow->timeout >> 0) & 0xffff)) {
		if (rhash % 2 == 0) {
			if (!mtk_check_entry_available(eth, rhash + 1)) {
				return -EINVAL;
			} else {
				rhash += 1;
				if (rhash != ((flow->timeout >> 0) & 0xffff)) {
					return -EINVAL;
				}
			}
		} else {
			return -EINVAL;
		}
	}

	mtk_foe_set_mac(&orig, dest->eth_src, dest->eth_dest);
	mtk_foe_set_mac(&reply, src->eth_src, src->eth_dest);
	mtk_foe_write(eth, ohash, &orig);
	mtk_foe_write(eth, rhash, &reply);

	//sync ag hash with foe hash
	ag_idx = orig.ipv4_hnapt.iblk2.port_ag;
	ag = mtk_ppe_account_group_get(ag_idx);
	if (ag) {
		ag->priv = NULL;
		ag->hash = ohash;
		ag->state = FOE_STATE_BIND;
	}
	ag_idx = reply.ipv4_hnapt.iblk2.port_ag;
	ag = mtk_ppe_account_group_get(ag_idx);
	if (ag) {
		ag->priv = NULL;
		ag->hash = rhash;
		ag->state = FOE_STATE_BIND;
	}

	rcu_assign_pointer(eth->foe_flow_table[ohash], flow);
	rcu_assign_pointer(eth->foe_flow_table[rhash], flow);

	/* XXX: also the same was set in natflow
	rhash |= ohash << 16;
	flow->timeout = (void *)(unsigned long)rhash;
	*/

	return 0;
}

void ra_flow_offload_stop(void)
{
	int i;
	struct mtk_eth *eth = (struct mtk_eth *)ag_timer_eth;

	if (eth) {
		for (i = 0; i < MTK_PPE_ENTRY_CNT; i++) {
			rcu_assign_pointer(eth->foe_flow_table[i], NULL);
		}
	}
	mtk_ppe_account_group_walk_stop();
}

#ifdef CONFIG_NET_RALINK_HW_QOS

#define QDMA_TX_SCH_TX	  0x1a14

static void mtk_ppe_scheduler(struct mtk_eth *eth, int id, u32 rate)
{
	int exp = 0, shift = 0;
	u32 reg = mtk_r32(eth, QDMA_TX_SCH_TX);
	u32 val = 0;

	if (rate)
		val = BIT(11);

	while (rate > 127) {
		rate /= 10;
		exp++;
	}

	val |= (rate & 0x7f) << 4;
	val |= exp & 0xf;
	if (id)
		shift = 16;
	reg &= ~(0xffff << shift);
	reg |= val << shift;
	mtk_w32(eth, val, QDMA_TX_SCH_TX);
}

#define QTX_CFG(x)	(0x1800 + (x * 0x10))
#define QTX_SCH(x)	(0x1804 + (x * 0x10))

static void mtk_ppe_queue(struct mtk_eth *eth, int id, int sched, int weight, int resv, u32 min_rate, u32 max_rate)
{
	int max_exp = 0, min_exp = 0;
	u32 reg;

	if (id >= 16)
		return;

	reg = mtk_r32(eth, QTX_SCH(id));
	reg &= 0x70000000;

	if (sched)
		reg |= BIT(31);

	if (min_rate)
		reg |= BIT(27);

	if (max_rate)
		reg |= BIT(11);

	while (max_rate > 127) {
		max_rate /= 10;
		max_exp++;
	}

	while (min_rate > 127) {
		min_rate /= 10;
		min_exp++;
	}

	reg |= (min_rate & 0x7f) << 20;
	reg |= (min_exp & 0xf) << 16;
	reg |= (weight & 0xf) << 12;
	reg |= (max_rate & 0x7f) << 4;
	reg |= max_exp & 0xf;
	mtk_w32(eth, reg, QTX_SCH(id));

	resv &= 0xff;
	reg = mtk_r32(eth, QTX_CFG(id));
	reg &= 0xffff0000;
	reg |= (resv << 8) | resv;
	mtk_w32(eth, reg, QTX_CFG(id));
}
#endif

static int mtk_init_foe_table(struct mtk_eth *eth)
{
	if (eth->foe_table)
		return 0;

	eth->foe_flow_table = devm_kcalloc(eth->dev, MTK_PPE_ENTRY_CNT,
					   sizeof(*eth->foe_flow_table),
					   GFP_KERNEL);
	if (!eth->foe_flow_table)
		return -EINVAL;

	/* map the FOE table */
	eth->foe_table = dmam_alloc_coherent(eth->dev, MTK_PPE_TBL_SZ,
					     &eth->foe_table_phys, GFP_KERNEL);
	if (!eth->foe_table) {
		dev_err(eth->dev, "failed to allocate foe table\n");
		kfree(eth->foe_flow_table);
		return -ENOMEM;
	}


	return 0;
}

static int mtk_ppe_start(struct mtk_eth *eth)
{
	int ret;

	ret = mtk_init_foe_table(eth);
	if (ret)
		return ret;

	/* tell the PPE about the tables base address */
	mtk_w32(eth, eth->foe_table_phys, MTK_REG_PPE_TB_BASE);

	/* flush the table */
	memset(eth->foe_table, 0, MTK_PPE_TBL_SZ);

	if (IS_ENABLED(CONFIG_SOC_MT7621)) {
		/* skip all entries that cross the 1024 byte boundary */
		static const u8 skip[] = { 12, 25, 38, 51, 76, 89, 102 };
		int i, k;
		for (i = 0; i < MTK_PPE_ENTRY_CNT; i += 128)
			for (k = 0; k < ARRAY_SIZE(skip); k++)
				((struct mtk_foe_entry *)eth->foe_table)[i + skip[k]].udib1.sta = 1;
	}

	/* setup hashing */
	mtk_m32(eth,
		MTK_PPE_TB_CFG_HASH_MODE_MASK | MTK_PPE_TB_CFG_TBL_SZ_MASK,
		MTK_PPE_TB_CFG_HASH_MODE1 | MTK_PPE_TB_CFG_TBL_SZ_8K,
		MTK_REG_PPE_TB_CFG);

	/* set the default hashing seed */
	mtk_w32(eth, MTK_PPE_HASH_SEED, MTK_REG_PPE_HASH_SEED);

	/* each foe entry is 64bytes and is setup by cpu forwarding*/
	mtk_m32(eth, MTK_PPE_CAH_CTRL_X_MODE | MTK_PPE_TB_CFG_ENTRY_SZ_MASK |
		MTK_PPE_TB_CFG_SMA_MASK,
		MTK_PPE_TB_CFG_ENTRY_SZ_64B |  MTK_PPE_TB_CFG_SMA_FWD_CPU,
		MTK_REG_PPE_TB_CFG);

	/* set ip proto */
	mtk_w32(eth, 0xFFFFFFFF, MTK_REG_PPE_IP_PROT_CHK);

	/* setup caching */
	mtk_m32(eth, 0, MTK_PPE_CAH_CTRL_X_MODE, MTK_REG_PPE_CAH_CTRL);
	mtk_m32(eth, MTK_PPE_CAH_CTRL_X_MODE, MTK_PPE_CAH_CTRL_EN,
		MTK_REG_PPE_CAH_CTRL);

	/* enable FOE */
	mtk_m32(eth, 0, MTK_PPE_FLOW_CFG_IPV4_NAT_FRAG_EN |
		MTK_PPE_FLOW_CFG_IPV4_NAPT_EN | MTK_PPE_FLOW_CFG_IPV4_NAT_EN |
		MTK_PPE_FLOW_CFG_IPV4_GREK_EN,
		MTK_REG_PPE_FLOW_CFG);

	/* setup flow entry un/bind aging */
	mtk_m32(eth, 0,
		MTK_PPE_TB_CFG_UNBD_AGE | MTK_PPE_TB_CFG_NTU_AGE |
		MTK_PPE_TB_CFG_FIN_AGE | MTK_PPE_TB_CFG_UDP_AGE |
		MTK_PPE_TB_CFG_TCP_AGE,
		MTK_REG_PPE_TB_CFG);

	mtk_m32(eth, MTK_PPE_UNB_AGE_MNP_MASK | MTK_PPE_UNB_AGE_DLTA_MASK,
		MTK_PPE_UNB_AGE_MNP | MTK_PPE_UNB_AGE_DLTA,
		MTK_REG_PPE_UNB_AGE);
	mtk_m32(eth, MTK_PPE_BND_AGE0_NTU_DLTA_MASK |
		MTK_PPE_BND_AGE0_UDP_DLTA_MASK,
		MTK_PPE_BND_AGE0_NTU_DLTA | MTK_PPE_BND_AGE0_UDP_DLTA,
		MTK_REG_PPE_BND_AGE0);
	mtk_m32(eth, MTK_PPE_BND_AGE1_FIN_DLTA_MASK |
		MTK_PPE_BND_AGE1_TCP_DLTA_MASK,
		MTK_PPE_BND_AGE1_FIN_DLTA | MTK_PPE_BND_AGE1_TCP_DLTA,
		MTK_REG_PPE_BND_AGE1);

	/* setup flow entry keep alive */
	mtk_m32(eth, MTK_PPE_TB_CFG_KA_MASK, MTK_PPE_TB_CFG_KA,
		MTK_REG_PPE_TB_CFG);
	mtk_w32(eth, MTK_PPE_KA_UDP | MTK_PPE_KA_TCP | MTK_PPE_KA_T, MTK_REG_PPE_KA);

	/* setup flow entry rate limit */
	mtk_w32(eth, (0x3fff << 16) | 0x3fff, MTK_REG_PPE_BIND_LMT_0);
	mtk_w32(eth, MTK_PPE_NTU_KA | 0x3fff, MTK_REG_PPE_BIND_LMT_1);
	mtk_m32(eth, MTK_PPE_BNDR_RATE_MASK, 1, MTK_REG_PPE_BNDR);

	/* enable the PPE */
	mtk_m32(eth, 0, MTK_PPE_GLO_CFG_EN, MTK_REG_PPE_GLO_CFG);

#ifdef CONFIG_RALINK
	/* set the default forwarding port to QDMA */
	mtk_w32(eth, 0x0, MTK_REG_PPE_DFT_CPORT);
#else
	/* set the default forwarding port to QDMA */
	mtk_w32(eth, 0x55555555, MTK_REG_PPE_DFT_CPORT);
#endif

	/* allow packets with TTL=0 */
	mtk_m32(eth, MTK_PPE_GLO_CFG_TTL0_DROP, 0, MTK_REG_PPE_GLO_CFG);

	/* send all traffic from gmac to the ppe */
	mtk_m32(eth, 0xffff, 0x4444, MTK_GDMA_FWD_CFG(0));
	mtk_m32(eth, 0xffff, 0x4444, MTK_GDMA_FWD_CFG(1));

	dev_info(eth->dev, "PPE started\n");

#ifdef CONFIG_NET_RALINK_HW_QOS
	mtk_ppe_scheduler(eth, 0, 500000);
	mtk_ppe_scheduler(eth, 1, 500000);
	mtk_ppe_queue(eth, 0, 0, 7, 32, 250000, 0);
	mtk_ppe_queue(eth, 1, 0, 7, 32, 250000, 0);
	mtk_ppe_queue(eth, 8, 1, 7, 32, 250000, 0);
	mtk_ppe_queue(eth, 9, 1, 7, 32, 250000, 0);
#endif

	memset(mtk_ppe_account_group_entry, 0, sizeof(*mtk_ppe_account_group_entry) * 64);

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)
	init_timer(&ag_timer);
	ag_timer.data = 0;
	ag_timer.function = mtk_ppe_account_group_walk;
#else
	timer_setup(&ag_timer, mtk_ppe_account_group_walk, 0);
#endif
	ag_timer_eth = eth;
	mod_timer(&ag_timer, jiffies + 8 * HZ);

	return 0;
}

static int mtk_ppe_busy_wait(struct mtk_eth *eth)
{
	unsigned long t_start = jiffies;
	u32 r = 0;

	while (1) {
		r = mtk_r32(eth, MTK_REG_PPE_GLO_CFG);
		if (!(r & MTK_PPE_GLO_CFG_BUSY))
			return 0;
		if (time_after(jiffies, t_start + HZ))
			break;
		cond_resched();
	}

	dev_err(eth->dev, "ppe: table busy timeout - resetting\n");
	reset_control_reset(eth->rst_ppe);

	return -ETIMEDOUT;
}

static int mtk_ppe_stop(struct mtk_eth *eth)
{
	u32 r1 = 0, r2 = 0;
	int i;

	del_timer(&ag_timer);

	/* discard all traffic while we disable the PPE */
	mtk_m32(eth, 0xffff, 0x7777, MTK_GDMA_FWD_CFG(0));
	mtk_m32(eth, 0xffff, 0x7777, MTK_GDMA_FWD_CFG(1));

	if (mtk_ppe_busy_wait(eth))
		return -ETIMEDOUT;

	/* invalidate all flow table entries */
	for (i = 0; i < MTK_PPE_ENTRY_CNT; i++)
		eth->foe_table[i].bfib1.state = FOE_STATE_INVALID;

	/* disable caching */
	mtk_m32(eth, 0, MTK_PPE_CAH_CTRL_X_MODE, MTK_REG_PPE_CAH_CTRL);
	mtk_m32(eth, MTK_PPE_CAH_CTRL_X_MODE | MTK_PPE_CAH_CTRL_EN, 0,
		MTK_REG_PPE_CAH_CTRL);

	/* flush cache has to be ahead of hnat diable --*/
	mtk_m32(eth, MTK_PPE_GLO_CFG_EN, 0, MTK_REG_PPE_GLO_CFG);

	/* disable FOE */
	mtk_m32(eth,
		MTK_PPE_FLOW_CFG_IPV4_NAT_FRAG_EN |
		MTK_PPE_FLOW_CFG_IPV4_NAPT_EN | MTK_PPE_FLOW_CFG_IPV4_NAT_EN |
		MTK_PPE_FLOW_CFG_FUC_FOE | MTK_PPE_FLOW_CFG_FMC_FOE,
		0, MTK_REG_PPE_FLOW_CFG);

	/* disable FOE aging */
	mtk_m32(eth, 0,
		MTK_PPE_TB_CFG_FIN_AGE | MTK_PPE_TB_CFG_UDP_AGE |
		MTK_PPE_TB_CFG_TCP_AGE | MTK_PPE_TB_CFG_UNBD_AGE |
		MTK_PPE_TB_CFG_NTU_AGE, MTK_REG_PPE_TB_CFG);

	r1 = mtk_r32(eth, 0x100);
	r2 = mtk_r32(eth, 0x10c);

	dev_info(eth->dev, "0x100 = 0x%x, 0x10c = 0x%x\n", r1, r2);

	if (((r1 & 0xff00) >> 0x8) >= (r1 & 0xff) ||
	    ((r1 & 0xff00) >> 0x8) >= (r2 & 0xff)) {
		dev_info(eth->dev, "reset pse\n");
		mtk_w32(eth, 0x1, 0x4);
	}

	/* set the foe entry base address to 0 */
	mtk_w32(eth, 0, MTK_REG_PPE_TB_BASE);

	if (mtk_ppe_busy_wait(eth))
		return -ETIMEDOUT;

	/* send all traffic back to the DMA engine */
#ifdef CONFIG_RALINK
	mtk_m32(eth, 0xffff, 0x0, MTK_GDMA_FWD_CFG(0));
	mtk_m32(eth, 0xffff, 0x0, MTK_GDMA_FWD_CFG(1));
#else
	mtk_m32(eth, 0xffff, 0x5555, MTK_GDMA_FWD_CFG(0));
	mtk_m32(eth, 0xffff, 0x5555, MTK_GDMA_FWD_CFG(1));
#endif
	return 0;
}

static void mtk_offload_keepalive(struct fe_priv *eth, unsigned int hash)
{
	flow_offload_t *flow;

	rcu_read_lock();
	flow = rcu_dereference(eth->foe_flow_table[hash]);
	if (flow) {
		void (*func)(unsigned int, unsigned long, unsigned long, unsigned int *, unsigned int *);
		func = (void *)flow->priv;
		if (func) {
			struct mtk_foe_entry *entry = &eth->foe_table[hash];
			u32 ag_idx = entry->ipv4_hnapt.iblk2.port_ag;
			struct mtk_ppe_account_group *ag = mtk_ppe_account_group_get(ag_idx);
			if (ag && ag->state == FOE_STATE_BIND && ag->hash == hash) {
				if (ag->priv != func) {
					unsigned long bytes = ag->bytes;
					unsigned long packets = ag->packets;
					func(hash, bytes, packets, ag->speed_bytes, ag->speed_packets);
					ag->bytes -= bytes;
					ag->packets -= packets;
					ag->priv = func;
					rcu_read_unlock();
					return;
				}
			}
			func(hash, 0, 0, NULL, NULL);
		}
	}
	rcu_read_unlock();
}

int ra_offload_check_rx(struct fe_priv *eth, struct sk_buff *skb, u32 rxd4)
{
	unsigned int hash;

	switch (FIELD_GET(MTK_RXD4_CPU_REASON, rxd4)) {
	case MTK_CPU_REASON_KEEPALIVE_UC_OLD_HDR:
	case MTK_CPU_REASON_KEEPALIVE_MC_NEW_HDR:
	case MTK_CPU_REASON_KEEPALIVE_DUP_OLD_HDR:
		hash = FIELD_GET(MTK_RXD4_FOE_ENTRY, rxd4);
		mtk_offload_keepalive(eth, hash);
		return -1;
	case MTK_CPU_REASON_PACKET_SAMPLING:
		return -1;
	case MTK_CPU_REASON_HIT_BIND_FORCE_CPU:
		hash = FIELD_GET(MTK_RXD4_FOE_ENTRY, rxd4);
		skb_set_hash(skb, (HWNAT_QUEUE_MAPPING_MAGIC | hash), PKT_HASH_TYPE_L4);
		skb->vlan_tci |= HWNAT_QUEUE_MAPPING_MAGIC;
		skb->pkt_type = PACKET_HOST;
		/* fallthrough */
	default:
		return 0;
	}
}

int ra_ppe_probe(struct mtk_eth *eth)
{
	int err;

	err = mtk_ppe_start(eth);
	if (err)
		return err;

	err = ra_ppe_debugfs_init(eth);
	if (err)
		return err;

	return 0;
}

void ra_ppe_remove(struct mtk_eth *eth)
{
	mtk_ppe_stop(eth);
}
