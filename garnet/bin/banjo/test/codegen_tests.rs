// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg(test)]

use super::codegen_test;

mod c {
    use super::*;

    codegen_test!(alignment, CBackend, ["banjo/alignment.test.banjo"], "c/alignment.h");
    codegen_test!(empty, CBackend, ["banjo/empty.test.banjo"], "c/empty.h");
    codegen_test!(enums, CBackend, ["banjo/enums.test.banjo"], "c/enums.h");
    codegen_test!(example_0, CBackend, ["banjo/example-0.test.banjo"], "c/example-0.h");
    codegen_test!(example_1, CBackend, ["banjo/example-1.test.banjo"], "c/example-1.h");
    codegen_test!(example_2, CBackend, ["banjo/example-2.test.banjo"], "c/example-2.h");
    codegen_test!(example_3, CBackend, ["banjo/example-3.test.banjo"], "c/example-3.h");
    codegen_test!(example_4, CBackend, ["banjo/example-4.test.banjo"], "c/example-4.h");
    codegen_test!(example_6, CBackend, ["banjo/example-6.test.banjo"], "c/example-6.h");
    codegen_test!(example_7, CBackend, ["banjo/example-7.test.banjo"], "c/example-7.h");
    codegen_test!(example_8, CBackend, ["banjo/example-8.test.banjo"], "c/example-8.h");
    codegen_test!(example_9, CBackend, ["banjo/example-9.test.banjo"], "c/example-9.h");
    codegen_test!(point, CBackend, ["banjo/point.test.banjo"], "c/point.h");
    codegen_test!(table, CBackend, ["banjo/tables.test.banjo"], "c/tables.h");
    codegen_test!(
        simple,
        CBackend,
        ["../zx.banjo", "banjo/simple.test.banjo"],
        "c/simple.h"
    );
    codegen_test!(
        view,
        CBackend,
        ["banjo/point.test.banjo", "banjo/view.test.banjo"],
        "c/view.h"
    );
}

mod cpp {
    use super::*;

    codegen_test!(empty, CppBackend, ["banjo/empty.test.banjo"], "cpp/empty.h");
    codegen_test!(example_4, CppBackend, ["banjo/example-4.test.banjo"], "cpp/example-4.h");
    codegen_test!(example_6, CppBackend, ["banjo/example-6.test.banjo"], "cpp/example-6.h");
    codegen_test!(example_7, CppBackend, ["banjo/example-7.test.banjo"], "cpp/example-7.h");
    codegen_test!(example_9, CppBackend, ["banjo/example-9.test.banjo"], "cpp/example-9.h");
    codegen_test!(simple, CppBackend, ["../zx.banjo", "banjo/simple.test.banjo"], "cpp/simple.h");
    codegen_test!(
        view,
        CppBackend,
        ["banjo/point.test.banjo", "banjo/view.test.banjo"],
        "cpp/view.h"
    );
}

mod cpp_internal {
    use super::*;

    codegen_test!(empty, CppInternalBackend, ["banjo/empty.test.banjo"], "cpp/empty-internal.h");
    codegen_test!(
        example_4,
        CppInternalBackend,
        ["banjo/example-4.test.banjo"],
        "cpp/example-4-internal.h"
    );
    codegen_test!(
        example_6,
        CppInternalBackend,
        ["banjo/example-6.test.banjo"],
        "cpp/example-6-internal.h"
    );
    codegen_test!(
        example_7,
        CppInternalBackend,
        ["banjo/example-7.test.banjo"],
        "cpp/example-7-internal.h"
    );
    codegen_test!(
        example_9,
        CppInternalBackend,
        ["banjo/example-9.test.banjo"],
        "cpp/example-9-internal.h"
    );
    codegen_test!(
        simple,
        CppInternalBackend,
        ["../zx.banjo", "banjo/simple.test.banjo"],
        "cpp/simple-internal.h"
    );
    codegen_test!(
        view,
        CppInternalBackend,
        ["banjo/point.test.banjo", "banjo/view.test.banjo"],
        "cpp/view-internal.h"
    );
}
