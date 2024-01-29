.. SPDX-License-Identifier: GPL-2.0

===========
TPH Support
===========


:Copyright: |copy| 2023 Advanced Micro Devices, Inc.
:Authors: - Eric van Tassell <eric.vantassell@amd.com>
          - Wei Huang <wei.huang2@amd.com>

Overview
========
TPH (TLP Processing Hints) is a PCIe feature that allows endpoint devices
to provide optimization hints, such as desired caching behavior, for
requests that target memory space. These hints, in a format called steering
tags, are provided in the requester's TLP headers and can empower the system
hardware, including the Root Complex, to optimize the utilization of platform
resources for the requests.

User Guide
==========

Kernel Options
--------------
There are two kernel command line options available to control TPH feature

   * "notph": All API functions will return and TPH will not be enabled for any endpoint.
   * "nostmode": TPH will be enabled but the ST Mode Select will be set to "No ST Mode".

Device Driver API
-----------------
In brief, an endpoint device driver using the TPH interface to configure
Interrupt Vector Mode will call pcie_tph_set_st() when setting up MSI-X
interrupts as shown below:

.. code-block:: c

    for (i = 0, j = 0; i < nr_rings; i++) {
        ...
        rc = request_irq(irq->vector, irq->handler, flags, irq->name, NULL);
        ...
        if (!pcie_tph_set_st(pdev, i, cpumask_first(irq->cpu_mask),
                             TPH_MEM_TYPE_VM, PCI_TPH_REQ_TPH_ONLY))
               pr_err("Error in configuring steering tag\n");
        ...
    }

.. kernel-doc:: drivers/pci/pcie/tph.c
   :export:

.. kernel-doc:: drivers/pci/pcie/tph.c
   :identifiers: pcie_tph_set_st
