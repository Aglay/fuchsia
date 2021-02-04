// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The implementation for the ConvertingTreeVisitor that re-prints a raw::File
// back into text format per some set of syntax rules.
#include "fidl/converter.h"

namespace fidl::conv {

void ConvertingTreeVisitor::OnFile(std::unique_ptr<fidl::raw::File> const& element) {
  last_conversion_end_ = element->start_.previous_end().data().data();
  DeclarationOrderTreeVisitor::OnFile(element);
  converted_output_ += last_conversion_end_;
}

void ConvertingTreeVisitor::OnStructDeclaration(const std::unique_ptr<raw::StructDeclaration> &element) {
  std::unique_ptr<Conversion> conv = std::make_unique<StructDeclarationConversion>(element->identifier, element->resourceness);
  Converting converting(this, std::move(conv), *element->decl_start_token, element->identifier->end_);
  TreeVisitor::OnStructDeclaration(element);
}

void ConvertingTreeVisitor::OnStructMember(const std::unique_ptr<raw::StructMember> &element) {
  std::unique_ptr<Conversion> conv = std::make_unique<NameAndTypeConversion>(element->identifier, element->type_ctor);
  Converting converting(this, std::move(conv), element->type_ctor->start_, element->end_);
  TreeVisitor::OnStructMember(element);
}

void ConvertingTreeVisitor::OnTypeConstructor(const std::unique_ptr<raw::TypeConstructor>& element) {
  std::unique_ptr<Conversion> conv = std::make_unique<TypeConversion>(element);
  Converting converting(this, std::move(conv), element->start_, element->end_);
  TreeVisitor::OnTypeConstructor(element);
}

Converting::Converting(ConvertingTreeVisitor* ctv, std::unique_ptr<Conversion> conversion, const Token& start, const Token& end)
    : ctv_(ctv) {
  const char* copy_from = ctv_->last_conversion_end_;
  const char* copy_until = start.data().data();
  const char* conversion_end = end.data().data() + end.data().length();

  if (conversion_end > ctv_->last_conversion_end_) {
    // We should only enter this block if we are in a nested conversion.
    ctv_->last_conversion_end_ = conversion_end;
  }
  if (copy_from < copy_until) {
    auto cr = std::make_unique<CopyRange>(copy_from, copy_until);
    conversion->AddPrefix(std::move(cr));
  }

  ctv_->open_conversions_.push(std::move(conversion));
}

Converting::~Converting() {
  std::unique_ptr<Conversion> conv = std::move(ctv_->open_conversions_.top());
  ctv_->open_conversions_.pop();
  std::string text = conv->Write(ctv_->to_syntax_);
  if (!ctv_->open_conversions_.empty()) {
    ctv_->open_conversions_.top()->AddChildText(text);
  } else {
    ctv_->converted_output_ += text;
  }
}

}  // namespace fidl::conv
