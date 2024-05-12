// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 AIROHA Inc
 * Author: Lorenzo Bianconi <lorenzo@kernel.org>
 */
#include <linux/etherdevice.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/phylink.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <net/page_pool/helpers.h>
#include "airoha_eth.h"

static u32 airoha_rr(void __iomem *base, u32 offset)
{
	return readl(base + offset);
}

static void airoha_wr(void __iomem *base, u32 offset, u32 val)
{
	writel(val, base + offset);
}

static u32 airoha_rmw(void __iomem *base, u32 offset, u32 mask, u32 val)
{
	val |= airoha_rr(base, offset) & ~mask;
	airoha_wr(base, offset, val);

	return val;
}

#define airoha_fe_rr(eth, offset)		airoha_rr((eth)->fe_regs, (offset))
#define airoha_fe_wr(eth, offset, val)		airoha_wr((eth)->fe_regs, (offset), (val))
#define airoha_fe_rmw(eth, offset, mask, val)	airoha_rmw((eth)->fe_regs, (offset), (mask), (val))
#define airoha_fe_set(eth, offset, val)		airoha_rmw((eth)->fe_regs, (offset), 0, (val))
#define airoha_fe_clear(eth, offset, val)	airoha_rmw((eth)->fe_regs, (offset), (val), 0)

#define airoha_qdma_rr(eth, offset)		airoha_rr((eth)->qdma_regs, (offset))
#define airoha_qdma_wr(eth, offset, val)	airoha_wr((eth)->qdma_regs, (offset), (val))
#define airoha_qdma_rmw(eth, offset, mask, val)	airoha_rmw((eth)->qdma_regs, (offset), (mask), (val))
#define airoha_qdma_set(eth, offset, val)	airoha_rmw((eth)->qdma_regs, (offset), 0, (val))
#define airoha_qdma_clear(eth, offset, val)	airoha_rmw((eth)->qdma_regs, (offset), (val), 0)

static void airoha_set_irqmask(struct airoha_eth *eth, int index,
			       u32 clear, u32 set)
{
	unsigned long flags;

	if (WARN_ON_ONCE(index >= ARRAY_SIZE(eth->irqmask)))
		return;

	spin_lock_irqsave(&eth->irq_lock, flags);

	eth->irqmask[index] &= ~clear;
	eth->irqmask[index] |= set;
	airoha_qdma_wr(eth, REG_INT_ENABLE(index), eth->irqmask[index]);

	spin_unlock_irqrestore(&eth->irq_lock, flags);
}

#define airoha_irq_enable(eth, index, mask)	airoha_set_irqmask((eth), (index), 0, (mask))
#define airoha_irq_disable(eth, index, mask)	airoha_set_irqmask((eth), (index), (mask), 0)

static int airoha_dev_init(struct net_device *dev)
{
	struct airoha_eth *eth = netdev_priv(dev);
	const u8 *addr = dev->dev_addr;
	u32 mac_h, mac_lmin;

	mac_h = (addr[0] << 16) | (addr[1] << 8) | addr[2];
	mac_lmin = (addr[3] << 16) | (addr[4] << 8) | addr[5];

	airoha_fe_wr(eth, REG_FE_LAN_MAC_H, mac_h);
	airoha_fe_wr(eth, REG_FE_LAN_MAC_LMIN, mac_lmin);
	airoha_fe_wr(eth, REG_FE_LAN_MAC_LMAX, mac_lmin);

	return 0;
}

static void airoha_set_port_fwd_cfg(struct airoha_eth *eth, u32 addr, u32 val)
{
	airoha_fe_rmw(eth, addr, GDMA1_OCFQ_MASK,
		      FIELD_PREP(GDMA1_OCFQ_MASK, val));
	airoha_fe_rmw(eth, addr, GDMA1_MCFQ_MASK,
		      FIELD_PREP(GDMA1_MCFQ_MASK, val));
	airoha_fe_rmw(eth, addr, GDMA1_BCFQ_MASK,
		      FIELD_PREP(GDMA1_BCFQ_MASK, val));
	airoha_fe_rmw(eth, addr, GDMA1_UCFQ_MASK,
		      FIELD_PREP(GDMA1_UCFQ_MASK, val));
}

static int airoha_set_gdma_port(struct airoha_eth *eth, int port, bool enable)
{
	u32 vip_port, cfg_addr, val = enable ? 4 : 0xf;

	switch (port) {
	case 0:
		vip_port = BIT(22);
		cfg_addr = REG_GDMA3_FWD_CFG;
		break;
	case 1:
		vip_port = BIT(23);
		cfg_addr = REG_GDMA3_FWD_CFG;
		break;
	case 2:
		vip_port = BIT(25);
		cfg_addr = REG_GDMA4_FWD_CFG;
		break;
	case 4:
		vip_port = BIT(24);
		cfg_addr = REG_GDMA4_FWD_CFG;
		break;
	default:
		return -EINVAL;
	}

	if (enable) {
		airoha_fe_set(eth, REG_FE_VIP_PORT_EN, vip_port);
		airoha_fe_set(eth, REG_FE_IFC_PORT_EN, vip_port);
	} else {
		airoha_fe_clear(eth, REG_FE_VIP_PORT_EN, vip_port);
		airoha_fe_clear(eth, REG_FE_IFC_PORT_EN, vip_port);
	}

	airoha_set_port_fwd_cfg(eth, cfg_addr, val);

	return 0;
}

static int airoha_set_gdma_ports(struct airoha_eth *eth, bool enable)
{
	const int port_list[] = { 0, 1, 2, 4 };
	int i, err;

	for (i = 0; i < ARRAY_SIZE(port_list); i++) {
		err = airoha_set_gdma_port(eth, port_list[i], enable);
		if (err)
			return err;
	}

	return 0;
}

static int airoha_dev_open(struct net_device *dev)
{
	struct airoha_eth *eth = netdev_priv(dev);

	netif_start_queue(dev);

	return airoha_set_gdma_ports(eth, true);
}

static int airoha_dev_stop(struct net_device *dev)
{
	struct airoha_eth *eth = netdev_priv(dev);

	netif_stop_queue(dev);

	return airoha_set_gdma_ports(eth, false);
}

static netdev_tx_t airoha_dev_start_xmit(struct sk_buff *skb,
					 struct net_device *dev)
{
	return NETDEV_TX_OK;
}

static int airoha_dev_change_mtu(struct net_device *dev, int new_mtu)
{
	dev->mtu = new_mtu;

	return 0;
}

static const struct net_device_ops airoha_netdev_ops = {
	.ndo_init		= airoha_dev_init,
	.ndo_open		= airoha_dev_open,
	.ndo_stop		= airoha_dev_stop,
	.ndo_start_xmit		= airoha_dev_start_xmit,
	.ndo_change_mtu		= airoha_dev_change_mtu,
};

static const struct phylink_mac_ops airoha_phylink_ops = {
};

static void airoha_maccr_init(struct airoha_eth *eth)
{
	airoha_fe_set(eth, REG_GDMA1_FWD_CFG, GDMA1_TCP_CKSUM);
	airoha_set_port_fwd_cfg(eth, REG_GDMA1_FWD_CFG, 0);

	airoha_fe_set(eth, REG_FE_CPORT_CFG, FE_CPORT_PAD);
	airoha_fe_rmw(eth, REG_CDMA1_VLAN_CTRL, CDMA1_VLAN_MASK,
		      FIELD_PREP(CDMA1_VLAN_MASK, 0x8100));
	airoha_fe_rmw(eth, REG_GDMA1_LEN_CFG, GDMA1_SHORT_LEN_MASK,
		      FIELD_PREP(GDMA1_SHORT_LEN_MASK, 60));
	airoha_fe_rmw(eth, REG_GDMA1_LEN_CFG, GDMA1_LONG_LEN_MASK,
		      FIELD_PREP(GDMA1_LONG_LEN_MASK, 4004));
}

static int airoha_qdma_fill_rx_queue(struct airoha_eth *eth,
				     struct airoha_queue *q)
{
	int qid = q - &eth->q_rx[0], nframes = 0;

	spin_lock_bh(&q->lock);
	while (q->queued < q->ndesc - 1) {
		struct airoha_queue_entry *e = &q->entry[q->head];
		struct airoha_qdma_desc *desc = &q->desc[q->head];
		enum dma_data_direction dir;
		struct page *page;
		int offset;
		u32 val;

		page = page_pool_dev_alloc_frag(q->page_pool, &offset,
						q->buf_size);
		if (!page)
			break;

		e->buf = page_address(page) + offset;
		e->dma_addr = page_pool_get_dma_addr(page) + offset;
		e->dma_len = SKB_WITH_OVERHEAD(q->buf_size);

		dir = page_pool_get_dma_dir(q->page_pool);
		dma_sync_single_for_device(eth->dev, e->dma_addr, e->dma_len,
					   dir);

		val = FIELD_PREP(QDMA_DESC_LEN_MASK, e->dma_len);
		WRITE_ONCE(desc->ctrl, cpu_to_le32(val));
		WRITE_ONCE(desc->addr, cpu_to_le32(e->dma_addr));
		val = FIELD_PREP(QDMA_DESC_NEXT_ID_MASK, q->head);
		WRITE_ONCE(desc->data, cpu_to_le32(val));
		WRITE_ONCE(desc->msg0, 0);
		WRITE_ONCE(desc->msg1, 0);
		WRITE_ONCE(desc->msg2, 0);
		WRITE_ONCE(desc->msg3, 0);

		mb();

		airoha_qdma_rmw(eth, REG_RX_CPU_IDX(qid), RX_RING_CPU_IDX_MASK,
				FIELD_PREP(RX_RING_CPU_IDX_MASK, q->head));

		q->head = (q->head + 1) % q->ndesc;
		q->queued++;
		nframes++;
	}
	spin_unlock_bh(&q->lock);

	return nframes;
}

static int airoha_qdma_init_rx_queue(struct airoha_eth *eth,
				     struct airoha_queue *q, int ndesc)
{
	struct page_pool_params pp_params = {
		.order = 0,
		.pool_size = 256,
		.flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
		.dma_dir = DMA_FROM_DEVICE,
		.max_len = PAGE_SIZE,
		.nid = NUMA_NO_NODE,
		.dev = eth->dev,
	};
	int qid = q - &eth->q_rx[0], thr;
	dma_addr_t dma_addr;

	spin_lock_init(&q->lock);
	q->ndesc = ndesc;
	q->buf_size = PAGE_SIZE;

	q->entry = devm_kzalloc(eth->dev, q->ndesc * sizeof(*q->entry),
				GFP_KERNEL);
	if (!q->entry)
		return -ENOMEM;

	q->page_pool = page_pool_create(&pp_params);
	if (IS_ERR(q->page_pool)) {
		int err = PTR_ERR(q->page_pool);

		q->page_pool = NULL;
		return err;
	}

	q->desc = dmam_alloc_coherent(eth->dev, q->ndesc * sizeof(*q->desc),
				      &dma_addr, GFP_KERNEL);
	if (!q->desc)
		return -ENOMEM;

	airoha_qdma_wr(eth, REG_RX_RING_BASE(qid), dma_addr);
	airoha_qdma_rmw(eth, REG_RX_RING_SIZE(qid), RX_RING_SIZE_MASK,
			FIELD_PREP(RX_RING_SIZE_MASK, ndesc));

	thr = clamp(ndesc >> 3, 1, 32);
	airoha_qdma_rmw(eth, REG_RX_RING_SIZE(qid), RX_RING_THR_MASK,
			FIELD_PREP(RX_RING_THR_MASK, thr));
	airoha_qdma_rmw(eth, REG_RX_DMA_IDX(qid), RX_RING_DMA_IDX_MASK,
			FIELD_PREP(RX_RING_DMA_IDX_MASK, q->head));

	airoha_qdma_fill_rx_queue(eth, q);
	return 0;
}

static void airoha_qdma_clenaup_rx_queue(struct airoha_eth *eth,
					 struct airoha_queue *q)
{
	spin_lock_bh(&q->lock);

	while (q->queued) {
		struct airoha_queue_entry *e = &q->entry[q->tail];
		struct page *page = virt_to_head_page(e->buf);
		enum dma_data_direction dir;

		dir = page_pool_get_dma_dir(q->page_pool);
		dma_sync_single_for_cpu(eth->dev, e->dma_addr, e->dma_len,
					dir);
		page_pool_put_full_page(q->page_pool, page, false);
		q->tail = (q->tail + 1) % q->ndesc;
		q->queued--;
	}

	spin_unlock_bh(&q->lock);
}

static irqreturn_t airoha_irq_handler(int irq, void *dev_instance)
{
	return IRQ_HANDLED;
}

static int airoha_qdma_init_rx(struct airoha_eth *eth)
{
	int i, err;

	err = devm_request_irq(eth->dev, eth->irq, airoha_irq_handler,
			        IRQF_SHARED, KBUILD_MODNAME, eth);
	if (err)
		return err;

	for (i = 0; i < ARRAY_SIZE(eth->q_rx); i++) {
		err = airoha_qdma_init_rx_queue(eth, &eth->q_rx[i],
						RX_DSCP_NUM(i));
		if (err)
			return err;
	}

	return 0;
}

static int airoha_qdma_init_tx_queue(struct airoha_eth *eth,
				     struct airoha_queue *q, int size)
{
	int i, qid = q - &eth->q_xmit[0];
	dma_addr_t dma_addr;

	spin_lock_init(&q->lock);
	q->ndesc = size;

	q->entry = devm_kzalloc(eth->dev, q->ndesc * sizeof(*q->entry),
				GFP_KERNEL);
	if (!q->entry)
		return -ENOMEM;

	q->desc = dmam_alloc_coherent(eth->dev, q->ndesc * sizeof(*q->desc),
				      &dma_addr, GFP_KERNEL);
	if (!q->desc)
		return -ENOMEM;

	for (i = 0; i < q->ndesc; i++) {
		u32 val;

		val = FIELD_PREP(QDMA_DESC_DONE_MASK, 1);
		WRITE_ONCE(q->desc[i].ctrl, cpu_to_le32(val));
	}

	airoha_qdma_wr(eth, REG_TX_RING_BASE(qid), dma_addr);
	airoha_qdma_rmw(eth, REG_TX_CPU_IDX(qid), TX_RING_CPU_IDX_MASK,
			FIELD_PREP(TX_RING_CPU_IDX_MASK, q->head));
	airoha_qdma_rmw(eth, REG_TX_DMA_IDX(qid), TX_RING_DMA_IDX_MASK,
			FIELD_PREP(TX_RING_DMA_IDX_MASK, q->head));

	return 0;
}

static int airoha_qdma_init_tx(struct airoha_eth *eth)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(eth->q_xmit); i++) {
		int err;

		err = airoha_qdma_init_tx_queue(eth, &eth->q_xmit[i],
						TX_DSCP_NUM(i));
		if (err)
			return err;
	}

	return 0;
}

static u32 airoha_qdma_lmgr_rr(struct airoha_eth *eth)
{
	return airoha_qdma_rr(eth, REG_LMGR_INIT_CFG);
}

static int airoha_qdma_init_hw_queues(struct airoha_eth *eth)
{
	dma_addr_t dma_addr;
	u32 status;
	int size;

	eth->irq_q = dmam_alloc_coherent(eth->dev, MAX_PKT_LEN, &dma_addr,
					 GFP_KERNEL);
	if (!eth->irq_q)
		return -ENOMEM;

	memset(eth->irq_q, 0xff, MAX_PKT_LEN);
	airoha_qdma_wr(eth, REG_TX_IRQ_BASE, dma_addr);
	airoha_qdma_rmw(eth, REG_TX_IRQ_CFG, TX_IRQ_DEPTH_MASK,
			FIELD_PREP(TX_IRQ_DEPTH_MASK, MAX_PKT_LEN >> 2));
	airoha_qdma_rmw(eth, REG_TX_IRQ_CFG, TX_IRQ_THR_MASK,
			FIELD_PREP(TX_IRQ_THR_MASK, 32));

	size = HW_DSCP_NUM * sizeof(struct airoha_qdma_fwd_desc);
	eth->hfwd_desc = dmam_alloc_coherent(eth->dev, size, &dma_addr,
					     GFP_KERNEL);
	if (!eth->hfwd_desc)
		return -ENOMEM;

	airoha_qdma_wr(eth, REG_FWD_DSCP_BASE, dma_addr);

	size = MAX_PKT_LEN * HW_DSCP_NUM;
	eth->hfwd_q = dmam_alloc_coherent(eth->dev, size, &dma_addr,
					  GFP_KERNEL);
	if (!eth->hfwd_q)
		return -ENOMEM;

	airoha_qdma_wr(eth, REG_FWD_BUF_BASE, dma_addr);
	airoha_qdma_rmw(eth, REG_LMGR_INIT_CFG, HW_FWD_DESC_NUM_MASK,
			FIELD_PREP(HW_FWD_DESC_NUM_MASK, HW_DSCP_NUM));
	airoha_qdma_rmw(eth, REG_HW_FWD_DSCP_CFG,
			HW_FWD_DSCP_PAYLOAD_SIZE_MASK,
			FIELD_PREP(HW_FWD_DSCP_PAYLOAD_SIZE_MASK, 0));
	airoha_qdma_rmw(eth, REG_FWD_DSCP_LOW_THR, FWD_DSCP_LOW_THR_MASK,
			FIELD_PREP(FWD_DSCP_LOW_THR_MASK, 256));
	airoha_qdma_set(eth, REG_LMGR_INIT_CFG, LGMR_INIT_START);

	return readx_poll_timeout(airoha_qdma_lmgr_rr, eth, status,
				  !(status & LGMR_INIT_START), USEC_PER_MSEC,
				  25 * USEC_PER_MSEC);
}

static int airoha_qdma_hw_init(struct airoha_eth *eth)
{
	airoha_qdma_wr(eth, REG_INT_STATUS(QDMA_INT_REG_IDX0), 0xffffffff);
	airoha_qdma_wr(eth, REG_INT_STATUS(QDMA_INT_REG_IDX1), 0xffffffff);
	airoha_qdma_wr(eth, REG_INT_STATUS(QDMA_INT_REG_IDX2), 0xffffffff);
	airoha_qdma_wr(eth, REG_INT_STATUS(QDMA_INT_REG_IDX3), 0xffffffff);
	airoha_qdma_wr(eth, REG_INT_STATUS(QDMA_INT_REG_IDX4), 0xffffffff);

	airoha_irq_enable(eth, QDMA_INT_REG_IDX0, INT_IDX0_MASK);
	airoha_irq_enable(eth, QDMA_INT_REG_IDX1, INT_IDX1_MASK);
	airoha_irq_enable(eth, QDMA_INT_REG_IDX4, INT_IDX4_MASK);

	airoha_wr(eth, REG_QDMA_GLOBAL_CFG,
		  GLOBAL_CFG_PAYLOAD_BYTE_SWAP | GLOBAL_CFG_IRQ0_EN |
		  FIELD_PREP(GLOBAL_CFG_DMA_PREFERENCE_MASK, 3) |
		  FIELD_PREP(GLOBAL_CFG_MAX_ISSUE_NUM_MASK, 2) |
		  GLOBAL_CFG_CPU_TXR_ROUND_ROBIN);

	/* FIXME add QoS configuration here */

	airoha_qdma_set(eth, REG_TXQ_CNGST_CFG, TXQ_CNGST_DEI_DROP_EN);

	airoha_qdma_set(eth, REG_QDMA_GLOBAL_CFG, GLOBAL_CFG_TX_DMA_EN);
	airoha_qdma_set(eth, REG_QDMA_GLOBAL_CFG, GLOBAL_CFG_RX_DMA_EN);

	return 0;
}

static int airoha_qdma_init(struct airoha_eth *eth)
{
	int err;

	err = airoha_qdma_init_rx(eth);
	if (err)
		return err;

	err = airoha_qdma_init_tx(eth);
	if (err)
		return err;

	err = airoha_qdma_init_hw_queues(eth);
	if (err)
		return err;

	return airoha_qdma_hw_init(eth);
}

static int airoha_hw_init(struct airoha_eth *eth)
{
	reset_control_bulk_assert(AIROHA_MAX_NUM_RSTS, eth->resets);
	msleep(20);
	reset_control_bulk_deassert(AIROHA_MAX_NUM_RSTS, eth->resets);
	msleep(20);

	airoha_maccr_init(eth);

	return airoha_qdma_init(eth);
}

static int airoha_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	phy_interface_t phy_mode;
	struct net_device *dev;
	struct airoha_eth *eth;
	int err;

	dev = devm_alloc_etherdev_mqs(&pdev->dev, sizeof(*eth),
				      AIROHA_NUM_TX_RING, AIROHA_NUM_RX_RING);
	if (!dev) {
		dev_err(&pdev->dev, "alloc_etherdev failed\n");
		return -ENOMEM;
	}

	eth = netdev_priv(dev);
	platform_set_drvdata(pdev, eth);
	eth->dev = &pdev->dev;

	eth->fe_regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(eth->fe_regs))
		return dev_err_probe(eth->dev, PTR_ERR(eth->fe_regs),
				     "failed to iomap fe regs\n");

	eth->qdma_regs = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(eth->qdma_regs))
		return dev_err_probe(eth->dev, PTR_ERR(eth->qdma_regs),
				     "failed to iomap qdma regs\n");

	eth->resets[0].id = "fe";
	eth->resets[1].id = "pdma";
	eth->resets[2].id = "qdma";
	err = devm_reset_control_bulk_get_exclusive(eth->dev,
						    AIROHA_MAX_NUM_RSTS,
						    eth->resets);
	if (err) {
		dev_err(eth->dev, "failed to get bulk reset lines\n");
		return err;
	}

	spin_lock_init(&eth->irq_lock);
	eth->irq = platform_get_irq(pdev, 0);
	if (eth->irq < 0) {
		dev_err(eth->dev, "failed reading irq line\n");
		return eth->irq;
	}

	dev->netdev_ops = &airoha_netdev_ops;
	dev->max_mtu = AIROHA_MAX_MTU;
	dev->watchdog_timeo = 5 * HZ;
	/* FIXME: check HW features */
	dev->hw_features = AIROHA_HW_FEATURES;
	dev->features |= dev->hw_features;
	dev->dev.of_node = np;
	dev->irq = eth->irq;
	SET_NETDEV_DEV(dev, eth->dev);

	err = of_get_ethdev_address(np, dev);
	if (err) {
		if (err == -EPROBE_DEFER)
			return err;

		eth_hw_addr_random(dev);
		dev_err(eth->dev, "generated random MAC address %pM\n",
			dev->dev_addr);
	}

	err = airoha_hw_init(eth);
	if (err)
		return err;

	/* phylink create */
	err = of_get_phy_mode(np, &phy_mode);
	if (err) {
		dev_err(eth->dev, "incorrect phy-mode\n");
		return err;
	}

	/* mac config is not set */
	eth->interface = PHY_INTERFACE_MODE_NA;
	eth->speed = SPEED_UNKNOWN;

	eth->phylink_config.dev = &dev->dev;
	eth->phylink_config.type = PHYLINK_NETDEV;
	eth->phylink_config.mac_capabilities = MAC_ASYM_PAUSE | MAC_SYM_PAUSE |
					       MAC_10 | MAC_100 | MAC_1000 |
					       MAC_2500FD;

	/* FIXME: missing phylink_ops callbacks */
	eth->phylink = phylink_create(&eth->phylink_config,
				      of_fwnode_handle(np), phy_mode,
				      &airoha_phylink_ops);
	if (IS_ERR(eth->phylink))
		return PTR_ERR(eth->phylink);

	return register_netdev(dev);
}

static void airoha_remove(struct platform_device *pdev)
{
	struct airoha_eth *eth = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < ARRAY_SIZE(eth->q_rx); i++) {
		airoha_qdma_clenaup_rx_queue(eth, &eth->q_rx[i]);
		page_pool_destroy(eth->q_rx[i].page_pool);
	}
}

const struct of_device_id of_airoha_match[] = {
	{ .compatible = "airoha,en7581-eth" },
	{ /* sentinel */ }
};

static struct platform_driver airoha_driver = {
	.probe = airoha_probe,
	.remove_new = airoha_remove,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = of_airoha_match,
	},
};
module_platform_driver(airoha_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lorenzo Bianconi <lorenzo@kernel.org>");
MODULE_DESCRIPTION("Ethernet driver for Airoha SoC");
