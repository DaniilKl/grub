/*
 * Copyright (c) 2024, Oracle and/or its affiliates.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <grub/types.h>
#include <grub/misc.h>
#include <grub/err.h>
#include <grub/mm.h>
#include <grub/pci.h>
#include <grub/i386/pci.h>
#include <grub/i386/psp.h>

struct psp_drtm_interface
{
  volatile grub_uint32_t *c2pmsg_72;
  volatile grub_uint32_t *c2pmsg_93;
  volatile grub_uint32_t *c2pmsg_94;
  volatile grub_uint32_t *c2pmsg_95;
};

typedef enum
{
  PSP_NONE = 0,
  PSP_V1,
  PSP_V2,
  PSP_V3
} psp_version_t;

struct pci_psp_device
{
  grub_uint16_t vendor_id;
  grub_uint16_t dev_id;
  psp_version_t version;
};

static const struct pci_psp_device psp_devs_list[] = {
  {0x1022, 0x1537, PSP_NONE},
  {0x1022, 0x1456, PSP_V1},
  {0x1022, 0x1468, PSP_NONE},
  {0x1022, 0x1486, PSP_V2},
  {0x1022, 0x15DF, PSP_V3},
  {0x1022, 0x1649, PSP_V2},
  {0x1022, 0x14CA, PSP_V3},
  {0x1022, 0x15C7, PSP_NONE}
};

static struct psp_drtm_interface psp_drtm;

static grub_err_t init_drtm_interface (grub_uint64_t base_addr, psp_version_t psp_version);
static void init_drtm_device (grub_pci_device_t dev);

static void
smn_register_read (grub_uint32_t address, grub_uint32_t *value)
{
  grub_pci_device_t dev = {0, 0, 0};
  grub_pci_address_t addr;
  grub_uint32_t val;

  val = address;
  addr = grub_pci_make_address (dev, 0xb8);
  grub_pci_write (addr, val);
  addr = grub_pci_make_address (dev, 0xbc);
  val = grub_pci_read (addr);
  *value = val;
}

#define IOHC0NBCFG_SMNBASE 0x13b00000
#define PSP_BASE_ADDR_LO_SMN_ADDRESS (IOHC0NBCFG_SMNBASE + 0x102e0)

static grub_uint64_t
get_psp_bar_addr (void)
{
  grub_uint32_t pspbaselo;

  pspbaselo = 0;
  smn_register_read (PSP_BASE_ADDR_LO_SMN_ADDRESS, &pspbaselo);

  /* Mask out the lower bits */
  pspbaselo &= 0xfff00000;
  return (grub_uint64_t) pspbaselo;
}

static const struct pci_psp_device *
is_drtm_device (grub_uint16_t vendor_id, grub_uint16_t dev_id)
{
  grub_uint32_t max_psp_devs = sizeof (psp_devs_list) / sizeof (psp_devs_list[0]);
  const struct pci_psp_device *psp = NULL;
  grub_uint32_t i;

  for (i = 0; i < max_psp_devs; i++)
    {
      if ((psp_devs_list[i].vendor_id == vendor_id) &&
	  (psp_devs_list[i].dev_id == dev_id))
	{
	  psp = &psp_devs_list[i];
	  break;
	}
    }

  if (psp && psp->version == PSP_NONE)
    {
      grub_dprintf ("slaunch", "DRTM: AMD SP device (PCI info: 0x%04x, 0x%04x) does not have PSP\n",
		    psp->vendor_id, psp->dev_id);
      psp = NULL;
    }

  return psp;
}

grub_err_t
grub_psp_discover (void)
{
  grub_pci_device_t dev;
  grub_pci_address_t addr;
  grub_uint16_t vendor_id, dev_id;
  const struct pci_psp_device *psp = NULL;
  grub_uint64_t bar2_addr = 0;

  for (dev.bus = 0; dev.bus < GRUB_PCI_NUM_BUS; dev.bus++)
    {
      for (dev.device = 0; dev.device < GRUB_PCI_NUM_DEVICES; dev.device++)
	{
	  for (dev.function = 0; dev.function < GRUB_PCI_NUM_FUNCTIONS; dev.function++)
	    {
	      addr = grub_pci_make_address (dev, 0);
	      vendor_id = grub_pci_read_word (addr);
	      addr = grub_pci_make_address (dev, 2);
	      dev_id = grub_pci_read_word (addr);
	      psp = is_drtm_device (vendor_id, dev_id);
	      if (psp)
		goto psp_found;
	    }
	}
    }

  if (!psp)
    return grub_error (GRUB_ERR_BAD_DEVICE, N_("DRTM: failed to find PSP\n"));

psp_found:
  init_drtm_device (dev);

  bar2_addr = get_psp_bar_addr();
  if (!bar2_addr)
    return grub_error (GRUB_ERR_BAD_DEVICE, N_("DRTM: failed to find PSP\n"));

  return init_drtm_interface (bar2_addr, PSP_V2);
}

static void
init_drtm_device (grub_pci_device_t dev)
{
  grub_uint16_t pci_cmd;
  grub_uint8_t pin, lat;
  grub_pci_address_t pci_cmd_addr, pin_addr, lat_addr;

  /* Enable memory space access for PSP */
  pci_cmd = 0;
  pci_cmd_addr = grub_pci_make_address (dev, GRUB_PCI_REG_COMMAND);
  pci_cmd = grub_pci_read_word (pci_cmd_addr);
  pci_cmd |= 0x2;
  grub_pci_write_word (pci_cmd_addr, pci_cmd);

  /* Enable PCI interrupts */
  pin = 0;
  pin_addr = grub_pci_make_address (dev, GRUB_PCI_REG_IRQ_PIN);
  pin = grub_pci_read_byte (pin_addr);
  if (pin)
    {
      pci_cmd = 0;
      pci_cmd = grub_pci_read_word (pci_cmd_addr);
      if (pci_cmd & 0x400)
	{
	  pci_cmd &= ~0x400;
	  grub_pci_write_word (pci_cmd_addr, pci_cmd);
	}
    }

  /* Set PSP at bus master */
  pci_cmd = 0;
  pci_cmd = grub_pci_read_word (pci_cmd_addr);
  pci_cmd |= 0x4;
  grub_pci_write_word (pci_cmd_addr, pci_cmd);

  /* SET PCI latency timer */
  lat = 0;
  lat_addr = grub_pci_make_address (dev, GRUB_PCI_REG_LAT_TIMER);
  lat = grub_pci_read_byte (lat_addr);
  if (lat < 16)
    {
      lat = 64;
      grub_pci_write_byte (lat_addr, lat);
    }
}

static grub_err_t
init_drtm_interface (grub_uint64_t base_addr, psp_version_t psp_version)
{
#ifdef __i386__
  grub_uint32_t base = (grub_uint32_t) base_addr;
#else
  grub_uint64_t base = base_addr;
#endif

  switch (psp_version)
    {
    case PSP_V2:
      psp_drtm.c2pmsg_72 = (volatile grub_uint32_t *)(base + 0x10a20);
      psp_drtm.c2pmsg_93 = (volatile grub_uint32_t *)(base + 0x10a74);
      psp_drtm.c2pmsg_94 = (volatile grub_uint32_t *)(base + 0x10a78);
      psp_drtm.c2pmsg_95 = (volatile grub_uint32_t *)(base + 0x10a7c);
      break;
    default:
      return grub_error (GRUB_ERR_BAD_DEVICE, N_("DRTM: Unrecognized PSP version %d\n"), psp_version);
    }

  return GRUB_ERR_NONE;
}
