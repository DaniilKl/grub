/* chainloader.c - boot another boot loader */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2002,2004,2006,2007,2008  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

/* TODO: support load options.  */

#include <grub/loader.h>
#include <grub/file.h>
#include <grub/err.h>
#include <grub/device.h>
#include <grub/disk.h>
#include <grub/misc.h>
#include <grub/charset.h>
#include <grub/mm.h>
#include <grub/types.h>
#include <grub/dl.h>
#include <grub/efi/api.h>
#include <grub/efi/efi.h>
#include <grub/efi/disk.h>
#include <grub/efi/memory.h>
#include <grub/command.h>
#include <grub/i18n.h>
#include <grub/net.h>
#include <grub/slaunch.h>
#include <grub/time.h>
#if defined (__i386__) || defined (__x86_64__)
#include <grub/macho.h>
#include <grub/i386/macho.h>
#endif

GRUB_MOD_LICENSE ("GPLv3+");

static grub_dl_t my_mod;

static void *image_mem;
static grub_efi_physical_address_t image_address;
static grub_efi_uintn_t image_pages;

static struct grub_slaunch_params slparams = {0};

static void
free_image (void)
{
  if (image_address)
    {
      grub_efi_system_table->boot_services->free_pages (image_address,
                                                        image_pages);

      image_mem = NULL;
      image_address = 0;
      image_pages = 0;
    }
}

static grub_err_t
grub_chainloader_unload (void *context)
{
  grub_efi_handle_t image_handle = (grub_efi_handle_t) context;
  grub_efi_loaded_image_t *loaded_image;
  grub_efi_boot_services_t *b;

  free_image ();

  loaded_image = grub_efi_get_loaded_image (image_handle);
  if (loaded_image != NULL)
    grub_free (loaded_image->load_options);

  b = grub_efi_system_table->boot_services;
  b->unload_image (image_handle);

  grub_dl_unref (my_mod);
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_chainloader_boot (void *context)
{
  grub_efi_handle_t image_handle = (grub_efi_handle_t) context;
  grub_efi_boot_services_t *b;
  grub_efi_status_t status;
  grub_efi_uintn_t exit_data_size;
  grub_efi_char16_t *exit_data = NULL;
  grub_err_t err;
  grub_efi_loaded_image_t *loaded_image;

  loaded_image = grub_efi_get_loaded_image (image_handle);
  if (loaded_image == NULL)
    return grub_error (GRUB_ERR_BUG, "Couldn't query EFI loaded image");

  if (grub_slaunch_platform_type () == SLP_INTEL_TXT)
    {
      err = grub_sl_efi_txt_setup (&slparams, image_mem, loaded_image, /*is_linux=*/false);
      if (err != GRUB_ERR_NONE)
        return grub_error (err, "Secure Launch setup TXT failed");
    }
  else if (grub_slaunch_platform_type () == SLP_AMD_SKINIT)
    {
      err = grub_sl_efi_skinit_setup (&slparams, image_mem, loaded_image, /*is_linux=*/false);
      if (err != GRUB_ERR_NONE)
        return grub_error (err, "Secure Launch setup SKINIT failed");
    }

  b = grub_efi_system_table->boot_services;
  status = b->start_image (image_handle, &exit_data_size, &exit_data);
  if (status != GRUB_EFI_SUCCESS)
    {
      if (exit_data)
	{
	  char *buf;

	  buf = grub_malloc (exit_data_size * 4 + 1);
	  if (buf)
	    {
	      *grub_utf16_to_utf8 ((grub_uint8_t *) buf,
				   exit_data, exit_data_size) = 0;

	      grub_error (GRUB_ERR_BAD_OS, "%s", buf);
	      grub_free (buf);
	    }
	}
      else
	grub_error (GRUB_ERR_BAD_OS, "unknown error");
    }

  if (exit_data)
    b->free_pool (exit_data);

  grub_loader_unset ();

  return grub_errno;
}

static grub_err_t
copy_file_path (grub_efi_file_path_device_path_t *fp,
		const char *str, grub_efi_uint16_t len)
{
  grub_efi_char16_t *p, *path_name;
  grub_efi_uint16_t size;

  fp->header.type = GRUB_EFI_MEDIA_DEVICE_PATH_TYPE;
  fp->header.subtype = GRUB_EFI_FILE_PATH_DEVICE_PATH_SUBTYPE;

  path_name = grub_calloc (len, GRUB_MAX_UTF16_PER_UTF8 * sizeof (*path_name));
  if (!path_name)
    return grub_error (GRUB_ERR_OUT_OF_MEMORY, "failed to allocate path buffer");

  size = grub_utf8_to_utf16 (path_name, len * GRUB_MAX_UTF16_PER_UTF8,
			     (const grub_uint8_t *) str, len, 0);
  for (p = path_name; p < path_name + size; p++)
    if (*p == '/')
      *p = '\\';

  grub_memcpy (fp->path_name, path_name, size * sizeof (*fp->path_name));
  /* File Path is NULL terminated */
  fp->path_name[size++] = '\0';
  fp->header.length = size * sizeof (grub_efi_char16_t) + sizeof (*fp);
  grub_free (path_name);
  return GRUB_ERR_NONE;
}

static grub_efi_device_path_t *
make_file_path (grub_efi_device_path_t *dp, const char *filename)
{
  char *dir_start;
  char *dir_end;
  grub_size_t size;
  grub_efi_device_path_t *d, *file_path;

  dir_start = grub_strchr (filename, ')');
  if (! dir_start)
    dir_start = (char *) filename;
  else
    dir_start++;

  dir_end = grub_strrchr (dir_start, '/');
  if (! dir_end)
    {
      grub_error (GRUB_ERR_BAD_FILENAME, "invalid EFI file path");
      return 0;
    }

  size = 0;
  d = dp;
  while (d)
    {
      grub_size_t len = GRUB_EFI_DEVICE_PATH_LENGTH (d);

      if (len < 4)
	{
	  grub_error (GRUB_ERR_OUT_OF_RANGE,
		      "malformed EFI Device Path node has length=%" PRIuGRUB_SIZE, len);
	  return NULL;
	}

      size += len;
      if ((GRUB_EFI_END_ENTIRE_DEVICE_PATH (d)))
	break;
      d = GRUB_EFI_NEXT_DEVICE_PATH (d);
    }

  /* File Path is NULL terminated. Allocate space for 2 extra characters */
  /* FIXME why we split path in two components? */
  file_path = grub_malloc (size
			   + ((grub_strlen (dir_start) + 2)
			      * GRUB_MAX_UTF16_PER_UTF8
			      * sizeof (grub_efi_char16_t))
			   + sizeof (grub_efi_file_path_device_path_t) * 2);
  if (! file_path)
    return 0;

  grub_memcpy (file_path, dp, size);

  /* Fill the file path for the directory.  */
  d = (grub_efi_device_path_t *) ((char *) file_path
				  + ((char *) d - (char *) dp));
  if (copy_file_path ((grub_efi_file_path_device_path_t *) d,
		      dir_start, dir_end - dir_start) != GRUB_ERR_NONE)
    {
 fail:
      grub_free (file_path);
      return 0;
    }

  /* Fill the file path for the file.  */
  d = GRUB_EFI_NEXT_DEVICE_PATH (d);
  if (copy_file_path ((grub_efi_file_path_device_path_t *) d,
		      dir_end + 1, grub_strlen (dir_end + 1)) != GRUB_ERR_NONE)
    goto fail;

  /* Fill the end of device path nodes.  */
  d = GRUB_EFI_NEXT_DEVICE_PATH (d);
  d->type = GRUB_EFI_END_DEVICE_PATH_TYPE;
  d->subtype = GRUB_EFI_END_ENTIRE_DEVICE_PATH_SUBTYPE;
  d->length = sizeof (*d);

  return file_path;
}

static grub_err_t
clear_unused_memory_below (grub_uint64_t limit)
{
  grub_efi_memory_descriptor_t *memory_map, *desc;
  grub_efi_uintn_t memory_map_size, desc_size;
  grub_efi_boot_services_t *b;
  grub_efi_uint64_t total_cleared = 0;
  int ret;

  b = grub_efi_system_table->boot_services;

  memory_map_size = grub_efi_find_mmap_size ();

  memory_map = grub_malloc (memory_map_size);
  if (!memory_map)
    return GRUB_ERR_OUT_OF_MEMORY;

  ret = grub_efi_get_memory_map (&memory_map_size, memory_map, NULL,
                                 &desc_size, NULL);
  if (ret < 1)
    {
      grub_free (memory_map);
      return GRUB_ERR_BUG;
    }

  for (desc = memory_map;
       (grub_addr_t)desc < ((grub_addr_t)memory_map + memory_map_size);
       desc = (void *)((grub_uint8_t *)desc + desc_size))
    {
      grub_efi_uint64_t size = desc->num_pages << 12;

      if (desc->type != GRUB_EFI_CONVENTIONAL_MEMORY ||
          desc->physical_start >= limit)
        continue;

      if (desc->physical_start + size > limit)
        size = limit - desc->physical_start;

      grub_dprintf ("chain", "Clearing %lu bytes at 0x%lx...\n", size,
                    desc->physical_start);
      b->set_mem ((void *)desc->physical_start, size, 0x00);
      total_cleared += size;
    }

  grub_free (memory_map);

  grub_dprintf ("chain", "Cleared %llu bytes in total.\n",
                (unsigned long long)total_cleared);
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_chainloader (grub_command_t cmd __attribute__ ((unused)),
		      int argc, char *argv[])
{
  grub_file_t file = 0;
  grub_ssize_t size;
  grub_efi_status_t status;
  grub_efi_boot_services_t *b;
  grub_device_t dev = 0;
  grub_efi_device_path_t *dp = NULL, *file_path = NULL;
  grub_efi_loaded_image_t *loaded_image;
  char *filename;
  grub_efi_handle_t dev_handle = 0;
  grub_efi_char16_t *cmdline = NULL;
  grub_efi_handle_t image_handle = NULL;

  if (argc == 0)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("filename expected"));
  filename = argv[0];

  free_image ();

  grub_dl_ref (my_mod);

  b = grub_efi_system_table->boot_services;

  file = grub_file_open (filename, GRUB_FILE_TYPE_EFI_CHAINLOADED_IMAGE);
  if (! file)
    goto fail;

  /* Get the root device's device path.  */
  dev = grub_device_open (0);
  if (dev == NULL)
    ;
  else if (dev->disk)
    dev_handle = grub_efidisk_get_device_handle (dev->disk);
  else if (dev->net && dev->net->server)
    {
      grub_net_network_level_address_t addr;
      struct grub_net_network_level_interface *inf;
      grub_net_network_level_address_t gateway;
      grub_err_t err;

      err = grub_net_resolve_address (dev->net->server, &addr);
      if (err)
	goto fail;

      err = grub_net_route_address (addr, &gateway, &inf);
      if (err)
	goto fail;

      dev_handle = grub_efinet_get_device_handle (inf->card);
    }

  if (dev_handle)
    dp = grub_efi_get_device_path (dev_handle);

  if (dp != NULL)
    {
      file_path = make_file_path (dp, filename);
      if (file_path == NULL)
	goto fail;
    }

  size = grub_file_size (file);
  if (!size)
    {
      grub_error (GRUB_ERR_BAD_OS, N_("premature end of file %s"),
		  filename);
      goto fail;
    }
  image_pages = (grub_efi_uintn_t) GRUB_EFI_BYTES_TO_PAGES (size);

  status = b->allocate_pages (GRUB_EFI_ALLOCATE_ANY_PAGES,
			      GRUB_EFI_LOADER_CODE,
			      image_pages, &image_address);
  if (status != GRUB_EFI_SUCCESS)
    {
      grub_dprintf ("chain", "Failed to allocate %u pages\n",
		    (unsigned int) image_pages);
      grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("out of memory"));
      goto fail;
    }

  image_mem = (void *) ((grub_addr_t) image_address);
  if (grub_file_read (file, image_mem, size) != size)
    {
      if (grub_errno == GRUB_ERR_NONE)
	grub_error (GRUB_ERR_BAD_OS, N_("premature end of file %s"),
		    filename);

      goto fail;
    }

  if (grub_slaunch_platform_type () != SLP_NONE)
    {
      grub_uint64_t before_time, after_time;
      grub_err_t err;

      /*
       * Firmware doesn't necessarily zero area allocated for an EFI image
       * before filling some of its subranges with data from a file.  This can
       * leave areas not backed by a file holding arbitrary values.
       *
       * So clear all currently unused memory below 4 GiB to avoid DRTM
       * measuring random data.
       */

      before_time = grub_get_time_ms ();
      err = clear_unused_memory_below (1ULL << 32);
      after_time = grub_get_time_ms ();
      if (err != GRUB_ERR_NONE)
        {
          grub_dprintf ("chain", "Failed to clear unused memory < 4 GiB\n");
          goto fail;
        }

      grub_dprintf ("chain", "Took %llu ms to clear the memory\n",
                    (unsigned long long)(after_time - before_time));
    }

#if defined (__i386__) || defined (__x86_64__)
  if (size >= (grub_ssize_t) sizeof (struct grub_macho_fat_header))
    {
      struct grub_macho_fat_header *head = image_mem;
      if (head->magic
	  == grub_cpu_to_le32_compile_time (GRUB_MACHO_FAT_EFI_MAGIC))
	{
	  grub_uint32_t i;
	  struct grub_macho_fat_arch *archs
	    = (struct grub_macho_fat_arch *) (head + 1);
	  for (i = 0; i < grub_cpu_to_le32 (head->nfat_arch); i++)
	    {
	      if (GRUB_MACHO_CPUTYPE_IS_HOST_CURRENT (archs[i].cputype))
		break;
	    }
	  if (i == grub_cpu_to_le32 (head->nfat_arch))
	    {
	      grub_error (GRUB_ERR_BAD_OS, "no compatible arch found");
	      goto fail;
	    }
	  if (grub_cpu_to_le32 (archs[i].offset)
	      > ~grub_cpu_to_le32 (archs[i].size)
	      || grub_cpu_to_le32 (archs[i].offset)
	      + grub_cpu_to_le32 (archs[i].size)
	      > (grub_size_t) size)
	    {
	      grub_error (GRUB_ERR_BAD_OS, N_("premature end of file %s"),
			  filename);
	      goto fail;
	    }
	  image_mem = (char *) image_mem + grub_cpu_to_le32 (archs[i].offset);
	  size = grub_cpu_to_le32 (archs[i].size);
	}
    }
#endif

  status = b->load_image (0, grub_efi_image_handle, file_path,
			  image_mem, size,
			  &image_handle);
  if (status != GRUB_EFI_SUCCESS)
    {
      if (status == GRUB_EFI_OUT_OF_RESOURCES)
	grub_error (GRUB_ERR_OUT_OF_MEMORY, "out of resources");
      else
	grub_error (GRUB_ERR_BAD_OS, "cannot load image");

      goto fail;
    }

  /* LoadImage does not set a device handler when the image is
     loaded from memory, so it is necessary to set it explicitly here.
     This is a mess.  */
  loaded_image = grub_efi_get_loaded_image (image_handle);
  if (! loaded_image)
    {
      grub_error (GRUB_ERR_BAD_OS, "no loaded image available");
      goto fail;
    }
  loaded_image->device_handle = dev_handle;

  /* Build load options with arguments from chainloader command line. */
  if (argc > 1)
    {
      int i, len;
      grub_efi_char16_t *p16;

      for (i = 1, len = 0; i < argc; i++)
        len += grub_strlen (argv[i]) + 1;

      len *= sizeof (grub_efi_char16_t);
      cmdline = p16 = grub_malloc (len);
      if (! cmdline)
        goto fail;

      for (i = 1; i < argc; i++)
        {
          char *p8;

          p8 = argv[i];
          while (*p8)
            *(p16++) = *(p8++);

          *(p16++) = ' ';
        }
      *(--p16) = 0;

      loaded_image->load_options = cmdline;
      loaded_image->load_options_size = len;
    }

  grub_file_close (file);
  grub_device_close (dev);

  /* We're typically finished with the source image buffer and file path here. */
  if (grub_slaunch_platform_type () == SLP_NONE)
    free_image ();
  grub_free (file_path);

  grub_loader_set_ex (grub_chainloader_boot, grub_chainloader_unload, image_handle, 0);
  return 0;

 fail:

  if (dev)
    grub_device_close (dev);

  if (file)
    grub_file_close (file);

  grub_free (cmdline);
  grub_free (file_path);

  free_image ();

  if (image_handle != NULL)
    b->unload_image (image_handle);

  grub_dl_unref (my_mod);

  return grub_errno;
}

static grub_command_t cmd;

GRUB_MOD_INIT(chainloader)
{
  cmd = grub_register_command ("chainloader", grub_cmd_chainloader,
			       0, N_("Load another boot loader."));
  my_mod = mod;
}

GRUB_MOD_FINI(chainloader)
{
  grub_unregister_command (cmd);
}
