// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package tap_test

import (
	"os"

	"fuchsia.googlesource.com/tools/tap"
)

func ExampleProducer_single_test() {
	p := tap.NewProducer(os.Stdout)
	p.Plan(1)
	p.Ok(true, "- this test passed")
	// Output:
	// TAP version 13
	// 1..1
	// ok 1 - this test passed
}

func ExampleProducer_Todo() {
	p := tap.NewProducer(os.Stdout)
	p.Plan(1)
	p.Todo().Ok(true, "implement this test")
	// Output:
	// TAP version 13
	// 1..1
	// ok 1 # TODO implement this test
}

func ExampleProducer_Skip() {
	p := tap.NewProducer(os.Stdout)
	p.Plan(1)
	p.Skip().Ok(true, "implement this test")
	// Output:
	// TAP version 13
	// 1..1
	// ok 1 # SKIP implement this test
}

func ExampleProducer_many_test() {
	p := tap.NewProducer(os.Stdout)
	p.Plan(4)
	p.Ok(true, "- this test passed")
	p.Ok(false, "")
	p.Ok(false, "- this test failed")
	p.Skip().Ok(true, "this test is skippable")
	// Output:
	// TAP version 13
	// 1..4
	// ok 1 - this test passed
	// not ok 2
	// not ok 3 - this test failed
	// ok 4 # SKIP this test is skippable
}

func ExampleProducer_skip_todo_alternating() {
	p := tap.NewProducer(os.Stdout)
	p.Plan(4)
	p.Skip().Ok(true, "implement this test")
	p.Todo().Ok(false, "oh no!")
	p.Skip().Ok(false, "skipped another")
	p.Todo().Skip().Todo().Ok(true, "please don't write code like this")
	// Output:
	// TAP version 13
	// 1..4
	// ok 1 # SKIP implement this test
	// not ok 2 # TODO oh no!
	// not ok 3 # SKIP skipped another
	// ok 4 # TODO please don't write code like this
}
