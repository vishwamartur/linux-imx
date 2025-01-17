/* Copyright 2017 NXP
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *	 notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *	 notice, this list of conditions and the following disclaimer in the
 *	 documentation and/or other materials provided with the distribution.
 *     * Neither the name of Freescale Semiconductor nor the
 *	 names of its contributors may be used to endorse or promote products
 *	 derived from this software without specific prior written permission.
 *
 *
 * ALTERNATIVELY, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") as published by the Free Software
 * Foundation, either version 2 of that License or (at your option) any
 * later version.
 *
 * THIS SOFTWARE IS PROVIDED BY Freescale Semiconductor ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL Freescale Semiconductor BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/init.h>
#include <linux/module.h>

#include "dpaa2-eth-ceetm.h"
#include "dpaa2-eth.h"

#define DPAA2_CEETM_DESCRIPTION "FSL DPAA2 CEETM qdisc"
/* Conversion formula from userspace passed Bps to expected Mbit */
#define dpaa2_eth_bps_to_mbit(rate) (rate >> 17)

static const struct nla_policy dpaa2_ceetm_policy[DPAA2_CEETM_TCA_MAX] = {
	[DPAA2_CEETM_TCA_COPT] = { .len = sizeof(struct dpaa2_ceetm_tc_copt) },
	[DPAA2_CEETM_TCA_QOPS] = { .len = sizeof(struct dpaa2_ceetm_tc_qopt) },
};

struct Qdisc_ops dpaa2_ceetm_qdisc_ops;

static int dpaa2_ceetm_cls_delete(struct Qdisc *sch, unsigned long arg,
				  struct netlink_ext_ack *extack);

static inline int dpaa2_eth_set_ch_shaping(struct dpaa2_eth_priv *priv,
					   struct dpni_tx_shaping_cfg *scfg,
					   struct dpni_tx_shaping_cfg *ecfg,
					   int coupled, int ch_id)
{
	int err = 0;

	netdev_dbg(priv->net_dev, "%s: ch_id %d rate %d mbps\n", __func__,
		   ch_id, scfg->rate_limit);
	err = dpni_set_tx_shaping(priv->mc_io, 0, priv->mc_token, scfg,
				  ecfg, coupled);
	if (err)
		netdev_err(priv->net_dev, "dpni_set_tx_shaping err\n");

	return err;
}

static inline int dpaa2_eth_reset_ch_shaping(struct dpaa2_eth_priv *priv,
					     int ch_id)
{
	struct dpni_tx_shaping_cfg cfg = { 0 };

	return dpaa2_eth_set_ch_shaping(priv, &cfg, &cfg, 0, ch_id);
}

static inline int
dpaa2_eth_update_shaping_cfg(struct net_device *dev,
			     struct dpaa2_ceetm_shaping_cfg cfg,
			     struct dpni_tx_shaping_cfg *scfg,
			     struct dpni_tx_shaping_cfg *ecfg)
{
	scfg->rate_limit = dpaa2_eth_bps_to_mbit(cfg.cir);
	ecfg->rate_limit = dpaa2_eth_bps_to_mbit(cfg.eir);

	if (cfg.cbs > DPAA2_ETH_MAX_BURST_SIZE) {
		netdev_err(dev, "Committed burst size must be under %d\n",
			   DPAA2_ETH_MAX_BURST_SIZE);
		return -EINVAL;
	}

	scfg->max_burst_size = cfg.cbs;

	if (cfg.ebs > DPAA2_ETH_MAX_BURST_SIZE) {
		netdev_err(dev, "Excess burst size must be under %d\n",
			   DPAA2_ETH_MAX_BURST_SIZE);
		return -EINVAL;
	}

	ecfg->max_burst_size = cfg.ebs;

	if ((!cfg.cir || !cfg.eir) && cfg.coupled) {
		netdev_err(dev, "Coupling can be set when both CIR and EIR are finite\n");
		return -EINVAL;
	}

	return 0;
}

enum update_tx_prio {
	DPAA2_ETH_ADD_CQ,
	DPAA2_ETH_DEL_CQ,
};

/* Normalize weights based on max passed value */
static inline int dpaa2_eth_normalize_tx_prio(struct dpaa2_ceetm_qdisc *priv)
{
	struct dpni_tx_schedule_cfg *sched_cfg;
	struct dpaa2_ceetm_class *cl;
	u32 qpri;
	u16 weight_max = 0, increment;
	int i;

	/* Check the boundaries of the provided values */
	for (i = 0; i < priv->clhash.hashsize; i++)
		hlist_for_each_entry(cl, &priv->clhash.hash[i], common.hnode)
			weight_max = (weight_max == 0 ? cl->prio.weight :
				     (weight_max < cl->prio.weight ?
				      cl->prio.weight : weight_max));

	/* If there are no elements, there's nothing to do */
	if (weight_max == 0)
		return 0;

	increment = (DPAA2_CEETM_MAX_WEIGHT - DPAA2_CEETM_MIN_WEIGHT) /
		    weight_max;

	for (i = 0; i < priv->clhash.hashsize; i++) {
		hlist_for_each_entry(cl, &priv->clhash.hash[i], common.hnode) {
			if (cl->prio.mode == STRICT_PRIORITY)
				continue;

			qpri = cl->prio.qpri;
			sched_cfg = &priv->prio.tx_prio_cfg.tc_sched[qpri >= 8 ? qpri % 8 : qpri];

			sched_cfg->delta_bandwidth =
				DPAA2_CEETM_MIN_WEIGHT +
				(cl->prio.weight * increment);

			pr_debug("%s: Normalized CQ qpri %d weight to %d\n",
				 __func__, qpri, sched_cfg->delta_bandwidth);
		}
	}

	return 0;
}

static inline int dpaa2_eth_update_tx_prio(struct dpaa2_eth_priv *priv,
					   struct dpaa2_ceetm_class *cl,
					   enum update_tx_prio type)
{
	struct dpaa2_ceetm_qdisc *sch = qdisc_priv(cl->parent);
	struct dpni_tx_schedule_cfg *sched_cfg;
	struct dpni_taildrop td = {0};
	u8 ch_id = 0, tc_id = 0;
	u32 qpri = 0;
	int err = 0;

	qpri = cl->prio.qpri;
	tc_id = DPNI_BUILD_CH_TC(ch_id, qpri);

	switch (type) {
	case DPAA2_ETH_ADD_CQ:
		/* Enable taildrop */
		td.enable = 1;
		td.units = DPNI_CONGESTION_UNIT_FRAMES;
		td.threshold = DPAA2_CEETM_TD_THRESHOLD;
		err = dpni_set_taildrop(priv->mc_io, 0, priv->mc_token,
					DPNI_CP_GROUP, DPNI_QUEUE_TX, tc_id,
					0, &td);
		if (err) {
			netdev_err(priv->net_dev, "Error enabling Tx taildrop %d\n",
				   err);
			return err;
		}
		break;
	case DPAA2_ETH_DEL_CQ:
		/* Disable taildrop */
		td.enable = 0;
		err = dpni_set_taildrop(priv->mc_io, 0, priv->mc_token,
					DPNI_CP_GROUP, DPNI_QUEUE_TX, tc_id,
					0, &td);
		if (err) {
			netdev_err(priv->net_dev, "Error disabling Tx taildrop %d\n",
				   err);
			return err;
		}
		break;
	}

	/* When num_tx_tcs > 8, the first 8 TCs can only be used as strict
	 * priority which is configured by default. No need to call
	 * dpni_set_tx_priorities() for the first 8 TCs.
	 */
	if (dpaa2_eth_tx_tc_count(priv) > 8 && qpri < 8)
		return 0;

	/* Revert the scheduling configuration for a particular TCs to its
	 * default state. For the case in which the qpri < 8 this means a
	 * memset of the entire dpni_tx_schedule_cfg structure (STRICT
	 * priority, delta_bandwidth = 0), while for the qpri >=8 it means only
	 * a reset of the delta_bandwidth since the scheduling mode cannot be
	 * changed
	 */
	if (type == DPAA2_ETH_DEL_CQ) {
		if (qpri < 8) {
			sched_cfg = &sch->prio.tx_prio_cfg.tc_sched[qpri];
			memset(sched_cfg, 0, sizeof(*sched_cfg));
		} else {
			sched_cfg = &sch->prio.tx_prio_cfg.tc_sched[qpri % 8];
			sched_cfg->delta_bandwidth = 0;
		}
	}

	/* Normalize priorities */
	err = dpaa2_eth_normalize_tx_prio(sch);

	/* Debug print goes here */
	print_hex_dump_debug("tx_prio: ", DUMP_PREFIX_OFFSET, 16, 1,
			     &sch->prio.tx_prio_cfg,
			     sizeof(sch->prio.tx_prio_cfg), 0);

	/* Call dpni_set_tx_priorities for the entire prio qdisc */
	err = dpni_set_tx_priorities(priv->mc_io, 0, priv->mc_token,
				     &sch->prio.tx_prio_cfg);
	if (err)
		netdev_err(priv->net_dev, "dpni_set_tx_priorities err %d\n",
			   err);

	return err;
}

static void dpaa2_eth_ceetm_enable(struct dpaa2_eth_priv *priv)
{
	priv->ceetm_en = true;
}

static void dpaa2_eth_ceetm_disable(struct dpaa2_eth_priv *priv)
{
	priv->ceetm_en = false;
}

/* Find class in qdisc hash table using given handle */
static inline struct dpaa2_ceetm_class *dpaa2_ceetm_find(u32 handle,
							 struct Qdisc *sch)
{
	struct dpaa2_ceetm_qdisc *priv = qdisc_priv(sch);
	struct Qdisc_class_common *clc;

	pr_debug(KBUILD_BASENAME " : %s : find class %X in qdisc %X\n",
		 __func__, handle, sch->handle);

	clc = qdisc_class_find(&priv->clhash, handle);
	return clc ? container_of(clc, struct dpaa2_ceetm_class, common) : NULL;
}

/* Insert a class in the qdisc's class hash */
static void dpaa2_ceetm_link_class(struct Qdisc *sch,
				   struct Qdisc_class_hash *clhash,
				   struct Qdisc_class_common *common)
{
	sch_tree_lock(sch);
	qdisc_class_hash_insert(clhash, common);
	sch_tree_unlock(sch);
	qdisc_class_hash_grow(sch, clhash);
}

/* Destroy a ceetm class */
static void dpaa2_ceetm_cls_destroy(struct Qdisc *sch,
				    struct dpaa2_ceetm_class *cl)
{
	struct net_device *dev = qdisc_dev(sch);
	struct dpaa2_eth_priv *priv = netdev_priv(dev);

	if (!cl)
		return;

	pr_debug(KBUILD_BASENAME " : %s : destroy class %X from under %X\n",
		 __func__, cl->common.classid, sch->handle);

	/* Recurse into child first */
	if (cl->child) {
		qdisc_put(cl->child);
		cl->child = NULL;
	}

	switch (cl->type) {
	case CEETM_ROOT:
		if (dpaa2_eth_reset_ch_shaping(priv, cl->root.ch_id))
			netdev_err(dev, "Error resetting channel shaping\n");

		break;

	case CEETM_PRIO:
		if (dpaa2_eth_update_tx_prio(priv, cl, DPAA2_ETH_DEL_CQ))
			netdev_err(dev, "Error resetting tx_priorities\n");

		if (cl->prio.cstats)
			free_percpu(cl->prio.cstats);

		break;
	}

	tcf_block_put(cl->block);
	kfree(cl);
}

/* Destroy a ceetm qdisc */
static void dpaa2_ceetm_destroy(struct Qdisc *sch)
{
	unsigned int i;
	struct hlist_node *next;
	struct dpaa2_ceetm_class *cl;
	struct dpaa2_ceetm_qdisc *priv = qdisc_priv(sch);
	struct net_device *dev = qdisc_dev(sch);
	struct dpaa2_eth_priv *priv_eth = netdev_priv(dev);

	pr_debug(KBUILD_BASENAME " : %s : destroy qdisc %X\n",
		 __func__, sch->handle);

	/* All filters need to be removed before destroying the classes */
	tcf_block_put(priv->block);

	for (i = 0; i < priv->clhash.hashsize; i++) {
		hlist_for_each_entry(cl, &priv->clhash.hash[i], common.hnode)
			tcf_block_put(cl->block);
	}

	for (i = 0; i < priv->clhash.hashsize; i++) {
		hlist_for_each_entry_safe(cl, next, &priv->clhash.hash[i],
					  common.hnode)
			dpaa2_ceetm_cls_destroy(sch, cl);
	}

	qdisc_class_hash_destroy(&priv->clhash);

	switch (priv->type) {
	case CEETM_ROOT:
		dpaa2_eth_ceetm_disable(priv_eth);

		if (priv->root.qstats)
			free_percpu(priv->root.qstats);

		if (!priv->root.qdiscs)
			break;

		/* Destroy the pfifo qdiscs in case they haven't been attached
		 * to the netdev queues yet.
		 */
		for (i = 0; i < dev->num_tx_queues; i++)
			if (priv->root.qdiscs[i])
				qdisc_put(priv->root.qdiscs[i]);

		kfree(priv->root.qdiscs);
		break;

	case CEETM_PRIO:
		if (priv->prio.parent)
			priv->prio.parent->child = NULL;
		break;
	}
}

static int dpaa2_ceetm_dump(struct Qdisc *sch, struct sk_buff *skb)
{
	struct Qdisc *qdisc;
	unsigned int ntx, i;
	struct nlattr *nest;
	struct dpaa2_ceetm_tc_qopt qopt;
	struct dpaa2_ceetm_qdisc_stats *qstats;
	struct net_device *dev = qdisc_dev(sch);
	struct dpaa2_ceetm_qdisc *priv = qdisc_priv(sch);

	pr_debug(KBUILD_BASENAME " : %s : qdisc %X\n", __func__, sch->handle);

	sch_tree_lock(sch);
	memset(&qopt, 0, sizeof(qopt));
	qopt.type = priv->type;
	qopt.shaped = priv->shaped;

	switch (priv->type) {
	case CEETM_ROOT:
		/* Gather statistics from the underlying pfifo qdiscs */
		sch->q.qlen = 0;
		memset(&sch->bstats, 0, sizeof(sch->bstats));
		memset(&sch->qstats, 0, sizeof(sch->qstats));

		for (ntx = 0; ntx < dev->num_tx_queues; ntx++) {
			qdisc = netdev_get_tx_queue(dev, ntx)->qdisc_sleeping;

			_bstats_update(&sch->bstats,
				       u64_stats_read(&qdisc->bstats.bytes),
				       u64_stats_read(&qdisc->bstats.packets));

			sch->q.qlen		+= qdisc->q.qlen;
			sch->qstats.qlen	+= qdisc->qstats.qlen;
			sch->qstats.backlog	+= qdisc->qstats.backlog;
			sch->qstats.drops	+= qdisc->qstats.drops;
			sch->qstats.requeues	+= qdisc->qstats.requeues;
			sch->qstats.overlimits	+= qdisc->qstats.overlimits;
		}

		for_each_online_cpu(i) {
			qstats = per_cpu_ptr(priv->root.qstats, i);
			sch->qstats.drops += qstats->drops;
		}

		break;

	case CEETM_PRIO:
		qopt.prio_group_A = priv->prio.tx_prio_cfg.prio_group_A;
		qopt.prio_group_B = priv->prio.tx_prio_cfg.prio_group_B;
		qopt.separate_groups = priv->prio.tx_prio_cfg.separate_groups;
		break;

	default:
		pr_err(KBUILD_BASENAME " : %s : invalid qdisc\n", __func__);
		sch_tree_unlock(sch);
		return -EINVAL;
	}

	nest = nla_nest_start_noflag(skb, TCA_OPTIONS);
	if (!nest)
		goto nla_put_failure;
	if (nla_put(skb, DPAA2_CEETM_TCA_QOPS, sizeof(qopt), &qopt))
		goto nla_put_failure;
	nla_nest_end(skb, nest);

	sch_tree_unlock(sch);
	return skb->len;

nla_put_failure:
	sch_tree_unlock(sch);
	nla_nest_cancel(skb, nest);
	return -EMSGSIZE;
}

static int dpaa2_ceetm_change_prio(struct Qdisc *sch,
				   struct dpaa2_ceetm_qdisc *priv,
				   struct dpaa2_ceetm_tc_qopt *qopt)
{
	/* TODO: Once LX2 support is added */
	/* priv->shaped = parent_cl->shaped; */
	priv->prio.tx_prio_cfg.prio_group_A = qopt->prio_group_A;
	priv->prio.tx_prio_cfg.prio_group_B = qopt->prio_group_B;
	priv->prio.tx_prio_cfg.separate_groups = qopt->separate_groups;

	return 0;
}

static void dpaa2_ceetm_setup_default_prio(struct Qdisc *sch,
					   struct dpaa2_ceetm_qdisc *priv,
					   struct dpaa2_ceetm_tc_qopt *qopt)
{
	struct net_device *dev = qdisc_dev(sch);
	struct dpaa2_eth_priv *priv_eth = netdev_priv(dev);
	int i;

	if (dpaa2_eth_tx_tc_count(priv_eth) <= 8)
		return;

	for (i = 0; i < DPNI_MAX_TC; i++) {
		if (qopt->separate_groups)
			if (i < 4)
				priv->prio.tx_prio_cfg.tc_sched[i].mode = DPNI_TX_SCHED_WEIGHTED_A;
			else
				priv->prio.tx_prio_cfg.tc_sched[i].mode = DPNI_TX_SCHED_WEIGHTED_B;
		else
			priv->prio.tx_prio_cfg.tc_sched[i].mode = DPNI_TX_SCHED_WEIGHTED_A;
	}
}

/* Edit a ceetm qdisc */
static int dpaa2_ceetm_change(struct Qdisc *sch, struct nlattr *opt,
			      struct netlink_ext_ack *extack)
{
	struct dpaa2_ceetm_qdisc *priv = qdisc_priv(sch);
	struct nlattr *tb[DPAA2_CEETM_TCA_QOPS + 1];
	struct dpaa2_ceetm_tc_qopt *qopt;
	int err;

	pr_debug(KBUILD_BASENAME " : %s : qdisc %X\n", __func__, sch->handle);

	err = nla_parse_nested_deprecated(tb, DPAA2_CEETM_TCA_QOPS, opt,
					  dpaa2_ceetm_policy, extack);
	if (err < 0) {
		pr_err(KBUILD_BASENAME " : %s : tc error in %s\n", __func__,
		       "nla_parse_nested_deprecated");
		return err;
	}

	if (!tb[DPAA2_CEETM_TCA_QOPS]) {
		pr_err(KBUILD_BASENAME " : %s : tc error in %s\n", __func__,
		       "tb");
		return -EINVAL;
	}

	if (TC_H_MIN(sch->handle)) {
		pr_err("CEETM: a qdisc should not have a minor\n");
		return -EINVAL;
	}

	qopt = nla_data(tb[DPAA2_CEETM_TCA_QOPS]);

	if (priv->type != qopt->type) {
		pr_err("CEETM: qdisc %X is not of the provided type\n",
		       sch->handle);
		return -EINVAL;
	}

	switch (priv->type) {
	case CEETM_PRIO:
		err = dpaa2_ceetm_change_prio(sch, priv, qopt);
		break;
	default:
		pr_err(KBUILD_BASENAME " : %s : invalid qdisc\n", __func__);
		err = -EINVAL;
	}

	return err;
}

/* Configure a root ceetm qdisc */
static int dpaa2_ceetm_init_root(struct Qdisc *sch,
				 struct dpaa2_ceetm_qdisc *priv,
				 struct dpaa2_ceetm_tc_qopt *qopt,
				 struct netlink_ext_ack *extack)
{
	struct net_device *dev = qdisc_dev(sch);
	struct dpaa2_eth_priv *priv_eth = netdev_priv(dev);
	struct netdev_queue *dev_queue;
	unsigned int i, parent_id;
	struct Qdisc *qdisc;

	pr_debug(KBUILD_BASENAME " : %s : qdisc %X\n", __func__, sch->handle);

	/* Validate inputs */
	if (sch->parent != TC_H_ROOT) {
		pr_err("CEETM: a root ceetm qdisc must be root\n");
		return -EINVAL;
	}

	/* Pre-allocate underlying pfifo qdiscs.
	 *
	 * We want to offload shaping and scheduling decisions to the hardware.
	 * The pfifo qdiscs will be attached to the netdev queues and will
	 * guide the traffic from the IP stack down to the driver with minimum
	 * interference.
	 *
	 * The CEETM qdiscs and classes will be crossed when the traffic
	 * reaches the driver.
	 */
	priv->root.qdiscs = kcalloc(dev->num_tx_queues,
				    sizeof(priv->root.qdiscs[0]),
				    GFP_KERNEL);
	if (!priv->root.qdiscs)
		return -ENOMEM;

	for (i = 0; i < dev->num_tx_queues; i++) {
		dev_queue = netdev_get_tx_queue(dev, i);
		parent_id = TC_H_MAKE(TC_H_MAJ(sch->handle),
				      TC_H_MIN(i + PFIFO_MIN_OFFSET));

		qdisc = qdisc_create_dflt(dev_queue, &pfifo_qdisc_ops,
					  parent_id, extack);
		if (!qdisc)
			return -ENOMEM;

		priv->root.qdiscs[i] = qdisc;
		qdisc->flags |= TCQ_F_ONETXQUEUE;
	}

	sch->flags |= TCQ_F_MQROOT;

	priv->root.qstats = alloc_percpu(struct dpaa2_ceetm_qdisc_stats);
	if (!priv->root.qstats) {
		pr_err(KBUILD_BASENAME " : %s : alloc_percpu() failed\n",
		       __func__);
		return -ENOMEM;
	}

	dpaa2_eth_ceetm_enable(priv_eth);
	return 0;
}

/* Configure a prio ceetm qdisc */
static int dpaa2_ceetm_init_prio(struct Qdisc *sch,
				 struct dpaa2_ceetm_qdisc *priv,
				 struct dpaa2_ceetm_tc_qopt *qopt)
{
	struct net_device *dev = qdisc_dev(sch);
	struct dpaa2_ceetm_class *parent_cl;
	struct Qdisc *parent_qdisc;

	pr_debug(KBUILD_BASENAME " : %s : qdisc %X\n", __func__, sch->handle);

	if (sch->parent == TC_H_ROOT) {
		pr_err("CEETM: a prio ceetm qdisc can not be root\n");
		return -EINVAL;
	}

	parent_qdisc = qdisc_lookup(dev, TC_H_MAJ(sch->parent));
	if (strcmp(parent_qdisc->ops->id, dpaa2_ceetm_qdisc_ops.id)) {
		pr_err("CEETM: a ceetm qdisc can not be attached to other qdisc/class types\n");
		return -EINVAL;
	}

	/* Obtain the parent root ceetm_class */
	parent_cl = dpaa2_ceetm_find(sch->parent, parent_qdisc);

	if (!parent_cl || parent_cl->type != CEETM_ROOT) {
		pr_err("CEETM: a prio ceetm qdiscs can be added only under a root ceetm class\n");
		return -EINVAL;
	}

	priv->prio.parent = parent_cl;
	parent_cl->child = sch;

	dpaa2_ceetm_setup_default_prio(sch, priv, qopt);

	return dpaa2_ceetm_change_prio(sch, priv, qopt);
}

/* Configure a generic ceetm qdisc */
static int dpaa2_ceetm_init(struct Qdisc *sch, struct nlattr *opt,
			    struct netlink_ext_ack *extack)
{
	struct dpaa2_ceetm_qdisc *priv = qdisc_priv(sch);
	struct net_device *dev = qdisc_dev(sch);
	struct nlattr *tb[DPAA2_CEETM_TCA_QOPS + 1];
	struct dpaa2_ceetm_tc_qopt *qopt;
	int err;

	pr_debug(KBUILD_BASENAME " : %s : qdisc %X\n", __func__, sch->handle);

	if (!netif_is_multiqueue(dev))
		return -EOPNOTSUPP;

	err = tcf_block_get(&priv->block, &priv->filter_list, sch, extack);
	if (err) {
		pr_err("CEETM: unable to get tcf_block\n");
		return err;
	}

	if (!opt) {
		pr_err(KBUILD_BASENAME " : %s : tc error - opt = NULL\n",
		       __func__);
		return -EINVAL;
	}

	err = nla_parse_nested_deprecated(tb, DPAA2_CEETM_TCA_QOPS, opt,
					  dpaa2_ceetm_policy, extack);
	if (err < 0) {
		pr_err(KBUILD_BASENAME " : %s : tc error in %s\n", __func__,
		       "nla_parse_nested_deprecated");
		return err;
	}

	if (!tb[DPAA2_CEETM_TCA_QOPS]) {
		pr_err(KBUILD_BASENAME " : %s : tc error in %s\n", __func__,
		       "tb");
		return -EINVAL;
	}

	if (TC_H_MIN(sch->handle)) {
		pr_err("CEETM: a qdisc should not have a minor\n");
		return -EINVAL;
	}

	qopt = nla_data(tb[DPAA2_CEETM_TCA_QOPS]);

	/* Initialize the class hash list. Each qdisc has its own class hash */
	err = qdisc_class_hash_init(&priv->clhash);
	if (err < 0) {
		pr_err(KBUILD_BASENAME " : %s : qdisc_class_hash_init failed\n",
		       __func__);
		return err;
	}

	priv->type = qopt->type;
	priv->shaped = qopt->shaped;

	switch (priv->type) {
	case CEETM_ROOT:
		err = dpaa2_ceetm_init_root(sch, priv, qopt, extack);
		break;
	case CEETM_PRIO:
		err = dpaa2_ceetm_init_prio(sch, priv, qopt);
		break;
	default:
		pr_err(KBUILD_BASENAME " : %s : invalid qdisc\n", __func__);
		/* Note: dpaa2_ceetm_destroy() will be called by our caller */
		err = -EINVAL;
	}

	return err;
}

/* Attach the underlying pfifo qdiscs */
static void dpaa2_ceetm_attach(struct Qdisc *sch)
{
	struct net_device *dev = qdisc_dev(sch);
	struct dpaa2_ceetm_qdisc *priv = qdisc_priv(sch);
	struct Qdisc *qdisc, *old_qdisc;
	unsigned int i;

	pr_debug(KBUILD_BASENAME " : %s : qdisc %X\n", __func__, sch->handle);

	for (i = 0; i < dev->num_tx_queues; i++) {
		qdisc = priv->root.qdiscs[i];
		old_qdisc = dev_graft_qdisc(qdisc->dev_queue, qdisc);
		if (old_qdisc)
			qdisc_put(old_qdisc);
	}

	/* Remove the references to the pfifo qdiscs since the kernel will
	 * destroy them when needed. No cleanup from our part is required from
	 * this point on.
	 */
	kfree(priv->root.qdiscs);
	priv->root.qdiscs = NULL;
}

static unsigned long dpaa2_ceetm_cls_find(struct Qdisc *sch, u32 classid)
{
	struct dpaa2_ceetm_class *cl;

	pr_debug(KBUILD_BASENAME " : %s : classid %X from qdisc %X\n",
		 __func__, classid, sch->handle);
	cl = dpaa2_ceetm_find(classid, sch);

	return (unsigned long)cl;
}

static int dpaa2_ceetm_cls_change_root(struct dpaa2_ceetm_class *cl,
				       struct dpaa2_ceetm_tc_copt *copt,
				       struct net_device *dev)
{
	struct dpaa2_eth_priv *priv = netdev_priv(dev);
	struct dpni_tx_shaping_cfg scfg = { 0 }, ecfg = { 0 };
	int err = 0;

	pr_debug(KBUILD_BASENAME " : %s : class %X\n", __func__,
		 cl->common.classid);

	if (!cl->shaped)
		return 0;

	if (dpaa2_eth_update_shaping_cfg(dev, copt->shaping_cfg,
					 &scfg, &ecfg))
		return -EINVAL;

	err = dpaa2_eth_set_ch_shaping(priv, &scfg, &ecfg,
				       copt->shaping_cfg.coupled,
				       cl->root.ch_id);
	if (err)
		return err;

	memcpy(&cl->root.shaping_cfg, &copt->shaping_cfg,
	       sizeof(struct dpaa2_ceetm_shaping_cfg));

	return err;
}

static int dpaa2_ceetm_cls_change_prio(struct dpaa2_ceetm_class *cl,
				       struct dpaa2_ceetm_tc_copt *copt,
				       struct net_device *dev)
{
	struct dpaa2_ceetm_qdisc *sch = qdisc_priv(cl->parent);
	struct dpaa2_eth_priv *priv = netdev_priv(dev);
	struct dpni_tx_schedule_cfg *sched_cfg = NULL;
	int err;

	pr_debug(KBUILD_BASENAME " : %s : class %X mode %d weight %d\n",
		 __func__, cl->common.classid, copt->mode, copt->weight);

	/* Impose the MC API restrictions for when num_tx_tcs > 8. */
	if (dpaa2_eth_tx_tc_count(priv) > 8) {
		if (cl->prio.qpri < 8) {
			if (copt->mode != STRICT_PRIORITY) {
				pr_err(KBUILD_BASENAME " : %s : When num_tx_tcs > 8, first 8 TCs can only be in strict priority\n",
				       __func__);
				return -EINVAL;
			}
		} else if (cl->prio.qpri < 12) {
			if (copt->mode != WEIGHTED_A) {
				pr_err(KBUILD_BASENAME " : %s : When num_tx_tcs > 8, TCs [8-12) can only be WEIGHTED_A\n",
				       __func__);
				return -EINVAL;
			}
		} else if (cl->prio.qpri < 16) {
			if (copt->mode != WEIGHTED_B) {
				pr_err(KBUILD_BASENAME " : %s : When num_tx_tcs > 8, TCs [12-16) can only be WEIGHTED_B\n",
				       __func__);
				return -EINVAL;
			}
		}
	}

	if (!cl->prio.cstats) {
		cl->prio.cstats = alloc_percpu(struct dpaa2_ceetm_class_stats);
		if (!cl->prio.cstats) {
			pr_err(KBUILD_BASENAME " : %s : alloc_percpu() failed\n",
			       __func__);
			return -ENOMEM;
		}
	}

	cl->prio.mode = copt->mode;
	cl->prio.weight = copt->weight;

	/* When the DPNI has 8 or more Tx TCs, the driver should not update the
	 * per TC scheduling structures passed to the MC firmware for any qpri
	 * < 8. This is because the first 8 TCs are always in strict priority
	 * order which cannot be changed.
	 */
	if (dpaa2_eth_tx_tc_count(priv) > 8 && cl->prio.qpri >= 8)
		sched_cfg = &sch->prio.tx_prio_cfg.tc_sched[cl->prio.qpri % 8];
	else if (dpaa2_eth_tx_tc_count(priv) <= 8)
		sched_cfg = &sch->prio.tx_prio_cfg.tc_sched[cl->prio.qpri];

	if (sched_cfg) {
		switch (copt->mode) {
		case STRICT_PRIORITY:
			sched_cfg->mode = DPNI_TX_SCHED_STRICT_PRIORITY;
			break;
		case WEIGHTED_A:
			sched_cfg->mode = DPNI_TX_SCHED_WEIGHTED_A;
			break;
		case WEIGHTED_B:
			sched_cfg->mode = DPNI_TX_SCHED_WEIGHTED_B;
			break;
		}
	}

	err = dpaa2_eth_update_tx_prio(priv, cl, DPAA2_ETH_ADD_CQ);

	return err;
}

/* Add a new ceetm class */
static int dpaa2_ceetm_cls_add(struct Qdisc *sch, u32 classid,
			       struct dpaa2_ceetm_tc_copt *copt,
			       unsigned long *arg,
			       struct netlink_ext_ack *extack)
{
	struct dpaa2_ceetm_qdisc *priv = qdisc_priv(sch);
	struct net_device *dev = qdisc_dev(sch);
	struct dpaa2_eth_priv *priv_eth = netdev_priv(dev);
	struct dpaa2_ceetm_class *cl;
	bool class_linked = false;
	int err;

	if (copt->type == CEETM_ROOT &&
	    priv->clhash.hashelems == dpaa2_eth_ch_count(priv_eth)) {
		pr_err("CEETM: only %d channel%s per DPNI allowed, sorry\n",
		       dpaa2_eth_ch_count(priv_eth),
		       dpaa2_eth_ch_count(priv_eth) == 1 ? "" : "s");
		return -EINVAL;
	}

	if (copt->type == CEETM_PRIO &&
	    priv->clhash.hashelems == dpaa2_eth_tx_tc_count(priv_eth)) {
		pr_err("CEETM: only %d queue%s per channel allowed, sorry\n",
		       dpaa2_eth_tx_tc_count(priv_eth),
		       dpaa2_eth_tx_tc_count(priv_eth) == 1 ? "" : "s");
		return -EINVAL;
	}

	cl = kzalloc(sizeof(*cl), GFP_KERNEL);
	if (!cl)
		return -ENOMEM;

	err = tcf_block_get(&cl->block, &cl->filter_list, sch, extack);
	if (err) {
		pr_err("%s: Unable to set new root class\n", __func__);
		goto out_free;
	}

	cl->common.classid = classid;
	cl->parent = sch;
	cl->child = NULL;

	/* Add class handle in Qdisc */
	dpaa2_ceetm_link_class(sch, &priv->clhash, &cl->common);
	class_linked = true;

	cl->shaped = copt->shaped;
	cl->type = copt->type;

	/* Claim a CEETM channel / tc - DPAA2. will assume transition from
	 * classid to qdid/qpri, starting from qdid / qpri 0
	 */
	switch (copt->type) {
	case CEETM_ROOT:
		cl->root.ch_id = classid - sch->handle - 1;
		err = dpaa2_ceetm_cls_change_root(cl, copt, dev);
		break;
	case CEETM_PRIO:
		cl->prio.qpri = classid - sch->handle - 1;
		if (cl->prio.qpri >= dpaa2_eth_tx_tc_count(priv_eth)) {
			pr_err("%s: Cannot support classid %x:%x with qpri %d, maximum qpri is %d\n",
			       __func__, TC_H_MAJ(classid), TC_H_MIN(classid), cl->prio.qpri,
			       dpaa2_eth_tx_tc_count(priv_eth) - 1);
			err = -EINVAL;
			goto out_free;
		}
		err = dpaa2_ceetm_cls_change_prio(cl, copt, dev);
		break;
	}

	if (err) {
		pr_err("%s: Unable to set new %s class\n", __func__,
		       (copt->type == CEETM_ROOT ? "root" : "prio"));
		goto out_free;
	}

	switch (copt->type) {
	case CEETM_ROOT:
		pr_debug(KBUILD_BASENAME " : %s : configured root class %X associated with channel qdid %d\n",
			 __func__, classid, cl->root.ch_id);
		break;
	case CEETM_PRIO:
		pr_debug(KBUILD_BASENAME " : %s : configured prio class %X associated with queue qpri %d\n",
			 __func__, classid, cl->prio.qpri);
		break;
	}

	*arg = (unsigned long)cl;
	return 0;

out_free:
	if (class_linked)
		dpaa2_ceetm_cls_delete(sch, (unsigned long)cl, extack);

	kfree(cl);
	return err;
}

/* Add or configure a ceetm class */
static int dpaa2_ceetm_cls_change(struct Qdisc *sch, u32 classid, u32 parentid,
				  struct nlattr **tca, unsigned long *arg,
				  struct netlink_ext_ack *extack)
{
	struct dpaa2_ceetm_class *cl = (struct dpaa2_ceetm_class *)*arg;
	struct nlattr *opt = tca[TCA_OPTIONS];
	struct nlattr *tb[DPAA2_CEETM_TCA_MAX];
	struct dpaa2_ceetm_tc_copt *copt;
	struct net_device *dev = qdisc_dev(sch);
	int err;

	pr_debug(KBUILD_BASENAME " : %s : classid %X under qdisc %X\n",
		 __func__, classid, sch->handle);

	if (strcmp(sch->ops->id, dpaa2_ceetm_qdisc_ops.id)) {
		pr_err("CEETM: a ceetm class can not be attached to other qdisc/class types\n");
		return -EINVAL;
	}

	if (!opt) {
		pr_err(KBUILD_BASENAME " : %s : tc error NULL opt\n", __func__);
		return -EINVAL;
	}

	err = nla_parse_nested_deprecated(tb, DPAA2_CEETM_TCA_COPT, opt,
					  dpaa2_ceetm_policy, extack);
	if (err < 0) {
		pr_err(KBUILD_BASENAME " : %s : tc error in %s\n", __func__,
		       "nla_parse_nested_deprecated");
		return -EINVAL;
	}

	if (!tb[DPAA2_CEETM_TCA_COPT]) {
		pr_err(KBUILD_BASENAME " : %s : tc error in %s\n", __func__,
		       "tb");
		return -EINVAL;
	}

	copt = nla_data(tb[DPAA2_CEETM_TCA_COPT]);

	/* Configure an existing ceetm class */
	if (cl) {
		if (copt->type != cl->type) {
			pr_err("CEETM: class %X is not of the provided type\n",
			       cl->common.classid);
			return -EINVAL;
		}

		switch (copt->type) {
		case CEETM_ROOT:
			return dpaa2_ceetm_cls_change_root(cl, copt, dev);
		case CEETM_PRIO:
			return dpaa2_ceetm_cls_change_prio(cl, copt, dev);

		default:
			pr_err(KBUILD_BASENAME " : %s : invalid class\n",
			       __func__);
			return -EINVAL;
		}
	}

	return dpaa2_ceetm_cls_add(sch, classid, copt, arg, extack);
}

static void dpaa2_ceetm_cls_walk(struct Qdisc *sch, struct qdisc_walker *arg)
{
	struct dpaa2_ceetm_qdisc *priv = qdisc_priv(sch);
	struct dpaa2_ceetm_class *cl;
	unsigned int i;

	pr_debug(KBUILD_BASENAME " : %s : qdisc %X\n", __func__, sch->handle);

	if (arg->stop)
		return;

	for (i = 0; i < priv->clhash.hashsize; i++) {
		hlist_for_each_entry(cl, &priv->clhash.hash[i], common.hnode) {
			if (arg->count < arg->skip) {
				arg->count++;
				continue;
			}
			if (arg->fn(sch, (unsigned long)cl, arg) < 0) {
				arg->stop = 1;
				return;
			}
			arg->count++;
		}
	}
}

static int dpaa2_ceetm_cls_dump(struct Qdisc *sch, unsigned long arg,
				struct sk_buff *skb, struct tcmsg *tcm)
{
	struct dpaa2_ceetm_class *cl = (struct dpaa2_ceetm_class *)arg;
	struct nlattr *nest;
	struct dpaa2_ceetm_tc_copt copt;

	pr_debug(KBUILD_BASENAME " : %s : class %X under qdisc %X\n",
		 __func__, cl->common.classid, sch->handle);

	sch_tree_lock(sch);

	tcm->tcm_parent = ((struct Qdisc *)cl->parent)->handle;
	tcm->tcm_handle = cl->common.classid;

	memset(&copt, 0, sizeof(copt));

	copt.shaped = cl->shaped;
	copt.type = cl->type;

	switch (cl->type) {
	case CEETM_ROOT:
		if (cl->child)
			tcm->tcm_info = cl->child->handle;

		memcpy(&copt.shaping_cfg, &cl->root.shaping_cfg,
		       sizeof(struct dpaa2_ceetm_shaping_cfg));

		break;

	case CEETM_PRIO:
		if (cl->child)
			tcm->tcm_info = cl->child->handle;

		copt.mode = cl->prio.mode;
		copt.weight = cl->prio.weight;

		break;
	}

	nest = nla_nest_start_noflag(skb, TCA_OPTIONS);
	if (!nest)
		goto nla_put_failure;
	if (nla_put(skb, DPAA2_CEETM_TCA_COPT, sizeof(copt), &copt))
		goto nla_put_failure;
	nla_nest_end(skb, nest);
	sch_tree_unlock(sch);
	return skb->len;

nla_put_failure:
	sch_tree_unlock(sch);
	nla_nest_cancel(skb, nest);
	return -EMSGSIZE;
}

static int dpaa2_ceetm_cls_delete(struct Qdisc *sch, unsigned long arg,
				  struct netlink_ext_ack *extack)
{
	struct dpaa2_ceetm_qdisc *priv = qdisc_priv(sch);
	struct dpaa2_ceetm_class *cl = (struct dpaa2_ceetm_class *)arg;

	pr_debug(KBUILD_BASENAME " : %s : class %X under qdisc %X\n",
		 __func__, cl->common.classid, sch->handle);

	sch_tree_lock(sch);
	qdisc_class_hash_remove(&priv->clhash, &cl->common);
	sch_tree_unlock(sch);
	return 0;
}

/* Get the class' child qdisc, if any */
static struct Qdisc *dpaa2_ceetm_cls_leaf(struct Qdisc *sch, unsigned long arg)
{
	struct dpaa2_ceetm_class *cl = (struct dpaa2_ceetm_class *)arg;

	pr_debug(KBUILD_BASENAME " : %s : class %X under qdisc %X\n",
		 __func__, cl->common.classid, sch->handle);

	switch (cl->type) {
	case CEETM_ROOT:
	case CEETM_PRIO:
		return cl->child;
	}

	return NULL;
}

static int dpaa2_ceetm_cls_graft(struct Qdisc *sch, unsigned long arg,
				 struct Qdisc *new, struct Qdisc **old,
				 struct netlink_ext_ack *extack)
{
	if (new && strcmp(new->ops->id, dpaa2_ceetm_qdisc_ops.id)) {
		pr_err("CEETM: only ceetm qdiscs can be attached to ceetm classes\n");
		return -EOPNOTSUPP;
	}

	return 0;
}

static int dpaa2_ceetm_cls_dump_stats(struct Qdisc *sch, unsigned long arg,
				      struct gnet_dump *d)
{
	struct dpaa2_ceetm_class *cl = (struct dpaa2_ceetm_class *)arg;
	struct dpaa2_ceetm_tc_xstats xstats;
	union dpni_statistics dpni_stats;
	struct net_device *dev = qdisc_dev(sch);
	struct dpaa2_eth_priv *priv_eth = netdev_priv(dev);
	u8 ch_id = 0;
	int err;

	memset(&xstats, 0, sizeof(xstats));

	if (cl->type == CEETM_ROOT)
		return 0;

	err = dpni_get_statistics(priv_eth->mc_io, 0, priv_eth->mc_token, 3,
				  DPNI_BUILD_CH_TC(ch_id, cl->prio.qpri),
				  &dpni_stats);
	if (err)
		netdev_warn(dev, "dpni_get_stats(%d) failed - %d\n", 3, err);

	xstats.ceetm_dequeue_bytes = dpni_stats.page_3.egress_dequeue_bytes;
	xstats.ceetm_dequeue_frames = dpni_stats.page_3.egress_dequeue_frames;
	xstats.ceetm_reject_bytes = dpni_stats.page_3.egress_reject_bytes;
	xstats.ceetm_reject_frames = dpni_stats.page_3.egress_reject_frames;

	return gnet_stats_copy_app(d, &xstats, sizeof(xstats));
}

static struct tcf_block *dpaa2_ceetm_tcf_block(struct Qdisc *sch,
					       unsigned long arg,
					       struct netlink_ext_ack *extack)
{
	struct dpaa2_ceetm_qdisc *priv = qdisc_priv(sch);
	struct dpaa2_ceetm_class *cl = (struct dpaa2_ceetm_class *)arg;

	pr_debug(KBUILD_BASENAME " : %s : class %X under qdisc %X\n", __func__,
		 cl ? cl->common.classid : 0, sch->handle);
	return cl ? cl->block : priv->block;
}

static unsigned long dpaa2_ceetm_tcf_bind(struct Qdisc *sch,
					  unsigned long parent,
					  u32 classid)
{
	struct dpaa2_ceetm_class *cl = dpaa2_ceetm_find(classid, sch);

	pr_debug(KBUILD_BASENAME " : %s : class %X under qdisc %X\n", __func__,
		 cl ? cl->common.classid : 0, sch->handle);
	return (unsigned long)cl;
}

static void dpaa2_ceetm_tcf_unbind(struct Qdisc *sch, unsigned long arg)
{
	struct dpaa2_ceetm_class *cl = (struct dpaa2_ceetm_class *)arg;

	pr_debug(KBUILD_BASENAME " : %s : class %X under qdisc %X\n", __func__,
		 cl ? cl->common.classid : 0, sch->handle);
}

const struct Qdisc_class_ops dpaa2_ceetm_cls_ops = {
	.graft		=	dpaa2_ceetm_cls_graft,
	.leaf		=	dpaa2_ceetm_cls_leaf,
	.find		=	dpaa2_ceetm_cls_find,
	.change		=	dpaa2_ceetm_cls_change,
	.delete		=	dpaa2_ceetm_cls_delete,
	.walk		=	dpaa2_ceetm_cls_walk,
	.tcf_block	=	dpaa2_ceetm_tcf_block,
	.bind_tcf	=	dpaa2_ceetm_tcf_bind,
	.unbind_tcf	=	dpaa2_ceetm_tcf_unbind,
	.dump		=	dpaa2_ceetm_cls_dump,
	.dump_stats	=	dpaa2_ceetm_cls_dump_stats,
};

struct Qdisc_ops dpaa2_ceetm_qdisc_ops __read_mostly = {
	.id		=	"ceetm",
	.priv_size	=	sizeof(struct dpaa2_ceetm_qdisc),
	.cl_ops		=	&dpaa2_ceetm_cls_ops,
	.init		=	dpaa2_ceetm_init,
	.destroy	=	dpaa2_ceetm_destroy,
	.change		=	dpaa2_ceetm_change,
	.dump		=	dpaa2_ceetm_dump,
	.attach		=	dpaa2_ceetm_attach,
	.owner		=	THIS_MODULE,
};

/* Run the filters and classifiers attached to the qdisc on the provided skb */
int dpaa2_ceetm_classify(struct sk_buff *skb, struct Qdisc *sch,
			 int *qdid, u8 *qpri)
{
	struct dpaa2_ceetm_qdisc *priv = qdisc_priv(sch);
	struct dpaa2_ceetm_class *cl = NULL;
	struct tcf_result res;
	struct tcf_proto *tcf;
	int result;

	tcf = rcu_dereference_bh(priv->filter_list);
	while (tcf && (result = tcf_classify(skb, priv->block, tcf, &res, false)) >= 0) {
#ifdef CONFIG_NET_CLS_ACT
		switch (result) {
		case TC_ACT_QUEUED:
		case TC_ACT_STOLEN:
		case TC_ACT_SHOT:
			/* No valid class found due to action */
			return -1;
		}
#endif
		cl = (void *)res.class;
		if (!cl) {
			/* The filter leads to the qdisc */
			if (res.classid == sch->handle)
				return 0;

			cl = dpaa2_ceetm_find(res.classid, sch);
			/* The filter leads to an invalid class */
			if (!cl)
				break;
		}

		/* The class might have its own filters attached */
		tcf = rcu_dereference_bh(cl->filter_list);
	}

	/* No valid class found */
	if (!cl)
		return 0;

	switch (cl->type) {
	case CEETM_ROOT:
		*qdid = cl->root.ch_id;

		/* The root class does not have a child prio qdisc */
		if (!cl->child)
			return 0;

		/* Run the prio qdisc classifiers */
		return dpaa2_ceetm_classify(skb, cl->child, qdid, qpri);

	case CEETM_PRIO:
		*qpri = cl->prio.qpri;
		break;
	}

	return 0;
}

int __init dpaa2_ceetm_register(void)
{
	int err = 0;

	pr_debug(KBUILD_MODNAME ": " DPAA2_CEETM_DESCRIPTION "\n");

	err = register_qdisc(&dpaa2_ceetm_qdisc_ops);
	if (unlikely(err))
		pr_err(KBUILD_MODNAME
		       ": %s:%hu:%s(): register_qdisc() = %d\n",
		       KBUILD_BASENAME ".c", __LINE__, __func__, err);

	return err;
}

void __exit dpaa2_ceetm_unregister(void)
{
	pr_debug(KBUILD_MODNAME ": %s:%s() ->\n",
		 KBUILD_BASENAME ".c", __func__);

	unregister_qdisc(&dpaa2_ceetm_qdisc_ops);
}
