#include "tools/fidlcat/lib/code_generator/test_generator.h"

#include <set>

#include "tools/fidlcat/lib/code_generator/cpp_visitor.h"
#include "tools/fidlcat/lib/syscall_decoder_dispatcher.h"

namespace fidlcat {

void TestGenerator::GenerateTests() {
  if (dispatcher_->processes().size() != 1) {
    std::cout << "Error: Cannot generate tests for more than one process.\n";
    return;
  }

  for (const auto event : dispatcher_->decoded_events()) {
    OutputEvent* output_event = event->AsOutputEvent();
    if (output_event) {
      auto call_info = OutputEventToFidlCallInfo(output_event);
      if (call_info) {
        AddFidlHeaderForInterface(call_info->enclosing_interface_name());
        AddEventToLog(std::move(call_info));
      }
    }
  }

  std::cout << "Writing tests on disk\n"
            << "  process name: " << dispatcher_->processes().begin()->second->name() << "\n"
            << "  output directory: " << output_directory_ << "\n";

  for (const auto& [handle_id, calls] : call_log()) {
    std::string protocol_name;

    for (const auto& call_info : calls) {
      if (protocol_name.empty()) {
        protocol_name = call_info->enclosing_interface_name();
      }

      std::cout << call_info->handle_id() << " ";
      switch (call_info->kind()) {
        case SyscallKind::kChannelWrite:
          std::cout << "zx_channel_write";
          break;
        case SyscallKind::kChannelRead:
          std::cout << "zx_channel_read";
          break;
        case SyscallKind::kChannelCall:
          std::cout << "zx_channel_call";
          break;
        default:
          break;
      }
      if (call_info->crashed()) {
        std::cout << " (crashed)";
      }
      std::cout << " " << call_info->enclosing_interface_name() << "." << call_info->method_name()
                << "\n";
    }

    WriteTestToFile(protocol_name);
    std::cout << "\n";
  }
}

std::vector<std::unique_ptr<std::vector<std::pair<FidlCallInfo*, FidlCallInfo*>>>>
TestGenerator::SplitChannelCallsIntoGroups(const std::vector<FidlCallInfo*>& calls) {
  size_t sequence_number = 0;
  std::set<std::string> fire_and_forgets;
  for (const auto& call_info : calls) {
    call_info->SetSequenceNumber(sequence_number++);

    if (call_info->kind() == SyscallKind::kChannelWrite) {
      fire_and_forgets.insert(call_info->method_name());
    } else if (call_info->kind() == SyscallKind::kChannelRead) {
      fire_and_forgets.erase(call_info->method_name());
    } else if (call_info->kind() == SyscallKind::kChannelCall) {
      call_info->SetSequenceNumber(sequence_number++);
    }
  }

  auto trace = std::make_unique<std::vector<std::pair<FidlCallInfo*, FidlCallInfo*>>>();
  auto events = std::make_unique<std::vector<std::pair<FidlCallInfo*, FidlCallInfo*>>>();
  std::map<std::pair<zx_handle_t, zx_txid_t>, FidlCallInfo*> unfinished_writes;
  std::vector<std::unique_ptr<std::vector<std::pair<FidlCallInfo*, FidlCallInfo*>>>> groups;

  for (const auto& call_info : calls) {
    auto write_key = std::make_pair(call_info->handle_id(), call_info->txid());

    if (call_info->kind() == SyscallKind::kChannelWrite) {
      if (fire_and_forgets.count(call_info->method_name()) == 0) {
        unfinished_writes[write_key] = call_info;
      } else {
        // Dealing with a fire and forget call
        trace->push_back(std::make_pair(call_info, nullptr));
      }
    } else if (call_info->kind() == SyscallKind::kChannelRead) {
      if (call_info->txid() != 0 && unfinished_writes.count(write_key) > 0) {
        // Succeeded in renconciling the write to the read
        trace->push_back(std::make_pair(unfinished_writes[write_key], call_info));
        unfinished_writes.erase(write_key);
      } else {
        // Dealing with an event
        trace->push_back(std::make_pair(nullptr, call_info));
      }
    } else if (call_info->kind() == SyscallKind::kChannelCall) {
      trace->push_back(std::make_pair(call_info, nullptr));
    }

    if (unfinished_writes.size() == 0) {
      // Sorts based on the order of write calls
      std::sort(trace->begin(), trace->end(),
                [](std::pair<FidlCallInfo*, FidlCallInfo*> c1,
                   std::pair<FidlCallInfo*, FidlCallInfo*> c2) {
                  return (c1.first ? c1.first : c1.second)->sequence_number() <
                         (c2.first ? c2.first : c2.second)->sequence_number();
                });
      // Adds the new group
      groups.emplace_back(std::move(trace));
      // Prepares for the next group
      trace = std::make_unique<std::vector<std::pair<FidlCallInfo*, FidlCallInfo*>>>();
    }
  }
  return groups;
}

void TestGenerator::WriteTestToFile(std::string_view protocol_name) {
  std::error_code err;
  std::filesystem::create_directories(output_directory_, err);
  if (err) {
    FX_LOGS(ERROR) << err.message();
    return;
  }

  std::filesystem::path file_name =
      output_directory_ /
      std::filesystem::path(ToSnakeCase(protocol_name) + "_" +
                            std::to_string(test_counter_[std::string(protocol_name)]++) + ".cc");
  std::cout << "... Writing to " << file_name << "\n";

  std::ofstream target_file;
  target_file.open(file_name, std::ofstream::out);
  if (target_file.fail()) {
    FX_LOGS(ERROR) << "Could not open " << file_name << "\n";
    return;
  }

  fidl_codec::PrettyPrinter printer =
      fidl_codec::PrettyPrinter(target_file, fidl_codec::WithoutColors, true, "", 0, false);

  GenerateIncludes(printer);

  target_file << "TEST(" << ToSnakeCase(dispatcher_->processes().begin()->second->name()) << ", "
              << ToSnakeCase(protocol_name) << ") {\n";
  target_file << "  Proxy proxy;\n";
  target_file << "  proxy.run();\n";
  target_file << "}\n";

  target_file.close();
}

void TestGenerator::GenerateAsyncCallsFromIterator(
    fidl_codec::PrettyPrinter& printer,
    const std::vector<std::pair<FidlCallInfo*, FidlCallInfo*>>& async_calls,
    std::vector<std::pair<FidlCallInfo*, FidlCallInfo*>>::iterator iterator,
    std::string_view final_statement) {
  if (iterator == async_calls.end()) {
    printer << final_statement;
    return;
  }

  FidlCallInfo* call_write = (*iterator).first;
  FidlCallInfo* call_read = (*iterator).second;

  std::vector<std::shared_ptr<fidl_codec::CppVariable>> input_arguments;
  // Print outline declaration of input
  if (call_write) {
    input_arguments = GenerateInputInitializers(printer, call_write);
  }

  // Print outline declaration of output
  std::vector<std::shared_ptr<fidl_codec::CppVariable>> output_arguments =
      GenerateOutputDeclarations(printer, call_read);

  // Make an async fidl call
  printer << "proxy_->";
  if (call_write) {
    printer << call_write->method_name();
  } else {
    printer << call_read->method_name();
  }
  printer << "(";

  // Pass input arguments to the fidl call
  std::string separator = "";
  for (const auto& argument : input_arguments) {
    printer << separator;
    argument->GenerateName(printer);
    separator = ", ";
  }

  printer << separator << "[this](";
  separator = "";
  for (const auto& argument : output_arguments) {
    // Pass output arguments by reference
    printer << separator;
    argument->GenerateTypeAndName(printer);
    separator = ", ";
  }

  printer << ") {\n";
  {
    fidl_codec::Indent indent(printer);
    separator = "";
    for (const auto& argument : output_arguments) {
      printer << separator;
      argument->GenerateAssertStatement(printer);
      separator = "\n";
    }
    printer << "\n";
    GenerateAsyncCallsFromIterator(printer, async_calls, std::next(iterator), final_statement);
  }
  printer << "});";

  printer << "\n";
}

void TestGenerator::GenerateAsyncCall(fidl_codec::PrettyPrinter& printer,
                                      std::pair<FidlCallInfo*, FidlCallInfo*> call_info_pair,
                                      std::string_view final_statement) {
  auto async_calls = std::vector<std::pair<FidlCallInfo*, FidlCallInfo*>>{call_info_pair};
  GenerateAsyncCallsFromIterator(printer, async_calls, async_calls.begin(), final_statement);
}

void TestGenerator::GenerateSyncCall(fidl_codec::PrettyPrinter& printer, FidlCallInfo* call_info) {
  fidl_codec::CppVisitor visitor_input = fidl_codec::CppVisitor();

  std::vector<std::shared_ptr<fidl_codec::CppVariable>> input_arguments =
      GenerateInputInitializers(printer, call_info);

  // Prints outline declaration of output
  std::vector<std::shared_ptr<fidl_codec::CppVariable>> output_arguments =
      CollectArgumentsFromDecodedValue("out_", call_info->decoded_output_value());

  for (const auto& argument : output_arguments) {
    argument->GenerateDeclaration(printer);
  }

  printer << "proxy_sync_->" << call_info->method_name();
  printer << "(";

  // Passes input arguments to the fidl call
  std::string separator = "";
  for (auto argument : input_arguments) {
    printer << separator;
    argument->GenerateName(printer);
    separator = ", ";
  }

  for (auto& argument : output_arguments) {
    printer << separator << "&";  // Passes output arguments by reference
    argument->GenerateName(printer);
    separator = ", ";
  }

  printer << ");\n";

  separator = "";
  for (const auto& argument : output_arguments) {
    printer << separator;
    argument->GenerateAssertStatement(printer);
    separator = "\n";
  }
}

void TestGenerator::GenerateEvent(fidl_codec::PrettyPrinter& printer, FidlCallInfo* call,
                                  std::string_view finish_statement) {
  // Prints outline declaration of output variables
  std::vector<std::shared_ptr<fidl_codec::CppVariable>> output_arguments =
      GenerateOutputDeclarations(printer, call);

  // Registers a callback for the event
  printer << "proxy_.events()." << call->method_name() << " = ";

  std::string separator = "";
  printer << "[this](";

  separator = "";
  for (auto& argument : output_arguments) {
    printer << separator;
    argument->GenerateTypeAndName(printer);
    separator = ", ";
  }

  printer << ") {\n";
  {
    fidl_codec::Indent indent(printer);
    separator = "";
    for (const auto& argument : output_arguments) {
      printer << separator;
      argument->GenerateAssertStatement(printer);
      separator = "\n";
    }
    printer << separator;
    printer << finish_statement;
  }
  printer << "};";

  printer << "\n";
}

void TestGenerator::GenerateFireAndForget(fidl_codec::PrettyPrinter& printer,
                                          FidlCallInfo* call_info) {
  std::vector<std::shared_ptr<fidl_codec::CppVariable>> input_arguments =
      GenerateInputInitializers(printer, call_info);

  printer << "proxy_->";
  printer << call_info->method_name();
  printer << "(";

  std::string separator = "";
  for (auto argument : input_arguments) {
    printer << separator;
    argument->GenerateName(printer);
    separator = ", ";
  }

  printer << ");";
  printer << "\n";
}

std::string TestGenerator::GenerateSynchronizingConditionalWithinGroup(
    std::vector<std::pair<FidlCallInfo*, FidlCallInfo*>>* batch, size_t index, size_t req_index,
    std::string_view final_statement) {
  std::ostringstream output;
  // Prints boolean values that ensure all responses in the group are received before proceeding to
  // the next group
  if (batch->size() > 1) {
    output << "received_" << index << "_" << req_index << "_ = "
           << "true;\n";
    output << "if (";
    auto separator = "";
    for (size_t i = 0; i < batch->size(); i++) {
      if (i != req_index) {
        output << separator << "received_" << index << "_" << i << "_";
        separator = " && ";
      }
    }
    output << ") {\n";
    output << "  " << final_statement;
    output << "}\n";
  } else {
    output << final_statement;
  }

  return output.str();
}

void TestGenerator::GenerateGroup(
    fidl_codec::PrettyPrinter& printer,
    std::vector<std::unique_ptr<std::vector<std::pair<FidlCallInfo*, FidlCallInfo*>>>>& groups,
    size_t index) {
  printer << "void Proxy::group_" << index << "() {\n";
  {
    fidl_codec::Indent indent(printer);
    std::string final_statement;

    if (index == groups.size() - 1) {
      final_statement = "loop_.Quit();\n";
    } else {
      final_statement = "group_" + std::to_string(index + 1) + "();\n";
    }

    // Prints each call within the group
    for (size_t i = 0; i < groups[index]->size(); i++) {
      std::pair<FidlCallInfo*, FidlCallInfo*> call_info_pair = groups[index]->at(i);
      std::string final_statement_join = GenerateSynchronizingConditionalWithinGroup(
          groups[index].get(), index, i, final_statement);

      if (call_info_pair.first && call_info_pair.second) {
        // Both elements of the pair are present. This is an async call.
        GenerateAsyncCall(printer, call_info_pair, final_statement_join);
      } else if (call_info_pair.second == nullptr) {
        // Only the first element of the pair is present. Either a a sync call, or a "fire and
        // forget".

        if (call_info_pair.first->kind() == SyscallKind::kChannelCall) {
          GenerateSyncCall(printer, call_info_pair.first);
        } else {
          GenerateFireAndForget(printer, call_info_pair.first);
        }
        printer << final_statement_join;
      } else if (call_info_pair.first == nullptr) {
        // Only the first element of the pair is present. This is an event.
        GenerateEvent(printer, call_info_pair.second, final_statement_join);
      }
    }
  }
  printer << "}\n";
}

std::vector<std::shared_ptr<fidl_codec::CppVariable>>
TestGenerator::CollectArgumentsFromDecodedValue(const std::string& variable_prefix,
                                                const fidl_codec::StructValue* struct_value) {
  std::vector<std::shared_ptr<fidl_codec::CppVariable>> cpp_vars;

  if (!struct_value) {
    return cpp_vars;
  }

  // The input to this method is the decoded_input_value/decoded_output_value from the message.
  // Each member in decoded_value will be treated as a argument to a HLCPP call,
  // Therefore we only need to traverse the decoded_value for one level.

  for (const std::unique_ptr<fidl_codec::StructMember>& struct_member :
       struct_value->struct_definition().members()) {
    const fidl_codec::Value* value = struct_value->GetFieldValue(struct_member->name());
    fidl_codec::CppVisitor visitor(AcquireUniqueName(variable_prefix + struct_member->name()));
    value->Visit(&visitor, struct_member->type());

    std::shared_ptr<fidl_codec::CppVariable> argument = visitor.result();
    cpp_vars.emplace_back(argument);
  }

  return cpp_vars;
}

std::vector<std::shared_ptr<fidl_codec::CppVariable>> TestGenerator::GenerateInputInitializers(
    fidl_codec::PrettyPrinter& printer, FidlCallInfo* call_info) {
  std::vector<std::shared_ptr<fidl_codec::CppVariable>> input_arguments =
      CollectArgumentsFromDecodedValue("in_", call_info->decoded_input_value());

  for (const auto& argument : input_arguments) {
    argument->GenerateInitialization(printer);
  }
  return input_arguments;
}

std::vector<std::shared_ptr<fidl_codec::CppVariable>> TestGenerator::GenerateOutputDeclarations(
    fidl_codec::PrettyPrinter& printer, FidlCallInfo* call_info) {
  std::vector<std::shared_ptr<fidl_codec::CppVariable>> output_arguments =
      CollectArgumentsFromDecodedValue("out_", call_info->decoded_output_value());

  for (const auto& argument : output_arguments) {
    argument->GenerateDeclaration(printer);
  }
  return output_arguments;
}

}  // namespace fidlcat
