// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FS_PAGED_VNODE_H_
#define FS_PAGED_VNODE_H_

#include <lib/async/cpp/wait.h>

#include <fs/vnode.h>

namespace fs {

class PagedVfs;

// A Vnode that supports paged I/O.
//
// To implement, derive from this class and:
//  - Implement Vnode::GetVmo().
//     - Use PagedVnode::EnsureCreateVmo() to create the data mapping. This will create it in such a
//       way that it's registered with the paging system for callbacks.
//     - Do vmo().create_child() to clone the VMO backing this node.
//     - Set the rights on the cloned VMO with the rights passed to GetVmo().
//     - Populate the GetVmo() out parameter with the child VMO.
//  - Implement VmoRead() to fill the VMO data when requested.
class PagedVnode : public Vnode {
 public:
  ~PagedVnode() override;

  // This will be null if the Vfs has shut down. Since Vnodes are refcounted, it's possible for them
  // to outlive their associated Vfs. Always null check before using. If there is no Vfs associated
  // with this object, all operations are expected to fail.
  PagedVfs* vfs() { return vfs_; }

  // This will be a null handle if there is no VMO associated with this vnode.
  zx::vmo& vmo() { return vmo_; }

  // Called by the paging system in response to a kernel request to fill data into this node's VMO.
  //
  //  - On success, calls vfs()->SupplyPages() with the created data range.
  //  - On failure, calls vfs()->ReportPagerError() with the error information.
  //
  // The success or failure cases can happen synchronously (from within this call stack) or
  // asynchronously in the future. Failure to report success or failure will hang the requesting
  // process.
  //
  // Note that offset + length will be page-aligned so can extend beyond the end of the file.
  virtual void VmoRead(uint64_t offset, uint64_t length) = 0;

  // Clears the vfs_ back pointer. Called when the associated PagedVfs is being destroyed.
  void DetachVfs();

 protected:
  explicit PagedVnode(PagedVfs* vfs);

  // Populates the vmo() if necessary. Does nothing if it already exists. Access the created vmo
  // with this class' vmo() getter.
  //
  // When a mapping is requested, the derived class should call this function and then create a
  // clone of this VMO with the desired flags. This class registers an observer for when the clone
  // count drops to 0 to clean up the VMO. This means that if the caller doesn't create a clone the
  // VMO could possibly leak.
  zx::status<> EnsureCreateVmo(uint64_t size);

  // Implementors of this class can override this function to response to the event that there
  // are no more clones of the vmo_. The default implementation frees the vmo_.
  virtual void OnNoClones();

 private:
  // Callback handler for the "no clones" message. Due to kernel message delivery race conditions
  // there might actually be clones. This checks and calls OnNoClones() when needed.
  void OnNoClonesMessage(async_dispatcher_t* dispatcher, async::WaitBase* wait, zx_status_t status,
                         const zx_packet_signal_t* signal);

  // Starts the clone_watcher_ to observe the case of no vmo_ clones. The WaitMethod is called only
  // once per "watch" call so this needs to be re-called after triggering. The vmo_ must exist.
  void WatchForZeroVmoClones();

  PagedVfs* vfs_;  // Non-owning, possibly null. See vfs() and DetachVfs() above.

  // The root VMO that paging happens out of for this vnode. VMOs that map the data into user
  // processes will be children of this VMO.
  zx::vmo vmo_;

  // Watches any clones of "vmo_" provided to clients. Observes the ZX_VMO_ZERO_CHILDREN signal.
  // See WatchForZeroChildren().
  async::WaitMethod<PagedVnode, &PagedVnode::OnNoClonesMessage> clone_watcher_;
};

}  // namespace fs

#endif  // FS_PAGED_VNODE_H_
