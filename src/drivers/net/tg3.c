/* $Id$
 * tg3.c: Broadcom Tigon3 ethernet driver.
 *
 * Copyright (C) 2001, 2002, 2003 David S. Miller (davem@redhat.com)
 * Copyright (C) 2001, 2002 Jeff Garzik (jgarzik@mandrakesoft.com)
 * Copyright (C0 2003 Eric Biederman (ebiederman@lnxi.com)  [etherboot port]
 */

#include "etherboot.h"
#include "nic.h"
#include "pci.h"
#include "timer.h"
#include "string.h"
#include "mii.h"
#include "tg3.h"

#define REALLY_TEST_DMA    1
#define SUPPORT_COPPER_PHY 1
#define SUPPORT_FIBER_PHY  1
#define SUPPORT_PARTNO_STR 1

/**
 * pci_save_state - save the PCI configuration space of a device before suspending
 * @dev: - PCI device that we're dealing with
 * @buffer: - buffer to hold config space context
 *
 * @buffer must be large enough to hold the entire PCI 2.2 config space 
 * (>= 64 bytes).
 */
static int pci_save_state(struct pci_device *dev, uint32_t *buffer)
{
	int i;
	for (i = 0; i < 16; i++)
		pci_read_config_dword(dev, i * 4,&buffer[i]);
	return 0;
}

/** 
 * pci_restore_state - Restore the saved state of a PCI device
 * @dev: - PCI device that we're dealing with
 * @buffer: - saved PCI config space
 *
 */
static int pci_restore_state(struct pci_device *dev, uint32_t *buffer)
{
	int i;

	for (i = 0; i < 16; i++)
		pci_write_config_dword(dev,i * 4, buffer[i]);
	return 0;
}

struct tg3 tg3;

/* Dummy defines for error handling */
#define EBUSY  1
#define ENODEV 1
#define EINVAL 1
#define ENOMEM 1

#define TG3_DEF_MAC_MODE	0
#define TG3_DEF_RX_MODE		0
#define TG3_DEF_TX_MODE		0

/* These numbers seem to be hard coded in the NIC firmware somehow.
 * You can't change the ring sizes, but you can change where you place
 * them in the NIC onboard memory.
 */
#define TG3_RX_RING_SIZE		512
#define TG3_DEF_RX_RING_PENDING		20	/* RX_RING_PENDING seems to be o.k. at 20 and 200 */
#define TG3_RX_RCB_RING_SIZE		1024
#define TG3_TX_RING_SIZE		512
#define TG3_DEF_TX_RING_PENDING		(TG3_TX_RING_SIZE - 1)

#define TG3_RX_RING_BYTES	(sizeof(struct tg3_rx_buffer_desc) * TG3_RX_RING_SIZE)
#define TG3_RX_RCB_RING_BYTES	(sizeof(struct tg3_rx_buffer_desc) * TG3_RX_RCB_RING_SIZE)

#define TG3_TX_RING_BYTES	(sizeof(struct tg3_tx_buffer_desc) * TG3_TX_RING_SIZE)
#define NEXT_TX(N)		(((N) + 1) & (TG3_TX_RING_SIZE - 1))

#define MAX_RX_PKT_BUF_SZ	(1536 + 2 + 64)
#define RX_PKT_BUF_SZ		(1536 + tp->rx_offset + 64)

static void tg3_write_indirect_reg32(struct tg3 *tp, uint32_t off, uint32_t val)
{
	pci_write_config_dword(tp->pdev, TG3PCI_REG_BASE_ADDR, off);
	pci_write_config_dword(tp->pdev, TG3PCI_REG_DATA, val);
}

#define tw32(reg,val)		tg3_write_indirect_reg32(tp,(reg),(val))
#define tw32_mailbox(reg, val)	writel(((val) & 0xffffffff), tp->regs + (reg))
#define tw16(reg,val)		writew(((val) & 0xffff), tp->regs + (reg))
#define tw8(reg,val)		writeb(((val) & 0xff), tp->regs + (reg))
#define tr32(reg)		readl(tp->regs + (reg))
#define tr16(reg)		readw(tp->regs + (reg))
#define tr8(reg)		readb(tp->regs + (reg))

static void tg3_write_mem(struct tg3 *tp, uint32_t off, uint32_t val)
{
	pci_write_config_dword(tp->pdev, TG3PCI_MEM_WIN_BASE_ADDR, off);
	pci_write_config_dword(tp->pdev, TG3PCI_MEM_WIN_DATA, val);

	/* Always leave this as zero. */
	pci_write_config_dword(tp->pdev, TG3PCI_MEM_WIN_BASE_ADDR, 0);
}

static void tg3_read_mem(struct tg3 *tp, uint32_t off, uint32_t *val)
{
	pci_write_config_dword(tp->pdev, TG3PCI_MEM_WIN_BASE_ADDR, off);
	pci_read_config_dword(tp->pdev, TG3PCI_MEM_WIN_DATA, val);

	/* Always leave this as zero. */
	pci_write_config_dword(tp->pdev, TG3PCI_MEM_WIN_BASE_ADDR, 0);
}

static void tg3_disable_ints(struct tg3 *tp)
{
	tw32(TG3PCI_MISC_HOST_CTRL,
	     (tp->misc_host_ctrl | MISC_HOST_CTRL_MASK_PCI_INT));
	tw32_mailbox(MAILBOX_INTERRUPT_0 + TG3_64BIT_REG_LOW, 0x00000001);
	tr32(MAILBOX_INTERRUPT_0 + TG3_64BIT_REG_LOW);
}

static void tg3_switch_clocks(struct tg3 *tp)
{
	if (tr32(TG3PCI_CLOCK_CTRL) & CLOCK_CTRL_44MHZ_CORE) {
		tw32(TG3PCI_CLOCK_CTRL,
		     (CLOCK_CTRL_44MHZ_CORE | CLOCK_CTRL_ALTCLK));
		tr32(TG3PCI_CLOCK_CTRL);
		udelay(40);
		tw32(TG3PCI_CLOCK_CTRL,
		     (CLOCK_CTRL_ALTCLK));
		tr32(TG3PCI_CLOCK_CTRL);
		udelay(40);
	}
	tw32(TG3PCI_CLOCK_CTRL, 0);
	tr32(TG3PCI_CLOCK_CTRL);
	udelay(40);
}

#define PHY_BUSY_LOOPS	5000

static int tg3_readphy(struct tg3 *tp, int reg, uint32_t *val)
{
	uint32_t frame_val;
	int loops, ret;

	if ((tp->mi_mode & MAC_MI_MODE_AUTO_POLL) != 0) {
		tw32(MAC_MI_MODE,
		     (tp->mi_mode & ~MAC_MI_MODE_AUTO_POLL));
		tr32(MAC_MI_MODE);
		udelay(40);
	}

	*val = 0xffffffff;

	frame_val  = ((PHY_ADDR << MI_COM_PHY_ADDR_SHIFT) &
		      MI_COM_PHY_ADDR_MASK);
	frame_val |= ((reg << MI_COM_REG_ADDR_SHIFT) &
		      MI_COM_REG_ADDR_MASK);
	frame_val |= (MI_COM_CMD_READ | MI_COM_START);
	
	tw32(MAC_MI_COM, frame_val);
	tr32(MAC_MI_COM);

	loops = PHY_BUSY_LOOPS;
	while (loops-- > 0) {
		udelay(10);
		frame_val = tr32(MAC_MI_COM);

		if ((frame_val & MI_COM_BUSY) == 0) {
			udelay(5);
			frame_val = tr32(MAC_MI_COM);
			break;
		}
	}

	ret = -EBUSY;
	if (loops > 0) {
		*val = frame_val & MI_COM_DATA_MASK;
		ret = 0;
	}

	if ((tp->mi_mode & MAC_MI_MODE_AUTO_POLL) != 0) {
		tw32(MAC_MI_MODE, tp->mi_mode);
		tr32(MAC_MI_MODE);
		udelay(40);
	}

	return ret;
}

static int tg3_writephy(struct tg3 *tp, int reg, uint32_t val)
{
	uint32_t frame_val;
	int loops, ret;

	if ((tp->mi_mode & MAC_MI_MODE_AUTO_POLL) != 0) {
		tw32(MAC_MI_MODE,
		     (tp->mi_mode & ~MAC_MI_MODE_AUTO_POLL));
		tr32(MAC_MI_MODE);
		udelay(40);
	}

	frame_val  = ((PHY_ADDR << MI_COM_PHY_ADDR_SHIFT) &
		      MI_COM_PHY_ADDR_MASK);
	frame_val |= ((reg << MI_COM_REG_ADDR_SHIFT) &
		      MI_COM_REG_ADDR_MASK);
	frame_val |= (val & MI_COM_DATA_MASK);
	frame_val |= (MI_COM_CMD_WRITE | MI_COM_START);
	
	tw32(MAC_MI_COM, frame_val);
	tr32(MAC_MI_COM);

	loops = PHY_BUSY_LOOPS;
	while (loops-- > 0) {
		udelay(10);
		frame_val = tr32(MAC_MI_COM);
		if ((frame_val & MI_COM_BUSY) == 0) {
			udelay(5);
			frame_val = tr32(MAC_MI_COM);
			break;
		}
	}

	ret = -EBUSY;
	if (loops > 0)
		ret = 0;

	if ((tp->mi_mode & MAC_MI_MODE_AUTO_POLL) != 0) {
		tw32(MAC_MI_MODE, tp->mi_mode);
		tr32(MAC_MI_MODE);
		udelay(40);
	}

	return ret;
}

/* This will reset the tigon3 PHY if there is no valid
 * link.
 */
static int tg3_phy_reset(struct tg3 *tp)
{
	uint32_t phy_status, phy_control;
	int err, limit;

	err  = tg3_readphy(tp, MII_BMSR, &phy_status);
	err |= tg3_readphy(tp, MII_BMSR, &phy_status);
	if (err != 0)
		return -EBUSY;

	/* OK, reset it, and poll the BMCR_RESET bit until it
	 * clears or we time out.
	 */
	phy_control = BMCR_RESET;
	err = tg3_writephy(tp, MII_BMCR, phy_control);
	if (err != 0)
		return -EBUSY;

	limit = 5000;
	while (limit--) {
		err = tg3_readphy(tp, MII_BMCR, &phy_control);
		if (err != 0)
			return -EBUSY;

		if ((phy_control & BMCR_RESET) == 0) {
			udelay(40);
			return 0;
		}
		udelay(10);
	}

	return -EBUSY;
}

static void tg3_set_power_state_0(struct tg3 *tp)
{
	uint16_t power_control;
	int pm = tp->pm_cap;

	/* Make sure register accesses (indirect or otherwise)
	 * will function correctly.
	 */
	pci_write_config_dword(tp->pdev,  TG3PCI_MISC_HOST_CTRL, tp->misc_host_ctrl);

	pci_read_config_word(tp->pdev, pm + PCI_PM_CTRL, &power_control);

	power_control |= PCI_PM_CTRL_PME_STATUS;
	power_control &= ~(PCI_PM_CTRL_STATE_MASK);
	power_control |= 0;
	pci_write_config_word(tp->pdev, pm + PCI_PM_CTRL, power_control);

	tw32(GRC_LOCAL_CTRL, tp->grc_local_ctrl);
	tr32(GRC_LOCAL_CTRL);
	udelay(100);

	return;
}


#if 1
static void tg3_link_report(struct tg3 *tp)
{
	if (!tp->carrier_ok) {
		printf("Link is down.\n");
	} else {
		printf("Link is up at %d Mbps, %s duplex. %s %s %s\n",
			(tp->link_config.active_speed == SPEED_1000 ?
			       1000 :
			(tp->link_config.active_speed == SPEED_100 ?
				100 : 10)),
			(tp->link_config.active_duplex == DUPLEX_FULL ?  
				"full" : "half"),
			(tp->tg3_flags & TG3_FLAG_TX_PAUSE) ? "TX" : "",
			(tp->tg3_flags & TG3_FLAG_RX_PAUSE) ? "RX" : "",
			(tp->tg3_flags & (TG3_FLAG_TX_PAUSE |TG3_FLAG_RX_PAUSE)) ? "flow control" : "");
	}
}
#endif

static void tg3_setup_flow_control(struct tg3 *tp, uint32_t local_adv, uint32_t remote_adv)
{
	uint32_t new_tg3_flags = 0;

	if (local_adv & ADVERTISE_PAUSE_CAP) {
		if (local_adv & ADVERTISE_PAUSE_ASYM) {
			if (remote_adv & LPA_PAUSE_CAP)
				new_tg3_flags |=
					(TG3_FLAG_RX_PAUSE |
					 TG3_FLAG_TX_PAUSE);
			else if (remote_adv & LPA_PAUSE_ASYM)
				new_tg3_flags |=
					(TG3_FLAG_RX_PAUSE);
		} else {
			if (remote_adv & LPA_PAUSE_CAP)
				new_tg3_flags |=
					(TG3_FLAG_RX_PAUSE |
					 TG3_FLAG_TX_PAUSE);
		}
	} else if (local_adv & ADVERTISE_PAUSE_ASYM) {
		if ((remote_adv & LPA_PAUSE_CAP) &&
		    (remote_adv & LPA_PAUSE_ASYM))
			new_tg3_flags |= TG3_FLAG_TX_PAUSE;
	}

	tp->tg3_flags &= ~(TG3_FLAG_RX_PAUSE | TG3_FLAG_TX_PAUSE);
	tp->tg3_flags |= new_tg3_flags;

	if (new_tg3_flags & TG3_FLAG_RX_PAUSE)
		tp->rx_mode |= RX_MODE_FLOW_CTRL_ENABLE;
	else
		tp->rx_mode &= ~RX_MODE_FLOW_CTRL_ENABLE;

	if (new_tg3_flags & TG3_FLAG_TX_PAUSE)
		tp->tx_mode |= TX_MODE_FLOW_CTRL_ENABLE;
	else
		tp->tx_mode &= ~TX_MODE_FLOW_CTRL_ENABLE;
}

#if SUPPORT_COPPER_PHY
static void tg3_aux_stat_to_speed_duplex(
	struct tg3 *tp __unused, uint32_t val, uint16_t *speed, uint8_t *duplex)
{
	switch (val & MII_TG3_AUX_STAT_SPDMASK) {
	case MII_TG3_AUX_STAT_10HALF:
		*speed = SPEED_10;
		*duplex = DUPLEX_HALF;
		break;

	case MII_TG3_AUX_STAT_10FULL:
		*speed = SPEED_10;
		*duplex = DUPLEX_FULL;
		break;

	case MII_TG3_AUX_STAT_100HALF:
		*speed = SPEED_100;
		*duplex = DUPLEX_HALF;
		break;

	case MII_TG3_AUX_STAT_100FULL:
		*speed = SPEED_100;
		*duplex = DUPLEX_FULL;
		break;

	case MII_TG3_AUX_STAT_1000HALF:
		*speed = SPEED_1000;
		*duplex = DUPLEX_HALF;
		break;

	case MII_TG3_AUX_STAT_1000FULL:
		*speed = SPEED_1000;
		*duplex = DUPLEX_FULL;
		break;

	default:
		*speed = SPEED_INVALID;
		*duplex = DUPLEX_INVALID;
		break;
	};
}

static int tg3_phy_copper_begin(struct tg3 *tp)
{
	uint32_t new_adv;

	tp->link_config.advertising =
		(ADVERTISED_10baseT_Half | ADVERTISED_10baseT_Full |
			ADVERTISED_100baseT_Half | ADVERTISED_100baseT_Full |
			ADVERTISED_1000baseT_Half | ADVERTISED_1000baseT_Full |
			ADVERTISED_Autoneg | ADVERTISED_MII);
	
	if (tp->tg3_flags & TG3_FLAG_10_100_ONLY) {
		tp->link_config.advertising &=
			~(ADVERTISED_1000baseT_Half | ADVERTISED_1000baseT_Full);
	}
	
	new_adv = (ADVERTISE_CSMA | ADVERTISE_PAUSE_CAP);
	if (tp->link_config.advertising & ADVERTISED_10baseT_Half) {
		new_adv |= ADVERTISE_10HALF;
	}
	if (tp->link_config.advertising & ADVERTISED_10baseT_Full) {
		new_adv |= ADVERTISE_10FULL;
	}
	if (tp->link_config.advertising & ADVERTISED_100baseT_Half) {
		new_adv |= ADVERTISE_100HALF;
	}
	if (tp->link_config.advertising & ADVERTISED_100baseT_Full) {
		new_adv |= ADVERTISE_100FULL;
	}
	tg3_writephy(tp, MII_ADVERTISE, new_adv);
	
	if (tp->link_config.advertising &
		(ADVERTISED_1000baseT_Half | ADVERTISED_1000baseT_Full)) {
		new_adv = 0;
		if (tp->link_config.advertising & ADVERTISED_1000baseT_Half) {
			new_adv |= MII_TG3_CTRL_ADV_1000_HALF;
		}
		if (tp->link_config.advertising & ADVERTISED_1000baseT_Full) {
			new_adv |= MII_TG3_CTRL_ADV_1000_FULL;
		}
		if (!(tp->tg3_flags & TG3_FLAG_10_100_ONLY) &&
			(tp->pci_chip_rev_id == CHIPREV_ID_5701_A0 ||
				tp->pci_chip_rev_id == CHIPREV_ID_5701_B0)) {
			new_adv |= (MII_TG3_CTRL_AS_MASTER |
				MII_TG3_CTRL_ENABLE_AS_MASTER);
		}
		tg3_writephy(tp, MII_TG3_CTRL, new_adv);
	} else {
		tg3_writephy(tp, MII_TG3_CTRL, 0);
	}

	tg3_writephy(tp, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);

	return 0;
}

static int tg3_init_5401phy_dsp(struct tg3 *tp)
{
	int err;

	/* Turn off tap power management. */
	err  = tg3_writephy(tp, MII_TG3_AUX_CTRL, 0x0c20);

	err |= tg3_writephy(tp, MII_TG3_DSP_ADDRESS, 0x0012);
	err |= tg3_writephy(tp, MII_TG3_DSP_RW_PORT, 0x1804);

	err |= tg3_writephy(tp, MII_TG3_DSP_ADDRESS, 0x0013);
	err |= tg3_writephy(tp, MII_TG3_DSP_RW_PORT, 0x1204);

	err |= tg3_writephy(tp, MII_TG3_DSP_ADDRESS, 0x8006);
	err |= tg3_writephy(tp, MII_TG3_DSP_RW_PORT, 0x0132);

	err |= tg3_writephy(tp, MII_TG3_DSP_ADDRESS, 0x8006);
	err |= tg3_writephy(tp, MII_TG3_DSP_RW_PORT, 0x0232);

	err |= tg3_writephy(tp, MII_TG3_DSP_ADDRESS, 0x201f);
	err |= tg3_writephy(tp, MII_TG3_DSP_RW_PORT, 0x0a20);

	udelay(40);

	return err;
}

static int tg3_setup_copper_phy(struct tg3 *tp)
{
	int current_link_up;
	uint32_t bmsr, dummy;
	uint16_t current_speed;
	uint8_t current_duplex;
	int i, err;

	tw32(MAC_STATUS,
	     (MAC_STATUS_SYNC_CHANGED |
	      MAC_STATUS_CFG_CHANGED));
	tr32(MAC_STATUS);
	udelay(40);

	tp->mi_mode = MAC_MI_MODE_BASE;
	tw32(MAC_MI_MODE, tp->mi_mode);
	tr32(MAC_MI_MODE);
	udelay(40);

	tg3_writephy(tp, MII_TG3_AUX_CTRL, 0x02);

	if ((tp->phy_id & PHY_ID_MASK) == PHY_ID_BCM5401) {
		tg3_readphy(tp, MII_BMSR, &bmsr);
		tg3_readphy(tp, MII_BMSR, &bmsr);

		if (!(tp->tg3_flags & TG3_FLAG_INIT_COMPLETE))
			bmsr = 0;

		if (!(bmsr & BMSR_LSTATUS)) {
			err = tg3_init_5401phy_dsp(tp);
			if (err)
				return err;

			tg3_readphy(tp, MII_BMSR, &bmsr);
			for (i = 0; i < 1000; i++) {
				udelay(10);
				tg3_readphy(tp, MII_BMSR, &bmsr);
				if (bmsr & BMSR_LSTATUS) {
					udelay(40);
					break;
				}
			}

			if ((tp->phy_id & PHY_ID_REV_MASK) == PHY_REV_BCM5401_B0 &&
			    !(bmsr & BMSR_LSTATUS) &&
			    tp->link_config.active_speed == SPEED_1000) {
				err = tg3_phy_reset(tp);
				if (!err)
					err = tg3_init_5401phy_dsp(tp);
				if (err)
					return err;
			}
		}
	} else if (tp->pci_chip_rev_id == CHIPREV_ID_5701_A0 ||
		   tp->pci_chip_rev_id == CHIPREV_ID_5701_B0) {
		/* 5701 {A0,B0} CRC bug workaround */
		tg3_writephy(tp, 0x15, 0x0a75);
		tg3_writephy(tp, 0x1c, 0x8c68);
		tg3_writephy(tp, 0x1c, 0x8d68);
		tg3_writephy(tp, 0x1c, 0x8c68);
	}

	/* Clear pending interrupts... */
	tg3_readphy(tp, MII_TG3_ISTAT, &dummy);
	tg3_readphy(tp, MII_TG3_ISTAT, &dummy);

	if (tp->tg3_flags & TG3_FLAG_USE_MI_INTERRUPT)
		tg3_writephy(tp, MII_TG3_IMASK, ~MII_TG3_INT_LINKCHG);
	else
		tg3_writephy(tp, MII_TG3_IMASK, ~0);

	if (tp->led_mode == led_mode_three_link)
		tg3_writephy(tp, MII_TG3_EXT_CTRL,
			     MII_TG3_EXT_CTRL_LNK3_LED_MODE);
	else
		tg3_writephy(tp, MII_TG3_EXT_CTRL, 0);

	current_link_up = 0;
	current_speed = SPEED_INVALID;
	current_duplex = DUPLEX_INVALID;

	tg3_readphy(tp, MII_BMSR, &bmsr);
	tg3_readphy(tp, MII_BMSR, &bmsr);

	if (bmsr & BMSR_LSTATUS) {
		uint32_t aux_stat, bmcr;

		tg3_readphy(tp, MII_TG3_AUX_STAT, &aux_stat);
		for (i = 0; i < 2000; i++) {
			udelay(10);
			tg3_readphy(tp, MII_TG3_AUX_STAT, &aux_stat);
			if (aux_stat)
				break;
		}

		tg3_aux_stat_to_speed_duplex(tp, aux_stat,
					     &current_speed,
					     &current_duplex);
		tg3_readphy(tp, MII_BMCR, &bmcr);
		tg3_readphy(tp, MII_BMCR, &bmcr);
		if (bmcr & BMCR_ANENABLE) {
			uint32_t gig_ctrl;
			
			current_link_up = 1;
			
			/* Force autoneg restart if we are exiting
			 * low power mode.
			 */
			tg3_readphy(tp, MII_TG3_CTRL, &gig_ctrl);
			if (!(gig_ctrl & (MII_TG3_CTRL_ADV_1000_HALF |
				      MII_TG3_CTRL_ADV_1000_FULL))) {
				current_link_up = 0;
			}
		} else {
			current_link_up = 0;
		}

		tp->link_config.active_speed = current_speed;
		tp->link_config.active_duplex = current_duplex;
	}

	if (current_link_up == 1 &&
		(tp->link_config.active_duplex == DUPLEX_FULL)) {
		uint32_t local_adv, remote_adv;

		tg3_readphy(tp, MII_ADVERTISE, &local_adv);
		local_adv &= (ADVERTISE_PAUSE_CAP | ADVERTISE_PAUSE_ASYM);

		tg3_readphy(tp, MII_LPA, &remote_adv);
		remote_adv &= (LPA_PAUSE_CAP | LPA_PAUSE_ASYM);

		/* If we are not advertising full pause capability,
		 * something is wrong.  Bring the link down and reconfigure.
		 */
		if (local_adv != ADVERTISE_PAUSE_CAP) {
			current_link_up = 0;
		} else {
			tg3_setup_flow_control(tp, local_adv, remote_adv);
		}
	}

	if (current_link_up == 0) {
		uint32_t tmp;

		tg3_phy_copper_begin(tp);

		tg3_readphy(tp, MII_BMSR, &tmp);
		tg3_readphy(tp, MII_BMSR, &tmp);
		if (tmp & BMSR_LSTATUS)
			current_link_up = 1;
	}

	tp->mac_mode &= ~MAC_MODE_PORT_MODE_MASK;
	if (current_link_up == 1) {
		if (tp->link_config.active_speed == SPEED_100 ||
		    tp->link_config.active_speed == SPEED_10)
			tp->mac_mode |= MAC_MODE_PORT_MODE_MII;
		else
			tp->mac_mode |= MAC_MODE_PORT_MODE_GMII;
	} else
		tp->mac_mode |= MAC_MODE_PORT_MODE_GMII;

	tp->mac_mode &= ~MAC_MODE_HALF_DUPLEX;
	if (tp->link_config.active_duplex == DUPLEX_HALF)
		tp->mac_mode |= MAC_MODE_HALF_DUPLEX;

	tp->mac_mode &= ~MAC_MODE_LINK_POLARITY;
	if (GET_ASIC_REV(tp->pci_chip_rev_id) == ASIC_REV_5700) {
		if ((tp->led_mode == led_mode_link10) ||
		    (current_link_up == 1 &&
		     tp->link_config.active_speed == SPEED_10))
			tp->mac_mode |= MAC_MODE_LINK_POLARITY;
	} else {
		if (current_link_up == 1)
			tp->mac_mode |= MAC_MODE_LINK_POLARITY;
		tw32(MAC_LED_CTRL, LED_CTRL_PHY_MODE_1);
	}

	/* ??? Without this setting Netgear GA302T PHY does not
	 * ??? send/receive packets...
	 */
	if ((tp->phy_id & PHY_ID_MASK) == PHY_ID_BCM5411 &&
	    tp->pci_chip_rev_id == CHIPREV_ID_5700_ALTIMA) {
		tp->mi_mode |= MAC_MI_MODE_AUTO_POLL;
		tw32(MAC_MI_MODE, tp->mi_mode);
		tr32(MAC_MI_MODE);
		udelay(40);
	}

	tw32(MAC_MODE, tp->mac_mode);
	tr32(MAC_MODE);
	udelay(40);

	if (tp->tg3_flags &
	    (TG3_FLAG_USE_LINKCHG_REG |
	     TG3_FLAG_POLL_SERDES)) {
		/* Polled via timer. */
		tw32(MAC_EVENT, 0);
	} else {
		tw32(MAC_EVENT, MAC_EVENT_LNKSTATE_CHANGED);
	}
	tr32(MAC_EVENT);
	udelay(40);

	if (GET_ASIC_REV(tp->pci_chip_rev_id) == ASIC_REV_5700 &&
	    current_link_up == 1 &&
	    tp->link_config.active_speed == SPEED_1000 &&
	    ((tp->tg3_flags & TG3_FLAG_PCIX_MODE) ||
	     (tp->tg3_flags & TG3_FLAG_PCI_HIGH_SPEED))) {
		udelay(120);
		tw32(MAC_STATUS,
		     (MAC_STATUS_SYNC_CHANGED |
		      MAC_STATUS_CFG_CHANGED));
		tr32(MAC_STATUS);
		udelay(40);
		tg3_write_mem(tp,
			      NIC_SRAM_FIRMWARE_MBOX,
			      NIC_SRAM_FIRMWARE_MBOX_MAGIC2);
	}

	if (current_link_up != tp->carrier_ok) {
		tp->carrier_ok = current_link_up;
		tg3_link_report(tp);
	}

	return 0;
}
#else
#define tg3_setup_coppyer_phy(TP) (-EINVAL)
#endif /* SUPPORT_COPPER_PHY */

#if SUPPORT_FIBER_PHY
struct tg3_fiber_aneginfo {
	int state;
#define ANEG_STATE_UNKNOWN		0
#define ANEG_STATE_AN_ENABLE		1
#define ANEG_STATE_RESTART_INIT		2
#define ANEG_STATE_RESTART		3
#define ANEG_STATE_DISABLE_LINK_OK	4
#define ANEG_STATE_ABILITY_DETECT_INIT	5
#define ANEG_STATE_ABILITY_DETECT	6
#define ANEG_STATE_ACK_DETECT_INIT	7
#define ANEG_STATE_ACK_DETECT		8
#define ANEG_STATE_COMPLETE_ACK_INIT	9
#define ANEG_STATE_COMPLETE_ACK		10
#define ANEG_STATE_IDLE_DETECT_INIT	11
#define ANEG_STATE_IDLE_DETECT		12
#define ANEG_STATE_LINK_OK		13
#define ANEG_STATE_NEXT_PAGE_WAIT_INIT	14
#define ANEG_STATE_NEXT_PAGE_WAIT	15

	uint32_t flags;
#define MR_AN_ENABLE		0x00000001
#define MR_RESTART_AN		0x00000002
#define MR_AN_COMPLETE		0x00000004
#define MR_PAGE_RX		0x00000008
#define MR_NP_LOADED		0x00000010
#define MR_TOGGLE_TX		0x00000020
#define MR_LP_ADV_FULL_DUPLEX	0x00000040
#define MR_LP_ADV_HALF_DUPLEX	0x00000080
#define MR_LP_ADV_SYM_PAUSE	0x00000100
#define MR_LP_ADV_ASYM_PAUSE	0x00000200
#define MR_LP_ADV_REMOTE_FAULT1	0x00000400
#define MR_LP_ADV_REMOTE_FAULT2	0x00000800
#define MR_LP_ADV_NEXT_PAGE	0x00001000
#define MR_TOGGLE_RX		0x00002000
#define MR_NP_RX		0x00004000

#define MR_LINK_OK		0x80000000

	unsigned long link_time, cur_time;

	uint32_t ability_match_cfg;
	int ability_match_count;

	char ability_match, idle_match, ack_match;

	uint32_t txconfig, rxconfig;
#define ANEG_CFG_NP		0x00000080
#define ANEG_CFG_ACK		0x00000040
#define ANEG_CFG_RF2		0x00000020
#define ANEG_CFG_RF1		0x00000010
#define ANEG_CFG_PS2		0x00000001
#define ANEG_CFG_PS1		0x00008000
#define ANEG_CFG_HD		0x00004000
#define ANEG_CFG_FD		0x00002000
#define ANEG_CFG_INVAL		0x00001f06

};
#define ANEG_OK		0
#define ANEG_DONE	1
#define ANEG_TIMER_ENAB	2
#define ANEG_FAILED	-1

#define ANEG_STATE_SETTLE_TIME	10000

static int tg3_fiber_aneg_smachine(struct tg3 *tp,
				   struct tg3_fiber_aneginfo *ap)
{
	unsigned long delta;
	uint32_t rx_cfg_reg;
	int ret;

	if (ap->state == ANEG_STATE_UNKNOWN) {
		ap->rxconfig = 0;
		ap->link_time = 0;
		ap->cur_time = 0;
		ap->ability_match_cfg = 0;
		ap->ability_match_count = 0;
		ap->ability_match = 0;
		ap->idle_match = 0;
		ap->ack_match = 0;
	}
	ap->cur_time++;

	if (tr32(MAC_STATUS) & MAC_STATUS_RCVD_CFG) {
		rx_cfg_reg = tr32(MAC_RX_AUTO_NEG);

		if (rx_cfg_reg != ap->ability_match_cfg) {
			ap->ability_match_cfg = rx_cfg_reg;
			ap->ability_match = 0;
			ap->ability_match_count = 0;
		} else {
			if (++ap->ability_match_count > 1) {
				ap->ability_match = 1;
				ap->ability_match_cfg = rx_cfg_reg;
			}
		}
		if (rx_cfg_reg & ANEG_CFG_ACK)
			ap->ack_match = 1;
		else
			ap->ack_match = 0;

		ap->idle_match = 0;
	} else {
		ap->idle_match = 1;
		ap->ability_match_cfg = 0;
		ap->ability_match_count = 0;
		ap->ability_match = 0;
		ap->ack_match = 0;

		rx_cfg_reg = 0;
	}

	ap->rxconfig = rx_cfg_reg;
	ret = ANEG_OK;

	switch(ap->state) {
	case ANEG_STATE_UNKNOWN:
		if (ap->flags & (MR_AN_ENABLE | MR_RESTART_AN))
			ap->state = ANEG_STATE_AN_ENABLE;

		/* fallthru */
	case ANEG_STATE_AN_ENABLE:
		ap->flags &= ~(MR_AN_COMPLETE | MR_PAGE_RX);
		if (ap->flags & MR_AN_ENABLE) {
			ap->link_time = 0;
			ap->cur_time = 0;
			ap->ability_match_cfg = 0;
			ap->ability_match_count = 0;
			ap->ability_match = 0;
			ap->idle_match = 0;
			ap->ack_match = 0;

			ap->state = ANEG_STATE_RESTART_INIT;
		} else {
			ap->state = ANEG_STATE_DISABLE_LINK_OK;
		}
		break;

	case ANEG_STATE_RESTART_INIT:
		ap->link_time = ap->cur_time;
		ap->flags &= ~(MR_NP_LOADED);
		ap->txconfig = 0;
		tw32(MAC_TX_AUTO_NEG, 0);
		tp->mac_mode |= MAC_MODE_SEND_CONFIGS;
		tw32(MAC_MODE, tp->mac_mode);
		tr32(MAC_MODE);
		udelay(40);

		ret = ANEG_TIMER_ENAB;
		ap->state = ANEG_STATE_RESTART;

		/* fallthru */
	case ANEG_STATE_RESTART:
		delta = ap->cur_time - ap->link_time;
		if (delta > ANEG_STATE_SETTLE_TIME) {
			ap->state = ANEG_STATE_ABILITY_DETECT_INIT;
		} else {
			ret = ANEG_TIMER_ENAB;
		}
		break;

	case ANEG_STATE_DISABLE_LINK_OK:
		ret = ANEG_DONE;
		break;

	case ANEG_STATE_ABILITY_DETECT_INIT:
		ap->flags &= ~(MR_TOGGLE_TX);
		ap->txconfig = (ANEG_CFG_FD | ANEG_CFG_PS1);
		tw32(MAC_TX_AUTO_NEG, ap->txconfig);
		tp->mac_mode |= MAC_MODE_SEND_CONFIGS;
		tw32(MAC_MODE, tp->mac_mode);
		tr32(MAC_MODE);
		udelay(40);

		ap->state = ANEG_STATE_ABILITY_DETECT;
		break;

	case ANEG_STATE_ABILITY_DETECT:
		if (ap->ability_match != 0 && ap->rxconfig != 0) {
			ap->state = ANEG_STATE_ACK_DETECT_INIT;
		}
		break;

	case ANEG_STATE_ACK_DETECT_INIT:
		ap->txconfig |= ANEG_CFG_ACK;
		tw32(MAC_TX_AUTO_NEG, ap->txconfig);
		tp->mac_mode |= MAC_MODE_SEND_CONFIGS;
		tw32(MAC_MODE, tp->mac_mode);
		tr32(MAC_MODE);
		udelay(40);

		ap->state = ANEG_STATE_ACK_DETECT;

		/* fallthru */
	case ANEG_STATE_ACK_DETECT:
		if (ap->ack_match != 0) {
			if ((ap->rxconfig & ~ANEG_CFG_ACK) ==
			    (ap->ability_match_cfg & ~ANEG_CFG_ACK)) {
				ap->state = ANEG_STATE_COMPLETE_ACK_INIT;
			} else {
				ap->state = ANEG_STATE_AN_ENABLE;
			}
		} else if (ap->ability_match != 0 &&
			   ap->rxconfig == 0) {
			ap->state = ANEG_STATE_AN_ENABLE;
		}
		break;

	case ANEG_STATE_COMPLETE_ACK_INIT:
		if (ap->rxconfig & ANEG_CFG_INVAL) {
			ret = ANEG_FAILED;
			break;
		}
		ap->flags &= ~(MR_LP_ADV_FULL_DUPLEX |
			       MR_LP_ADV_HALF_DUPLEX |
			       MR_LP_ADV_SYM_PAUSE |
			       MR_LP_ADV_ASYM_PAUSE |
			       MR_LP_ADV_REMOTE_FAULT1 |
			       MR_LP_ADV_REMOTE_FAULT2 |
			       MR_LP_ADV_NEXT_PAGE |
			       MR_TOGGLE_RX |
			       MR_NP_RX);
		if (ap->rxconfig & ANEG_CFG_FD)
			ap->flags |= MR_LP_ADV_FULL_DUPLEX;
		if (ap->rxconfig & ANEG_CFG_HD)
			ap->flags |= MR_LP_ADV_HALF_DUPLEX;
		if (ap->rxconfig & ANEG_CFG_PS1)
			ap->flags |= MR_LP_ADV_SYM_PAUSE;
		if (ap->rxconfig & ANEG_CFG_PS2)
			ap->flags |= MR_LP_ADV_ASYM_PAUSE;
		if (ap->rxconfig & ANEG_CFG_RF1)
			ap->flags |= MR_LP_ADV_REMOTE_FAULT1;
		if (ap->rxconfig & ANEG_CFG_RF2)
			ap->flags |= MR_LP_ADV_REMOTE_FAULT2;
		if (ap->rxconfig & ANEG_CFG_NP)
			ap->flags |= MR_LP_ADV_NEXT_PAGE;

		ap->link_time = ap->cur_time;

		ap->flags ^= (MR_TOGGLE_TX);
		if (ap->rxconfig & 0x0008)
			ap->flags |= MR_TOGGLE_RX;
		if (ap->rxconfig & ANEG_CFG_NP)
			ap->flags |= MR_NP_RX;
		ap->flags |= MR_PAGE_RX;

		ap->state = ANEG_STATE_COMPLETE_ACK;
		ret = ANEG_TIMER_ENAB;
		break;

	case ANEG_STATE_COMPLETE_ACK:
		if (ap->ability_match != 0 &&
		    ap->rxconfig == 0) {
			ap->state = ANEG_STATE_AN_ENABLE;
			break;
		}
		delta = ap->cur_time - ap->link_time;
		if (delta > ANEG_STATE_SETTLE_TIME) {
			if (!(ap->flags & (MR_LP_ADV_NEXT_PAGE))) {
				ap->state = ANEG_STATE_IDLE_DETECT_INIT;
			} else {
				if ((ap->txconfig & ANEG_CFG_NP) == 0 &&
				    !(ap->flags & MR_NP_RX)) {
					ap->state = ANEG_STATE_IDLE_DETECT_INIT;
				} else {
					ret = ANEG_FAILED;
				}
			}
		}
		break;

	case ANEG_STATE_IDLE_DETECT_INIT:
		ap->link_time = ap->cur_time;
		tp->mac_mode &= ~MAC_MODE_SEND_CONFIGS;
		tw32(MAC_MODE, tp->mac_mode);
		tr32(MAC_MODE);
		udelay(40);

		ap->state = ANEG_STATE_IDLE_DETECT;
		ret = ANEG_TIMER_ENAB;
		break;

	case ANEG_STATE_IDLE_DETECT:
		if (ap->ability_match != 0 &&
		    ap->rxconfig == 0) {
			ap->state = ANEG_STATE_AN_ENABLE;
			break;
		}
		delta = ap->cur_time - ap->link_time;
		if (delta > ANEG_STATE_SETTLE_TIME) {
			/* XXX another gem from the Broadcom driver :( */
			ap->state = ANEG_STATE_LINK_OK;
		}
		break;

	case ANEG_STATE_LINK_OK:
		ap->flags |= (MR_AN_COMPLETE | MR_LINK_OK);
		ret = ANEG_DONE;
		break;

	case ANEG_STATE_NEXT_PAGE_WAIT_INIT:
		/* ??? unimplemented */
		break;

	case ANEG_STATE_NEXT_PAGE_WAIT:
		/* ??? unimplemented */
		break;

	default:
		ret = ANEG_FAILED;
		break;
	};

	return ret;
}

static int tg3_setup_fiber_phy(struct tg3 *tp)
{
	uint32_t orig_pause_cfg;
	uint16_t orig_active_speed;
	uint8_t orig_active_duplex;
	int current_link_up;
	int i;

	orig_pause_cfg =
		(tp->tg3_flags & (TG3_FLAG_RX_PAUSE |
				  TG3_FLAG_TX_PAUSE));
	orig_active_speed = tp->link_config.active_speed;
	orig_active_duplex = tp->link_config.active_duplex;

	tp->mac_mode &= ~(MAC_MODE_PORT_MODE_MASK | MAC_MODE_HALF_DUPLEX);
	tp->mac_mode |= MAC_MODE_PORT_MODE_TBI;
	tw32(MAC_MODE, tp->mac_mode);
	tr32(MAC_MODE);
	udelay(40);

	/* Reset when initting first time or we have a link. */
	if (!(tp->tg3_flags & TG3_FLAG_INIT_COMPLETE) ||
	    (tr32(MAC_STATUS) & MAC_STATUS_PCS_SYNCED)) {
		/* Set PLL lock range. */
		tg3_writephy(tp, 0x16, 0x8007);

		/* SW reset */
		tg3_writephy(tp, MII_BMCR, BMCR_RESET);

		/* Wait for reset to complete. */
		/* XXX schedule_timeout() ... */
		for (i = 0; i < 500; i++)
			udelay(10);

		/* Config mode; select PMA/Ch 1 regs. */
		tg3_writephy(tp, 0x10, 0x8411);

		/* Enable auto-lock and comdet, select txclk for tx. */
		tg3_writephy(tp, 0x11, 0x0a10);

		tg3_writephy(tp, 0x18, 0x00a0);
		tg3_writephy(tp, 0x16, 0x41ff);

		/* Assert and deassert POR. */
		tg3_writephy(tp, 0x13, 0x0400);
		udelay(40);
		tg3_writephy(tp, 0x13, 0x0000);

		tg3_writephy(tp, 0x11, 0x0a50);
		udelay(40);
		tg3_writephy(tp, 0x11, 0x0a10);

		/* Wait for signal to stabilize */
		mdelay(150);

		/* Deselect the channel register so we can read the PHYID
		 * later.
		 */
		tg3_writephy(tp, 0x10, 0x8011);
	}

	/* Enable link change interrupt unless serdes polling.  */
	if (!(tp->tg3_flags & TG3_FLAG_POLL_SERDES))
		tw32(MAC_EVENT, MAC_EVENT_LNKSTATE_CHANGED);
	else
		tw32(MAC_EVENT, 0);
	tr32(MAC_EVENT);
	udelay(40);

	current_link_up = 0;
	if (tr32(MAC_STATUS) & MAC_STATUS_PCS_SYNCED) {
		if (!(tp->tg3_flags & TG3_FLAG_GOT_SERDES_FLOWCTL)) {
			struct tg3_fiber_aneginfo aninfo;
			int status = ANEG_FAILED;
			unsigned int tick;
			uint32_t tmp;

			memset(&aninfo, 0, sizeof(aninfo));
			aninfo.flags |= (MR_AN_ENABLE);

			tw32(MAC_TX_AUTO_NEG, 0);

			tmp = tp->mac_mode & ~MAC_MODE_PORT_MODE_MASK;
			tw32(MAC_MODE, tmp | MAC_MODE_PORT_MODE_GMII);
			tr32(MAC_MODE);
			udelay(40);

			tw32(MAC_MODE, tp->mac_mode | MAC_MODE_SEND_CONFIGS);
			tr32(MAC_MODE);
			udelay(40);

			aninfo.state = ANEG_STATE_UNKNOWN;
			aninfo.cur_time = 0;
			tick = 0;
			while (++tick < 195000) {
				status = tg3_fiber_aneg_smachine(tp, &aninfo);
				if (status == ANEG_DONE ||
				    status == ANEG_FAILED)
					break;

				udelay(1);
			}

			tp->mac_mode &= ~MAC_MODE_SEND_CONFIGS;
			tw32(MAC_MODE, tp->mac_mode);
			tr32(MAC_MODE);
			udelay(40);

			if (status == ANEG_DONE &&
			    (aninfo.flags &
			     (MR_AN_COMPLETE | MR_LINK_OK |
			      MR_LP_ADV_FULL_DUPLEX))) {
				uint32_t local_adv, remote_adv;

				local_adv = ADVERTISE_PAUSE_CAP;
				remote_adv = 0;
				if (aninfo.flags & MR_LP_ADV_SYM_PAUSE)
					remote_adv |= LPA_PAUSE_CAP;
				if (aninfo.flags & MR_LP_ADV_ASYM_PAUSE)
					remote_adv |= LPA_PAUSE_ASYM;

				tg3_setup_flow_control(tp, local_adv, remote_adv);

				tp->tg3_flags |=
					TG3_FLAG_GOT_SERDES_FLOWCTL;
				current_link_up = 1;
			}
			for (i = 0; i < 60; i++) {
				udelay(20);
				tw32(MAC_STATUS,
				     (MAC_STATUS_SYNC_CHANGED |
				      MAC_STATUS_CFG_CHANGED));
				tr32(MAC_STATUS);
				udelay(40);
				if ((tr32(MAC_STATUS) &
				     (MAC_STATUS_SYNC_CHANGED |
				      MAC_STATUS_CFG_CHANGED)) == 0)
					break;
			}
			if (current_link_up == 0 &&
			    (tr32(MAC_STATUS) & MAC_STATUS_PCS_SYNCED)) {
				current_link_up = 1;
			}
		} else {
			/* Forcing 1000FD link up. */
			current_link_up = 1;
		}
	}

	tp->mac_mode &= ~MAC_MODE_LINK_POLARITY;
	tw32(MAC_MODE, tp->mac_mode);
	tr32(MAC_MODE);
	udelay(40);

	tp->hw_status->status =
		(SD_STATUS_UPDATED |
		 (tp->hw_status->status & ~SD_STATUS_LINK_CHG));

	for (i = 0; i < 100; i++) {
		udelay(20);
		tw32(MAC_STATUS,
		     (MAC_STATUS_SYNC_CHANGED |
		      MAC_STATUS_CFG_CHANGED));
		tr32(MAC_STATUS);
		udelay(40);
		if ((tr32(MAC_STATUS) &
		     (MAC_STATUS_SYNC_CHANGED |
		      MAC_STATUS_CFG_CHANGED)) == 0)
			break;
	}

	if ((tr32(MAC_STATUS) & MAC_STATUS_PCS_SYNCED) == 0)
		current_link_up = 0;

	if (current_link_up == 1) {
		tp->link_config.active_speed = SPEED_1000;
		tp->link_config.active_duplex = DUPLEX_FULL;
	} else {
		tp->link_config.active_speed = SPEED_INVALID;
		tp->link_config.active_duplex = DUPLEX_INVALID;
	}

	if (current_link_up != tp->carrier_ok) {
		tp->carrier_ok = current_link_up;
		tg3_link_report(tp);
	} else {
		uint32_t now_pause_cfg =
			tp->tg3_flags & (TG3_FLAG_RX_PAUSE |
					 TG3_FLAG_TX_PAUSE);
		if (orig_pause_cfg != now_pause_cfg ||
		    orig_active_speed != tp->link_config.active_speed ||
		    orig_active_duplex != tp->link_config.active_duplex)
			tg3_link_report(tp);
	}

	if ((tr32(MAC_STATUS) & MAC_STATUS_PCS_SYNCED) == 0) {
		tw32(MAC_MODE, tp->mac_mode | MAC_MODE_LINK_POLARITY);
		tr32(MAC_MODE);
		udelay(40);
		if (tp->tg3_flags & TG3_FLAG_INIT_COMPLETE) {
			tw32(MAC_MODE, tp->mac_mode);
			tr32(MAC_MODE);
			udelay(40);
		}
	}

	return 0;
}
#else
#define tg3_setup_fiber_phy(TP) (-EINVAL)
#endif /* SUPPORT_FIBER_PHY */

static int tg3_setup_phy(struct tg3 *tp)
{
	int err;

	if (tp->phy_id == PHY_ID_SERDES) {
		err = tg3_setup_fiber_phy(tp);
	} else {
		err = tg3_setup_copper_phy(tp);
	}

	if (tp->link_config.active_speed == SPEED_1000 &&
	    tp->link_config.active_duplex == DUPLEX_HALF)
		tw32(MAC_TX_LENGTHS,
		     ((2 << TX_LENGTHS_IPG_CRS_SHIFT) |
		      (6 << TX_LENGTHS_IPG_SHIFT) |
		      (0xff << TX_LENGTHS_SLOT_TIME_SHIFT)));
	else
		tw32(MAC_TX_LENGTHS,
		     ((2 << TX_LENGTHS_IPG_CRS_SHIFT) |
		      (6 << TX_LENGTHS_IPG_SHIFT) |
		      (32 << TX_LENGTHS_SLOT_TIME_SHIFT)));

	return err;
}


/* tg3_free_rings is currently a noop */

/* Initialize tx/rx rings for packet processing.
 *
 * The chip has been shut down and the driver detached from
 * the networking, so no interrupts or new tx packets will
 * end up in the driver.  tp->{tx,}lock is not held and we are not
 * in an interrupt context and thus may sleep.
 */
static void tg3_init_rings(struct tg3 *tp)
{
	static unsigned char buf[TG3_DEF_RX_RING_PENDING][MAX_RX_PKT_BUF_SZ];
	uint32_t i;

	/* Zero out all descriptors. */
	memset(tp->rx_std, 0, TG3_RX_RING_BYTES);
	memset(tp->rx_rcb, 0, TG3_RX_RCB_RING_BYTES);

	memset(tp->tx_ring, 0, TG3_TX_RING_BYTES);

	/* Initialize invariants of the rings, we only set this
	 * stuff once.  This works because the card does not
	 * write into the rx buffer posting rings.
	 */
	for (i = 0; i < TG3_RX_RING_SIZE; i++) {
		struct tg3_rx_buffer_desc *rxd;

		rxd = &tp->rx_std[i];
		rxd->idx_len = (RX_PKT_BUF_SZ - tp->rx_offset - 64)
			<< RXD_LEN_SHIFT;
		rxd->type_flags = (RXD_FLAG_END << RXD_FLAGS_SHIFT);
		rxd->opaque = (RXD_OPAQUE_RING_STD |
			       (i << RXD_OPAQUE_INDEX_SHIFT));

		/* Note where the receive buffer for the ring is placed */
		rxd->addr_hi = 0;
		rxd->addr_lo = virt_to_bus(&buf[i%TG3_DEF_RX_RING_PENDING]);
	}
}

/*
 * Must not be invoked with interrupt sources disabled and
 * the hardware shutdown down.  Can sleep.
 */
static int tg3_alloc_consistent(struct tg3 *tp)
{
	static struct tg3_rx_buffer_desc rx_std[TG3_RX_RING_SIZE];
	static struct tg3_rx_buffer_desc rx_rcb[TG3_RX_RCB_RING_SIZE];
	static struct tg3_tx_buffer_desc tx_ring[TG3_TX_RING_SIZE];
	static char buffer6[TG3_HW_STATUS_SIZE];
	static char buffer7[sizeof(struct tg3_hw_stats)];

	tp->rx_std = rx_std;
	tp->rx_std_mapping = virt_to_bus(tp->rx_std);
	tp->rx_rcb = rx_rcb;
	tp->rx_rcb_mapping = virt_to_bus(tp->rx_rcb);


	tp->tx_ring = tx_ring;
	tp->tx_desc_mapping = virt_to_bus(tp->tx_ring);

	tp->hw_status = (void *)&buffer6;
	tp->status_mapping = virt_to_bus(tp->hw_status);

	tp->hw_stats = (void *)&buffer7;
	tp->stats_mapping = virt_to_bus(tp->hw_stats);

	memset(tp->hw_status, 0, TG3_HW_STATUS_SIZE);
	memset(tp->hw_stats, 0, sizeof(struct tg3_hw_stats));

	return 0;

}

#define MAX_WAIT_CNT 1000

/* To stop a block, clear the enable bit and poll till it
 * clears.  tp->lock is held.
 */
static int tg3_stop_block(struct tg3 *tp, unsigned long ofs, uint32_t enable_bit)
{
	unsigned int i;
	uint32_t val;

	val = tr32(ofs);
	val &= ~enable_bit;
	tw32(ofs, val);
	tr32(ofs);

	for (i = 0; i < MAX_WAIT_CNT; i++) {
		udelay(100);
		val = tr32(ofs);
		if ((val & enable_bit) == 0)
			break;
	}

	if (i == MAX_WAIT_CNT) {
		printf("tg3_stop_block timed out, "
		       "ofs=%lx enable_bit=%x\n",
		       ofs, enable_bit);
		return -ENODEV;
	}

	return 0;
}

/* tp->lock is held. */
static int tg3_abort_hw(struct tg3 *tp)
{
	int i, err;

	tg3_disable_ints(tp);

	tp->rx_mode &= ~RX_MODE_ENABLE;
	tw32(MAC_RX_MODE, tp->rx_mode);
	tr32(MAC_RX_MODE);
	udelay(10);

	err  = tg3_stop_block(tp, RCVBDI_MODE,   RCVBDI_MODE_ENABLE);
	err |= tg3_stop_block(tp, RCVLPC_MODE,   RCVLPC_MODE_ENABLE);
	err |= tg3_stop_block(tp, RCVLSC_MODE,   RCVLSC_MODE_ENABLE);
	err |= tg3_stop_block(tp, RCVDBDI_MODE,  RCVDBDI_MODE_ENABLE);
	err |= tg3_stop_block(tp, RCVDCC_MODE,   RCVDCC_MODE_ENABLE);
	err |= tg3_stop_block(tp, RCVCC_MODE,    RCVCC_MODE_ENABLE);

	err |= tg3_stop_block(tp, SNDBDS_MODE,   SNDBDS_MODE_ENABLE);
	err |= tg3_stop_block(tp, SNDBDI_MODE,   SNDBDI_MODE_ENABLE);
	err |= tg3_stop_block(tp, SNDDATAI_MODE, SNDDATAI_MODE_ENABLE);
	err |= tg3_stop_block(tp, RDMAC_MODE,    RDMAC_MODE_ENABLE);
	err |= tg3_stop_block(tp, SNDDATAC_MODE, SNDDATAC_MODE_ENABLE);
	err |= tg3_stop_block(tp, SNDBDC_MODE,   SNDBDC_MODE_ENABLE);
	if (err)
		goto out;

	tp->mac_mode &= ~MAC_MODE_TDE_ENABLE;
	tw32(MAC_MODE, tp->mac_mode);
	tr32(MAC_MODE);
	udelay(40);

	tp->tx_mode &= ~TX_MODE_ENABLE;
	tw32(MAC_TX_MODE, tp->tx_mode);
	tr32(MAC_TX_MODE);

	for (i = 0; i < MAX_WAIT_CNT; i++) {
		udelay(100);
		if (!(tr32(MAC_TX_MODE) & TX_MODE_ENABLE))
			break;
	}
	if (i >= MAX_WAIT_CNT) {
		printf("tg3_abort_hw timed out TX_MODE_ENABLE will not clear MAC_TX_MODE=%x\n",
			tr32(MAC_TX_MODE));
		return -ENODEV;
	}

	err  = tg3_stop_block(tp, HOSTCC_MODE, HOSTCC_MODE_ENABLE);
	err |= tg3_stop_block(tp, WDMAC_MODE,  WDMAC_MODE_ENABLE);
	err |= tg3_stop_block(tp, MBFREE_MODE, MBFREE_MODE_ENABLE);

	tw32(FTQ_RESET, 0xffffffff);
	tw32(FTQ_RESET, 0x00000000);

	err |= tg3_stop_block(tp, BUFMGR_MODE, BUFMGR_MODE_ENABLE);
	err |= tg3_stop_block(tp, MEMARB_MODE, MEMARB_MODE_ENABLE);
	if (err)
		goto out;

	memset(tp->hw_status, 0, TG3_HW_STATUS_SIZE);

out:
	return err;
}

/* tp->lock is held. */
static void tg3_chip_reset(struct tg3 *tp)
{
	uint32_t val;


	/* Force NVRAM to settle.
	 * This deals with a chip bug which can result in EEPROM
	 * corruption.
	 */
	if (tp->tg3_flags & TG3_FLAG_NVRAM) {
		int i;

		tw32(NVRAM_SWARB, SWARB_REQ_SET1);
		for (i = 0; i < 100000; i++) {
			if (tr32(NVRAM_SWARB) & SWARB_GNT1)
				break;
			udelay(10);
		}
	}
	tw32(GRC_MISC_CFG, GRC_MISC_CFG_CORECLK_RESET);

	/* Flush PCI posted writes.  The normal MMIO registers
	 * are inaccessible at this time so this is the only
	 * way to make this reliably.  I tried to use indirect
	 * register read/write but this upset some 5701 variants.
	 */
	pci_read_config_dword(tp->pdev, PCI_COMMAND, &val);

	udelay(120);

	/* Re-enable indirect register accesses. */
	pci_write_config_dword(tp->pdev, TG3PCI_MISC_HOST_CTRL,
			       tp->misc_host_ctrl);

	/* Set MAX PCI retry to zero. */
	val = (PCISTATE_ROM_ENABLE | PCISTATE_ROM_RETRY_ENABLE);
	if (tp->pci_chip_rev_id == CHIPREV_ID_5704_A0 &&
	    (tp->tg3_flags & TG3_FLAG_PCIX_MODE))
		val |= PCISTATE_RETRY_SAME_DMA;
	pci_write_config_dword(tp->pdev, TG3PCI_PCISTATE, val);

	pci_restore_state(tp->pdev, tp->pci_cfg_state);

	/* Make sure PCI-X relaxed ordering bit is clear. */
	pci_read_config_dword(tp->pdev, TG3PCI_X_CAPS, &val);
	val &= ~PCIX_CAPS_RELAXED_ORDERING;
	pci_write_config_dword(tp->pdev, TG3PCI_X_CAPS, val);

	tw32(MEMARB_MODE, MEMARB_MODE_ENABLE);

	tw32(TG3PCI_MISC_HOST_CTRL, tp->misc_host_ctrl);
}

/* tp->lock is held. */
static void tg3_stop_fw(struct tg3 *tp)
{
	if (tp->tg3_flags & TG3_FLAG_ENABLE_ASF) {
		uint32_t val;
		int i;

		tg3_write_mem(tp, NIC_SRAM_FW_CMD_MBOX, FWCMD_NICDRV_PAUSE_FW);
		val = tr32(GRC_RX_CPU_EVENT);
		val |= (1 << 14);
		tw32(GRC_RX_CPU_EVENT, val);

		/* Wait for RX cpu to ACK the event.  */
		for (i = 0; i < 100; i++) {
			if (!(tr32(GRC_RX_CPU_EVENT) & (1 << 14)))
				break;
			udelay(1);
		}
	}
}

/* tp->lock is held. */
static int tg3_halt(struct tg3 *tp)
{
	uint32_t val;
	int i;

	tg3_stop_fw(tp);
	tg3_abort_hw(tp);
	tg3_chip_reset(tp);
	tg3_write_mem(tp,
		      NIC_SRAM_FIRMWARE_MBOX,
		      NIC_SRAM_FIRMWARE_MBOX_MAGIC1);
	for (i = 0; i < 100000; i++) {
		tg3_read_mem(tp, NIC_SRAM_FIRMWARE_MBOX, &val);
		if (val == ~NIC_SRAM_FIRMWARE_MBOX_MAGIC1)
			break;
		udelay(10);
	}

	if (i >= 100000) {
		printf("tg3_halt timed out firmware will not restart magic=%x\n",
			val);
		return -ENODEV;
	}

	if (tp->tg3_flags & TG3_FLAG_ENABLE_ASF) {
		tg3_write_mem(tp, NIC_SRAM_FW_DRV_STATE_MBOX,
			DRV_STATE_UNLOAD);
	} else
		tg3_write_mem(tp, NIC_SRAM_FW_DRV_STATE_MBOX,
			      DRV_STATE_SUSPEND);

	return 0;
}

/* tp->lock is held. */
static void __tg3_set_mac_addr(struct tg3 *tp)
{
	uint32_t addr_high, addr_low;
	int i;

	addr_high = ((tp->nic->node_addr[0] << 8) |
		     tp->nic->node_addr[1]);
	addr_low = ((tp->nic->node_addr[2] << 24) |
		    (tp->nic->node_addr[3] << 16) |
		    (tp->nic->node_addr[4] <<  8) |
		    (tp->nic->node_addr[5] <<  0));
	for (i = 0; i < 4; i++) {
		tw32(MAC_ADDR_0_HIGH + (i * 8), addr_high);
		tw32(MAC_ADDR_0_LOW + (i * 8), addr_low);
	}

	addr_high = (tp->nic->node_addr[0] +
		     tp->nic->node_addr[1] +
		     tp->nic->node_addr[2] +
		     tp->nic->node_addr[3] +
		     tp->nic->node_addr[4] +
		     tp->nic->node_addr[5]) &
		TX_BACKOFF_SEED_MASK;
	tw32(MAC_TX_BACKOFF_SEED, addr_high);
}

/* tp->lock is held. */
static void tg3_set_bdinfo(struct tg3 *tp, uint32_t bdinfo_addr,
			   dma_addr_t mapping, uint32_t maxlen_flags,
			   uint32_t nic_addr)
{
	tg3_write_mem(tp,
		      (bdinfo_addr +
		       TG3_BDINFO_HOST_ADDR +
		       TG3_64BIT_REG_HIGH),
		      ((uint64_t) mapping >> 32));
	tg3_write_mem(tp,
		      (bdinfo_addr +
		       TG3_BDINFO_HOST_ADDR +
		       TG3_64BIT_REG_LOW),
		      ((uint64_t) mapping & 0xffffffff));
	tg3_write_mem(tp,
		      (bdinfo_addr +
		       TG3_BDINFO_MAXLEN_FLAGS),
		       maxlen_flags);
	tg3_write_mem(tp,
		      (bdinfo_addr +
		       TG3_BDINFO_NIC_ADDR),
		      nic_addr);
}

static void __tg3_set_rx_mode(struct tg3 *tp);

/* tp->lock is held. */
static int tg3_reset_hw(struct tg3 *tp)
{
	uint32_t val;
	int i, err;

	tg3_disable_ints(tp);

	tg3_stop_fw(tp);

	if (tp->tg3_flags & TG3_FLAG_INIT_COMPLETE) {
		err = tg3_abort_hw(tp);
		if (err)
			return err;
	}

	tg3_chip_reset(tp);

	tw32(GRC_MODE, tp->grc_mode);
	tg3_write_mem(tp, NIC_SRAM_FIRMWARE_MBOX,
		      NIC_SRAM_FIRMWARE_MBOX_MAGIC1);
	if (tp->phy_id == PHY_ID_SERDES) {
		tp->mac_mode = MAC_MODE_PORT_MODE_TBI;
		tw32(MAC_MODE, tp->mac_mode);
	} else
		tw32(MAC_MODE, 0);
	tr32(MAC_MODE);
	udelay(40);

	/* Wait for firmware initialization to complete. */
	for (i = 0; i < 100000; i++) {
		tg3_read_mem(tp, NIC_SRAM_FIRMWARE_MBOX, &val);
		if (val == ~NIC_SRAM_FIRMWARE_MBOX_MAGIC1)
			break;
		udelay(10);
	}
	if (i >= 100000) {
		printf("tg3_reset_hw timed out firmware will not restart magic=%x\n",
			val);
		return -ENODEV;
	}

	if (tp->tg3_flags & TG3_FLAG_ENABLE_ASF)
		tg3_write_mem(tp, NIC_SRAM_FW_DRV_STATE_MBOX,
			      DRV_STATE_START);
	else
		tg3_write_mem(tp, NIC_SRAM_FW_DRV_STATE_MBOX,
			      DRV_STATE_SUSPEND);

	/* This works around an issue with Athlon chipsets on
	 * B3 tigon3 silicon.  This bit has no effect on any
	 * other revision.
	 */
	val = tr32(TG3PCI_CLOCK_CTRL);
	val |= CLOCK_CTRL_DELAY_PCI_GRANT;
	tw32(TG3PCI_CLOCK_CTRL, val);
	tr32(TG3PCI_CLOCK_CTRL);

	if (tp->pci_chip_rev_id == CHIPREV_ID_5704_A0 &&
	    (tp->tg3_flags & TG3_FLAG_PCIX_MODE)) {
		val = tr32(TG3PCI_PCISTATE);
		val |= PCISTATE_RETRY_SAME_DMA;
		tw32(TG3PCI_PCISTATE, val);
	}

	/* Clear statistics/status block in chip, and status block in ram. */
	for (i = NIC_SRAM_STATS_BLK;
	     i < NIC_SRAM_STATUS_BLK + TG3_HW_STATUS_SIZE;
	     i += sizeof(uint32_t)) {
		tg3_write_mem(tp, i, 0);
		udelay(40);
	}
	memset(tp->hw_status, 0, TG3_HW_STATUS_SIZE);

	/* This value is determined during the probe time DMA
	 * engine test, tg3_test_dma.
	 */
	tw32(TG3PCI_DMA_RW_CTRL, tp->dma_rwctrl);

	tp->grc_mode &= ~(GRC_MODE_HOST_SENDBDS |
			  GRC_MODE_4X_NIC_SEND_RINGS |
			  GRC_MODE_NO_TX_PHDR_CSUM |
			  GRC_MODE_NO_RX_PHDR_CSUM);
	tp->grc_mode |= GRC_MODE_HOST_SENDBDS;
	tp->grc_mode |= GRC_MODE_NO_TX_PHDR_CSUM;
	tp->grc_mode |= GRC_MODE_NO_RX_PHDR_CSUM;

	tw32(GRC_MODE,
	     tp->grc_mode |
	     (GRC_MODE_IRQ_ON_MAC_ATTN | GRC_MODE_HOST_STACKUP));

	/* Setup the timer prescalar register.  Clock is always 66Mhz. */
	tw32(GRC_MISC_CFG,
	     (65 << GRC_MISC_CFG_PRESCALAR_SHIFT));

	/* Initialize MBUF/DESC pool. */
	tw32(BUFMGR_MB_POOL_ADDR, NIC_SRAM_MBUF_POOL_BASE);
	if (GET_ASIC_REV(tp->pci_chip_rev_id) == ASIC_REV_5704)
		tw32(BUFMGR_MB_POOL_SIZE, NIC_SRAM_MBUF_POOL_SIZE64);
	else
		tw32(BUFMGR_MB_POOL_SIZE, NIC_SRAM_MBUF_POOL_SIZE96);
	tw32(BUFMGR_DMA_DESC_POOL_ADDR, NIC_SRAM_DMA_DESC_POOL_BASE);
	tw32(BUFMGR_DMA_DESC_POOL_SIZE, NIC_SRAM_DMA_DESC_POOL_SIZE);

	tw32(BUFMGR_MB_RDMA_LOW_WATER,
		tp->bufmgr_config.mbuf_read_dma_low_water);
	tw32(BUFMGR_MB_MACRX_LOW_WATER,
		tp->bufmgr_config.mbuf_mac_rx_low_water);
	tw32(BUFMGR_MB_HIGH_WATER,
		tp->bufmgr_config.mbuf_high_water);

	tw32(BUFMGR_DMA_LOW_WATER,
	     tp->bufmgr_config.dma_low_water);
	tw32(BUFMGR_DMA_HIGH_WATER,
	     tp->bufmgr_config.dma_high_water);

	tw32(BUFMGR_MODE, BUFMGR_MODE_ENABLE | BUFMGR_MODE_ATTN_ENABLE);
	for (i = 0; i < 2000; i++) {
		if (tr32(BUFMGR_MODE) & BUFMGR_MODE_ENABLE)
			break;
		udelay(10);
	}
	if (i >= 2000) {
		printf("tg3_reset_hw cannot enable BUFMGR\n");
		return -ENODEV;
	}

	tw32(FTQ_RESET, 0xffffffff);
	tw32(FTQ_RESET, 0x00000000);
	for (i = 0; i < 2000; i++) {
		if (tr32(FTQ_RESET) == 0x00000000)
			break;
		udelay(10);
	}
	if (i >= 2000) {
		printf("tg3_reset_hw cannot reset FTQ\n");
		return -ENODEV;
	}

	/* Initialize TG3_BDINFO's at:
	 *  RCVDBDI_STD_BD:	standard eth size rx ring
	 *  RCVDBDI_JUMBO_BD:	jumbo frame rx ring
	 *  RCVDBDI_MINI_BD:	small frame rx ring (??? does not work)
	 *
	 * like so:
	 *  TG3_BDINFO_HOST_ADDR:	high/low parts of DMA address of ring
	 *  TG3_BDINFO_MAXLEN_FLAGS:	(rx max buffer size << 16) |
	 *                              ring attribute flags
	 *  TG3_BDINFO_NIC_ADDR:	location of descriptors in nic SRAM
	 *
	 * Standard receive ring @ NIC_SRAM_RX_BUFFER_DESC, 512 entries.
	 * Jumbo receive ring @ NIC_SRAM_RX_JUMBO_BUFFER_DESC, 256 entries.
	 *
	 * ??? No space allocated for mini receive ring? :(
	 *
	 * The size of each ring is fixed in the firmware, but the location is
	 * configurable.
	 */
	tw32(RCVDBDI_STD_BD + TG3_BDINFO_HOST_ADDR + TG3_64BIT_REG_HIGH,
		((uint64_t) tp->rx_std_mapping >> 32));
	tw32(RCVDBDI_STD_BD + TG3_BDINFO_HOST_ADDR + TG3_64BIT_REG_LOW,
		((uint64_t) tp->rx_std_mapping & 0xffffffff));
	tw32(RCVDBDI_STD_BD + TG3_BDINFO_MAXLEN_FLAGS,
		RX_STD_MAX_SIZE << BDINFO_FLAGS_MAXLEN_SHIFT);
	tw32(RCVDBDI_STD_BD + TG3_BDINFO_NIC_ADDR,
		NIC_SRAM_RX_BUFFER_DESC);

	/* Disable the mini frame rx ring */
	tw32(RCVDBDI_MINI_BD + TG3_BDINFO_MAXLEN_FLAGS,
		BDINFO_FLAGS_DISABLED);

	/* Disable the jumbo frame rx ring */
	tw32(RCVDBDI_JUMBO_BD + TG3_BDINFO_MAXLEN_FLAGS,
		BDINFO_FLAGS_DISABLED);

	/* Setup replenish thresholds. */
	tw32(RCVBDI_STD_THRESH, tp->rx_pending / 8);
	tw32(RCVBDI_JUMBO_THRESH, 0);

	/* Clear out send RCB ring in SRAM. */
	for (i = NIC_SRAM_SEND_RCB; i < NIC_SRAM_RCV_RET_RCB; i += TG3_BDINFO_SIZE)
		tg3_write_mem(tp, i + TG3_BDINFO_MAXLEN_FLAGS, BDINFO_FLAGS_DISABLED);

	tp->tx_prod = 0;
	tw32_mailbox(MAILBOX_SNDHOST_PROD_IDX_0 + TG3_64BIT_REG_LOW, 0);
	tw32_mailbox(MAILBOX_SNDNIC_PROD_IDX_0 + TG3_64BIT_REG_LOW, 0);
	tr32(MAILBOX_SNDNIC_PROD_IDX_0 + TG3_64BIT_REG_LOW);

	tg3_set_bdinfo(tp, NIC_SRAM_SEND_RCB,
		tp->tx_desc_mapping,
		(TG3_TX_RING_SIZE <<
			BDINFO_FLAGS_MAXLEN_SHIFT),
		NIC_SRAM_TX_BUFFER_DESC);

	for (i = NIC_SRAM_RCV_RET_RCB; i < NIC_SRAM_STATS_BLK; i += TG3_BDINFO_SIZE) {
		tg3_write_mem(tp, i + TG3_BDINFO_MAXLEN_FLAGS,
			      BDINFO_FLAGS_DISABLED);
	}

	tp->rx_rcb_ptr = 0;
	tw32_mailbox(MAILBOX_RCVRET_CON_IDX_0 + TG3_64BIT_REG_LOW, 0);
	tr32(MAILBOX_RCVRET_CON_IDX_0 + TG3_64BIT_REG_LOW);

	tg3_set_bdinfo(tp, NIC_SRAM_RCV_RET_RCB,
		       tp->rx_rcb_mapping,
		       (TG3_RX_RCB_RING_SIZE <<
			BDINFO_FLAGS_MAXLEN_SHIFT),
		       0);

	tp->rx_std_ptr = tp->rx_pending;
	tw32_mailbox(MAILBOX_RCV_STD_PROD_IDX + TG3_64BIT_REG_LOW,
		     tp->rx_std_ptr);
	tr32(MAILBOX_RCV_STD_PROD_IDX + TG3_64BIT_REG_LOW);

	tw32_mailbox(MAILBOX_RCV_JUMBO_PROD_IDX + TG3_64BIT_REG_LOW, 0);
	tr32(MAILBOX_RCV_JUMBO_PROD_IDX + TG3_64BIT_REG_LOW);

	/* Initialize MAC address and backoff seed. */
	__tg3_set_mac_addr(tp);

	/* MTU + ethernet header + FCS + optional VLAN tag */
	tw32(MAC_RX_MTU_SIZE, ETH_MAX_MTU + ETH_HLEN + 8);

	/* The slot time is changed by tg3_setup_phy if we
	 * run at gigabit with half duplex.
	 */
	tw32(MAC_TX_LENGTHS,
	     (2 << TX_LENGTHS_IPG_CRS_SHIFT) |
	     (6 << TX_LENGTHS_IPG_SHIFT) |
	     (32 << TX_LENGTHS_SLOT_TIME_SHIFT));

	/* Receive rules. */
	tw32(MAC_RCV_RULE_CFG, RCV_RULE_CFG_DEFAULT_CLASS);
	tw32(RCVLPC_CONFIG, 0x0181);

	/* Receive/send statistics. */
	tw32(RCVLPC_STATS_ENABLE, 0xffffff);
	tw32(RCVLPC_STATSCTRL, RCVLPC_STATSCTRL_ENABLE);
	tw32(SNDDATAI_STATSENAB, 0xffffff);
	tw32(SNDDATAI_STATSCTRL,
	     (SNDDATAI_SCTRL_ENABLE |
	      SNDDATAI_SCTRL_FASTUPD));

	/* Setup host coalescing engine. */
	tw32(HOSTCC_MODE, 0);
	for (i = 0; i < 2000; i++) {
		if (!(tr32(HOSTCC_MODE) & HOSTCC_MODE_ENABLE))
			break;
		udelay(10);
	}

	tw32(HOSTCC_RXCOL_TICKS, 0);
	tw32(HOSTCC_RXMAX_FRAMES, 1);
	tw32(HOSTCC_RXCOAL_TICK_INT, 0);
	tw32(HOSTCC_RXCOAL_MAXF_INT, 1);
	tw32(HOSTCC_TXCOL_TICKS, LOW_TXCOL_TICKS);
	tw32(HOSTCC_TXMAX_FRAMES, LOW_RXMAX_FRAMES);
	tw32(HOSTCC_TXCOAL_TICK_INT, 0);
	tw32(HOSTCC_TXCOAL_MAXF_INT, 0);
	tw32(HOSTCC_STAT_COAL_TICKS,
	     DEFAULT_STAT_COAL_TICKS);

	/* Status/statistics block address. */
	tw32(HOSTCC_STATS_BLK_HOST_ADDR + TG3_64BIT_REG_HIGH,
	     ((uint64_t) tp->stats_mapping >> 32));
	tw32(HOSTCC_STATS_BLK_HOST_ADDR + TG3_64BIT_REG_LOW,
	     ((uint64_t) tp->stats_mapping & 0xffffffff));
	tw32(HOSTCC_STATUS_BLK_HOST_ADDR + TG3_64BIT_REG_HIGH,
	     ((uint64_t) tp->status_mapping >> 32));
	tw32(HOSTCC_STATUS_BLK_HOST_ADDR + TG3_64BIT_REG_LOW,
	     ((uint64_t) tp->status_mapping & 0xffffffff));
	tw32(HOSTCC_STATS_BLK_NIC_ADDR, NIC_SRAM_STATS_BLK);
	tw32(HOSTCC_STATUS_BLK_NIC_ADDR, NIC_SRAM_STATUS_BLK);

	tw32(HOSTCC_MODE, HOSTCC_MODE_ENABLE | tp->coalesce_mode);

	tw32(RCVCC_MODE, RCVCC_MODE_ENABLE | RCVCC_MODE_ATTN_ENABLE);
	tw32(RCVLPC_MODE, RCVLPC_MODE_ENABLE);
	tw32(RCVLSC_MODE, RCVLSC_MODE_ENABLE | RCVLSC_MODE_ATTN_ENABLE);

	tp->mac_mode = MAC_MODE_TXSTAT_ENABLE | MAC_MODE_RXSTAT_ENABLE |
		MAC_MODE_TDE_ENABLE | MAC_MODE_RDE_ENABLE | MAC_MODE_FHDE_ENABLE;
	tw32(MAC_MODE, tp->mac_mode | MAC_MODE_RXSTAT_CLEAR | MAC_MODE_TXSTAT_CLEAR);
	tr32(MAC_MODE);
	udelay(40);

	tp->grc_local_ctrl = GRC_LCLCTRL_INT_ON_ATTN | GRC_LCLCTRL_AUTO_SEEPROM;
	if (GET_ASIC_REV(tp->pci_chip_rev_id) == ASIC_REV_5700)
		tp->grc_local_ctrl |= (GRC_LCLCTRL_GPIO_OE1 |
				       GRC_LCLCTRL_GPIO_OUTPUT1);
	tw32(GRC_LOCAL_CTRL, tp->grc_local_ctrl);
	tr32(GRC_LOCAL_CTRL);
	udelay(100);

	tw32_mailbox(MAILBOX_INTERRUPT_0 + TG3_64BIT_REG_LOW, 0);
	tr32(MAILBOX_INTERRUPT_0);

	tw32(DMAC_MODE, DMAC_MODE_ENABLE);
	tr32(DMAC_MODE);
	udelay(40);

	tw32(WDMAC_MODE, (WDMAC_MODE_ENABLE | WDMAC_MODE_TGTABORT_ENAB |
			  WDMAC_MODE_MSTABORT_ENAB | WDMAC_MODE_PARITYERR_ENAB |
			  WDMAC_MODE_ADDROFLOW_ENAB | WDMAC_MODE_FIFOOFLOW_ENAB |
			  WDMAC_MODE_FIFOURUN_ENAB | WDMAC_MODE_FIFOOREAD_ENAB |
			  WDMAC_MODE_LNGREAD_ENAB));
	tr32(WDMAC_MODE);
	udelay(40);

	if (GET_ASIC_REV(tp->pci_chip_rev_id) == ASIC_REV_5704 &&
	    (tp->tg3_flags & TG3_FLAG_PCIX_MODE)) {
		val = tr32(TG3PCI_X_CAPS);
		val &= ~(PCIX_CAPS_SPLIT_MASK | PCIX_CAPS_BURST_MASK);
		val |= (PCIX_CAPS_MAX_BURST_5704 << PCIX_CAPS_BURST_SHIFT);
		if (tp->tg3_flags & TG3_FLAG_SPLIT_MODE)
			val |= (tp->split_mode_max_reqs <<
				PCIX_CAPS_SPLIT_SHIFT);
		tw32(TG3PCI_X_CAPS, val);
	}

	val = (RDMAC_MODE_ENABLE | RDMAC_MODE_TGTABORT_ENAB |
	       RDMAC_MODE_MSTABORT_ENAB | RDMAC_MODE_PARITYERR_ENAB |
	       RDMAC_MODE_ADDROFLOW_ENAB | RDMAC_MODE_FIFOOFLOW_ENAB |
	       RDMAC_MODE_FIFOURUN_ENAB | RDMAC_MODE_FIFOOREAD_ENAB |
	       RDMAC_MODE_LNGREAD_ENAB);
	if (tp->tg3_flags & TG3_FLAG_SPLIT_MODE)
		val |= RDMAC_MODE_SPLIT_ENABLE;
	tw32(RDMAC_MODE, val);
	tr32(RDMAC_MODE);
	udelay(40);

	tw32(RCVDCC_MODE, RCVDCC_MODE_ENABLE | RCVDCC_MODE_ATTN_ENABLE);
	tw32(MBFREE_MODE, MBFREE_MODE_ENABLE);
	tw32(SNDDATAC_MODE, SNDDATAC_MODE_ENABLE);
	tw32(SNDBDC_MODE, SNDBDC_MODE_ENABLE | SNDBDC_MODE_ATTN_ENABLE);
	tw32(RCVBDI_MODE, RCVBDI_MODE_ENABLE | RCVBDI_MODE_RCB_ATTN_ENAB);
	tw32(RCVDBDI_MODE, RCVDBDI_MODE_ENABLE | RCVDBDI_MODE_INV_RING_SZ);
	tw32(SNDDATAI_MODE, SNDDATAI_MODE_ENABLE);
	tw32(SNDBDI_MODE, SNDBDI_MODE_ENABLE | SNDBDI_MODE_ATTN_ENABLE);
	tw32(SNDBDS_MODE, SNDBDS_MODE_ENABLE | SNDBDS_MODE_ATTN_ENABLE);

#warning "FIXME tg3_load_5701_a0_firware_fix not called"
	if (tp->pci_chip_rev_id == CHIPREV_ID_5701_A0) {
#if 0
		err = tg3_load_5701_a0_firmware_fix(tp);
#else
		printf("\n\n\nFIXME implement the tg3_load_5701_a0_firmware_fix\n\n\n");
		err = 1;
#endif
		if (err)
			return err;
	}

	tp->tx_mode = TX_MODE_ENABLE;
	tw32(MAC_TX_MODE, tp->tx_mode);
	tr32(MAC_TX_MODE);
	udelay(100);

	tp->rx_mode = RX_MODE_ENABLE;
	tw32(MAC_RX_MODE, tp->rx_mode);
	tr32(MAC_RX_MODE);
	udelay(10);

	tp->mi_mode = MAC_MI_MODE_BASE;
	tw32(MAC_MI_MODE, tp->mi_mode);
	tr32(MAC_MI_MODE);
	udelay(40);

	tw32(MAC_LED_CTRL, 0);
	tw32(MAC_MI_STAT, MAC_MI_STAT_LNKSTAT_ATTN_ENAB);
	tw32(MAC_RX_MODE, RX_MODE_RESET);
	tr32(MAC_RX_MODE);
	udelay(10);
	tw32(MAC_RX_MODE, tp->rx_mode);
	tr32(MAC_RX_MODE);
	udelay(10);

	if (tp->pci_chip_rev_id == CHIPREV_ID_5703_A1)
		tw32(MAC_SERDES_CFG, 0x616000);

	err = tg3_setup_phy(tp);
	if (err)
		return err;

	if (tp->phy_id != PHY_ID_SERDES) {
		uint32_t tmp;

		/* Clear CRC stats. */
		tg3_readphy(tp, 0x1e, &tmp);
		tg3_writephy(tp, 0x1e, tmp | 0x8000);
		tg3_readphy(tp, 0x14, &tmp);
	}

	__tg3_set_rx_mode(tp);

	/* Initialize receive rules. */
	tw32(MAC_RCV_RULE_0,  0xc2000000 & RCV_RULE_DISABLE_MASK);
	tw32(MAC_RCV_VALUE_0, 0xffffffff & RCV_RULE_DISABLE_MASK);
	tw32(MAC_RCV_RULE_1,  0x86000004 & RCV_RULE_DISABLE_MASK);
	tw32(MAC_RCV_VALUE_1, 0xffffffff & RCV_RULE_DISABLE_MASK);
#if 0
	tw32(MAC_RCV_RULE_2,  0); tw32(MAC_RCV_VALUE_2,  0);
	tw32(MAC_RCV_RULE_3,  0); tw32(MAC_RCV_VALUE_3,  0);
#endif
	tw32(MAC_RCV_RULE_4,  0); tw32(MAC_RCV_VALUE_4,  0);
	tw32(MAC_RCV_RULE_5,  0); tw32(MAC_RCV_VALUE_5,  0);
	tw32(MAC_RCV_RULE_6,  0); tw32(MAC_RCV_VALUE_6,  0);
	tw32(MAC_RCV_RULE_7,  0); tw32(MAC_RCV_VALUE_7,  0);
	tw32(MAC_RCV_RULE_8,  0); tw32(MAC_RCV_VALUE_8,  0);
	tw32(MAC_RCV_RULE_9,  0); tw32(MAC_RCV_VALUE_9,  0);
	tw32(MAC_RCV_RULE_10,  0); tw32(MAC_RCV_VALUE_10,  0);
	tw32(MAC_RCV_RULE_11,  0); tw32(MAC_RCV_VALUE_11,  0);
	tw32(MAC_RCV_RULE_12,  0); tw32(MAC_RCV_VALUE_12,  0);
	tw32(MAC_RCV_RULE_13,  0); tw32(MAC_RCV_VALUE_13,  0);
	tw32(MAC_RCV_RULE_14,  0); tw32(MAC_RCV_VALUE_14,  0);
	tw32(MAC_RCV_RULE_15,  0); tw32(MAC_RCV_VALUE_15,  0);

	return 0;
}

/* Called at device open time to get the chip ready for
 * packet processing.  Invoked with tp->lock held.
 */
static int tg3_init_hw(struct tg3 *tp)
{
	int err;

	/* Force the chip into D0. */
	tg3_set_power_state_0(tp);

	tg3_switch_clocks(tp);

	tw32(TG3PCI_MEM_WIN_BASE_ADDR, 0);

	err = tg3_reset_hw(tp);


	return err;
}

#if 0
static void tg3_dump_state(struct tg3 *tp)
{
	uint32_t val32, val32_2, val32_3, val32_4, val32_5;
	uint16_t val16;
	int i;

	pci_read_config_word(tp->pdev, PCI_STATUS, &val16);
	pci_read_config_dword(tp->pdev, TG3PCI_PCISTATE, &val32);
	printf("DEBUG: PCI status [%hx] TG3PCI state[%x]\n",
	       val16, val32);

	/* MAC block */
	printf("DEBUG: MAC_MODE[%x] MAC_STATUS[%x]\n",
	       tr32(MAC_MODE), tr32(MAC_STATUS));
	printf("       MAC_EVENT[%x] MAC_LED_CTRL[%x]\n",
	       tr32(MAC_EVENT), tr32(MAC_LED_CTRL));
	printf("DEBUG: MAC_TX_MODE[%x] MAC_TX_STATUS[%x]\n",
	       tr32(MAC_TX_MODE), tr32(MAC_TX_STATUS));
	printf("       MAC_RX_MODE[%x] MAC_RX_STATUS[%x]\n",
	       tr32(MAC_RX_MODE), tr32(MAC_RX_STATUS));

	/* Send data initiator control block */
	printf("DEBUG: SNDDATAI_MODE[%x] SNDDATAI_STATUS[%x]\n",
	       tr32(SNDDATAI_MODE), tr32(SNDDATAI_STATUS));
	printf("       SNDDATAI_STATSCTRL[%x]\n",
	       tr32(SNDDATAI_STATSCTRL));

	/* Send data completion control block */
	printf("DEBUG: SNDDATAC_MODE[%x]\n", tr32(SNDDATAC_MODE));

	/* Send BD ring selector block */
	printf("DEBUG: SNDBDS_MODE[%x] SNDBDS_STATUS[%x]\n",
	       tr32(SNDBDS_MODE), tr32(SNDBDS_STATUS));

	/* Send BD initiator control block */
	printf("DEBUG: SNDBDI_MODE[%x] SNDBDI_STATUS[%x]\n",
	       tr32(SNDBDI_MODE), tr32(SNDBDI_STATUS));

	/* Send BD completion control block */
	printf("DEBUG: SNDBDC_MODE[%x]\n", tr32(SNDBDC_MODE));

	/* Receive list placement control block */
	printf("DEBUG: RCVLPC_MODE[%x] RCVLPC_STATUS[%x]\n",
	       tr32(RCVLPC_MODE), tr32(RCVLPC_STATUS));
	printf("       RCVLPC_STATSCTRL[%x]\n",
	       tr32(RCVLPC_STATSCTRL));

	/* Receive data and receive BD initiator control block */
	printf("DEBUG: RCVDBDI_MODE[%x] RCVDBDI_STATUS[%x]\n",
	       tr32(RCVDBDI_MODE), tr32(RCVDBDI_STATUS));

	/* Receive data completion control block */
	printf("DEBUG: RCVDCC_MODE[%x]\n",
	       tr32(RCVDCC_MODE));

	/* Receive BD initiator control block */
	printf("DEBUG: RCVBDI_MODE[%x] RCVBDI_STATUS[%x]\n",
	       tr32(RCVBDI_MODE), tr32(RCVBDI_STATUS));

	/* Receive BD completion control block */
	printf("DEBUG: RCVCC_MODE[%x] RCVCC_STATUS[%x]\n",
	       tr32(RCVCC_MODE), tr32(RCVCC_STATUS));

	/* Receive list selector control block */
	printf("DEBUG: RCVLSC_MODE[%x] RCVLSC_STATUS[%x]\n",
	       tr32(RCVLSC_MODE), tr32(RCVLSC_STATUS));

	/* Mbuf cluster free block */
	printf("DEBUG: MBFREE_MODE[%x] MBFREE_STATUS[%x]\n",
	       tr32(MBFREE_MODE), tr32(MBFREE_STATUS));

	/* Host coalescing control block */
	printf("DEBUG: HOSTCC_MODE[%x] HOSTCC_STATUS[%x]\n",
	       tr32(HOSTCC_MODE), tr32(HOSTCC_STATUS));
	printf("DEBUG: HOSTCC_STATS_BLK_HOST_ADDR[%x%x]\n",
	       tr32(HOSTCC_STATS_BLK_HOST_ADDR + TG3_64BIT_REG_HIGH),
	       tr32(HOSTCC_STATS_BLK_HOST_ADDR + TG3_64BIT_REG_LOW));
	printf("DEBUG: HOSTCC_STATUS_BLK_HOST_ADDR[%x%x]\n",
	       tr32(HOSTCC_STATUS_BLK_HOST_ADDR + TG3_64BIT_REG_HIGH),
	       tr32(HOSTCC_STATUS_BLK_HOST_ADDR + TG3_64BIT_REG_LOW));
	printf("DEBUG: HOSTCC_STATS_BLK_NIC_ADDR[%x]\n",
	       tr32(HOSTCC_STATS_BLK_NIC_ADDR));
	printf("DEBUG: HOSTCC_STATUS_BLK_NIC_ADDR[%x]\n",
	       tr32(HOSTCC_STATUS_BLK_NIC_ADDR));

	/* Memory arbiter control block */
	printf("DEBUG: MEMARB_MODE[%x] MEMARB_STATUS[%x]\n",
	       tr32(MEMARB_MODE), tr32(MEMARB_STATUS));

	/* Buffer manager control block */
	printf("DEBUG: BUFMGR_MODE[%x] BUFMGR_STATUS[%x]\n",
	       tr32(BUFMGR_MODE), tr32(BUFMGR_STATUS));
	printf("DEBUG: BUFMGR_MB_POOL_ADDR[%x] BUFMGR_MB_POOL_SIZE[%x]\n",
	       tr32(BUFMGR_MB_POOL_ADDR), tr32(BUFMGR_MB_POOL_SIZE));
	printf("DEBUG: BUFMGR_DMA_DESC_POOL_ADDR[%x] "
	       "BUFMGR_DMA_DESC_POOL_SIZE[%x]\n",
	       tr32(BUFMGR_DMA_DESC_POOL_ADDR),
	       tr32(BUFMGR_DMA_DESC_POOL_SIZE));

	/* Read DMA control block */
	printf("DEBUG: RDMAC_MODE[%x] RDMAC_STATUS[%x]\n",
	       tr32(RDMAC_MODE), tr32(RDMAC_STATUS));

	/* Write DMA control block */
	printf("DEBUG: WDMAC_MODE[%x] WDMAC_STATUS[%x]\n",
	       tr32(WDMAC_MODE), tr32(WDMAC_STATUS));

	/* DMA completion block */
	printf("DEBUG: DMAC_MODE[%x]\n",
	       tr32(DMAC_MODE));

	/* GRC block */
	printf("DEBUG: GRC_MODE[%x] GRC_MISC_CFG[%x]\n",
	       tr32(GRC_MODE), tr32(GRC_MISC_CFG));
	printf("DEBUG: GRC_LOCAL_CTRL[%x]\n",
	       tr32(GRC_LOCAL_CTRL));

	/* TG3_BDINFOs */
	printf("DEBUG: RCVDBDI_JUMBO_BD[%x%x:%x:%x]\n",
	       tr32(RCVDBDI_JUMBO_BD + 0x0),
	       tr32(RCVDBDI_JUMBO_BD + 0x4),
	       tr32(RCVDBDI_JUMBO_BD + 0x8),
	       tr32(RCVDBDI_JUMBO_BD + 0xc));
	printf("DEBUG: RCVDBDI_STD_BD[%x%x:%x:%x]\n",
	       tr32(RCVDBDI_STD_BD + 0x0),
	       tr32(RCVDBDI_STD_BD + 0x4),
	       tr32(RCVDBDI_STD_BD + 0x8),
	       tr32(RCVDBDI_STD_BD + 0xc));
	printf("DEBUG: RCVDBDI_MINI_BD[%x%x:%x:%x]\n",
	       tr32(RCVDBDI_MINI_BD + 0x0),
	       tr32(RCVDBDI_MINI_BD + 0x4),
	       tr32(RCVDBDI_MINI_BD + 0x8),
	       tr32(RCVDBDI_MINI_BD + 0xc));

	tg3_read_mem(tp, NIC_SRAM_SEND_RCB + 0x0, &val32);
	tg3_read_mem(tp, NIC_SRAM_SEND_RCB + 0x4, &val32_2);
	tg3_read_mem(tp, NIC_SRAM_SEND_RCB + 0x8, &val32_3);
	tg3_read_mem(tp, NIC_SRAM_SEND_RCB + 0xc, &val32_4);
	printf("DEBUG: SRAM_SEND_RCB_0[%x%x:%x:%x]\n",
	       val32, val32_2, val32_3, val32_4);

	tg3_read_mem(tp, NIC_SRAM_RCV_RET_RCB + 0x0, &val32);
	tg3_read_mem(tp, NIC_SRAM_RCV_RET_RCB + 0x4, &val32_2);
	tg3_read_mem(tp, NIC_SRAM_RCV_RET_RCB + 0x8, &val32_3);
	tg3_read_mem(tp, NIC_SRAM_RCV_RET_RCB + 0xc, &val32_4);
	printf("DEBUG: SRAM_RCV_RET_RCB_0[%x%x:%x:%x]\n",
	       val32, val32_2, val32_3, val32_4);

	tg3_read_mem(tp, NIC_SRAM_STATUS_BLK + 0x0, &val32);
	tg3_read_mem(tp, NIC_SRAM_STATUS_BLK + 0x4, &val32_2);
	tg3_read_mem(tp, NIC_SRAM_STATUS_BLK + 0x8, &val32_3);
	tg3_read_mem(tp, NIC_SRAM_STATUS_BLK + 0xc, &val32_4);
	tg3_read_mem(tp, NIC_SRAM_STATUS_BLK + 0x10, &val32_5);
	printf("DEBUG: SRAM_STATUS_BLK[%x:%x:%x:%x:%x]\n",
	       val32, val32_2, val32_3, val32_4, val32_5);

	/* SW status block */
	printf("DEBUG: Host status block [%x:%x:(%hx:%hx:%hx):(%hx:%hx)]\n",
	       tp->hw_status->status,
	       tp->hw_status->status_tag,
	       tp->hw_status->rx_jumbo_consumer,
	       tp->hw_status->rx_consumer,
	       tp->hw_status->rx_mini_consumer,
	       tp->hw_status->idx[0].rx_producer,
	       tp->hw_status->idx[0].tx_consumer);

	/* SW statistics block */
	printf("DEBUG: Host statistics block [%x:%x:%x:%x]\n",
	       ((uint32_t *)tp->hw_stats)[0],
	       ((uint32_t *)tp->hw_stats)[1],
	       ((uint32_t *)tp->hw_stats)[2],
	       ((uint32_t *)tp->hw_stats)[3]);

	/* Mailboxes */
	printf("DEBUG: SNDHOST_PROD[%x%x] SNDNIC_PROD[%x%x]\n",
	       tr32(MAILBOX_SNDHOST_PROD_IDX_0 + 0x0),
	       tr32(MAILBOX_SNDHOST_PROD_IDX_0 + 0x4),
	       tr32(MAILBOX_SNDNIC_PROD_IDX_0 + 0x0),
	       tr32(MAILBOX_SNDNIC_PROD_IDX_0 + 0x4));

	/* NIC side send descriptors. */
	for (i = 0; i < 6; i++) {
		unsigned long txd;

		txd = tp->regs + NIC_SRAM_WIN_BASE + NIC_SRAM_TX_BUFFER_DESC
			+ (i * sizeof(struct tg3_tx_buffer_desc));
		printf("DEBUG: NIC TXD(%d)[%x:%x:%x:%x]\n",
		       i,
		       readl(txd + 0x0), readl(txd + 0x4),
		       readl(txd + 0x8), readl(txd + 0xc));
	}

	/* NIC side RX descriptors. */
	for (i = 0; i < 6; i++) {
		unsigned long rxd;

		rxd = tp->regs + NIC_SRAM_WIN_BASE + NIC_SRAM_RX_BUFFER_DESC
			+ (i * sizeof(struct tg3_rx_buffer_desc));
		printf("DEBUG: NIC RXD_STD(%d)[0][%x:%x:%x:%x]\n",
		       i,
		       readl(rxd + 0x0), readl(rxd + 0x4),
		       readl(rxd + 0x8), readl(rxd + 0xc));
		rxd += (4 * sizeof(uint32_t));
		printf("DEBUG: NIC RXD_STD(%d)[1][%x:%x:%x:%x]\n",
		       i,
		       readl(rxd + 0x0), readl(rxd + 0x4),
		       readl(rxd + 0x8), readl(rxd + 0xc));
	}
	for (i = 0; i < 6; i++) {
		unsigned long rxd;

		rxd = tp->regs + NIC_SRAM_WIN_BASE + NIC_SRAM_RX_JUMBO_BUFFER_DESC
			+ (i * sizeof(struct tg3_rx_buffer_desc));
		printf("DEBUG: NIC RXD_JUMBO(%d)[0][%x:%x:%x:%x]\n",
		       i,
		       readl(rxd + 0x0), readl(rxd + 0x4),
		       readl(rxd + 0x8), readl(rxd + 0xc));
		rxd += (4 * sizeof(uint32_t));
		printf("DEBUG: NIC RXD_JUMBO(%d)[1][%x:%x:%x:%x]\n",
		       i,
		       readl(rxd + 0x0), readl(rxd + 0x4),
		       readl(rxd + 0x8), readl(rxd + 0xc));
	}
}
#else
#define tg3_dump_state(TP)
#endif

static void tg3_set_multi(struct tg3 *tp, unsigned int accept_all)
{
	/* accept or reject all multicast frames */
	tw32(MAC_HASH_REG_0, accept_all ? 0xffffffff : 0);
	tw32(MAC_HASH_REG_1, accept_all ? 0xffffffff : 0);
	tw32(MAC_HASH_REG_2, accept_all ? 0xffffffff : 0);
	tw32(MAC_HASH_REG_3, accept_all ? 0xffffffff : 0);
}

static void __tg3_set_rx_mode(struct tg3 *tp)
{
	uint32_t rx_mode;

	rx_mode = tp->rx_mode & ~(RX_MODE_PROMISC |
				  RX_MODE_KEEP_VLAN_TAG);
	/* By definition, VLAN is disabled always in this
	 * case.
	 */
	rx_mode |= RX_MODE_KEEP_VLAN_TAG;

	/* Accept all multicast. */
	tg3_set_multi (tp, 1);

	if (rx_mode != tp->rx_mode) {
		tp->rx_mode = rx_mode;
		tw32(MAC_RX_MODE, rx_mode);
		tr32(MAC_RX_MODE);
		udelay(10);
	}
}


/* Chips other than 5700/5701 use the NVRAM for fetching info. */
static void tg3_nvram_init(struct tg3 *tp)
{
	tw32(GRC_EEPROM_ADDR,
	     (EEPROM_ADDR_FSM_RESET |
	      (EEPROM_DEFAULT_CLOCK_PERIOD <<
	       EEPROM_ADDR_CLKPERD_SHIFT)));

	/* XXX schedule_timeout() ... */
	mdelay(1);

	/* Enable seeprom accesses. */
	tw32(GRC_LOCAL_CTRL,
	     tr32(GRC_LOCAL_CTRL) | GRC_LCLCTRL_AUTO_SEEPROM);
	tr32(GRC_LOCAL_CTRL);
	udelay(100);

	if (GET_ASIC_REV(tp->pci_chip_rev_id) != ASIC_REV_5700 &&
	    GET_ASIC_REV(tp->pci_chip_rev_id) != ASIC_REV_5701) {
		uint32_t nvcfg1 = tr32(NVRAM_CFG1);

		tp->tg3_flags |= TG3_FLAG_NVRAM;
		if (nvcfg1 & NVRAM_CFG1_FLASHIF_ENAB) {
			if (nvcfg1 & NVRAM_CFG1_BUFFERED_MODE)
				tp->tg3_flags |= TG3_FLAG_NVRAM_BUFFERED;
		} else {
			nvcfg1 &= ~NVRAM_CFG1_COMPAT_BYPASS;
			tw32(NVRAM_CFG1, nvcfg1);
		}

	} else {
		tp->tg3_flags &= ~(TG3_FLAG_NVRAM | TG3_FLAG_NVRAM_BUFFERED);
	}
}


static int tg3_nvram_read_using_eeprom(
	struct tg3 *tp, uint32_t offset, uint32_t *val)
{
	uint32_t tmp;
	int i;

	if (offset > EEPROM_ADDR_ADDR_MASK ||
	    (offset % 4) != 0)
		return -EINVAL;

	tmp = tr32(GRC_EEPROM_ADDR) & ~(EEPROM_ADDR_ADDR_MASK |
					EEPROM_ADDR_DEVID_MASK |
					EEPROM_ADDR_READ);
	tw32(GRC_EEPROM_ADDR,
	     tmp |
	     (0 << EEPROM_ADDR_DEVID_SHIFT) |
	     ((offset << EEPROM_ADDR_ADDR_SHIFT) &
	      EEPROM_ADDR_ADDR_MASK) |
	     EEPROM_ADDR_READ | EEPROM_ADDR_START);

	for (i = 0; i < 10000; i++) {
		tmp = tr32(GRC_EEPROM_ADDR);

		if (tmp & EEPROM_ADDR_COMPLETE)
			break;
		udelay(100);
	}
	if (!(tmp & EEPROM_ADDR_COMPLETE))
		return -EBUSY;

	*val = tr32(GRC_EEPROM_DATA);
	return 0;
}

static int tg3_nvram_read(struct tg3 *tp, uint32_t offset, uint32_t *val)
{
	int i, saw_done_clear;

	if (!(tp->tg3_flags & TG3_FLAG_NVRAM))
		return tg3_nvram_read_using_eeprom(tp, offset, val);

	if (tp->tg3_flags & TG3_FLAG_NVRAM_BUFFERED)
		offset = ((offset / NVRAM_BUFFERED_PAGE_SIZE) <<
			  NVRAM_BUFFERED_PAGE_POS) +
			(offset % NVRAM_BUFFERED_PAGE_SIZE);

	if (offset > NVRAM_ADDR_MSK)
		return -EINVAL;

	tw32(NVRAM_SWARB, SWARB_REQ_SET1);
	for (i = 0; i < 1000; i++) {
		if (tr32(NVRAM_SWARB) & SWARB_GNT1)
			break;
		udelay(20);
	}

	tw32(NVRAM_ADDR, offset);
	tw32(NVRAM_CMD,
	     NVRAM_CMD_RD | NVRAM_CMD_GO |
	     NVRAM_CMD_FIRST | NVRAM_CMD_LAST | NVRAM_CMD_DONE);

	/* Wait for done bit to clear then set again. */
	saw_done_clear = 0;
	for (i = 0; i < 1000; i++) {
		udelay(10);
		if (!saw_done_clear &&
		    !(tr32(NVRAM_CMD) & NVRAM_CMD_DONE))
			saw_done_clear = 1;
		else if (saw_done_clear &&
			 (tr32(NVRAM_CMD) & NVRAM_CMD_DONE))
			break;
	}
	if (i >= 1000) {
		tw32(NVRAM_SWARB, SWARB_REQ_CLR1);
		return -EBUSY;
	}

	*val = bswap_32(tr32(NVRAM_RDDATA));
	tw32(NVRAM_SWARB, 0x20);

	return 0;
}

struct subsys_tbl_ent {
	uint16_t subsys_vendor, subsys_devid;
	uint32_t phy_id;
};

static struct subsys_tbl_ent subsys_id_to_phy_id[] = {
	/* Broadcom boards. */
	{ 0x14e4, 0x1644, PHY_ID_BCM5401 }, /* BCM95700A6 */
	{ 0x14e4, 0x0001, PHY_ID_BCM5701 }, /* BCM95701A5 */
	{ 0x14e4, 0x0002, PHY_ID_BCM8002 }, /* BCM95700T6 */
	{ 0x14e4, 0x0003, PHY_ID_SERDES  }, /* BCM95700A9 */
	{ 0x14e4, 0x0005, PHY_ID_BCM5701 }, /* BCM95701T1 */
	{ 0x14e4, 0x0006, PHY_ID_BCM5701 }, /* BCM95701T8 */
	{ 0x14e4, 0x0007, PHY_ID_SERDES  }, /* BCM95701A7 */
	{ 0x14e4, 0x0008, PHY_ID_BCM5701 }, /* BCM95701A10 */
	{ 0x14e4, 0x8008, PHY_ID_BCM5701 }, /* BCM95701A12 */
	{ 0x14e4, 0x0009, PHY_ID_BCM5701 }, /* BCM95703Ax1 */
	{ 0x14e4, 0x8009, PHY_ID_BCM5701 }, /* BCM95703Ax2 */

	/* 3com boards. */
	{ PCI_VENDOR_ID_3COM, 0x1000, PHY_ID_BCM5401 }, /* 3C996T */
	{ PCI_VENDOR_ID_3COM, 0x1006, PHY_ID_BCM5701 }, /* 3C996BT */
	/* { PCI_VENDOR_ID_3COM, 0x1002, PHY_ID_XXX },     3C996CT */
	/* { PCI_VENDOR_ID_3COM, 0x1003, PHY_ID_XXX },     3C997T */
	{ PCI_VENDOR_ID_3COM, 0x1004, PHY_ID_SERDES  }, /* 3C996SX */
	/* { PCI_VENDOR_ID_3COM, 0x1005, PHY_ID_XXX },     3C997SZ */
	{ PCI_VENDOR_ID_3COM, 0x1007, PHY_ID_BCM5701 }, /* 3C1000T */
	{ PCI_VENDOR_ID_3COM, 0x1008, PHY_ID_BCM5701 }, /* 3C940BR01 */

	/* DELL boards. */
	{ PCI_VENDOR_ID_DELL, 0x00d1, PHY_ID_BCM5401 }, /* VIPER */
	{ PCI_VENDOR_ID_DELL, 0x0106, PHY_ID_BCM5401 }, /* JAGUAR */
	{ PCI_VENDOR_ID_DELL, 0x0109, PHY_ID_BCM5411 }, /* MERLOT */
	{ PCI_VENDOR_ID_DELL, 0x010a, PHY_ID_BCM5411 }, /* SLIM_MERLOT */

	/* Compaq boards. */
	{ PCI_VENDOR_ID_COMPAQ, 0x007c, PHY_ID_BCM5701 }, /* BANSHEE */
	{ PCI_VENDOR_ID_COMPAQ, 0x009a, PHY_ID_BCM5701 }, /* BANSHEE_2 */
	{ PCI_VENDOR_ID_COMPAQ, 0x007d, PHY_ID_SERDES  }, /* CHANGELING */
	{ PCI_VENDOR_ID_COMPAQ, 0x0085, PHY_ID_BCM5701 }, /* NC7780 */
	{ PCI_VENDOR_ID_COMPAQ, 0x0099, PHY_ID_BCM5701 }  /* NC7780_2 */
};

static int tg3_phy_probe(struct tg3 *tp)
{
	uint32_t eeprom_phy_id, hw_phy_id_1, hw_phy_id_2;
	uint32_t hw_phy_id, hw_phy_id_masked;
	enum phy_led_mode eeprom_led_mode;
	uint32_t val;
	unsigned i;
	int eeprom_signature_found, err;

	tp->phy_id = PHY_ID_INVALID;

	for (i = 0; i < sizeof(subsys_id_to_phy_id)/sizeof(subsys_id_to_phy_id[0]); i++) {
		if ((subsys_id_to_phy_id[i].subsys_vendor == tp->subsystem_vendor) &&
			(subsys_id_to_phy_id[i].subsys_devid == tp->subsystem_device)) {
			tp->phy_id = subsys_id_to_phy_id[i].phy_id;
			break;
		}
	}

	eeprom_phy_id = PHY_ID_INVALID;
	eeprom_led_mode = led_mode_auto;
	eeprom_signature_found = 0;
	tg3_read_mem(tp, NIC_SRAM_DATA_SIG, &val);
	if (val == NIC_SRAM_DATA_SIG_MAGIC) {
		uint32_t nic_cfg;

		tg3_read_mem(tp, NIC_SRAM_DATA_CFG, &nic_cfg);

		eeprom_signature_found = 1;

		if ((nic_cfg & NIC_SRAM_DATA_CFG_PHY_TYPE_MASK) ==
		    NIC_SRAM_DATA_CFG_PHY_TYPE_FIBER) {
			eeprom_phy_id = PHY_ID_SERDES;
		} else {
			uint32_t nic_phy_id;

			tg3_read_mem(tp, NIC_SRAM_DATA_PHY_ID, &nic_phy_id);
			if (nic_phy_id != 0) {
				uint32_t id1 = nic_phy_id & NIC_SRAM_DATA_PHY_ID1_MASK;
				uint32_t id2 = nic_phy_id & NIC_SRAM_DATA_PHY_ID2_MASK;

				eeprom_phy_id  = (id1 >> 16) << 10;
				eeprom_phy_id |= (id2 & 0xfc00) << 16;
				eeprom_phy_id |= (id2 & 0x03ff) <<  0;
			}
		}

		switch (nic_cfg & NIC_SRAM_DATA_CFG_LED_MODE_MASK) {
		case NIC_SRAM_DATA_CFG_LED_TRIPLE_SPD:
			eeprom_led_mode = led_mode_three_link;
			break;

		case NIC_SRAM_DATA_CFG_LED_LINK_SPD:
			eeprom_led_mode = led_mode_link10;
			break;

		default:
			eeprom_led_mode = led_mode_auto;
			break;
		};
		if ((tp->pci_chip_rev_id == CHIPREV_ID_5703_A1 ||
		     tp->pci_chip_rev_id == CHIPREV_ID_5703_A2) &&
		    (nic_cfg & NIC_SRAM_DATA_CFG_EEPROM_WP))
			tp->tg3_flags |= TG3_FLAG_EEPROM_WRITE_PROT;

		if (nic_cfg & NIC_SRAM_DATA_CFG_ASF_ENABLE)
			tp->tg3_flags |= TG3_FLAG_ENABLE_ASF;
		if (nic_cfg & NIC_SRAM_DATA_CFG_FIBER_WOL)
			tp->tg3_flags |= TG3_FLAG_SERDES_WOL_CAP;
	}

	/* Now read the physical PHY_ID from the chip and verify
	 * that it is sane.  If it doesn't look good, we fall back
	 * to either the hard-coded table based PHY_ID and failing
	 * that the value found in the eeprom area.
	 */
	err  = tg3_readphy(tp, MII_PHYSID1, &hw_phy_id_1);
	err |= tg3_readphy(tp, MII_PHYSID2, &hw_phy_id_2);

	hw_phy_id  = (hw_phy_id_1 & 0xffff) << 10;
	hw_phy_id |= (hw_phy_id_2 & 0xfc00) << 16;
	hw_phy_id |= (hw_phy_id_2 & 0x03ff) <<  0;

	hw_phy_id_masked = hw_phy_id & PHY_ID_MASK;

	if (!err && KNOWN_PHY_ID(hw_phy_id_masked)) {
		tp->phy_id = hw_phy_id;
	} else {
		/* phy_id currently holds the value found in the
		 * subsys_id_to_phy_id[] table or PHY_ID_INVALID
		 * if a match was not found there.
		 */
		if (tp->phy_id == PHY_ID_INVALID) {
			if (!eeprom_signature_found ||
			    !KNOWN_PHY_ID(eeprom_phy_id & PHY_ID_MASK))
				return -ENODEV;
			tp->phy_id = eeprom_phy_id;
		}
	}

	err = tg3_phy_reset(tp);
	if (err)
		return err;

	if (tp->pci_chip_rev_id == CHIPREV_ID_5701_A0 ||
	    tp->pci_chip_rev_id == CHIPREV_ID_5701_B0) {
		uint32_t mii_tg3_ctrl;
		
		/* These chips, when reset, only advertise 10Mb
		 * capabilities.  Fix that.
		 */
		err  = tg3_writephy(tp, MII_ADVERTISE,
				    (ADVERTISE_CSMA |
				     ADVERTISE_PAUSE_CAP |
				     ADVERTISE_10HALF |
				     ADVERTISE_10FULL |
				     ADVERTISE_100HALF |
				     ADVERTISE_100FULL));
		mii_tg3_ctrl = (MII_TG3_CTRL_ADV_1000_HALF |
				MII_TG3_CTRL_ADV_1000_FULL |
				MII_TG3_CTRL_AS_MASTER |
				MII_TG3_CTRL_ENABLE_AS_MASTER);
		if (tp->tg3_flags & TG3_FLAG_10_100_ONLY)
			mii_tg3_ctrl = 0;

		err |= tg3_writephy(tp, MII_TG3_CTRL, mii_tg3_ctrl);
		err |= tg3_writephy(tp, MII_BMCR,
				    (BMCR_ANRESTART | BMCR_ANENABLE));
	}

	if (GET_ASIC_REV(tp->pci_chip_rev_id) == ASIC_REV_5703) {
		tg3_writephy(tp, MII_TG3_AUX_CTRL, 0x0c00);
		tg3_writephy(tp, MII_TG3_DSP_ADDRESS, 0x201f);
		tg3_writephy(tp, MII_TG3_DSP_RW_PORT, 0x2aaa);
	}

	if (GET_ASIC_REV(tp->pci_chip_rev_id) == ASIC_REV_5704) {
		tg3_writephy(tp, 0x1c, 0x8d68);
		tg3_writephy(tp, 0x1c, 0x8d68);
	}

	/* Enable Ethernet@WireSpeed */
	tg3_writephy(tp, MII_TG3_AUX_CTRL, 0x7007);
	tg3_readphy(tp, MII_TG3_AUX_CTRL, &val);
	tg3_writephy(tp, MII_TG3_AUX_CTRL, (val | (1 << 15) | (1 << 4)));

	if (!err && ((tp->phy_id & PHY_ID_MASK) == PHY_ID_BCM5401)) {
		err = tg3_init_5401phy_dsp(tp);
	}

	/* Determine the PHY led mode. */
	if (tp->subsystem_vendor == PCI_VENDOR_ID_DELL) {
		tp->led_mode = led_mode_link10;
	} else {
		tp->led_mode = led_mode_three_link;
		if (eeprom_signature_found &&
		    eeprom_led_mode != led_mode_auto)
			tp->led_mode = eeprom_led_mode;
	}

	if (tp->phy_id == PHY_ID_SERDES)
		tp->link_config.advertising =
			(ADVERTISED_1000baseT_Half |
			 ADVERTISED_1000baseT_Full |
			 ADVERTISED_Autoneg |
			 ADVERTISED_FIBRE);
	if (tp->tg3_flags & TG3_FLAG_10_100_ONLY)
		tp->link_config.advertising &=
			~(ADVERTISED_1000baseT_Half |
			  ADVERTISED_1000baseT_Full);

	return err;
}

#if SUPPORT_PARTNO_STR
static void tg3_read_partno(struct tg3 *tp)
{
	unsigned char vpd_data[256];
	int i;

	for (i = 0; i < 256; i += 4) {
		uint32_t tmp;

		if (tg3_nvram_read(tp, 0x100 + i, &tmp))
			goto out_not_found;

		vpd_data[i + 0] = ((tmp >>  0) & 0xff);
		vpd_data[i + 1] = ((tmp >>  8) & 0xff);
		vpd_data[i + 2] = ((tmp >> 16) & 0xff);
		vpd_data[i + 3] = ((tmp >> 24) & 0xff);
	}

	/* Now parse and find the part number. */
	for (i = 0; i < 256; ) {
		unsigned char val = vpd_data[i];
		int block_end;

		if (val == 0x82 || val == 0x91) {
			i = (i + 3 +
			     (vpd_data[i + 1] +
			      (vpd_data[i + 2] << 8)));
			continue;
		}

		if (val != 0x90)
			goto out_not_found;

		block_end = (i + 3 +
			     (vpd_data[i + 1] +
			      (vpd_data[i + 2] << 8)));
		i += 3;
		while (i < block_end) {
			if (vpd_data[i + 0] == 'P' &&
			    vpd_data[i + 1] == 'N') {
				int partno_len = vpd_data[i + 2];

				if (partno_len > 24)
					goto out_not_found;

				memcpy(tp->board_part_number,
				       &vpd_data[i + 3],
				       partno_len);

				/* Success. */
				return;
			}
		}

		/* Part number not found. */
		goto out_not_found;
	}

out_not_found:
	memcpy(tp->board_part_number, "none", sizeof("none"));
}
#else
#define tg3_read_partno(TP) ((TP)->board_part_number[0] = '\0')
#endif

static int tg3_get_invariants(struct tg3 *tp)
{
	uint32_t misc_ctrl_reg;
	uint32_t cacheline_sz_reg;
	uint32_t pci_state_reg, grc_misc_cfg;
	uint16_t pci_cmd;
	int err;

	/* Read the subsystem vendor and device ids */
	pci_read_config_word(tp->pdev, PCI_SUBSYSTEM_VENDOR_ID, &tp->subsystem_vendor);
	pci_read_config_word(tp->pdev, PCI_SUBSYSTEM_ID, &tp->subsystem_device);

	/* If we have an AMD 762 or Intel ICH/ICH0 chipset, write
	 * reordering to the mailbox registers done by the host
	 * controller can cause major troubles.  We read back from
	 * every mailbox register write to force the writes to be
	 * posted to the chip in order.
	 *
	 * TG3_FLAG_MBOX_WRITE_REORDER has been forced on.
	 */

	/* Force memory write invalidate off.  If we leave it on,
	 * then on 5700_BX chips we have to enable a workaround.
	 * The workaround is to set the TG3PCI_DMA_RW_CTRL boundry
	 * to match the cacheline size.  The Broadcom driver have this
	 * workaround but turns MWI off all the times so never uses
	 * it.  This seems to suggest that the workaround is insufficient.
	 */
	pci_read_config_word(tp->pdev, PCI_COMMAND, &pci_cmd);
	pci_cmd &= ~PCI_COMMAND_INVALIDATE;
	/* Also, force SERR#/PERR# in PCI command. */
	pci_cmd |= PCI_COMMAND_PARITY | PCI_COMMAND_SERR;
	pci_write_config_word(tp->pdev, PCI_COMMAND, pci_cmd);

	/* It is absolutely critical that TG3PCI_MISC_HOST_CTRL
	 * has the register indirect write enable bit set before
	 * we try to access any of the MMIO registers.  It is also
	 * critical that the PCI-X hw workaround situation is decided
	 * before that as well.
	 */
	pci_read_config_dword(tp->pdev, TG3PCI_MISC_HOST_CTRL,
			      &misc_ctrl_reg);

	tp->pci_chip_rev_id = (misc_ctrl_reg >>
			       MISC_HOST_CTRL_CHIPREV_SHIFT);

	/* Initialize misc host control in PCI block. */
	tp->misc_host_ctrl |= (misc_ctrl_reg &
			       MISC_HOST_CTRL_CHIPREV);
	pci_write_config_dword(tp->pdev, TG3PCI_MISC_HOST_CTRL,
			       tp->misc_host_ctrl);

	pci_read_config_dword(tp->pdev, TG3PCI_CACHELINESZ,
			      &cacheline_sz_reg);

	tp->pci_cacheline_sz = (cacheline_sz_reg >>  0) & 0xff;
	tp->pci_lat_timer    = (cacheline_sz_reg >>  8) & 0xff;
	tp->pci_hdr_type     = (cacheline_sz_reg >> 16) & 0xff;
	tp->pci_bist         = (cacheline_sz_reg >> 24) & 0xff;

	if (GET_ASIC_REV(tp->pci_chip_rev_id) == ASIC_REV_5703 &&
	    tp->pci_lat_timer < 64) {
		tp->pci_lat_timer = 64;

		cacheline_sz_reg  = ((tp->pci_cacheline_sz & 0xff) <<  0);
		cacheline_sz_reg |= ((tp->pci_lat_timer    & 0xff) <<  8);
		cacheline_sz_reg |= ((tp->pci_hdr_type     & 0xff) << 16);
		cacheline_sz_reg |= ((tp->pci_bist         & 0xff) << 24);

		pci_write_config_dword(tp->pdev, TG3PCI_CACHELINESZ,
				       cacheline_sz_reg);
	}

	pci_read_config_dword(tp->pdev, TG3PCI_PCISTATE,
			      &pci_state_reg);

	/* If this is a 5700 BX chipset, and we are in PCI-X
	 * mode, enable register write workaround.
	 *
	 * The workaround is to use indirect register accesses
	 * for all chip writes not to mailbox registers.
	 *
	 * In etherboot to simplify things we just always use this work around.
	 */
	if ((pci_state_reg & PCISTATE_CONV_PCI_MODE) == 0) {
		tp->tg3_flags |= TG3_FLAG_PCIX_MODE;
	}

	if ((pci_state_reg & PCISTATE_BUS_SPEED_HIGH) != 0)
		tp->tg3_flags |= TG3_FLAG_PCI_HIGH_SPEED;
	if ((pci_state_reg & PCISTATE_BUS_32BIT) != 0)
		tp->tg3_flags |= TG3_FLAG_PCI_32BIT;

	/* Force the chip into D0. */
	tg3_set_power_state_0(tp);

	/* Only 5701 and later support tagged irq status mode.
	 *
	 * However, since we are using NAPI avoid tagged irq status
	 * because the interrupt condition is more difficult to
	 * fully clear in that mode.
	 */
	tp->coalesce_mode = 0;

	if (GET_CHIP_REV(tp->pci_chip_rev_id) != CHIPREV_5700_AX &&
	    GET_CHIP_REV(tp->pci_chip_rev_id) != CHIPREV_5700_BX)
		tp->coalesce_mode |= HOSTCC_MODE_32BYTE;

	/* Initialize MAC MI mode, polling disabled. */
	tw32(MAC_MI_MODE, tp->mi_mode);
	tr32(MAC_MI_MODE);
	udelay(40);

	/* Initialize data/descriptor byte/word swapping. */
	tw32(GRC_MODE, tp->grc_mode);

	tg3_switch_clocks(tp);

	/* Clear this out for sanity. */
	tw32(TG3PCI_MEM_WIN_BASE_ADDR, 0);

	pci_read_config_dword(tp->pdev, TG3PCI_PCISTATE, &pci_state_reg);

	udelay(50);
	tg3_nvram_init(tp);

	/* The TX descriptors will reside in main memory.
	 */

	/* See which board we are using.
	 */
	grc_misc_cfg = tr32(GRC_MISC_CFG);
	grc_misc_cfg &= GRC_MISC_CFG_BOARD_ID_MASK;

	if (GET_ASIC_REV(tp->pci_chip_rev_id) == ASIC_REV_5704 &&
	    grc_misc_cfg == GRC_MISC_CFG_BOARD_ID_5704CIOBE) {
		tp->tg3_flags |= TG3_FLAG_SPLIT_MODE;
		tp->split_mode_max_reqs = SPLIT_MODE_5704_MAX_REQ;
	}

	/* The 5702FE doesn't do 100Mbit */
	if (grc_misc_cfg == GRC_MISC_CFG_BOARD_ID_5702FE)
		tp->tg3_flags |= TG3_FLAG_10_100_ONLY;

	err = tg3_phy_probe(tp);

	tg3_read_partno(tp);

	if (tp->phy_id == PHY_ID_SERDES) {
		tp->tg3_flags &= ~TG3_FLAG_USE_MI_INTERRUPT;

		/* And override led_mode in case Dell ever makes
		 * a fibre board.
		 */
		tp->led_mode = led_mode_three_link;
	} else {
		if (GET_ASIC_REV(tp->pci_chip_rev_id) == ASIC_REV_5700)
			tp->tg3_flags |= TG3_FLAG_USE_MI_INTERRUPT;
		else
			tp->tg3_flags &= ~TG3_FLAG_USE_MI_INTERRUPT;
	}

#if 1
	/* 5700 {AX,BX} chips have a broken status block link
	 * change bit implementation, so we must use the
	 * status register in those cases.
	 */
	if (GET_ASIC_REV(tp->pci_chip_rev_id) == ASIC_REV_5700)
		tp->tg3_flags |= TG3_FLAG_USE_LINKCHG_REG;
	else
		tp->tg3_flags &= ~TG3_FLAG_USE_LINKCHG_REG;

	/* The led_mode is set during tg3_phy_probe, here we might
	 * have to force the link status polling mechanism based
	 * upon subsystem IDs.
	 */
	if (tp->subsystem_vendor == PCI_VENDOR_ID_DELL &&
		tp->phy_id != PHY_ID_SERDES) {
		tp->tg3_flags |= (TG3_FLAG_USE_MI_INTERRUPT |
				  TG3_FLAG_USE_LINKCHG_REG);
	}

	/* For all SERDES we poll the MAC status register. */
	if (tp->phy_id == PHY_ID_SERDES)
		tp->tg3_flags |= TG3_FLAG_POLL_SERDES;
	else
		tp->tg3_flags &= ~TG3_FLAG_POLL_SERDES;

#else
	tp->tg3_flags |= TG3_FLAG_USE_LINKCHG_REG;
#endif

	/* 5700 BX chips need to have their TX producer index mailboxes
	 * written twice to workaround a bug.
	 * In etherboot we do this unconditionally to simplify thigns.
	 */

	/* 5700 chips can get confused if TX buffers straddle the
	 * 4GB address boundary in some cases.
	 * 
	 * It does not matter in etherboot as etherboot lives below 4GB.
	 */
	tp->rx_offset = 2;
	if (GET_ASIC_REV(tp->pci_chip_rev_id) == ASIC_REV_5701 &&
	    (tp->tg3_flags & TG3_FLAG_PCIX_MODE) != 0)
		tp->rx_offset = 0;

#if 1
	printf("ASIC_REV_5701: %d\n", 
		GET_ASIC_REV(tp->pci_chip_rev_id) == ASIC_REV_5701);
#endif

	return err;
}

static int  tg3_get_device_address(struct tg3 *tp)
{
	struct nic *nic = tp->nic;
	uint32_t hi, lo, mac_offset;

	if (PCI_FUNC(tp->pdev->devfn) == 0)
		mac_offset = 0x7c;
	else
		mac_offset = 0xcc;

	/* First try to get it from MAC address mailbox. */
	tg3_read_mem(tp, NIC_SRAM_MAC_ADDR_HIGH_MBOX, &hi);
	if ((hi >> 16) == 0x484b) {
		nic->node_addr[0] = (hi >>  8) & 0xff;
		nic->node_addr[1] = (hi >>  0) & 0xff;

		tg3_read_mem(tp, NIC_SRAM_MAC_ADDR_LOW_MBOX, &lo);
		nic->node_addr[2] = (lo >> 24) & 0xff;
		nic->node_addr[3] = (lo >> 16) & 0xff;
		nic->node_addr[4] = (lo >>  8) & 0xff;
		nic->node_addr[5] = (lo >>  0) & 0xff;
	}
	/* Next, try NVRAM. */
	else if (!tg3_nvram_read(tp, mac_offset + 0, &hi) &&
		 !tg3_nvram_read(tp, mac_offset + 4, &lo)) {
		nic->node_addr[0] = ((hi >> 16) & 0xff);
		nic->node_addr[1] = ((hi >> 24) & 0xff);
		nic->node_addr[2] = ((lo >>  0) & 0xff);
		nic->node_addr[3] = ((lo >>  8) & 0xff);
		nic->node_addr[4] = ((lo >> 16) & 0xff);
		nic->node_addr[5] = ((lo >> 24) & 0xff);
	}
	/* Finally just fetch it out of the MAC control regs. */
	else {
		hi = tr32(MAC_ADDR_0_HIGH);
		lo = tr32(MAC_ADDR_0_LOW);

		nic->node_addr[5] = lo & 0xff;
		nic->node_addr[4] = (lo >> 8) & 0xff;
		nic->node_addr[3] = (lo >> 16) & 0xff;
		nic->node_addr[2] = (lo >> 24) & 0xff;
		nic->node_addr[1] = hi & 0xff;
		nic->node_addr[0] = (hi >> 8) & 0xff;
	}

#warning "Add a test for a valid mac address"
#if 0
	if (!is_valid_ether_addr(&nic->node_addr[0]))
		return -1;
#endif

	return 0;
}


#if REALLY_TEST_DMA
static int tg3_do_test_dma(struct tg3 *tp, uint32_t *buf, dma_addr_t buf_dma, int size, int to_device)
{
	struct tg3_internal_buffer_desc test_desc;
	uint32_t sram_dma_descs;
	int i, ret;

	sram_dma_descs = NIC_SRAM_DMA_DESC_POOL_BASE;

	tw32(FTQ_RCVBD_COMP_FIFO_ENQDEQ, 0);
	tw32(FTQ_RCVDATA_COMP_FIFO_ENQDEQ, 0);
	tw32(RDMAC_STATUS, 0);
	tw32(WDMAC_STATUS, 0);

	tw32(BUFMGR_MODE, 0);
	tw32(FTQ_RESET, 0);

	/* pci_alloc_consistent gives only non-DAC addresses */
	test_desc.addr_hi = 0;
	test_desc.addr_lo = buf_dma & 0xffffffff;
	test_desc.nic_mbuf = 0x00002100;
	test_desc.len = size;
	if (to_device) {
		test_desc.cqid_sqid = (13 << 8) | 2;
		tw32(RDMAC_MODE, RDMAC_MODE_RESET);
		tr32(RDMAC_MODE);
		udelay(40);

		tw32(RDMAC_MODE, RDMAC_MODE_ENABLE);
		tr32(RDMAC_MODE);
		udelay(40);
	} else {
		test_desc.cqid_sqid = (16 << 8) | 7;
		tw32(WDMAC_MODE, WDMAC_MODE_RESET);
		tr32(WDMAC_MODE);
		udelay(40);

		tw32(WDMAC_MODE, WDMAC_MODE_ENABLE);
		tr32(WDMAC_MODE);
		udelay(40);
	}
	test_desc.flags = 0x00000004;

	for (i = 0; i < (sizeof(test_desc) / sizeof(uint32_t)); i++) {
		uint32_t val;

		val = *(((uint32_t *)&test_desc) + i);
		pci_write_config_dword(tp->pdev, TG3PCI_MEM_WIN_BASE_ADDR,
				       sram_dma_descs + (i * sizeof(uint32_t)));
		pci_write_config_dword(tp->pdev, TG3PCI_MEM_WIN_DATA, val);
	}
	pci_write_config_dword(tp->pdev, TG3PCI_MEM_WIN_BASE_ADDR, 0);

	if (to_device) {
		tw32(FTQ_DMA_HIGH_READ_FIFO_ENQDEQ, sram_dma_descs);
	} else {
		tw32(FTQ_DMA_HIGH_WRITE_FIFO_ENQDEQ, sram_dma_descs);
	}

	ret = -ENODEV;
	for (i = 0; i < 40; i++) {
		uint32_t val;

		if (to_device)
			val = tr32(FTQ_RCVBD_COMP_FIFO_ENQDEQ);
		else
			val = tr32(FTQ_RCVDATA_COMP_FIFO_ENQDEQ);
		if ((val & 0xffff) == sram_dma_descs) {
			ret = 0;
			break;
		}

		udelay(100);
	}

	return ret;
}
#endif /* REALLY_TEST_DMA */

#define TEST_BUFFER_SIZE	0x400

static int tg3_test_dma(struct tg3 *tp)
{
	static char buffer[TEST_BUFFER_SIZE] __attribute__ ((aligned(4096)));
	dma_addr_t buf_dma;
	uint32_t *buf;
#if REALLY_TEST_DMA
	int ret;
#endif /* REALLY_TEST_DMA */

	buf = (uint32_t *)&buffer;
	buf_dma = virt_to_bus(buf);

	tw32(TG3PCI_CLOCK_CTRL, 0);

	if ((tp->tg3_flags & TG3_FLAG_PCIX_MODE) == 0) {
		tp->dma_rwctrl =
			(0x7 << DMA_RWCTRL_PCI_WRITE_CMD_SHIFT) |
			(0x6 << DMA_RWCTRL_PCI_READ_CMD_SHIFT) |
			(0x7 << DMA_RWCTRL_WRITE_WATER_SHIFT) |
			(0x7 << DMA_RWCTRL_READ_WATER_SHIFT) |
			(0x0f << DMA_RWCTRL_MIN_DMA_SHIFT);
	} else {
		if (GET_ASIC_REV(tp->pci_chip_rev_id) == ASIC_REV_5704)
			tp->dma_rwctrl =
				(0x7 << DMA_RWCTRL_PCI_WRITE_CMD_SHIFT) |
				(0x6 << DMA_RWCTRL_PCI_READ_CMD_SHIFT) |
				(0x3 << DMA_RWCTRL_WRITE_WATER_SHIFT) |
				(0x7 << DMA_RWCTRL_READ_WATER_SHIFT) |
				(0x00 << DMA_RWCTRL_MIN_DMA_SHIFT);
		else
			tp->dma_rwctrl =
				(0x7 << DMA_RWCTRL_PCI_WRITE_CMD_SHIFT) |
				(0x6 << DMA_RWCTRL_PCI_READ_CMD_SHIFT) |
				(0x3 << DMA_RWCTRL_WRITE_WATER_SHIFT) |
				(0x3 << DMA_RWCTRL_READ_WATER_SHIFT) |
				(0x0f << DMA_RWCTRL_MIN_DMA_SHIFT);

		/* Wheee, some more chip bugs... */
		if (tp->pci_chip_rev_id == CHIPREV_ID_5703_A1 ||
		    tp->pci_chip_rev_id == CHIPREV_ID_5703_A2 ||
		    tp->pci_chip_rev_id == CHIPREV_ID_5703_A3 ||
		    tp->pci_chip_rev_id == CHIPREV_ID_5704_A0)
			tp->dma_rwctrl |= DMA_RWCTRL_ONE_DMA;
	}

	/* We don't do this on x86 because it seems to hurt performace.
	 * It does help things on other platforms though.
	 */
#if 0 && !defined(CONFIG_X86)
	{
		u8 byte;
		int cacheline_size;
		pci_read_config_byte(tp->pdev, PCI_CACHE_LINE_SIZE, &byte);

		if (byte == 0)
			cacheline_size = 1024;
		else
			cacheline_size = (int) byte * 4;

		tp->dma_rwctrl &= ~(DMA_RWCTRL_READ_BNDRY_MASK |
				    DMA_RWCTRL_WRITE_BNDRY_MASK);

		switch (cacheline_size) {
		case 16:
			tp->dma_rwctrl |=
				(DMA_RWCTRL_READ_BNDRY_16 |
				 DMA_RWCTRL_WRITE_BNDRY_16);
			break;

		case 32:
			tp->dma_rwctrl |=
				(DMA_RWCTRL_READ_BNDRY_32 |
				 DMA_RWCTRL_WRITE_BNDRY_32);
			break;

		case 64:
			tp->dma_rwctrl |=
				(DMA_RWCTRL_READ_BNDRY_64 |
				 DMA_RWCTRL_WRITE_BNDRY_64);
			break;

		case 128:
			tp->dma_rwctrl |=
				(DMA_RWCTRL_READ_BNDRY_128 |
				 DMA_RWCTRL_WRITE_BNDRY_128);
			break;

		case 256:
			tp->dma_rwctrl |=
				(DMA_RWCTRL_READ_BNDRY_256 |
				 DMA_RWCTRL_WRITE_BNDRY_256);
			break;

		case 512:
			tp->dma_rwctrl |=
				(DMA_RWCTRL_READ_BNDRY_512 |
				 DMA_RWCTRL_WRITE_BNDRY_512);
			break;

		case 1024:
			tp->dma_rwctrl |=
				(DMA_RWCTRL_READ_BNDRY_1024 |
				 DMA_RWCTRL_WRITE_BNDRY_1024);
			break;
		};
	}
#endif

	/* Remove this if it causes problems for some boards. */
	tp->dma_rwctrl |= DMA_RWCTRL_USE_MEM_READ_MULT;

	tw32(TG3PCI_DMA_RW_CTRL, tp->dma_rwctrl);

#if !REALLY_TEST_DMA
	return 0;
#else
	if (GET_ASIC_REV(tp->pci_chip_rev_id) != ASIC_REV_5700 &&
	    GET_ASIC_REV(tp->pci_chip_rev_id) != ASIC_REV_5701)
		return 0;

	ret = 0;
	while (1) {
		uint32_t *p, i;

		p = buf;
		for (i = 0; i < TEST_BUFFER_SIZE / sizeof(uint32_t); i++)
			p[i] = i;

		/* Send the buffer to the chip. */
		ret = tg3_do_test_dma(tp, buf, buf_dma, TEST_BUFFER_SIZE, 1);
		if (ret)
			break;

		p = buf;
		for (i = 0; i < TEST_BUFFER_SIZE / sizeof(uint32_t); i++)
			p[i] = 0;

		/* Now read it back. */
		ret = tg3_do_test_dma(tp, buf, buf_dma, TEST_BUFFER_SIZE, 0);
		if (ret)
			break;

		/* Verify it. */
		p = buf;
		for (i = 0; i < TEST_BUFFER_SIZE / sizeof(uint32_t); i++) {
			if (p[i] == i)
				continue;

			if ((tp->dma_rwctrl & DMA_RWCTRL_WRITE_BNDRY_MASK) ==
			    DMA_RWCTRL_WRITE_BNDRY_DISAB) {

				tp->dma_rwctrl |= DMA_RWCTRL_WRITE_BNDRY_16;
				tw32(TG3PCI_DMA_RW_CTRL, tp->dma_rwctrl);
				break;
			} else {

				ret = -ENODEV;
				goto out;
			}
		}

		if (i == (TEST_BUFFER_SIZE / sizeof(uint32_t))) {

			/* Success. */
			ret = 0;
			break;
		}
	}

out:
	return ret;
#endif /* REALLY_TEST_DMA */
}

static void tg3_init_link_config(struct tg3 *tp)
{
	tp->link_config.advertising =
		(ADVERTISED_10baseT_Half | ADVERTISED_10baseT_Full |
		 ADVERTISED_100baseT_Half | ADVERTISED_100baseT_Full |
		 ADVERTISED_1000baseT_Half | ADVERTISED_1000baseT_Full |
		 ADVERTISED_Autoneg | ADVERTISED_MII);
	tp->carrier_ok = 0;
	tp->link_config.active_speed = SPEED_INVALID;
	tp->link_config.active_duplex = DUPLEX_INVALID;
}

static void tg3_init_bufmgr_config(struct tg3 *tp)
{
	tp->bufmgr_config.mbuf_read_dma_low_water =
		DEFAULT_MB_RDMA_LOW_WATER;
	tp->bufmgr_config.mbuf_mac_rx_low_water =
		DEFAULT_MB_MACRX_LOW_WATER;
	tp->bufmgr_config.mbuf_high_water =
		DEFAULT_MB_HIGH_WATER;

	tp->bufmgr_config.dma_low_water = DEFAULT_DMA_LOW_WATER;
	tp->bufmgr_config.dma_high_water = DEFAULT_DMA_HIGH_WATER;
}

static const char * tg3_phy_string(struct tg3 *tp)
{
	switch (tp->phy_id & PHY_ID_MASK) {
	case PHY_ID_BCM5400:	return "5400";
	case PHY_ID_BCM5401:	return "5401";
	case PHY_ID_BCM5411:	return "5411";
	case PHY_ID_BCM5701:	return "5701";
	case PHY_ID_BCM5703:	return "5703";
	case PHY_ID_BCM5704:	return "5704";
	case PHY_ID_BCM8002:	return "8002";
	case PHY_ID_SERDES:	return "serdes";
	default:		return "unknown";
	};
}




/**************************************************************************
POLL - Wait for a frame
***************************************************************************/
static int tg3_poll(struct nic *nic)
{
	/* return true if there's an ethernet packet ready to read */
	/* nic->packet should contain data on return */
	/* nic->packetlen should contain length of data */

	struct tg3 *tp = &tg3;
	int result;
	uint32_t mac_stat;

	result = 0;


	if (tp->hw_status->status & SD_STATUS_UPDATED) {
		tw32_mailbox(MAILBOX_INTERRUPT_0 + TG3_64BIT_REG_LOW, 
			0x00000001);
		tp->hw_status->status &= ~SD_STATUS_UPDATED;

		if (tp->hw_status->status & SD_STATUS_LINK_CHG) {
			tp->hw_status->status = SD_STATUS_UPDATED |
				(tp->hw_status->status & ~SD_STATUS_LINK_CHG);
			tg3_setup_phy(tp);
		}

		if (tp->hw_status->idx[0].rx_producer != tp->rx_rcb_ptr) {
			struct tg3_rx_buffer_desc *desc;
			unsigned int len;
			desc = &tp->rx_rcb[tp->rx_rcb_ptr];
			if ((desc->opaque & RXD_OPAQUE_RING_MASK) == RXD_OPAQUE_RING_STD) {
				len = ((desc->idx_len & RXD_LEN_MASK) >> RXD_LEN_SHIFT) - 4; /* omit crc */

				/* FIXME handle rx_offset */
#warning "FIXME handler tp->rx_offset"
				nic->packetlen = len;
				memcpy(nic->packet, bus_to_virt(desc->addr_lo), len);
				result = 1;
				
				tp->rx_std_ptr = (tp->rx_std_ptr + 1) % TG3_RX_RING_SIZE;
				/* Refill RX rings(s). */
				tw32_mailbox(MAILBOX_RCV_STD_PROD_IDX + TG3_64BIT_REG_LOW, 
					tp->rx_std_ptr);
				tr32(MAILBOX_RCV_STD_PROD_IDX + TG3_64BIT_REG_LOW);
			}
			tp->rx_rcb_ptr = (tp->rx_rcb_ptr + 1) % TG3_RX_RCB_RING_SIZE;

			/* ACK the status ring */
			tw32_mailbox(MAILBOX_RCVRET_CON_IDX_0 + TG3_64BIT_REG_LOW, 
				tp->rx_rcb_ptr);
			tr32(MAILBOX_RCVRET_CON_IDX_0 + TG3_64BIT_REG_LOW);
		}
		

		tw32_mailbox(MAILBOX_INTERRUPT_0 + TG3_64BIT_REG_LOW,
			0x00000000);
		tr32(MAILBOX_INTERRUPT_0 + TG3_64BIT_REG_LOW);
	}
#if 0
	mac_stat = tr32(MAC_STATUS);
	if (mac_stat & (MAC_STATUS_LNKSTATE_CHANGED | MAC_STATUS_PCS_SYNCED | MAC_STATUS_MI_INTERRUPT)) {
		if (tp->phy_id == PHY_ID_SERDES) {
			tw32(MAC_MODE, tp->mac_mode & ~MAC_MODE_PORT_MODE_MASK);
			tr32(MAC_MODE);
			udelay(40);
			tw32(MAC_MODE, tp->mac_mode);
			tr32(MAC_MODE);
			udelay(40);
		}
		tg3_setup_phy(tp);
	}
#endif
	return result;
}

/**************************************************************************
TRANSMIT - Transmit a frame
***************************************************************************/
static void tg3_transmit(struct nic *nic, const char *dst_addr, 
	unsigned int type, unsigned int size, const char *packet)
{
	/* send the packet to destination */
	struct eth_hdr {
		unsigned char dst_addr[ETH_ALEN];
		unsigned char src_addr[ETH_ALEN];
		unsigned short type;
	} hdr;
	uint32_t entry;
	struct tg3 *tp = &tg3;
	struct tg3_tx_buffer_desc *txd;
	int i;

	memcpy(&hdr.dst_addr, dst_addr, ETH_ALEN);
	memcpy(&hdr.src_addr, nic->node_addr, ETH_ALEN);
	hdr.type = htons(type);

	entry = tp->tx_prod;
	txd = &tp->tx_ring[entry];
	txd->addr_hi   = 0;
	txd->addr_lo   = virt_to_bus(&hdr);
	txd->len_flags = (sizeof(hdr) << TXD_LEN_SHIFT) | 0;
	txd->vlan_tag  = 0;

	entry = NEXT_TX(entry);
	txd = &tp->tx_ring[entry];
	txd->addr_hi   = 0;
	txd->addr_lo   = virt_to_bus(packet);
	txd->len_flags = (size << TXD_LEN_SHIFT) | TXD_FLAG_END;
	txd->vlan_tag  = 0;

	entry = NEXT_TX(entry);

	tw32_mailbox((MAILBOX_SNDHOST_PROD_IDX_0 + TG3_64BIT_REG_LOW), entry);
	tw32_mailbox((MAILBOX_SNDHOST_PROD_IDX_0 + TG3_64BIT_REG_LOW), entry);

	tr32(MAILBOX_SNDHOST_PROD_IDX_0 + TG3_64BIT_REG_LOW);
	tp->tx_prod = entry;

	i = 0; 
	/* Wait until the transmission completes */
	while(tp->hw_status->idx[0].tx_consumer != entry) {
		udelay(10);	/* give the nick a chance */
		poll_interruptions();
		if (++i > 500000) { /* timeout 5s for transmit */
			printf("transmit timed out\n");
			tg3_halt(tp);
			tg3_init_rings(tp);
			tg3_init_hw(tp);
			break;
		}
	}
}

/**************************************************************************
DISABLE - Turn off ethernet interface
***************************************************************************/
static void tg3_disable(struct dev *dev __unused)
{
	struct tg3 *tp = &tg3;
	/* put the card in its initial state */
	/* This function serves 3 purposes.
	 * This disables DMA and interrupts so we don't receive
	 *  unexpected packets or interrupts from the card after
	 *  etherboot has finished. 
	 * This frees resources so etherboot may use
	 *  this driver on another interface
	 * This allows etherboot to reinitialize the interface
	 *  if something is something goes wrong.
	 */
	tg3_halt(tp);
	tp->tg3_flags &= ~(TG3_FLAG_INIT_COMPLETE|TG3_FLAG_GOT_SERDES_FLOWCTL);
	tp->carrier_ok = 0;
	iounmap((void *)tp->regs);
}



/**************************************************************************
PROBE - Look for an adapter, this routine's visible to the outside
You should omit the last argument struct pci_device * for a non-PCI NIC
***************************************************************************/
static int tg3_probe(struct dev *dev, struct pci_device *pdev)
{
	struct nic *nic = (struct nic *)dev;
	struct tg3 *tp = &tg3;
	unsigned long tg3reg_base, tg3reg_len;
	int i, err, pm_cap;

	if (pdev == 0)
		return 0;

	memset(tp, 0, sizeof(*tp));

	adjust_pci_device(pdev);

	/* Find power-management capability. */
	pm_cap = pci_find_capability(pdev, PCI_CAP_ID_PM);
	if (pm_cap == 0) {
		printf("Cannot find PowerManagement capability, aborting.\n");
		return 0;
	}

	tg3reg_base = pci_bar_start(pdev, PCI_BASE_ADDRESS_0);
	tg3reg_len  = pci_bar_size(pdev,  PCI_BASE_ADDRESS_0);

	tp->pdev       = pdev;
	tp->nic        = nic;
	tp->pm_cap     = pm_cap;
	tp->mac_mode   = TG3_DEF_MAC_MODE;
	tp->rx_mode    = TG3_DEF_RX_MODE;
	tp->tx_mode    = TG3_DEF_TX_MODE;
	tp->mi_mode    = MAC_MI_MODE_BASE;
	tp->tg3_flags &= ~TG3_FLAG_INIT_COMPLETE;
	
	/* The word/byte swap controls here control register access byte
	 * swapping.  DMA data byte swapping is controlled in the GRC_MODE
	 * setting below.
	 */
	tp->misc_host_ctrl =
		MISC_HOST_CTRL_MASK_PCI_INT |
		MISC_HOST_CTRL_WORD_SWAP |
		MISC_HOST_CTRL_INDIR_ACCESS |
		MISC_HOST_CTRL_PCISTATE_RW;

	/* The NONFRM (non-frame) byte/word swap controls take effect
	 * on descriptor entries, anything which isn't packet data.
	 *
	 * The StrongARM chips on the board (one for tx, one for rx)
	 * are running in big-endian mode.
	 */
	tp->grc_mode = (GRC_MODE_WSWAP_DATA | GRC_MODE_BSWAP_DATA |
			GRC_MODE_WSWAP_NONFRM_DATA);
#if __BYTE_ORDER == __BIG_ENDIAN
	tp->grc_mode |= GRC_MODE_BSWAP_NONFRM_DATA;
#endif
	tp->regs = (unsigned long) ioremap(tg3reg_base, tg3reg_len);
	if (tp->regs == 0UL) {
		printf("Cannot map device registers, aborting\n");
		return 0;
	}
	
	tg3_init_link_config(tp);
	tg3_init_bufmgr_config(tp);
	
	tp->rx_pending = TG3_DEF_RX_RING_PENDING;

	err = tg3_get_invariants(tp);
	if (err) {
		printf("Problem fetching invariants of chip, aborting.\n");
		goto err_out_iounmap;
	}

	err = tg3_get_device_address(tp);
	if (err) {
		printf("Could not obtain valid ethernet address, aborting.\n");
		goto err_out_iounmap;
	}
	printf("Ethernet addr: %!\n", nic->node_addr);

	err = tg3_test_dma(tp);
	if (err) {
		printf("DMA engine test failed, aborting.\n");
		goto err_out_iounmap;
	}
	tp->tg3_flags &= ~TG3_FLAG_RX_CHECKSUMS;


	pci_save_state(tp->pdev, tp->pci_cfg_state);

	printf("Tigon3 [partno(%s) rev %hx PHY(%s)] (PCI%s:%s:%s)\n",
		tp->board_part_number,
		tp->pci_chip_rev_id,
		tg3_phy_string(tp),
		((tp->tg3_flags & TG3_FLAG_PCIX_MODE) ? "X" : ""),
		((tp->tg3_flags & TG3_FLAG_PCI_HIGH_SPEED) ?
			((tp->tg3_flags & TG3_FLAG_PCIX_MODE) ? "133MHz" : "66MHz") :
			((tp->tg3_flags & TG3_FLAG_PCIX_MODE) ? "100MHz" : "33MHz")),
		((tp->tg3_flags & TG3_FLAG_PCI_32BIT) ? "32-bit" : "64-bit"));


	tg3_disable_ints(tp);

	/* If you move this call, make sure TG3_FLAG_HOST_TXDS in
	 * tp->tg3_flags is accurate at that new place.
	 */
	tg3_alloc_consistent(tp);

	tg3_init_rings(tp);
	
	err = tg3_init_hw(tp);
	if (err) {
		goto err_out_disable;
	} 
	tp->tg3_flags |= TG3_FLAG_INIT_COMPLETE;

	for(i = 0; !tp->carrier_ok && (i < 3*1000); i++) {
#if 1
		tg3_poll(nic);
#else
		tg3_setup_phy(tp);
#endif
		mdelay(1);
	}
	if (!tp->carrier_ok){
		printf("Valid link not established\n");
		goto err_out_disable;
	}

	dev->disable  = tg3_disable;
	nic->poll     = tg3_poll;
	nic->transmit = tg3_transmit;
	return 1;

 err_out_iounmap:
	iounmap((void *)tp->regs);
	return 0;
 err_out_disable:
	tg3_disable(dev);
	return 0;
}

int after_tg3_probe(int foo)
{
	return foo +1;
}

static struct pci_id tg3_nics[] = {
PCI_ROM(0x14e4, 0x1644, "tg3-5700",       "Broadcom Tigon 3 5700" ),
PCI_ROM(0x14e4, 0x1645, "tg3-5701",       "Broadcom Tigon 3 5701" ),
PCI_ROM(0x14e4, 0x1646, "tg3-5702",       "Broadcom Tigon 3 5702" ),
PCI_ROM(0x14e4, 0x1647, "tg3-5703",       "Broadcom Tigon 3 5703" ),
PCI_ROM(0x14e4, 0x1648, "tg3-5704",       "Broadcom Tigon 3 5704" ),
PCI_ROM(0x14e4, 0x164d, "tg3-5702FE",     "Broadcom Tigon 3 5702FE" ),
PCI_ROM(0x14e4, 0x16a6, "tg3-5702X",      "Broadcom Tigon 3 5702X" ),
PCI_ROM(0x14e4, 0x16a7, "tg3-5703X",      "Broadcom Tigon 3 5703X" ),
PCI_ROM(0x1148, 0x4400, "tg3-syskonnect", "Sysconnet Tigon 3" ),
PCI_ROM(0x173b, 0x1644, "tg3-ac1000",     "Altima AC1000" ),
PCI_ROM(0x173b, 0x1644, "tg3-ac9100",     "Altima AC9100" ),
};

static struct pci_driver tg3_driver __pci_driver = {
	.type	  = NIC_DRIVER,
	.name	  = "TG3",
	.probe	  = tg3_probe,
	.ids	  = tg3_nics,
	.id_count = sizeof(tg3_nics)/sizeof(tg3_nics[0]),
	.class    = 0,
};
