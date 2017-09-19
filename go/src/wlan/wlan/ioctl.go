// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package wlan

import (
	"fmt"
	"syscall/mx"
	"syscall/mx/fdio"
)

const ioctlFamilyWLAN = 0x24 // IOCTL_FAMILY_WLAN
const (
	ioctlOpGetChannel = 0 // IOCTL_WLAN_GET_CHANNEL,        IOCTL_KIND_GET_HANDLE
)

func ioctlGetChannel(m fdio.FDIO) (ch mx.Handle, err error) {
	num := fdio.IoctlNum(fdio.IoctlKindGetHandle, ioctlFamilyWLAN, ioctlOpGetChannel)
	h, err := m.Ioctl(num, nil, nil)
	if err != nil {
		return ch, fmt.Errorf("IOCTL_WLAN_GET_CHANNEL: %v", err)
	}
	if len(h) < 1 {
		return ch, fmt.Errorf("IOCTL_WLAN_GET_CHANNEL: received no handles")
	}
	ch = h[0]
	return ch, nil
}
