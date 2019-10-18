/*
 * Copyright (c) 2010 Broadcom Corporation
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "common.h"

#include <stdarg.h>
#include <string.h>
#include <sys/random.h>
#include <zircon/status.h>

#include "brcmu_utils.h"
#include "brcmu_wifi.h"
#include "bus.h"
#include "debug.h"
#include "device.h"
#include "firmware.h"
#include "fwil.h"
#include "fwil_types.h"
#include "linuxisms.h"

MODULE_AUTHOR("Broadcom Corporation")
MODULE_DESCRIPTION("Broadcom 802.11 wireless LAN fullmac driver.")
MODULE_LICENSE("Dual BSD/GPL")

#define BRCMF_DEFAULT_SCAN_CHANNEL_TIME 40
#define BRCMF_DEFAULT_SCAN_UNASSOC_TIME 40

/* default boost value for RSSI_DELTA in preferred join selection */
#define BRCMF_JOIN_PREF_RSSI_BOOST 8

static int brcmf_p2p_enable;
module_param_named(p2pon, brcmf_p2p_enable, int, 0)
    MODULE_PARM_DESC(p2pon, "Enable legacy p2p management functionality")

        static int brcmf_feature_disable;
module_param_named(feature_disable, brcmf_feature_disable, int, 0)
    MODULE_PARM_DESC(feature_disable, "Disable features")

        static int brcmf_fcmode;
module_param_named(fcmode, brcmf_fcmode, int, 0)
    MODULE_PARM_DESC(fcmode, "Mode of firmware signalled flow control")

    /* Do not use internal roaming engine */
    static bool brcmf_roamoff = 1;

#if !defined(NDEBUG)
/* always succeed brcmf_bus_started() */
static int brcmf_ignore_probe_fail;
module_param_named(ignore_probe_fail, brcmf_ignore_probe_fail, int, 0)
    MODULE_PARM_DESC(ignore_probe_fail, "always succeed probe for debugging")
#endif  // !defined(NDEBUG)

        void brcmf_c_set_joinpref_default(struct brcmf_if* ifp) {
  struct brcmf_join_pref_params join_pref_params[2];
  zx_status_t err;
  int32_t fw_err = 0;

  /* Setup join_pref to select target by RSSI (boost on 5GHz) */
  join_pref_params[0].type = BRCMF_JOIN_PREF_RSSI_DELTA;
  join_pref_params[0].len = 2;
  join_pref_params[0].rssi_gain = BRCMF_JOIN_PREF_RSSI_BOOST;
  join_pref_params[0].band = WLC_BAND_5G;

  join_pref_params[1].type = BRCMF_JOIN_PREF_RSSI;
  join_pref_params[1].len = 2;
  join_pref_params[1].rssi_gain = 0;
  join_pref_params[1].band = 0;
  err = brcmf_fil_iovar_data_set(ifp, "join_pref", join_pref_params, sizeof(join_pref_params),
                                 &fw_err);
  if (err != ZX_OK) {
    BRCMF_ERR("Set join_pref error: %s, fw err %s\n", zx_status_get_string(err),
              brcmf_fil_get_errstr(fw_err));
  }
}

static zx_status_t brcmf_c_get_clm_name(struct brcmf_if* ifp, uint8_t* clm_name) {
  struct brcmf_bus* bus = ifp->drvr->bus_if;
  struct brcmf_rev_info* ri = &ifp->drvr->revinfo;
  uint8_t fw_name[BRCMF_FW_NAME_LEN];
  uint8_t* ptr;
  size_t len;
  zx_status_t err;

  memset(fw_name, 0, BRCMF_FW_NAME_LEN);
  err = brcmf_bus_get_fwname(bus, ri->chipnum, ri->chiprev, fw_name);
  if (err != ZX_OK) {
    BRCMF_ERR("get firmware name failed (%d)\n", err);
    goto done;
  }

  /* generate CLM blob file name */
  ptr = (uint8_t*)strrchr((char*)fw_name, '.');
  if (!ptr) {
    err = ZX_ERR_NOT_FOUND;
    goto done;
  }

  len = ptr - fw_name + 1;
  if (len + strlen(".clm_blob") > BRCMF_FW_NAME_LEN) {
    err = ZX_ERR_BUFFER_TOO_SMALL;
  } else {
    strlcpy((char*)clm_name, (const char*)fw_name, len);
    strlcat((char*)clm_name, ".clm_blob", BRCMF_FW_NAME_LEN);
  }
done:
  return err;
}

static zx_status_t brcmf_c_download(struct brcmf_if* ifp, uint16_t flag,
                                    struct brcmf_dload_data_le* dload_buf, uint32_t len) {
  zx_status_t status;
  uint32_t buflen;
  int32_t fw_err = 0;

  flag |= (DLOAD_HANDLER_VER << DLOAD_FLAG_VER_SHIFT);
  dload_buf->flag = flag;
  dload_buf->dload_type = DL_TYPE_CLM;
  dload_buf->len = len;
  dload_buf->crc = 0;
  buflen = sizeof(*dload_buf) + len;

  status = brcmf_fil_iovar_data_set(ifp, "clmload", dload_buf, buflen, &fw_err);
  if (status != ZX_OK) {
    BRCMF_ERR("clmload failed: %s, fw err %s\n", zx_status_get_string(status),
              brcmf_fil_get_errstr(fw_err));
  }
  return status;
}

/* callback routine for request_firmware_nowait() for reading in the contents of
 * CLM.blob
 */
static zx_status_t brcmf_request_clm_done(const struct brcmf_firmware* fw, void* ctx) {
  struct brcmf_if* ifp = (struct brcmf_if*)ctx;
  struct brcmf_dload_data_le* chunk_buf;
  uint32_t chunk_len;
  uint32_t datalen;
  uint32_t cumulative_len;
  uint16_t dl_flag = DL_BEGIN;
  uint32_t status;
  zx_status_t err;
  int32_t fw_err = 0;

  if (!fw) {
    return (ZX_ERR_INVALID_ARGS);
  }

  chunk_buf = static_cast<decltype(chunk_buf)>(calloc(1, sizeof(*chunk_buf) + MAX_CHUNK_LEN));
  if (!chunk_buf) {
    return (ZX_ERR_NO_MEMORY);
  }

  datalen = fw->size;
  cumulative_len = 0;
  do {
    if (datalen > MAX_CHUNK_LEN) {
      chunk_len = MAX_CHUNK_LEN;
    } else {
      chunk_len = datalen;
      dl_flag |= DL_END;
    }
    memcpy(chunk_buf->data, static_cast<char*>(fw->data) + cumulative_len, chunk_len);

    err = brcmf_c_download(ifp, dl_flag, chunk_buf, chunk_len);

    dl_flag &= ~DL_BEGIN;

    cumulative_len += chunk_len;
    datalen -= chunk_len;
  } while ((datalen > 0) && (err == ZX_OK));

  if (err != ZX_OK) {
    BRCMF_ERR("clmload (%zu byte file) failed (%d); ", fw->size, err);
    goto done;
  } else {
    BRCMF_INFO("CLM Load success\n");
  }
  err = brcmf_fil_iovar_int_get(ifp, "clmload_status", &status, &fw_err);
  if (err != ZX_OK) {
    BRCMF_ERR("get clmload_status failed: %s, fw err %s\n", zx_status_get_string(err),
              brcmf_fil_get_errstr(fw_err));
  } else {
    // If status is non-zero, CLM load failed, return error back to caller.
    if (status != 0) {
      BRCMF_ERR("clmload failed status=%d\n", status);
      err = ZX_ERR_IO;
    }
  }

done:
  free(chunk_buf);

  return err;
}

// Generate CLM blob filename from the firmware filename and if found, read and
// send it to firmware
static zx_status_t brcmf_c_process_clm_blob(struct brcmf_if* ifp) {
  uint8_t clm_name[BRCMF_FW_NAME_LEN];
  zx_status_t err;

  BRCMF_DBG(TRACE, "Enter\n");
  memset(clm_name, 0, BRCMF_FW_NAME_LEN);
  err = brcmf_c_get_clm_name(ifp, clm_name);
  if (err != ZX_OK) {
    BRCMF_ERR("get CLM blob file name failed (%d)\n", err);
    return err;
  }
  // Print out the CLM filename to the log
  BRCMF_ERR("CLM name %s\n", clm_name);

  // call request_firmware_nowait() to read out the contents of the
  // file into memory which is then processed by the callback to
  // write it out to firmware.
  return (request_firmware_nowait((const char*)clm_name, ifp->drvr, ifp, brcmf_request_clm_done));
}

static void brcmf_gen_random_mac_addr(uint8_t* mac_addr) {
  int err = getentropy(mac_addr, ETH_ALEN);
  ZX_ASSERT(!err);

  mac_addr[0] &= 0xfe;  // bit 0: 0 = unicast
  mac_addr[0] |= 0x02;  // bit 1: 1 = locally-administered
}

zx_status_t brcmf_set_macaddr_from_firmware(struct brcmf_if* ifp) {
  // Use static MAC address defined in the firmware.
  // eg. "macaddr" field of brcmfmac43455-sdio.txt
  uint8_t mac_addr[ETH_ALEN];
  int32_t fw_err = 0;

  zx_status_t err = brcmf_fil_iovar_data_get(ifp, "cur_etheraddr", mac_addr, ETH_ALEN, &fw_err);
  if (err != ZX_OK) {
    BRCMF_ERR("Retrieving mac address from firmware failed: %s, fw err %s\n",
              zx_status_get_string(err), brcmf_fil_get_errstr(fw_err));
    return err;
  }

  memcpy(ifp->mac_addr, mac_addr, sizeof(ifp->mac_addr));
  memcpy(ifp->drvr->mac, ifp->mac_addr, sizeof(ifp->drvr->mac));
  return ZX_OK;
}

static zx_status_t brcmf_set_macaddr(struct brcmf_if* ifp) {
  uint8_t mac_addr[ETH_ALEN];
  int32_t fw_err = 0;

  zx_status_t err = brcmf_bus_get_bootloader_macaddr(ifp->drvr->bus_if, mac_addr);
  if (err != ZX_OK) {
    // If desired, fall back to firmware mac address
    // by using brcmf_set_macaddr_from_firmware();

    // Fallback to a random mac address.
    BRCMF_ERR("Failed to get mac address from bootloader. Fallback to random mac address\n");
    brcmf_gen_random_mac_addr(mac_addr);
    BRCMF_ERR("random mac address to be assigned: %02x:%02x:%02x:%02x:%02x:%02x\n", mac_addr[0],
              mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
  }

  err = brcmf_fil_iovar_data_set(ifp, "cur_etheraddr", mac_addr, ETH_ALEN, &fw_err);
  if (err != ZX_OK) {
    BRCMF_ERR("Setting mac address failed: %s, fw err %s\n", zx_status_get_string(err),
              brcmf_fil_get_errstr(fw_err));
    return err;
  }

  memcpy(ifp->mac_addr, mac_addr, sizeof(ifp->mac_addr));
  memcpy(ifp->drvr->mac, ifp->mac_addr, sizeof(ifp->drvr->mac));
  return ZX_OK;
}

zx_status_t brcmf_c_preinit_dcmds(struct brcmf_if* ifp) {
  int8_t eventmask[BRCMF_EVENTING_MASK_LEN];
  uint8_t buf[BRCMF_DCMD_SMLEN];
  struct brcmf_rev_info_le revinfo;
  struct brcmf_rev_info* ri;
  char* clmver;
  char* ptr;
  zx_status_t err;
  int32_t fw_err = 0;

  err = brcmf_set_macaddr(ifp);
  if (err != ZX_OK) {
    goto done;
  }

  err = brcmf_fil_cmd_data_get(ifp, BRCMF_C_GET_REVINFO, &revinfo, sizeof(revinfo), &fw_err);
  ri = &ifp->drvr->revinfo;
  if (err != ZX_OK) {
    BRCMF_ERR("retrieving revision info failed: %s, fw err %s\n", zx_status_get_string(err),
              brcmf_fil_get_errstr(fw_err));
  } else {
    ri->vendorid = revinfo.vendorid;
    ri->deviceid = revinfo.deviceid;
    ri->radiorev = revinfo.radiorev;
    ri->chiprev = revinfo.chiprev;
    ri->corerev = revinfo.corerev;
    ri->boardid = revinfo.boardid;
    ri->boardvendor = revinfo.boardvendor;
    ri->boardrev = revinfo.boardrev;
    ri->driverrev = revinfo.driverrev;
    ri->ucoderev = revinfo.ucoderev;
    ri->bus = revinfo.bus;
    ri->chipnum = revinfo.chipnum;
    ri->phytype = revinfo.phytype;
    ri->phyrev = revinfo.phyrev;
    ri->anarev = revinfo.anarev;
    ri->chippkg = revinfo.chippkg;
    ri->nvramrev = revinfo.nvramrev;
  }
  ri->result = err;

  /* Do any CLM downloading */
  err = brcmf_c_process_clm_blob(ifp);
  if (err != ZX_OK) {
    BRCMF_ERR("download CLM blob file failed, %d\n", err);
  }

  /* query for 'ver' to get version info from firmware */
  memset(buf, 0, sizeof(buf));
  strcpy((char*)buf, "ver");
  err = brcmf_fil_iovar_data_get(ifp, "ver", buf, sizeof(buf), &fw_err);
  if (err != ZX_OK) {
    BRCMF_ERR("Retrieving version information failed: %s, fw err %s\n", zx_status_get_string(err),
              brcmf_fil_get_errstr(fw_err));
    goto done;
  } else {
    /* Print fw version info */
    BRCMF_ERR("Firmware version = %s\n", buf);
    ptr = (char*)buf;
    strsep(&ptr, "\n");

    /* locate firmware version number for ethtool */
    ptr = strrchr((char*)buf, ' ') + 1;
    strlcpy(ifp->drvr->fwver, ptr, sizeof(ifp->drvr->fwver));
  }

  /* Query for 'clmver' to get CLM version info from firmware */
  memset(buf, 0, sizeof(buf));
  err = brcmf_fil_iovar_data_get(ifp, "clmver", buf, sizeof(buf), &fw_err);
  if (err != ZX_OK) {
    BRCMF_DBG(TRACE, "retrieving clmver failed: %s, fw err %s\n", zx_status_get_string(err),
              brcmf_fil_get_errstr(fw_err));
  } else {
    clmver = (char*)buf;
    /* store CLM version for adding it to revinfo debugfs file */
    memcpy(ifp->drvr->clmver, clmver, sizeof(ifp->drvr->clmver));

    /* Replace all newline/linefeed characters with space
     * character
     */
    clmver[sizeof(buf) - 1] = 0;
    ptr = clmver;
    while ((ptr = strchr(ptr, '\n')) != NULL) {
      *ptr = ' ';
    }

    // Print out the CLM version to the log
    BRCMF_ERR("CLM version = %s\n", clmver);
  }

  /* set mpc */
  err = brcmf_fil_iovar_int_set(ifp, "mpc", 1, &fw_err);
  if (err != ZX_OK) {
    BRCMF_ERR("failed setting mpc: %s, fw err %s\n", zx_status_get_string(err),
              brcmf_fil_get_errstr(fw_err));
    // Does not work on all platforms. For now ignore the error and continue
  }

  brcmf_c_set_joinpref_default(ifp);

  /* Setup event_msgs, enable E_IF */
  err = brcmf_fil_iovar_data_get(ifp, "event_msgs", eventmask, BRCMF_EVENTING_MASK_LEN, &fw_err);
  if (err != ZX_OK) {
    BRCMF_ERR("Get event_msgs error: %s, fw err %s\n", zx_status_get_string(err),
              brcmf_fil_get_errstr(fw_err));
    goto done;
  }

  setbit(eventmask, BRCMF_E_IF);
  err = brcmf_fil_iovar_data_set(ifp, "event_msgs", eventmask, BRCMF_EVENTING_MASK_LEN, &fw_err);
  if (err != ZX_OK) {
    BRCMF_ERR("Set event_msgs error: %s, fw err %s\n", zx_status_get_string(err),
              brcmf_fil_get_errstr(fw_err));
    goto done;
  }

  /* Setup default scan channel time */
  err = brcmf_fil_cmd_int_set(ifp, BRCMF_C_SET_SCAN_CHANNEL_TIME, BRCMF_DEFAULT_SCAN_CHANNEL_TIME,
                              &fw_err);
  if (err != ZX_OK) {
    BRCMF_ERR("BRCMF_C_SET_SCAN_CHANNEL_TIME error: %s, fw err %s\n", zx_status_get_string(err),
              brcmf_fil_get_errstr(fw_err));
    goto done;
  }

  /* Setup default scan unassoc time */
  err = brcmf_fil_cmd_int_set(ifp, BRCMF_C_SET_SCAN_UNASSOC_TIME, BRCMF_DEFAULT_SCAN_UNASSOC_TIME,
                              &fw_err);
  if (err != ZX_OK) {
    BRCMF_ERR("BRCMF_C_SET_SCAN_UNASSOC_TIME error: %s, fw err %s\n", zx_status_get_string(err),
              brcmf_fil_get_errstr(fw_err));
    goto done;
  }

  /* Enable tx beamforming, errors can be ignored (not supported) */
  (void)brcmf_fil_iovar_int_set(ifp, "txbf", 1, nullptr);

  /* do bus specific preinit here */
  err = brcmf_bus_preinit(ifp->drvr->bus_if);
done:
  return err;
}

struct brcmf_mp_device* brcmf_get_module_param(enum brcmf_bus_type bus_type, uint32_t chip,
                                               uint32_t chiprev) {
  struct brcmf_mp_device* settings;

  BRCMF_DBG(TEMP, "Enter, bus=%d, chip=%d, rev=%d\n", bus_type, chip, chiprev);
  settings = static_cast<decltype(settings)>(calloc(1, sizeof(*settings)));
  if (!settings) {
    return NULL;
  }

  /* start by using the module paramaters */
  settings->p2p_enable = !!brcmf_p2p_enable;
  settings->feature_disable = brcmf_feature_disable;
  settings->fcmode = brcmf_fcmode;
  settings->roamoff = brcmf_roamoff;
#if !defined(NDEBUG)
  settings->ignore_probe_fail = !!brcmf_ignore_probe_fail;
#endif  // !defined(NDEBUG)

  if (bus_type == BRCMF_BUS_TYPE_SDIO) {
    // TODO(cphoenix): Do we really want to use default? (If so, delete =0 lines because calloc)
    settings->bus.sdio.sd_sgentry_align = 0;      // Use default
    settings->bus.sdio.sd_head_align = 0;         // Use default
    settings->bus.sdio.drive_strength = 0;        // Use default
    settings->bus.sdio.oob_irq_supported = true;  // TODO(cphoenix): Always?
  }
#ifdef USE_PLATFORM_DATA
  // TODO(WLAN-731): Do we need to do this?
  struct brcmfmac_pd_device {
    uint32_t bus_type;
    uint32_t id;
    int rev;
    struct brcmfmac_pd_cc country_codes[555];
    struct {
      void* sdio;
    } bus;
  };

  struct brcmfmac_pd_device* device_pd;
  bool found;
  int i;

  /* See if there is any device specific platform data configured */
  found = false;
  if (brcmfmac_pdata) {
    for (i = 0; i < brcmfmac_pdata->device_count; i++) {
      device_pd = &brcmfmac_pdata->devices[i];
      if ((device_pd->bus_type == bus_type) && (device_pd->id == chip) &&
          ((device_pd->rev == (int32_t)chiprev) || (device_pd->rev == -1))) {
        BRCMF_DBG(INFO, "Platform data for device found\n");
        settings->country_codes = device_pd->country_codes;
        if (device_pd->bus_type == BRCMF_BUS_TYPE_SDIO) {
          memcpy(&settings->bus.sdio, &device_pd->bus.sdio, sizeof(settings->bus.sdio));
        }
        found = true;
        break;
      }
    }
  }
#endif /* USE_PLATFORM_DATA */
  return settings;
}

void brcmf_release_module_param(struct brcmf_mp_device* module_param) { free(module_param); }
