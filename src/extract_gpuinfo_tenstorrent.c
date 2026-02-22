/*
 *
 * Copyright (C) 2025 Tenstorrent accelerator backend for nvtop
 *
 * This file is part of Nvtop.
 *
 * Nvtop is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Nvtop is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with nvtop.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "nvtop/common.h"
#include "nvtop/extract_gpuinfo_common.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TT_SYSFS_CLASS "/sys/class/tenstorrent"
#define TT_SYSFS_PREFIX "tenstorrent!"
#define TT_PATH_MAX 512

struct gpu_info_tenstorrent {
  struct gpu_info base;
  char sysfs_device_path[TT_PATH_MAX]; // /sys/class/tenstorrent/tenstorrent!N
  char pci_device_path[TT_PATH_MAX];   // /sys/devices/.../0000:XX:YY.Z
  char hwmon_path[TT_PATH_MAX];        // .../hwmon/hwmonN
  unsigned device_id;
  unsigned ordinal; // device index N from tenstorrent!N, matches /dev/tenstorrent/N
};

static struct gpu_info_tenstorrent *tt_devices;
extern struct gpu_vendor gpu_vendor_tenstorrent;

static bool read_sysfs_long(const char *path, long *value) {
  FILE *fp = fopen(path, "r");
  if (!fp)
    return false;
  bool ok = fscanf(fp, "%ld", value) == 1;
  fclose(fp);
  return ok;
}

static bool find_hwmon_path(const char *pci_path, char *hwmon_path, size_t size) {
  char hwmon_dir[PATH_MAX];
  snprintf(hwmon_dir, sizeof(hwmon_dir), "%s/hwmon", pci_path);

  DIR *dir = opendir(hwmon_dir);
  if (!dir)
    return false;

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "hwmon", 5) == 0 && entry->d_name[5] != '\0') {
      snprintf(hwmon_path, size, "%s/%s", hwmon_dir, entry->d_name);
      closedir(dir);
      return true;
    }
  }
  closedir(dir);
  return false;
}

static const char *chip_name_from_device_id(unsigned device_id) {
  switch (device_id) {
  case 0x401e:
    return "Wormhole";
  case 0xb140:
    return "Blackhole";
  case 0xfaca:
    return "Grayskull";
  default:
    return "Tenstorrent";
  }
}

static bool gpuinfo_tenstorrent_init(void) {
  return access(TT_SYSFS_CLASS, R_OK | X_OK) == 0;
}

static void gpuinfo_tenstorrent_shutdown(void) {
  free(tt_devices);
  tt_devices = NULL;
}

static const char *gpuinfo_tenstorrent_last_error_string(void) {
  return "Tenstorrent error";
}

static bool gpuinfo_tenstorrent_get_device_handles(struct list_head *devices, unsigned *count) {
  *count = 0;

  DIR *dir = opendir(TT_SYSFS_CLASS);
  if (!dir)
    return false;

  unsigned num_devices = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, TT_SYSFS_PREFIX, sizeof(TT_SYSFS_PREFIX) - 1) == 0)
      num_devices++;
  }

  if (num_devices == 0) {
    closedir(dir);
    return false;
  }

  tt_devices = calloc(num_devices, sizeof(*tt_devices));
  if (!tt_devices) {
    closedir(dir);
    return false;
  }

  rewinddir(dir);

  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, TT_SYSFS_PREFIX, sizeof(TT_SYSFS_PREFIX) - 1) != 0)
      continue;
    if (*count >= num_devices)
      break;

    struct gpu_info_tenstorrent *dev = &tt_devices[*count];

    snprintf(dev->sysfs_device_path, sizeof(dev->sysfs_device_path),
             "%s/%s", TT_SYSFS_CLASS, entry->d_name);

    // Parse ordinal from "tenstorrent!N"
    dev->ordinal = (unsigned)atoi(entry->d_name + sizeof(TT_SYSFS_PREFIX) - 1);

    // Resolve PCI device path via "device" symlink
    char device_link[PATH_MAX];
    snprintf(device_link, sizeof(device_link), "%s/device", dev->sysfs_device_path);
    char resolved[PATH_MAX];
    if (!realpath(device_link, resolved))
      continue;
    strncpy(dev->pci_device_path, resolved, sizeof(dev->pci_device_path) - 1);
    dev->pci_device_path[sizeof(dev->pci_device_path) - 1] = '\0';

    // Find hwmon directory under PCI device
    dev->hwmon_path[0] = '\0';
    find_hwmon_path(dev->pci_device_path, dev->hwmon_path, sizeof(dev->hwmon_path));

    // Read PCI device ID (hex format: 0xNNNN)
    char id_path[PATH_MAX];
    snprintf(id_path, sizeof(id_path), "%s/device", dev->pci_device_path);
    FILE *fp = fopen(id_path, "r");
    if (fp) {
      if (fscanf(fp, "%x", &dev->device_id) != 1)
        dev->device_id = 0;
      fclose(fp);
    }

    // Extract BDF from last component of PCI path
    const char *bdf = strrchr(dev->pci_device_path, '/');
    bdf = bdf ? bdf + 1 : dev->pci_device_path;
    strncpy(dev->base.pdev, bdf, PDEV_LEN - 1);
    dev->base.pdev[PDEV_LEN - 1] = '\0';

    dev->base.vendor = &gpu_vendor_tenstorrent;
    dev->base.processes_count = 0;
    dev->base.processes = NULL;
    dev->base.processes_array_size = 0;
    list_add_tail(&dev->base.list, devices);
    (*count)++;
  }

  closedir(dir);
  return *count > 0;
}

static void gpuinfo_tenstorrent_populate_static_info(struct gpu_info *_gpu_info) {
  struct gpu_info_tenstorrent *tt = container_of(_gpu_info, struct gpu_info_tenstorrent, base);
  struct gpuinfo_static_info *static_info = &tt->base.static_info;
  char attr_path[PATH_MAX];
  long val;

  static_info->integrated_graphics = false;
  static_info->encode_decode_shared = false;
  RESET_ALL(static_info->valid);

  // Device name: chip name + card type
  const char *chip = chip_name_from_device_id(tt->device_id);
  char card_type[64] = "";
  snprintf(attr_path, sizeof(attr_path), "%s/tt_card_type", tt->sysfs_device_path);
  FILE *fp = fopen(attr_path, "r");
  if (fp) {
    if (fgets(card_type, sizeof(card_type), fp)) {
      char *nl = strchr(card_type, '\n');
      if (nl)
        *nl = '\0';
    }
    fclose(fp);
  }

  if (card_type[0] && strcmp(card_type, "unknown") != 0)
    snprintf(static_info->device_name, sizeof(static_info->device_name), "%s %s", chip, card_type);
  else
    snprintf(static_info->device_name, sizeof(static_info->device_name), "%s", chip);
  SET_VALID(gpuinfo_device_name_valid, static_info->valid);

  // PCIe max gen and width
  snprintf(attr_path, sizeof(attr_path), "%s/max_link_speed", tt->pci_device_path);
  if (read_sysfs_long(attr_path, &val))
    SET_GPUINFO_STATIC(static_info, max_pcie_gen, nvtop_pcie_gen_from_link_speed(val));

  snprintf(attr_path, sizeof(attr_path), "%s/max_link_width", tt->pci_device_path);
  if (read_sysfs_long(attr_path, &val))
    SET_GPUINFO_STATIC(static_info, max_pcie_link_width, val);

  // Temperature threshold from hwmon
  if (tt->hwmon_path[0]) {
    snprintf(attr_path, sizeof(attr_path), "%s/temp1_max", tt->hwmon_path);
    if (read_sysfs_long(attr_path, &val))
      SET_GPUINFO_STATIC(static_info, temperature_slowdown_threshold, val / 1000);
  }
}

static void gpuinfo_tenstorrent_refresh_dynamic_info(struct gpu_info *_gpu_info) {
  struct gpu_info_tenstorrent *tt = container_of(_gpu_info, struct gpu_info_tenstorrent, base);
  struct gpuinfo_dynamic_info *dynamic_info = &tt->base.dynamic_info;
  char path[PATH_MAX];
  long val;

  RESET_ALL(dynamic_info->valid);

  if (tt->hwmon_path[0]) {
    // Temperature: millidegrees -> degrees
    snprintf(path, sizeof(path), "%s/temp1_input", tt->hwmon_path);
    if (read_sysfs_long(path, &val))
      SET_GPUINFO_DYNAMIC(dynamic_info, gpu_temp, val / 1000);

    // Power: microwatts -> milliwatts
    snprintf(path, sizeof(path), "%s/power1_input", tt->hwmon_path);
    if (read_sysfs_long(path, &val))
      SET_GPUINFO_DYNAMIC(dynamic_info, power_draw, val / 1000);

    // Power max: microwatts -> milliwatts
    snprintf(path, sizeof(path), "%s/power1_max", tt->hwmon_path);
    if (read_sysfs_long(path, &val))
      SET_GPUINFO_DYNAMIC(dynamic_info, power_draw_max, val / 1000);

    // Fan RPM
    snprintf(path, sizeof(path), "%s/fan1_input", tt->hwmon_path);
    if (read_sysfs_long(path, &val))
      SET_GPUINFO_DYNAMIC(dynamic_info, fan_rpm, val);
  }

  // AI clock (already in MHz)
  snprintf(path, sizeof(path), "%s/tt_aiclk", tt->sysfs_device_path);
  if (read_sysfs_long(path, &val))
    SET_GPUINFO_DYNAMIC(dynamic_info, gpu_clock_speed, val);

  // PCIe current gen
  snprintf(path, sizeof(path), "%s/current_link_speed", tt->pci_device_path);
  if (read_sysfs_long(path, &val))
    SET_GPUINFO_DYNAMIC(dynamic_info, pcie_link_gen, nvtop_pcie_gen_from_link_speed(val));

  // PCIe current width
  snprintf(path, sizeof(path), "%s/current_link_width", tt->pci_device_path);
  if (read_sysfs_long(path, &val))
    SET_GPUINFO_DYNAMIC(dynamic_info, pcie_link_width, val);
}

static bool parse_tenstorrent_fdinfo(FILE *f, unsigned *device,
                                     unsigned long long *dmabuf,
                                     unsigned long long *pinned) {
  char line[256];
  bool found = false;

  *device = 0;
  *dmabuf = 0;
  *pinned = 0;

  static const char key_device[] = "tenstorrent-device:\t";
  static const char key_dmabuf[] = "tenstorrent-memory-dmabuf:\t";
  static const char key_pinned[] = "tenstorrent-memory-pinned:\t";

  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, key_device, sizeof(key_device) - 1) == 0) {
      *device = (unsigned)atoi(line + sizeof(key_device) - 1);
      found = true;
    } else if (strncmp(line, key_dmabuf, sizeof(key_dmabuf) - 1) == 0) {
      *dmabuf = strtoull(line + sizeof(key_dmabuf) - 1, NULL, 10);
    } else if (strncmp(line, key_pinned, sizeof(key_pinned) - 1) == 0) {
      *pinned = strtoull(line + sizeof(key_pinned) - 1, NULL, 10);
    }
  }
  return found;
}

// Find or allocate a process entry for the given pid
static struct gpu_process *find_or_alloc_process(struct gpu_info *info, pid_t pid) {
  for (unsigned i = 0; i < info->processes_count; i++) {
    if (info->processes[i].pid == pid)
      return &info->processes[i];
  }
  if (info->processes_count == info->processes_array_size) {
    info->processes_array_size += COMMON_PROCESS_LINEAR_REALLOC_INC;
    info->processes = reallocarray(info->processes, info->processes_array_size,
                                   sizeof(*info->processes));
    if (!info->processes)
      return NULL;
  }
  unsigned idx = info->processes_count++;
  memset(&info->processes[idx], 0, sizeof(*info->processes));
  info->processes[idx].pid = pid;
  info->processes[idx].type = gpu_process_compute;
  return &info->processes[idx];
}

static void gpuinfo_tenstorrent_get_running_processes(struct gpu_info *_gpu_info) {
  struct gpu_info_tenstorrent *tt = container_of(_gpu_info, struct gpu_info_tenstorrent, base);
  _gpu_info->processes_count = 0;

  DIR *proc_dir = opendir("/proc");
  if (!proc_dir)
    return;

  struct dirent *proc_dent;
  while ((proc_dent = readdir(proc_dir)) != NULL) {
    if (proc_dent->d_type != DT_DIR || !isdigit(proc_dent->d_name[0]))
      continue;

    pid_t pid = (pid_t)atoi(proc_dent->d_name);
    if (!pid)
      continue;

    char fd_path[TT_PATH_MAX];
    snprintf(fd_path, sizeof(fd_path), "/proc/%d/fd", pid);
    int fd_dir_fd = open(fd_path, O_RDONLY | O_DIRECTORY);
    if (fd_dir_fd < 0)
      continue;

    char fdinfo_path[TT_PATH_MAX];
    snprintf(fdinfo_path, sizeof(fdinfo_path), "/proc/%d/fdinfo", pid);
    int fdinfo_dir_fd = open(fdinfo_path, O_RDONLY | O_DIRECTORY);
    if (fdinfo_dir_fd < 0) {
      close(fd_dir_fd);
      continue;
    }

    DIR *fd_dir = fdopendir(fd_dir_fd);
    if (!fd_dir) {
      close(fd_dir_fd);
      close(fdinfo_dir_fd);
      continue;
    }

    struct dirent *fd_dent;
    while ((fd_dent = readdir(fd_dir)) != NULL) {
      if (!isdigit(fd_dent->d_name[0]))
        continue;

      // Check if this fd points to /dev/tenstorrent/
      char link_target[TT_PATH_MAX];
      ssize_t len = readlinkat(fd_dir_fd, fd_dent->d_name, link_target, sizeof(link_target) - 1);
      if (len <= 0)
        continue;
      link_target[len] = '\0';

      static const char dev_prefix[] = "/dev/tenstorrent/";
      if (strncmp(link_target, dev_prefix, sizeof(dev_prefix) - 1) != 0)
        continue;

      // Read fdinfo for this fd
      int fi_fd = openat(fdinfo_dir_fd, fd_dent->d_name, O_RDONLY);
      if (fi_fd < 0)
        continue;
      FILE *fi = fdopen(fi_fd, "r");
      if (!fi) {
        close(fi_fd);
        continue;
      }

      unsigned device;
      unsigned long long dmabuf, pinned;
      bool ok = parse_tenstorrent_fdinfo(fi, &device, &dmabuf, &pinned);
      fclose(fi);

      if (!ok || device != tt->ordinal)
        continue;

      struct gpu_process *proc = find_or_alloc_process(_gpu_info, pid);
      if (!proc)
        continue;

      unsigned long long cur = 0;
      if (GPUINFO_PROCESS_FIELD_VALID(proc, gpu_memory_usage))
        cur = proc->gpu_memory_usage;
      SET_GPUINFO_PROCESS(proc, gpu_memory_usage, cur + dmabuf + pinned);
    }

    closedir(fd_dir); // also closes fd_dir_fd
    close(fdinfo_dir_fd);
  }

  closedir(proc_dir);
}

struct gpu_vendor gpu_vendor_tenstorrent = {
  .init = gpuinfo_tenstorrent_init,
  .shutdown = gpuinfo_tenstorrent_shutdown,
  .last_error_string = gpuinfo_tenstorrent_last_error_string,
  .get_device_handles = gpuinfo_tenstorrent_get_device_handles,
  .populate_static_info = gpuinfo_tenstorrent_populate_static_info,
  .refresh_dynamic_info = gpuinfo_tenstorrent_refresh_dynamic_info,
  .refresh_running_processes = gpuinfo_tenstorrent_get_running_processes,
  .name = "Tenstorrent",
};

__attribute__((constructor)) static void init_extract_gpuinfo_tenstorrent(void) {
  register_gpu_vendor(&gpu_vendor_tenstorrent);
}
