// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg(test)]

use super::codegen_test;

codegen_test!(alignment, AstBackend, ["banjo/alignment.test.banjo"], "ast/alignment.test.ast");
codegen_test!(empty, AstBackend, ["banjo/empty.test.banjo"], "ast/empty.test.ast");
codegen_test!(enums, AstBackend, ["banjo/enums.test.banjo"], "ast/enums.test.ast");
codegen_test!(example_0, AstBackend, ["banjo/example-0.test.banjo"], "ast/example-0.test.ast");
codegen_test!(example_1, AstBackend, ["banjo/example-1.test.banjo"], "ast/example-1.test.ast");
codegen_test!(example_2, AstBackend, ["banjo/example-2.test.banjo"], "ast/example-2.test.ast");
codegen_test!(example_3, AstBackend, ["banjo/example-3.test.banjo"], "ast/example-3.test.ast");
codegen_test!(example_4, AstBackend, ["banjo/example-4.test.banjo"], "ast/example-4.test.ast");
codegen_test!(example_6, AstBackend, ["banjo/example-6.test.banjo"], "ast/example-6.test.ast");
codegen_test!(example_7, AstBackend, ["banjo/example-7.test.banjo"], "ast/example-7.test.ast");
codegen_test!(example_8, AstBackend, ["banjo/example-8.test.banjo"], "ast/example-8.test.ast");
codegen_test!(example_9, AstBackend, ["banjo/example-9.test.banjo"], "ast/example-9.test.ast");
codegen_test!(point, AstBackend, ["banjo/point.test.banjo"], "ast/point.test.ast");
codegen_test!(tables, AstBackend, ["banjo/tables.test.banjo"], "ast/tables.test.ast");
codegen_test!(
    simple,
    AstBackend,
    ["../zx.banjo", "banjo/simple.test.banjo"],
    "ast/simple.test.ast"
);
codegen_test!(
    view,
    AstBackend,
    ["banjo/point.test.banjo", "banjo/view.test.banjo"],
    "ast/view.test.ast"
);
