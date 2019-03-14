// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdio>
#include <cstring>
#include <thread>

#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/port.h>
#include <zx/event.h>
#include <zx/port.h>

#include "garnet/lib/debugger_utils/breakpoints.h"
#include "garnet/lib/debugger_utils/util.h"

using debugger_utils::ZxErrorString;

static void ExceptionHandlerThreadFunc(
    zx_handle_t thread, zx::port* eport, zx::event* event) {
  zx_koid_t tid = debugger_utils::GetKoid(thread);
  zx_status_t status = zx_task_bind_exception_port(
      thread, eport->get(), tid, 0);
  FXL_CHECK(status == ZX_OK) << "status: " << ZxErrorString(status);

  // Now that we've bound to the thread, notify the test.
  status = event->signal(0, ZX_EVENT_SIGNALED);
  FXL_CHECK(status == ZX_OK) << "status: " << ZxErrorString(status);

  for (;;) {
    zx_port_packet_t packet;
    zx_status_t status = eport->wait(zx::time::infinite(), &packet);
    FXL_CHECK(status == ZX_OK) << "status: " << ZxErrorString(status);

    if (packet.type == ZX_PKT_TYPE_USER) {
      // Sent to trigger loop exit.
      break;
    }

    FXL_CHECK(ZX_PKT_IS_EXCEPTION(packet.type));
    FXL_CHECK(packet.type == ZX_EXCP_SW_BREAKPOINT);
    FXL_CHECK(packet.key == tid);
    status = debugger_utils::ResumeAfterSoftwareBreakpointInstruction(
        thread, eport->get());
    FXL_CHECK(status == ZX_OK);
  }
}

static void SendSelfThread(zx_handle_t channel) {
  // Send the parent a packet so that it knows we've started.
  zx_handle_t self_copy;
  zx_status_t status = zx_handle_duplicate(
      zx_thread_self(), ZX_RIGHT_SAME_RIGHTS, &self_copy);
  FXL_CHECK(status == ZX_OK) << "status: " << ZxErrorString(status);
  status = zx_channel_write(channel, 0, nullptr, 0, &self_copy, 1);
  FXL_CHECK(status == ZX_OK) << "status: " << ZxErrorString(status);
}

static void WaitPeerClosed(zx_handle_t channel) {
  zx_status_t status = zx_object_wait_one(channel, ZX_CHANNEL_PEER_CLOSED,
                                          ZX_TIME_INFINITE, nullptr);
  FXL_CHECK(status == ZX_OK) << "status: " << ZxErrorString(status);
}

static int PerformWaitPeerClosed(zx_handle_t channel) {
  SendSelfThread(channel);
  WaitPeerClosed(channel);
  printf("wait-peer-closed complete\n");
  return 0;
}

static int TriggerSoftwareBreakpoint(zx_handle_t channel) {
  zx::port eport;
  zx_status_t status = zx::port::create(0, &eport);
  FXL_CHECK(status == ZX_OK) << "status: " << ZxErrorString(status);

  zx::event event;
  status = zx::event::create(0, &event);
  FXL_CHECK(status == ZX_OK) << "status: " << ZxErrorString(status);

  zx_handle_t self_thread = zx_thread_self();
  std::thread exception_thread(
      &ExceptionHandlerThreadFunc, self_thread, &eport, &event);

  // Don't trigger the s/w breakpoint until the exception loop is ready
  // to handle it.
  status = event.wait_one(ZX_EVENT_SIGNALED, zx::time::infinite(), nullptr);
  FXL_CHECK(status == ZX_OK) << "status: " << ZxErrorString(status);

  debugger_utils::TriggerSoftwareBreakpoint();

  WaitPeerClosed(channel);

  // Tell the exception thread to exit.
  zx_port_packet_t packet{};
  status = eport.queue(&packet);
  FXL_CHECK(status == ZX_OK) << "status: " << ZxErrorString(status);
  exception_thread.join();

  printf("trigger-sw-bkpt complete\n");
  return 0;
}

int main(int argc, char* argv[]) {
  if (argc > 2) {
    fprintf(stderr, "Usage: %s [command]\n", argv[0]);
    return 1;
  }

  zx_handle_t channel = zx_take_startup_handle(PA_HND(PA_USER0, 0));
  // If no channel was passed we're running standalone.
  if (channel == ZX_HANDLE_INVALID) {
    FXL_LOG(INFO) << "No handle provided";
  }

  if (argc == 2) {
    const char* cmd = argv[1];
    if (strcmp(cmd, "wait-peer-closed") == 0) {
      return PerformWaitPeerClosed(channel);
    }
    if (strcmp(cmd, "trigger-sw-bkpt") == 0) {
      return TriggerSoftwareBreakpoint(channel);
    }
    fprintf(stderr, "Unrecognized command: %s\n", cmd);
    return 1;
  }

  printf("Hello.\n");
  return 0;
}
