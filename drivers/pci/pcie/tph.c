// SPDX-License-Identifier: GPL-2.0
/*
 * TPH (TLP Processing Hints) support
 *
 * Copyright (C) 2023 Advanced Micro Devices, Inc.
 *     Eric van Tassell <Eric.VanTassell@amd.com>
 *     Wei Huang <wei.huang2@amd.com>
 */

#define pr_fmt(fmt) "TPH: " fmt
#define dev_fmt pr_fmt

#include <uapi/linux/pci_regs.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/msi.h>
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

static int tph_get_reg_field_u32(struct pci_dev *dev, u8 offset, u32 mask,
				 u8 shift, u32 *field)
{
	u32 reg_val;
	int ret;

	if (!dev->tph_cap)
		return -EINVAL;

	ret = pci_read_config_dword(dev, dev->tph_cap + offset, &reg_val);
	if (ret)
		return ret;

	*field = (reg_val & mask) >> shift;

	return 0;
}

static int tph_get_table_size(struct pci_dev *dev, u16 *size_out)
{
	int ret;
	u32 tmp;

	ret = tph_get_reg_field_u32(dev, PCI_TPH_CAP,
				    PCI_TPH_CAP_ST_MASK,
				    PCI_TPH_CAP_ST_SHIFT, &tmp);
	if (ret)
		return ret;

	*size_out = (u16)tmp;

	return 0;
}

/*
 * For a given device, return a pointer to the MSI table entry at msi_index.
 */
static void __iomem *tph_msix_table_entry(struct pci_dev *dev,
					  __le16 msi_index)
{
	void *entry;
	u16 tbl_sz;
	int ret;

	ret = tph_get_table_size(dev, &tbl_sz);
	if (ret || msi_index > tbl_sz)
		return NULL;

	entry = dev->msix_base + msi_index * PCI_MSIX_ENTRY_SIZE;

	return entry;
}

/*
 * For a given device, return a pointer to the vector control register at
 * offset 0xc of MSI table entry at msi_index.
 */
static void __iomem *tph_msix_vector_control(struct pci_dev *dev,
					     __le16 msi_index)
{
	void __iomem *vec_ctrl_addr = tph_msix_table_entry(dev, msi_index);

	if (vec_ctrl_addr)
		vec_ctrl_addr += PCI_MSIX_ENTRY_VECTOR_CTRL;

	return vec_ctrl_addr;
}

/*
 * Translate from MSI-X interrupt index to struct msi_desc *
 */
static struct msi_desc *tph_msix_index_to_desc(struct pci_dev *dev, int index)
{
	struct msi_desc *entry;

	msi_lock_descs(&dev->dev);
	msi_for_each_desc(entry, &dev->dev, MSI_DESC_ASSOCIATED) {
		if (entry->msi_index == index)
			return entry;
	}
	msi_unlock_descs(&dev->dev);

	return NULL;
}

static bool tph_int_vec_mode_supported(struct pci_dev *dev)
{
	u32 tmp;
	int ret;

	ret = tph_get_reg_field_u32(dev, PCI_TPH_CAP,
				    PCI_TPH_CAP_INT_VEC,
				    PCI_TPH_CAP_INT_VEC_SHIFT, &tmp);
	if (ret)
		return false;

	return true;
}

static int tph_get_table_location(struct pci_dev *dev, u8 *loc_out)
{
	u32 tmp;
	int ret;

	ret = tph_get_reg_field_u32(dev, PCI_TPH_CAP, PCI_TPH_CAP_LOC_MASK,
				    PCI_TPH_CAP_LOC_SHIFT, &tmp);
	if (ret)
		return ret;

	*loc_out = tmp;

	return 0;
}

static u16 pcie_tph_retrieve_st(struct pci_dev *dev, unsigned int cpu,
				enum tph_mem_type tag_type, u8 req_enable)
{
	return 0;
}

static bool msix_nr_in_bounds(struct pci_dev *dev, int msix_nr)
{
	u16 tbl_sz;

	if (tph_get_table_size(dev, &tbl_sz))
		return false;

	return msix_nr <= tbl_sz;
}

static int get_root_port_completer_cap(struct pci_dev *dev)
{
	struct pci_dev *rp;
	int ret;
	int val;

	rp = pcie_find_root_port(dev);
	if (!rp) {
		pr_err("cannot find root port of %s\n", dev_name(&dev->dev));
		return 0;
	}

	ret = pcie_capability_read_dword(rp, PCI_EXP_DEVCAP2, &val);
	if (ret) {
		pr_err("cannot read device capabilities 2 of %s\n",
		       dev_name(&dev->dev));
		return 0;
	}

	val &= PCI_EXP_DEVCAP2_TPH_COMP;

	return val >> PCI_EXP_DEVCAP2_TPH_COMP_SHIFT;
}

/*
 * TPH device needs to be below a rootport with the TPH Completer and
 * the completer must offer a compatible level of completer support to that
 * requested by the device driver.
 */
static bool completer_support_ok(struct pci_dev *dev,
				 u8 req)
{
	int rp_cap;

	rp_cap = get_root_port_completer_cap(dev);

	if (req > rp_cap) {
		pr_err("root port lacks proper TPH completer capability\n");
		return false;
	}

	return true;
}

/*
 * The PCI Specification version 5.0 requires the "No ST Mode" mode
 * be supported by any compatible device.
 */
static bool no_st_mode_supported(struct pci_dev *dev)
{
	bool no_st;
	int ret;
	u32 tmp;

	ret = tph_get_reg_field_u32(dev, PCI_TPH_CAP, PCI_TPH_CAP_NO_ST,
				    PCI_TPH_CAP_NO_ST_SHIFT, &tmp);
	if (ret)
		return false;

	no_st = (bool)tmp;

	if (!no_st) {
		pr_err("TPH devices must support no ST mode\n");
		return false;
	}

	return true;
}

/* Sanity check before setting steering tags */
static bool can_set_st(struct pci_dev *dev, u8 st_mode, u8 req_enable,
		       int msix_nr)
{
	if (!dev->tph_cap || !dev->msix_enabled ||
	    !tph_int_vec_mode_supported(dev) || pci_tph_disabled() ||
	    pci_tph_nostmode() || !msix_nr_in_bounds(dev, msix_nr) ||
	    !completer_support_ok(dev, req_enable) ||
	    !no_st_mode_supported(dev))
		return false;

	return true;
}

static int tph_write_ctrl_reg(struct pci_dev *dev, u32 value)
{
	int ret;

	ret = tph_set_reg_field_u32(dev, PCI_TPH_CTRL, ~0L, 0, value);

	if (ret)
		goto err_out;

	return 0;

err_out:
	/* minimizing possible harm by disabling TPH.*/
	pcie_tph_disable(dev);
	return ret;
}

/* Update the ST Mode Select field of the TPH Control Register */
static int tph_set_ctrl_reg_mode_sel(struct pci_dev *dev, u8 st_mode)
{
	int ret;
	u32 ctrl_reg;

	ret = tph_get_reg_field_u32(dev, PCI_TPH_CTRL, ~0L, 0, &ctrl_reg);
	if (ret)
		return ret;

	/* clear the mode select and enable fields */
	ctrl_reg &= ~(PCI_TPH_CTRL_MODE_SEL_MASK);
	ctrl_reg |= ((u32)(st_mode << PCI_TPH_CTRL_MODE_SEL_SHIFT) &
		     PCI_TPH_CTRL_MODE_SEL_MASK);

	ret = tph_write_ctrl_reg(dev, ctrl_reg);
	if (ret)
		return ret;

	return 0;
}

/* Write the steering tag to MSI-X vector control register */
static void tph_write_tag_to_msix(struct pci_dev *dev, int msix_nr, u16 tagval)
{
	u32 val;
	void __iomem *vec_ctrl;
	struct msi_desc *msi_desc;

	msi_desc = tph_msix_index_to_desc(dev, msix_nr);
	if (!msi_desc) {
		pr_err("MSI-X descriptor for #%d not found\n", msix_nr);
		return;
	}

	vec_ctrl = tph_msix_vector_control(dev, msi_desc->msi_index);

	val = readl(vec_ctrl);
	val &= 0xffff;
	val |= (tagval << 16);
	writel(val, vec_ctrl);

	/* read back to flush the update */
	val = readl(vec_ctrl);
	msi_unlock_descs(&dev->dev);
}

/* Update the TPH Requester Enable field of the TPH Control Register */
static int tph_set_ctrl_reg_en(struct pci_dev *dev, u8 req_enable)
{
	int ret;
	u32 ctrl_reg;

	ret = tph_get_reg_field_u32(dev, PCI_TPH_CTRL, ~0L, 0,
				    &ctrl_reg);
	if (ret)
		return ret;

	/* clear the mode select and enable fields and set new values*/
	ctrl_reg &= ~(PCI_TPH_CTRL_REQ_EN_MASK);
	ctrl_reg |= (((u32)req_enable << PCI_TPH_CTRL_REQ_EN_SHIFT) &
			PCI_TPH_CTRL_REQ_EN_MASK);

	ret = tph_write_ctrl_reg(dev, ctrl_reg);
	if (ret)
		return ret;

	return 0;
}

static bool pcie_tph_write_st(struct pci_dev *dev, unsigned int msix_nr,
			      u8 req_enable, u16 tag)
{
	int offset;
	u8  loc;
	int ret;

	/* check if updating steering tag is ok */
	if (!can_set_st(dev, PCI_TPH_INT_VEC_MODE, req_enable, msix_nr))
		return false;

	/*
	 * disable TPH before updating the tag to avoid potential instability
	 * as cautioned about in the "ST Table Programming" of PCI-E spec
	 */
	pcie_tph_disable(dev);

	ret = tph_get_table_location(dev, &loc);
	if (ret)
		return false;

	switch (loc) {
	case PCI_TPH_LOC_MSIX:
		tph_write_tag_to_msix(dev, msix_nr, tag);
		break;
	case PCI_TPH_LOC_CAP:
		offset = dev->tph_cap + PCI_TPH_ST_TABLE
			  + msix_nr * sizeof(u16);
		pci_write_config_word(dev, offset, tag);
		break;
	default:
		pr_err("unable to write steering tag for device %s\n",
		       dev_name(&dev->dev));
		return false;
	}

	/* select interrupt vector mode */
	tph_set_ctrl_reg_mode_sel(dev, PCI_TPH_INT_VEC_MODE);
	tph_set_ctrl_reg_en(dev, req_enable);

	return true;
}

int tph_set_dev_nostmode(struct pci_dev *dev)
{
	int ret;

	/* set ST Mode Select to "No ST Mode" */
	ret = tph_set_reg_field_u32(dev, PCI_TPH_CTRL,
				    PCI_TPH_CTRL_MODE_SEL_MASK,
				    PCI_TPH_CTRL_MODE_SEL_SHIFT,
				    PCI_TPH_NO_ST_MODE);
	if (ret)
		return ret;

	/* set "TPH Requester Enable" to "TPH only" */
	ret = tph_set_reg_field_u32(dev, PCI_TPH_CTRL,
				    PCI_TPH_CTRL_REQ_EN_MASK,
				    PCI_TPH_CTRL_REQ_EN_SHIFT,
				    PCI_TPH_REQ_TPH_ONLY);

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

/**
 * pcie_tph_set_st() - Set steering tag in ST table entry
 * @dev: pci device
 * @msix_nr: ordinal number of msix interrupt.
 * @cpu: the acpi cpu_uid.
 * @tag_type: memory type (vram, nvram)
 * @req_enable: request type (disable, tph, extended tph)
 *
 * Return:
 *        true : success
 *        false: the error code (ex: -EINVAL)
 */
bool pcie_tph_set_st(struct pci_dev *dev, unsigned int msix_nr,
		     unsigned int cpu, enum tph_mem_type tag_type,
		     u8 req_enable)
{
	u16 tag;
	bool ret;

	tag = pcie_tph_retrieve_st(dev, cpu, tag_type, req_enable);
	ret = pcie_tph_write_st(dev, msix_nr, req_enable, tag);

	return ret;
}
EXPORT_SYMBOL(pcie_tph_set_st);
#endif
