// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package bindings2

type TestSimple struct {
	X int64
}

// Implements Payload.
func (_ *TestSimple) InlineAlignment() int {
	return 8
}

// Implements Payload.
func (_ *TestSimple) InlineSize() int {
	return 8
}

type TestSimpleBool struct {
	X bool
}

// Implements Payload.
func (_ *TestSimpleBool) InlineAlignment() int {
	return 1
}

// Implements Payload.
func (_ *TestSimpleBool) InlineSize() int {
	return 1
}

type TestAlignment1 struct {
	X int8
	Y int8
	Z uint32
}

// Implements Payload.
func (_ *TestAlignment1) InlineAlignment() int {
	return 4
}

// Implements Payload.
func (_ *TestAlignment1) InlineSize() int {
	return 8
}

type TestAlignment2 struct {
	A uint32
	B uint32
	C int8
	D int8
	E int8
	F uint8
	G uint32
	H uint16
	I uint16
}

// Implements Payload.
func (_ *TestAlignment2) InlineAlignment() int {
	return 4
}

// Implements Payload.
func (_ *TestAlignment2) InlineSize() int {
	return 20
}

type TestFloat1 struct {
	A float32
}

// Implements Payload.
func (_ *TestFloat1) InlineAlignment() int {
	return 4
}

// Implements Payload.
func (_ *TestFloat1) InlineSize() int {
	return 4
}

type TestFloat2 struct {
	A float64
}

// Implements Payload.
func (_ *TestFloat2) InlineAlignment() int {
	return 8
}

// Implements Payload.
func (_ *TestFloat2) InlineSize() int {
	return 8
}

type TestFloat3 struct {
	A float32
	B float64
	C uint64
	D float32
}

// Implements Payload.
func (_ *TestFloat3) InlineAlignment() int {
	return 8
}

// Implements Payload.
func (_ *TestFloat3) InlineSize() int {
	return 28
}

type TestArray1 struct {
	A [5]int8
}

// Implements Payload.
func (_ *TestArray1) InlineAlignment() int {
	return 1
}

// Implements Payload.
func (_ *TestArray1) InlineSize() int {
	return 5
}

type TestArray2 struct {
	A float64
	B [1]float32
}

// Implements Payload.
func (_ *TestArray2) InlineAlignment() int {
	return 8
}

// Implements Payload.
func (_ *TestArray2) InlineSize() int {
	return 12
}

type TestArray3 struct {
	A int32
	B [3]uint16
	C uint64
}

// Implements Payload.
func (_ *TestArray3) InlineAlignment() int {
	return 8
}

// Implements Payload.
func (_ *TestArray3) InlineSize() int {
	return 24
}

type TestArray4 struct {
	A [9]bool
}

// Implements Payload.
func (_ *TestArray4) InlineAlignment() int {
	return 1
}

// Implements Payload.
func (_ *TestArray4) InlineSize() int {
	return 9
}
