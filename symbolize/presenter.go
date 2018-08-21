// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package symbolize

import (
	"fmt"
	"io"
)

// BacktracePresenter intercepts backtrace elements on their own line and
// presents them in text. Inlines are output as separate lines.
// A PostProcessor is taken as an input to synchronously compose another
// PostProcessor
type BacktracePresenter struct {
	out  io.Writer
	next PostProcessor
}

// NewBacktracePresenter constructs a BacktracePresenter.
func NewBacktracePresenter(out io.Writer, next PostProcessor) *BacktracePresenter {
	return &BacktracePresenter{
		out:  out,
		next: next,
	}
}

func printBacktrace(out io.Writer, hdr LineHeader, frame uint64, info addressInfo) {
	modRelAddr := info.addr - info.seg.Vaddr + info.seg.ModRelAddr
	if len(info.locs) == 0 {
		fmt.Fprintf(out, "%s    #%-4d %#016x in <%s>+%#x", hdr.Present(), frame, info.addr, info.mod.Name, modRelAddr)
		return
	}
	for i, _ := range info.locs {
		i = len(info.locs) - i - 1
		loc := info.locs[i]
		fmt.Fprintf(out, "%s    ", hdr.Present())
		var frameStr string
		if i == 0 {
			frameStr = fmt.Sprintf("#%d", frame)
		} else {
			frameStr = fmt.Sprintf("#%d.%d", frame, i)
		}
		fmt.Fprintf(out, "%-5s", frameStr)
		fmt.Fprintf(out, " %#016x", info.addr)
		if !loc.function.IsEmpty() {
			fmt.Fprintf(out, " in %v", loc.function)
		}
		if !loc.file.IsEmpty() {
			fmt.Fprintf(out, " %s:%d", loc.file, loc.line)
		}
		fmt.Fprintf(out, " <%s>+%#x\n", info.mod.Name, modRelAddr)
	}
}

func (b *BacktracePresenter) Process(line OutputLine, out chan<- OutputLine) {
	if len(line.line) == 1 {
		if bt, ok := line.line[0].(*BacktraceElement); ok {
			printBacktrace(b.out, line.header, bt.num, bt.info)
			// Don't process a backtrace we've already output.
			return
		}
	}
	b.next.Process(line, out)
}

// FilterContextElements filters out lines that only contain contextual
// elements and colors.
type FilterContextElements struct {
}

func (f *FilterContextElements) Process(line OutputLine, out chan<- OutputLine) {
	for _, token := range line.line {
		switch token.(type) {
		case *ColorCode, *ResetElement, *ModuleElement, *MappingElement:
			continue
		}
		out <- line
		return
	}
}

// OptimizeColor attempts to transform output elements to use as few color
// transisitions as is possible
type OptimizeColor struct {
}

func (o *OptimizeColor) Process(line OutputLine, out chan<- OutputLine) {
	// Maintain a current simulated color state
	curColor := uint64(0)
	curBold := false
	// Keep track of the color state at the end of 'out'
	color := uint64(0)
	bold := false
	// The new list of tokens we will output
	newLine := []Node{}
	// Go though each token
	for _, token := range line.line {
		if colorCode, ok := token.(*ColorCode); ok {
			// If we encounter a color update the simulated color state
			if colorCode.color == 1 {
				curBold = true
			} else if colorCode.color == 0 {
				curColor = 0
				curBold = false
			} else {
				curColor = colorCode.color
			}
		} else {
			// If we encounter a non-color token make sure we output
			// colors to handle the transition from the last color to the
			// new color.
			if curColor == 0 && color != 0 {
				color = 0
				bold = false
				newLine = append(newLine, &ColorCode{color: 0})
			} else if curColor != color {
				color = curColor
				newLine = append(newLine, &ColorCode{color: curColor})
			}
			// Make sure to bold the output even if a color 0 code was just output
			if curBold && !bold {
				bold = true
				newLine = append(newLine, &ColorCode{color: 1})
			}
			// Append all non-color nodes
			newLine = append(newLine, token)
		}
	}
	// If the color state isn't already clear, clear it
	if color != 0 || bold != false {
		newLine = append(newLine, &ColorCode{color: 0})
	}
	line.line = newLine
	out <- line
}

// BasicPresenter is a presenter to output very basic uncolored output
type BasicPresenter struct {
	enableColor bool
	output      io.Writer
}

func NewBasicPresenter(output io.Writer, enableColor bool) *BasicPresenter {
	return &BasicPresenter{output: output, enableColor: enableColor}
}

func (b *BasicPresenter) printSrcLoc(loc SourceLocation, info addressInfo) {
	modRelAddr := info.addr - info.seg.Vaddr + info.seg.ModRelAddr
	if !loc.function.IsEmpty() {
		fmt.Fprintf(b.output, "%s at ", loc.function)
	}
	if !loc.file.IsEmpty() {
		fmt.Fprintf(b.output, "%s:%d", loc.file, loc.line)
	} else {
		fmt.Fprintf(b.output, "<%s>+0x%x", info.mod.Name, modRelAddr)
	}
}

func (b *BasicPresenter) Process(res OutputLine, out chan<- OutputLine) {
	if res.header != nil {
		fmt.Fprintf(b.output, "%s ", res.header.Present())
	}
	for _, token := range res.line {
		switch node := token.(type) {
		case *BacktraceElement:
			if len(node.info.locs) == 0 {
				b.printSrcLoc(SourceLocation{}, node.info)
			}
			for i, loc := range node.info.locs {
				b.printSrcLoc(loc, node.info)
				if i != len(node.info.locs)-1 {
					fmt.Fprintf(b.output, " inlined from ")
				}
			}
		case *PCElement:
			if len(node.info.locs) > 0 {
				b.printSrcLoc(node.info.locs[0], node.info)
			} else {
				b.printSrcLoc(SourceLocation{}, node.info)
			}
		case *ColorCode:
			if b.enableColor {
				fmt.Fprintf(b.output, "\033[%dm", node.color)
			}
		case *Text:
			fmt.Fprintf(b.output, "%s", node.text)
		case *DumpfileElement:
			fmt.Fprintf(b.output, "{{{dumpfile:%s:%s}}}", node.sinkType, node.name)
		case *ResetElement:
			fmt.Fprintf(b.output, "{{{reset}}}")
		case *ModuleElement:
			fmt.Fprintf(b.output, "{{{module:%s:%s:%d}}}", node.mod.Build, node.mod.Name, node.mod.Id)
		case *MappingElement:
			fmt.Fprintf(b.output, "{{{mmap:0x%x:0x%x:load:%d:%s:0x%x}}}", node.seg.Vaddr, node.seg.Size, node.seg.Mod, node.seg.Flags, node.seg.ModRelAddr)
		}
	}
	fmt.Fprintf(b.output, "\n")
}
