// SPDX-License-Identifier: GPL-2.0
/*
 * TPH (TLP Processing Hints) support
 *
 * Copyright (C) 2023 Advanced Micro Devices, Inc.
 *     Eric van Tassell <Eric.VanTassell@amd.com>
 *     Wei Huang <wei.huang2@amd.com>
 */
#include <uapi/linux/pci_regs.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/pci-tph.h>

#ifdef CONFIG_PCIE_TPH

static int tph_set_reg_field_u32(struct pci_dev *dev, u8 offset, u32 mask,
				 u8 shift, u32 field)
{
	u32 reg_val;
	int ret;

	if (!dev->tph_cap)
		return -EINVAL;

	ret = pci_read_config_dword(dev, dev->tph_cap + offset, &reg_val);
	if (ret)
		return ret;

	reg_val &= ~mask;
	reg_val |= (field << shift) & mask;

	ret = pci_write_config_dword(dev, dev->tph_cap + offset, reg_val);

	return ret;
}

int pcie_tph_disable(struct pci_dev *dev)
{
	return  tph_set_reg_field_u32(dev, PCI_TPH_CTRL,
				      PCI_TPH_CTRL_REQ_EN_MASK,
				      PCI_TPH_CTRL_REQ_EN_SHIFT,
				      PCI_TPH_REQ_DISABLE);
}

void pcie_tph_init(struct pci_dev *dev)
{
	dev->tph_cap = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_TPH);
}
#endif
