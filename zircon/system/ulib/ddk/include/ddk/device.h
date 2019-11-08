// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DDK_DEVICE_H_
#define DDK_DEVICE_H_

#include <zircon/compiler.h>
#include <zircon/listnode.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

__BEGIN_CDECLS

typedef struct zx_device zx_device_t;
typedef struct zx_driver zx_driver_t;
typedef struct zx_device_prop zx_device_prop_t;

typedef struct zx_protocol_device zx_protocol_device_t;

typedef struct fidl_msg fidl_msg_t;
typedef struct fidl_txn fidl_txn_t;

// Max device name length, not including a null-terminator
#define ZX_DEVICE_NAME_MAX 31

// echo -n "zx_device_ops_v0.52" | sha256sum | cut -c1-16
#define DEVICE_OPS_VERSION_0_52 0xb834fdab33623bb4

// Current Version
#define DEVICE_OPS_VERSION DEVICE_OPS_VERSION_0_52

// TODO: temporary flags used by devcoord to communicate
// with the system bus device.
#define DEVICE_SUSPEND_FLAG_REBOOT 0xdcdc0100
#define DEVICE_SUSPEND_FLAG_POWEROFF 0xdcdc0200
#define DEVICE_SUSPEND_FLAG_MEXEC 0xdcdc0300
#define DEVICE_SUSPEND_FLAG_SUSPEND_RAM 0xdcdc0400
#define DEVICE_SUSPEND_REASON_MASK 0xffffff00

// These values should be same as the enum fuchsia_device_DevicePowerState
// generated from FIDL. The system wide power manager will be using the
// power states from FIDL generated file.
#define DEV_POWER_STATE_D0 UINT8_C(0)
#define DEV_POWER_STATE_D1 UINT8_C(1)
#define DEV_POWER_STATE_D2 UINT8_C(2)
#define DEV_POWER_STATE_D3HOT UINT8_C(3)
#define DEV_POWER_STATE_DCOLD UINT8_C(4)

// reboot modifiers
#define DEVICE_SUSPEND_FLAG_REBOOT_BOOTLOADER (DEVICE_SUSPEND_FLAG_REBOOT | 0x01)
#define DEVICE_SUSPEND_FLAG_REBOOT_RECOVERY (DEVICE_SUSPEND_FLAG_REBOOT | 0x02)

//@doc(docs/ddk/device-ops.md)

//@ # The Device Protocol
//
// Device drivers implement a set of hooks (methods) to support the
// operations that may be done on the devices that they publish.
//
// These are described below, including the action that is taken
// by the default implementation that is used for each hook if the
// driver does not provide its own implementation.

typedef struct zx_protocol_device {
  //@ ## version
  // This field must be set to `DEVICE_OPS_VERSION`
  uint64_t version;

  //@ ## get_protocol
  // The get_protocol hook is called when a driver invokes
  // **device_get_protocol()** on a device object.  The implementation must
  // populate *protocol* with a protocol structure determined by *proto_id*.
  // If the requested *proto_id* is not supported, the implementation must
  // return ZX_ERR_NOT_SUPPORTED.
  //
  // The default get_protocol hook returns with *protocol*=*proto_ops* if *proto_id*
  // matches the one given when **device_add()** created the device, and returns
  // ZX_ERR_NOT_SUPPORTED otherwise.
  //
  // See the **device_get_protocol()** docs for a description of the layout of
  // *protocol*.
  //
  // This hook is never called by the devhost runtime other than when
  // **device_get_protocol()** is invoked by some driver.  It is executed
  // synchronously in the same thread as the caller.
  zx_status_t (*get_protocol)(void* ctx, uint32_t proto_id, void* protocol);

  //@ ## open
  // The open hook is called when a device is opened via the device filesystem,
  // or when an existing open connection to a device is cloned (for example,
  // when a device fd is shared with another process).  The default open hook,
  // if a driver does not implement one, simply returns **ZX_OK**.
  //
  // Drivers may want to implement open to disallow simultaneous access (by
  // failing if the device is already open), or to return a new **device instance**
  // instead.
  //
  // The optional *dev_out* parameter allows a device to create and return a
  // **device instance** child device, which can be used to manage per-instance
  // state instead of all client connections interacting with the device itself.
  // A child created for return as an instance **must** be created with the
  // **DEVICE_ADD_INSTANCE** flag set in the arguments to **device_add()**.
  //
  // This hook is almost always called from the devhost's main thread.  The
  // one exception is if **device_add()** is invoked with *client_remote* provided and
  // the neither **DEVICE_ADD_MUST_ISOLATE** nor **DEVICE_ADD_INVISIBLE** were
  // provided, in which case this hook will be executed synchronously from the thread
  // that invoked **device_add()**, before **device_add()** returns.
  // DO NOT rely on that exception being true.  The implementation may in the
  // future push all invocations to the main thread.
  zx_status_t (*open)(void* ctx, zx_device_t** dev_out, uint32_t flags);

  //@ ## close
  // The close hook is called when a connection to a device is closed. These
  // calls will balance the calls to open.
  //
  // **Note:** If open returns a **device instance**, the balancing close hook
  // that is called is the close hook on the **instance**, not the parent.
  //
  // The default close implementation returns **ZX_OK**.
  //
  // This hook is almost always called from the devhost's main thread.  The one
  // exception is in the same situation as for the open hook described
  // above, in which the close hook may run to handle certain failure conditions
  // after the open hook ran.
  zx_status_t (*close)(void* ctx, uint32_t flags);

  //@ ## unbind
  // The unbind hook is called to begin removal of a device (due to hot unplug, fatal error, etc).
  //
  // The driver should avoid further method calls to its parent device or any
  // protocols obtained from that device, and expect that any further such calls
  // will return with an error.
  //
  // The driver should adjust its state to encourage its client connections to close
  // (cause IO to error out, etc), and call **device_unbind_reply()** on itself when ready.
  // See the docs for **device_unbind_reply()** for important semantics.
  //
  // The driver must continue to handle all device hooks until the **release** hook
  // is invoked.
  //
  // This is an optional hook. The default implementation will be a hook that replies
  // immediately with **device_unbind_reply()**.
  //
  // This hook will be called from the devhost's main thread. It will be executed sometime
  // after any of the following events occuring: **device_async_remove()** is invoked on the
  // device, the device's parent has completed its unbind hook via **device_unbind_reply**,
  // or a fuchsia.device.Controller/ScheduleUnbind request is received.
  void (*unbind)(void* ctx);

  //@ ## release
  // The release hook is called after this device has finished unbinding, all open client
  // connections of the device have been closed, and all child devices have been unbound and
  // released.
  //
  // At the point release is invoked, the driver will not receive any further calls
  // and absolutely must not use the underlying **zx_device_t** or any protocols obtained
  // from that device once this method returns.
  //
  // The driver must free all memory and release all resources related to this device
  // before returning.
  //
  // This hook may be called from any thread including the devhost's main
  // thread.
  void (*release)(void* ctx);

  //@ ## read
  // DEPRECATED: DO NOT ADD NEW USES
  //
  // The read hook is an attempt to do a non-blocking read operation.
  //
  // On success *actual* must be set to the number of bytes read (which may be less
  // than the number requested in *count*), and return **ZX_OK**.
  //
  // A successful read of 0 bytes is generally treated as an End Of File notification
  // by clients.
  //
  // If no data is available now, **ZX_ERR_SHOULD_WAIT** must be returned and when
  // data becomes available `device_state_set(DEVICE_STATE_READABLE)` may be used to
  // signal waiting clients.
  //
  // This hook **must not block**.
  //
  // The default read implementation returns **ZX_ERR_NOT_SUPPORTED**.
  //
  // This hook will only be executed on the devhost's main thread.
  zx_status_t (*read)(void* ctx, void* buf, size_t count, zx_off_t off, size_t* actual);

  //@ ## write
  // DEPRECATED: DO NOT ADD NEW USES
  //
  // The write hook is an attempt to do a non-blocking write operation.
  //
  // On success *actual* must be set to the number of bytes written (which may be
  // less than the number requested in *count*), and **ZX_OK** should be returned.
  //
  // If it is not possible to write data at present **ZX_ERR_SHOULD_WAIT** must
  // be returned and when it is again possible to write,
  // `device_state_set(DEVICE_STATE_WRITABLE)` may be used to signal waiting clients.
  //
  // This hook **must not block**.
  //
  // The default write implementation returns **ZX_ERR_NOT_SUPPORTED**.
  //
  // This hook will only be executed on the devhost's main thread.
  zx_status_t (*write)(void* ctx, const void* buf, size_t count, zx_off_t off, size_t* actual);

  //@ ## get_size
  // DEPRECATED: DO NOT ADD NEW USES
  //
  // If the device is seekable, the get_size hook should return the size of the device.
  //
  // This is the offset at which no more reads or writes are possible.
  //
  // The default implementation returns 0.
  //
  // This hook may be executed on any thread, including the devhost's main
  // thread.
  zx_off_t (*get_size)(void* ctx);

  //@ ## suspend_new
  // The suspend_new hook is used for suspending a device from a working to
  // non-working low power state(sleep state), or from a non-working sleep state
  // to a deeper sleep state.
  //
  // requested_state is always a non-working sleep state.
  // enable_wake is whether to configure the device for wakeup from the requested non
  // working sleep state. If enable_wake is true and the device does not support
  // wake up, the hook fails without suspending the device.
  //
  // On success, the out_state is same as the requested_state.
  // On failure, the device is not suspended and the out_state is the sleep state
  // that the device can go into. For ex: Devices(buses) cannot go into a deeper
  // sleep state when its children are suspended and configured to wake up from
  // their sleep states.
  //
  // This hook assumes that the drivers are aware of their current state.
  //
  // This hook will only be executed on the devhost's main thread.
  //
  // TODO(ravoorir): Remove the old suspend when all the drivers are moved to
  // new suspend and rename suspend_new to suspend.
  zx_status_t (*suspend_new)(void* ctx, uint8_t requested_state, bool enable_wake,
                             uint8_t* out_state);

  //@ ## resume_new
  // The resume_new hook is used for resuming a device from a non-working sleep
  // state to a working state. It requires reinitializing the device completely
  // or partially depending on the sleep state that device was in, when the
  // resume call was made.
  //
  // requested_state is always a working state. It is fully working D0 for now.
  // When we add more performant states, requested_state can be one of the working
  // performant state.
  //
  // On success, the out_state is one of the working states that the device is
  // in, although it might not be the requested_state.
  //
  // If the device, is not able to resume to a working state, the hook returns a
  // failure.
  //
  // This hook assumes that the drivers are aware of their current state.
  //
  // This hook will only be executed on the devhost's main thread.
  //
  // TODO(ravoorir): Remove the old resume when all the drivers are moved to
  // new suspend and resume.
  zx_status_t (*resume_new)(void* ctx, uint8_t requested_state, uint8_t* out_state);

  //@ ## set_performance_state
  // The set_performance_state hook is used for transitioning the performant state of
  // a device.
  //
  // requested_state is always a working performant state that is published during
  // device_add.
  //
  // On success, the out_state is same as the requested_state. If the device is in working state,
  // the transition is made immediately. If the device is in non working state, the device will be
  // in this state, when it is working again.
  // On failure, the out_state is the transition state that the device can go into.
  //
  // This hook assumes that the drivers are aware of their current sleep state and current
  // performance state.
  //
  // This hook will only be executed on the devhost's main thread.
  //
  zx_status_t (*set_performance_state)(void* ctx, uint32_t requested_state, uint32_t* out_state);

  // Stops the device and puts it in a low power mode
  // DEPRECATED: Use suspend_new instead.
  zx_status_t (*suspend)(void* ctx, uint32_t flags);

  // This hook is never invoked.
  // DEPRECATED: Use resume_new instead.
  zx_status_t (*resume)(void* ctx, uint32_t flags);

  //@ ## rxrpc
  // Only called for bus devices.
  // When the "shadow" of a busdev sends an rpc message, the
  // device that is shadowing is notified by the rxrpc op and
  // should attempt to read and respond to a single message on
  // the provided channel.
  //
  // Any error return from this method will result in the channel
  // being closed and the remote "shadow" losing its connection.
  //
  // This method is called with ZX_HANDLE_INVALID for the channel
  // when a new client connects -- at which point any state from
  // the previous client should be torn down.
  //
  // This hook will only be executed on the devhost's main thread.
  zx_status_t (*rxrpc)(void* ctx, zx_handle_t channel);

  //@ ## message
  // Process a FIDL rpc message.  This is used to handle class or
  // device specific messaging.  fuchsia.io.{Node,File,Device} are
  // handles by the devhost itself.
  //
  // The entire message becomes the responsibility of the driver,
  // including the handles.
  //
  // The txn provided to respond to the message is only valid for
  // the duration of the message() call.  It must not be cached
  // and used later.
  //
  // If this method returns anything other than ZX_OK, the underlying
  // connection is closed.
  //
  // This hook will only be executed on the devhost's main thread.
  zx_status_t (*message)(void* ctx, fidl_msg_t* msg, fidl_txn_t* txn);

} zx_protocol_device_t;

// Device Accessors
const char* device_get_name(zx_device_t* dev);

zx_device_t* device_get_parent(zx_device_t* dev) __DEPRECATE;

// protocols look like:
// typedef struct {
//     protocol_xyz_ops_t* ops;
//     void* ctx;
// } protocol_xyz_t;
zx_status_t device_get_protocol(const zx_device_t* dev, uint32_t proto_id, void* protocol);

// Direct Device Ops Functions

zx_off_t device_get_size(zx_device_t* dev);

// Device Metadata Support

// retrieves metadata for a specific device
// searches parent devices to find a match
zx_status_t device_get_metadata(zx_device_t* dev, uint32_t type, void* buf, size_t buflen,
                                size_t* actual);

// retrieves metadata size for a specific device
// searches parent devices to find a match
zx_status_t device_get_metadata_size(zx_device_t* dev, uint32_t type, size_t* out_size);

// Adds metadata to a specific device.
zx_status_t device_add_metadata(zx_device_t* dev, uint32_t type, const void* data, size_t length);

// Adds metadata to be provided to future devices matching the specified topo path.
// Drivers may use this to publish metadata to a driver with a topo path that matches
// itself or one of its children. Only drivers running in the "sys" devhost may publish
// metadata to arbitrary topo paths.
zx_status_t device_publish_metadata(zx_device_t* dev, const char* path, uint32_t type,
                                    const void* data, size_t length);

// Schedule a callback to be run at a later point. Similar to the device callbacks, it
// is *not* okay to block in the callback.
//
// The callback will be executed on the devhost's main thread.
zx_status_t device_schedule_work(zx_device_t* dev, void (*callback)(void*), void* cookie);

// Device State Change Functions.  These match up with the signals defined in
// the fuchsia.device.Controller interface.
//
//@ #### Device State Bits
//{
#define DEV_STATE_READABLE ZX_USER_SIGNAL_0
#define DEV_STATE_WRITABLE ZX_USER_SIGNAL_2
#define DEV_STATE_ERROR ZX_USER_SIGNAL_3
#define DEV_STATE_HANGUP ZX_USER_SIGNAL_4
#define DEV_STATE_OOB ZX_USER_SIGNAL_1
//}

void device_state_clr_set(zx_device_t* dev, zx_signals_t clearflag, zx_signals_t setflag);

//@ #### device_state_set
static inline void device_state_set(zx_device_t* dev, zx_signals_t stateflag) {
  device_state_clr_set(dev, 0, stateflag);
}
static inline void device_state_clr(zx_device_t* dev, zx_signals_t stateflag) {
  device_state_clr_set(dev, stateflag, 0);
}

__END_CDECLS

#endif  // DDK_DEVICE_H_
