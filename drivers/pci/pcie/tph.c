// SPDX-License-Identifier: GPL-2.0
/*
 * TPH (TLP Processing Hints) support
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc.
 *     Eric Van Tassell <Eric.VanTassell@amd.com>
 *     Wei Huang <wei.huang2@amd.com>
 */
#include <linux/pci.h>
#include <linux/pci-acpi.h>
#include <linux/bitfield.h>
#include <linux/msi.h>
#include <linux/pci-tph.h>

#include "../pci.h"

/*
 * The st_info struct defines the Steering Tag (ST) info returned by the
 * firmware _DSM method defined in the approved ECN for PCI Firmware Spec,
 * available at https://members.pcisig.com/wg/PCI-SIG/document/15470.
 *
 * @vm_st_valid:  8-bit ST for volatile memory is valid
 * @vm_xst_valid: 16-bit extended ST for volatile memory is valid
 * @vm_ph_ignore: 1 => PH was and will be ignored, 0 => PH should be supplied
 * @vm_st:        8-bit ST for volatile mem
 * @vm_xst:       16-bit extended ST for volatile mem
 * @pm_st_valid:  8-bit ST for persistent memory is valid
 * @pm_xst_valid: 16-bit extended ST for persistent memory is valid
 * @pm_ph_ignore: 1 => PH was and will be ignored, 0 => PH should be supplied
 * @pm_st:        8-bit ST for persistent mem
 * @pm_xst:       16-bit extended ST for persistent mem
 */
union st_info {
	struct {
		u64 vm_st_valid : 1;
		u64 vm_xst_valid : 1;
		u64 vm_ph_ignore : 1;
		u64 rsvd1 : 5;
		u64 vm_st : 8;
		u64 vm_xst : 16;
		u64 pm_st_valid : 1;
		u64 pm_xst_valid : 1;
		u64 pm_ph_ignore : 1;
		u64 rsvd2 : 5;
		u64 pm_st : 8;
		u64 pm_xst : 16;
	};
	u64 value;
};

static u16 tph_extract_tag(enum tph_mem_type mem_type, u8 req_type,
			   union st_info *info)
{
	switch (req_type) {
	case PCI_TPH_REQ_TPH_ONLY: /* 8-bit tag */
		switch (mem_type) {
		case TPH_MEM_TYPE_VM:
			if (info->vm_st_valid)
				return info->vm_st;
			break;
		case TPH_MEM_TYPE_PM:
			if (info->pm_st_valid)
				return info->pm_st;
			break;
		}
		break;
	case PCI_TPH_REQ_EXT_TPH: /* 16-bit tag */
		switch (mem_type) {
		case TPH_MEM_TYPE_VM:
			if (info->vm_xst_valid)
				return info->vm_xst;
			break;
		case TPH_MEM_TYPE_PM:
			if (info->pm_xst_valid)
				return info->pm_xst;
			break;
		}
		break;
	default:
		return 0;
	}

	return 0;
}

#define TPH_ST_DSM_FUNC_INDEX	0xF
static acpi_status tph_invoke_dsm(acpi_handle handle, u32 cpu_uid,
				  union st_info *st_out)
{
	union acpi_object arg3[3], in_obj, *out_obj;

	if (!acpi_check_dsm(handle, &pci_acpi_dsm_guid, 7,
			    BIT(TPH_ST_DSM_FUNC_INDEX)))
		return AE_ERROR;

	/* DWORD: feature ID (0 for processor cache ST query) */
	arg3[0].integer.type = ACPI_TYPE_INTEGER;
	arg3[0].integer.value = 0;

	/* DWORD: target UID */
	arg3[1].integer.type = ACPI_TYPE_INTEGER;
	arg3[1].integer.value = cpu_uid;

	/* QWORD: properties, all 0's */
	arg3[2].integer.type = ACPI_TYPE_INTEGER;
	arg3[2].integer.value = 0;

	in_obj.type = ACPI_TYPE_PACKAGE;
	in_obj.package.count = ARRAY_SIZE(arg3);
	in_obj.package.elements = arg3;

	out_obj = acpi_evaluate_dsm(handle, &pci_acpi_dsm_guid, 7,
				    TPH_ST_DSM_FUNC_INDEX, &in_obj);
	if (!out_obj)
		return AE_ERROR;

	if (out_obj->type != ACPI_TYPE_BUFFER) {
		ACPI_FREE(out_obj);
		return AE_ERROR;
	}

	st_out->value = *((u64 *)(out_obj->buffer.pointer));

	ACPI_FREE(out_obj);

	return AE_OK;
}

/* Update the TPH Requester Enable field of TPH Control Register */
static void set_ctrl_reg_req_en(struct pci_dev *pdev, u8 req_type)
{
	u32 reg;

	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, &reg);

	reg &= ~PCI_TPH_CTRL_REQ_EN_MASK;
	reg |= FIELD_PREP(PCI_TPH_CTRL_REQ_EN_MASK, req_type);

	pci_write_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, reg);
}

static u8 get_st_modes(struct pci_dev *pdev)
{
	u32 reg;

	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CAP, &reg);
	reg &= PCI_TPH_CAP_NO_ST | PCI_TPH_CAP_INT_VEC | PCI_TPH_CAP_DEV_SPEC;

	return reg;
}

static u32 get_st_table_loc(struct pci_dev *pdev)
{
	u32 reg;

	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CAP, &reg);

	return FIELD_GET(PCI_TPH_CAP_LOC_MASK, reg);
}

/*
 * Return the size of ST table. If ST table is not in TPH Requester Extended
 * Capability space, return 0. Otherwise return the ST Table Size + 1.
 */
static u16 get_st_table_size(struct pci_dev *pdev)
{
	u32 reg;
	u32 loc;

	/* Check ST table location first */
	loc = get_st_table_loc(pdev);

	/* Convert loc to match with PCI_TPH_LOC_* defined in pci_regs.h */
	loc = FIELD_PREP(PCI_TPH_CAP_LOC_MASK, loc);
	if (loc != PCI_TPH_LOC_CAP)
		return 0;

	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CAP, &reg);

	return FIELD_GET(PCI_TPH_CAP_ST_MASK, reg) + 1;
}

/* Return device's Root Port completer capability */
static u8 get_rp_completer_type(struct pci_dev *pdev)
{
	struct pci_dev *rp;
	u32 reg;
	int ret;

	rp = pcie_find_root_port(pdev);
	if (!rp)
		return 0;

	ret = pcie_capability_read_dword(rp, PCI_EXP_DEVCAP2, &reg);
	if (ret)
		return 0;

	return FIELD_GET(PCI_EXP_DEVCAP2_TPH_COMP_MASK, reg);
}

/* Write ST to MSI-X vector control reg - Return 0 if OK, otherwise -errno */
static int write_tag_to_msix(struct pci_dev *pdev, int msix_idx, u16 tag)
{
	struct msi_desc *msi_desc = NULL;
	void __iomem *vec_ctrl;
	u32 val, mask;
	int err = 0;

	msi_lock_descs(&pdev->dev);

	/* Find the msi_desc entry with matching msix_idx */
	msi_for_each_desc(msi_desc, &pdev->dev, MSI_DESC_ASSOCIATED) {
		if (msi_desc->msi_index == msix_idx)
			break;
	}

	if (!msi_desc) {
		err = -ENXIO;
		goto err_out;
	}

	/* Get the vector control register (offset 0xc) pointed by msix_idx */
	vec_ctrl = pdev->msix_base + msix_idx * PCI_MSIX_ENTRY_SIZE;
	vec_ctrl += PCI_MSIX_ENTRY_VECTOR_CTRL;

	val = readl(vec_ctrl);
	mask = PCI_MSIX_ENTRY_CTRL_ST_LOWER | PCI_MSIX_ENTRY_CTRL_ST_UPPER;
	val &= ~mask;
	val |= FIELD_PREP(mask, (u32)tag);
	writel(val, vec_ctrl);

	/* Read back to flush the update */
	val = readl(vec_ctrl);

err_out:
	msi_unlock_descs(&pdev->dev);
	return err;
}

/* Write tag to ST table - Return 0 if OK, otherwise errno */
static int write_tag_to_st_table(struct pci_dev *pdev, int index, u16 tag)
{
	int st_table_size;
	int offset;

	/* Check if index is out of bound */
	st_table_size = get_st_table_size(pdev);
	if (index >= st_table_size)
		return -ENXIO;

	offset = pdev->tph_cap + PCI_TPH_BASE_SIZEOF + index * sizeof(u16);

	return pci_write_config_word(pdev, offset, tag);
}

/**
 * pcie_tph_get_cpu_st() - Retrieve Steering Tag for a target memory associated
 * with a specific CPU
 * @pdev: PCI device
 * @mem_type: target memory type (volatile or persistent RAM)
 * @cpu_uid: associated CPU id
 * @tag: Steering Tag to be returned
 *
 * This function returns the Steering Tag for a target memory that is
 * associated with a specific CPU as indicated by cpu_uid.
 *
 * Returns 0 if success, otherwise negative value (-errno)
 */
int pcie_tph_get_cpu_st(struct pci_dev *pdev, enum tph_mem_type mem_type,
			unsigned int cpu_uid, u16 *tag)
{
	struct pci_dev *rp;
	acpi_handle rp_acpi_handle;
	union st_info info;

	rp = pcie_find_root_port(pdev);
	if (!rp || !rp->bus || !rp->bus->bridge)
		return -ENODEV;

	rp_acpi_handle = ACPI_HANDLE(rp->bus->bridge);

	if (tph_invoke_dsm(rp_acpi_handle, cpu_uid, &info) != AE_OK) {
		*tag = 0;
		return -EINVAL;
	}

	*tag = tph_extract_tag(mem_type, pdev->tph_req_type, &info);

	pci_dbg(pdev, "get steering tag: mem_type=%s, cpu_uid=%d, tag=%#04x\n",
		(mem_type == TPH_MEM_TYPE_VM) ? "volatile" : "persistent",
		cpu_uid, *tag);

	return 0;
}
EXPORT_SYMBOL(pcie_tph_get_cpu_st);

/**
 * pcie_tph_set_st_entry() - Set Steering Tag in the ST table entry
 * @pdev: PCI device
 * @index: ST table entry index
 * @tag: Steering Tag to be written
 *
 * This function will figure out the proper location of ST table, either in
 * the MSI-X table or in the TPH Extended Capability space, and write the
 * Steering Tag into the ST entry pointed by index.
 *
 * Returns: 0 if success, otherwise negative value (-errno)
 */
int pcie_tph_set_st_entry(struct pci_dev *pdev, unsigned int index, u16 tag)
{
	u32 loc;
	int err = 0;

	if (!pdev->tph_cap)
		return -EINVAL;

	if (!pdev->tph_enabled)
		return -EINVAL;

	/* No need to write tag if device is in "No ST Mode" */
	if (pdev->tph_mode == PCI_TPH_NO_ST_MODE)
		return 0;

	/* Disable TPH before updating ST to avoid potential instability as
	 * cautioned in PCIe r6.2, sec 6.17.3, "ST Modes of Operation"
	 */
	set_ctrl_reg_req_en(pdev, PCI_TPH_REQ_DISABLE);

	loc = get_st_table_loc(pdev);
	/* Convert loc to match with PCI_TPH_LOC_* defined in pci_regs.h */
	loc = FIELD_PREP(PCI_TPH_CAP_LOC_MASK, loc);

	switch (loc) {
	case PCI_TPH_LOC_MSIX:
		err = write_tag_to_msix(pdev, index, tag);
		break;
	case PCI_TPH_LOC_CAP:
		err = write_tag_to_st_table(pdev, index, tag);
		break;
	default:
		err = -EINVAL;
	}

	if (err) {
		pcie_disable_tph(pdev);
		return err;
	}

	set_ctrl_reg_req_en(pdev, pdev->tph_mode);

	pci_dbg(pdev, "set steering tag: %s table, index=%d, tag=%#04x\n",
		(loc == PCI_TPH_LOC_MSIX) ? "MSI-X" : "ST", index, tag);

	return 0;
}
EXPORT_SYMBOL(pcie_tph_set_st_entry);

/**
 * pcie_tph_enabled - Check whether TPH is enabled in device
 * @pdev: PCI device
 *
 * Return: true if TPH is enabled, otherwise false
 */
bool pcie_tph_enabled(struct pci_dev *pdev)
{
	return pdev->tph_enabled;
}
EXPORT_SYMBOL(pcie_tph_enabled);

/**
 * pcie_disable_tph - Turn off TPH support for device
 * @pdev: PCI device
 *
 * Return: none
 */
void pcie_disable_tph(struct pci_dev *pdev)
{
	if (!pdev->tph_cap)
		return;

	if (!pdev->tph_enabled)
		return;

	pci_write_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, 0);

	pdev->tph_mode = 0;
	pdev->tph_req_type = 0;
	pdev->tph_enabled = 0;
}
EXPORT_SYMBOL(pcie_disable_tph);

/**
 * pcie_enable_tph - Enable TPH support for device using a specific ST mode
 * @pdev: PCI device
 * @mode: ST mode to enable, as returned by pcie_tph_modes()
 *
 * Checks whether the mode is actually supported by the device before enabling
 * and returns an error if not. Additionally determines what types of requests,
 * TPH or extended TPH, can be issued by the device based on its TPH requester
 * capability and the Root Port's completer capability.
 *
 * Return: 0 on success, otherwise negative value (-errno)
 */
int pcie_enable_tph(struct pci_dev *pdev, int mode)
{
	u32 reg;
	u8 dev_modes;
	u8 rp_req_type;

	if (!pdev->tph_cap)
		return -EINVAL;

	if (pdev->tph_enabled)
		return -EBUSY;

	/* Check ST mode comptability */
	dev_modes = get_st_modes(pdev);
	if (!(mode & dev_modes))
		return -EINVAL;

	/* Select a supported mode */
	switch (mode) {
	case PCI_TPH_CAP_INT_VEC:
		pdev->tph_mode = PCI_TPH_INT_VEC_MODE;
		break;
	case PCI_TPH_CAP_DEV_SPEC:
		pdev->tph_mode = PCI_TPH_DEV_SPEC_MODE;
		break;
	case PCI_TPH_CAP_NO_ST:
		pdev->tph_mode = PCI_TPH_NO_ST_MODE;
		break;
	default:
		return -EINVAL;
	}

	/* Get req_type supported by device and its Root Port */
	reg = pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CAP, &reg);
	if (FIELD_GET(PCI_TPH_CAP_EXT_TPH, reg))
		pdev->tph_req_type = PCI_TPH_REQ_EXT_TPH;
	else
		pdev->tph_req_type = PCI_TPH_REQ_TPH_ONLY;

	rp_req_type = get_rp_completer_type(pdev);

	/* Final req_type is the smallest value of two */
	pdev->tph_req_type = min(pdev->tph_req_type, rp_req_type);

	if (pdev->tph_req_type == PCI_TPH_REQ_DISABLE)
		return -ENOTSUPP;

	/* Write them into TPH control register */
	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, &reg);

	reg &= ~PCI_TPH_CTRL_MODE_SEL_MASK;
	reg |= FIELD_PREP(PCI_TPH_CTRL_MODE_SEL_MASK, pdev->tph_mode);

	reg &= ~PCI_TPH_CTRL_REQ_EN_MASK;
	reg |= FIELD_PREP(PCI_TPH_CTRL_REQ_EN_MASK, pdev->tph_req_type);

	pci_write_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, reg);

	pdev->tph_enabled = 1;

	return 0;
}
EXPORT_SYMBOL(pcie_enable_tph);

/**
 * pcie_tph_modes - Get the ST modes supported by device
 * @pdev: PCI device
 *
 * Returns a bitmask with all TPH modes supported by a device as shown in the
 * TPH capability register. Current supported modes include:
 *   PCI_TPH_CAP_NO_ST - NO ST Mode Supported
 *   PCI_TPH_CAP_INT_VEC - Interrupt Vector Mode Supported
 *   PCI_TPH_CAP_DEV_SPEC - Device Specific Mode Supported
 *
 * Return: 0 when TPH is not supported, otherwise bitmask of supported modes
 */
int pcie_tph_modes(struct pci_dev *pdev)
{
	if (!pdev->tph_cap)
		return 0;

	return get_st_modes(pdev);
}
EXPORT_SYMBOL(pcie_tph_modes);

void pci_tph_init(struct pci_dev *pdev)
{
	pdev->tph_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_TPH);
}
