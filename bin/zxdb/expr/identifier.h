// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <string>
#include <vector>

#include "garnet/bin/zxdb/expr/expr_token.h"

namespace zxdb {

// An identifier is a sequence of names. Currently this handles C++ and Rust,
// but could be enhanced in the future for other languages.
//
// One component can consist of a name and a template part (note currently the
// parser doesn't support the template part, but this class does in expectation
// that parsing support will be added in the future).
//
//   Component := [ "::" ] <Name> [ "<" <Template-Goop> ">" ]
//
// An identifier consists of one or more components. In C++, if the first
// component has a valid separator token, it's fully qualified ("::foo"), but
// it could be omitted for non-fully-qualified names. Subsequent components
// will always have separators.
//
// The identifier contains the token information for the original so that
// it can be used for syntax highlighting.
class Identifier {
 public:
  class Component {
   public:
    Component();
    Component(ExprToken separator, ExprToken name)
        : separator_(std::move(separator)), name_(std::move(name)) {}

    // Constructor for names with templates. The contents will be a
    // vector of somewhat-normalized type string in between the <>.
    Component(ExprToken separator, ExprToken name, ExprToken template_begin,
              std::vector<std::string> template_contents,
              ExprToken template_end)
        : separator_(std::move(separator)),
          name_(std::move(name)),
          template_begin_(std::move(template_begin)),
          template_contents_(std::move(template_contents)),
          template_end_(std::move(template_end)) {}

    bool has_separator() const {
      return separator_.type() != ExprToken::kInvalid;
    }
    bool has_template() const {
      return template_begin_.type() != ExprToken::kInvalid;
    }

    const ExprToken& separator() const { return separator_; }
    void set_separator(ExprToken t) { separator_ = std::move(t); }

    const ExprToken& name() const { return name_; }

    // This will be kInvalid if there is no template on this component.
    // The begin and end are the <> tokens, and the contents is the normalized
    // string in between. Note that the contents may not exactly match the
    // input string (some whitespace may be removed).
    const ExprToken& template_begin() const { return template_begin_; }
    const std::vector<std::string>& template_contents() const { return template_contents_; }
    const ExprToken& template_end() const { return template_end_; }

   private:
    ExprToken separator_;
    ExprToken name_;

    ExprToken template_begin_;
    std::vector<std::string> template_contents_;
    ExprToken template_end_;
  };

  Identifier() = default;

  // Makes a simple identifier with a standalone name.
  Identifier(ExprToken name);

  std::vector<Component>& components() { return components_; }
  const std::vector<Component>& components() const { return components_; }

  void AppendComponent(Component c);
  void AppendComponent(ExprToken separator, ExprToken name);
  void AppendComponent(ExprToken separator, ExprToken name,
                       ExprToken template_begin, std::vector<std::string> template_contents,
                       ExprToken template_end);

  // Returns the full name with all components concatenated together.
  std::string GetFullName() const;

  // Returns a form for debugging where the parsing is more visible.
  std::string GetDebugName() const;

 private:
  // Backend for the name getters.
  std::string GetName(bool include_debug) const;

  std::vector<Component> components_;
};

}  // namespace zxdb
