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

static psp_version_t psp_version;
static struct psp_drtm_interface psp_drtm;

static grub_err_t init_drtm_interface (grub_addr_t base_addr, psp_version_t version);

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

grub_err_t
grub_psp_discover (void)
{
  grub_uint64_t bar2_addr = 0;
  grub_err_t err;

  bar2_addr = get_psp_bar_addr();
  if (!bar2_addr)
    return grub_error (GRUB_ERR_BAD_DEVICE, N_("DRTM: failed to find PSP\n"));

  err = init_drtm_interface (bar2_addr, PSP_V2);
  if (err)
    return err;

  psp_version = PSP_V2;
  return GRUB_ERR_NONE;
}

static grub_err_t
init_drtm_interface (grub_addr_t base_addr, psp_version_t version)
{
  switch (version)
    {
    case PSP_V2:
    case PSP_V3:
      psp_drtm.c2pmsg_72 = (volatile grub_uint32_t *)(base_addr + 0x10a20);
      psp_drtm.c2pmsg_93 = (volatile grub_uint32_t *)(base_addr + 0x10a74);
      psp_drtm.c2pmsg_94 = (volatile grub_uint32_t *)(base_addr + 0x10a78);
      psp_drtm.c2pmsg_95 = (volatile grub_uint32_t *)(base_addr + 0x10a7c);
      break;
    default:
      return grub_error (GRUB_ERR_BAD_DEVICE, N_("DRTM: Unrecognized PSP version %d\n"), version);
    }

  return GRUB_ERR_NONE;
}

grub_uint16_t
grub_psp_version (void)
{
  return psp_version;
}
