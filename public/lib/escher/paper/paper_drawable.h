// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_ESCHER_PAPER_PAPER_DRAWABLE_H_
#define LIB_ESCHER_PAPER_PAPER_DRAWABLE_H_

#include "lib/escher/paper/paper_readme.h"

#include "lib/escher/paper/paper_drawable_flags.h"

namespace escher {

// |PaperDrawable| is an abstract drawable object which can be rendered by
// |PaperRenderer2|, or anything that passes the right args to |DrawInScene()|.
//
// There are three levels of sophstication for users of |PaperDrawable|:
// 1) Use pre-existing subclasses of |PaperDrawable|.
// 2) Implement new subclass of |PaperDrawable| using the existing functionality
//    provided by arguments to |DrawInScene()|.
// 3) Experts only!!  Extend the functionality of e.g. |PaperDrawCallFactory| to
//    support the requirements of the new |PaperDrawable| subclass.
class PaperDrawable {
 public:
  // Subclasses have a large amount of freedom when implementing this method.
  // The only constraint is that any items pushed onto |transform_stack| must be
  // popped before the method returns.
  virtual void DrawInScene(const PaperScene* scene,
                           PaperDrawCallFactory* draw_call_factory,
                           PaperTransformStack* transform_stack, Frame* frame,
                           PaperDrawableFlags flags) = 0;
};

}  // namespace escher

#endif  // LIB_ESCHER_PAPER_PAPER_DRAWABLE_H_
