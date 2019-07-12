/*
 * Copyright (c) 2013 Broadcom Corporation
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
#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_BCDC_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_BCDC_H_

#include "core.h"
#include "device.h"
#include "netbuf.h"

struct brcmf_proto_bcdc_dcmd {
    uint32_t cmd;       /* dongle command value */
    uint32_t len;       /* lower 16: output buflen;
                         * upper 16: input buflen (excludes header) */
    uint32_t flags;     /* flag defns given below */
    int32_t status;     /* status code returned from the device */
};

zx_status_t brcmf_proto_bcdc_attach(struct brcmf_pub* drvr);
void brcmf_proto_bcdc_detach(struct brcmf_pub* drvr);
void brcmf_proto_bcdc_txflowblock(struct brcmf_device* dev, bool state);
void brcmf_proto_bcdc_txcomplete(struct brcmf_device* dev, struct brcmf_netbuf* txp, bool success);
struct brcmf_fws_info* drvr_to_fws(struct brcmf_pub* drvr);

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_BCDC_H_
