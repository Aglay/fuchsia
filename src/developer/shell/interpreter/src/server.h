// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_SHELL_INTERPRETER_SRC_SERVER_H_
#define SRC_DEVELOPER_SHELL_INTERPRETER_SRC_SERVER_H_

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "fuchsia/shell/cpp/fidl.h"
#include "fuchsia/shell/llcpp/fidl.h"
#include "lib/async-loop/cpp/loop.h"
#include "src/developer/shell/interpreter/src/interpreter.h"
#include "zircon/status.h"

namespace shell {
namespace interpreter {
namespace server {

class Service;

// Holds a context at the server level.
class ServerInterpreterContext {
 public:
  explicit ServerInterpreterContext(ExecutionContext* execution_context)
      : execution_context_(execution_context) {}

  ExecutionContext* execution_context() const { return execution_context_; }

  // True if there are unused AST nodes.
  bool PendingNodes() const { return !expressions_.empty() || !instructions_.empty(); }

  // Adds an expression to the context. This expression must be used later by another node.
  void AddExpression(std::unique_ptr<Expression> expression) {
    expressions_.emplace(expression->id(), std::move(expression));
  }

  // Adds an instruction to the context. This instruction must be used later by another node.
  void AddInstruction(std::unique_ptr<Instruction> instruction) {
    instructions_.emplace(instruction->id(), std::move(instruction));
  }

  // Retrieves the expression for the given node id. If the expression is found, the expression is
  // removes from the waiting instruction map.
  std::unique_ptr<Expression> GetExpression(const NodeId& node_id);

 private:
  // The execution context (interpreter level) associated with this context.
  ExecutionContext* const execution_context_;
  // All the expressions waiting to be used.
  std::map<NodeId, std::unique_ptr<Expression>> expressions_;
  // All the instructions waiting to be used.
  std::map<NodeId, std::unique_ptr<Instruction>> instructions_;
};

// Defines an interpreter managed by a server.
class ServerInterpreter : public Interpreter {
 public:
  explicit ServerInterpreter(Service* service) : service_(service) {}

  void EmitError(ExecutionContext* context, std::string error_message) override;
  void ContextDone(ExecutionContext* context) override;
  void ContextDoneWithAnalysisError(ExecutionContext* context) override;
  void TextResult(ExecutionContext* context, std::string_view text) override;

  // Gets the server context for the given id.
  ServerInterpreterContext* GetServerContext(uint64_t id) {
    auto context = contexts_.find(id);
    if (context != contexts_.end()) {
      return context->second.get();
    }
    return nullptr;
  }

  // Creates a server context associated with the interpreter context.
  void CreateServerContext(ExecutionContext* context);

  // Erases a server context.
  void EraseServerContext(uint64_t context_id) { contexts_.erase(context_id); }

  // Adds an expression to this context. The expression then waits to be used by another node.
  // The argument root_node should always be false.
  void AddExpression(ServerInterpreterContext* context, std::unique_ptr<Expression> expression,
                     bool root_node);

  // Adds an instruction to this context. If root_node is true, the instruction is added to the
  // interpreter context's pending instruction list.
  // If global node is false, the instructions waits to be used by another node.
  void AddInstruction(ServerInterpreterContext* context, std::unique_ptr<Instruction> instruction,
                      bool root_node);

  // Retrives the expression for the given context/node id. If the expression is not found, it emits
  // an error.
  std::unique_ptr<Expression> GetExpression(ServerInterpreterContext* context,
                                            const NodeId& node_id);

 private:
  // The service which currently holds the interpreter.
  Service* service_;
  // All the server contexts.
  std::map<uint64_t, std::unique_ptr<ServerInterpreterContext>> contexts_;
};

// Defines a connection from a client to the interpreter.
class Service final : public llcpp::fuchsia::shell::Shell::Interface {
 public:
  explicit Service(zx_handle_t handle)
      : handle_(handle), interpreter_(std::make_unique<ServerInterpreter>(this)) {}

  Interpreter* interpreter() const { return interpreter_.get(); }

  void CreateExecutionContext(uint64_t context_id,
                              CreateExecutionContextCompleter::Sync completer) override;
  void ExecuteExecutionContext(uint64_t context_id,
                               ExecuteExecutionContextCompleter::Sync completer) override;
  void AddNodes(uint64_t context_id,
                ::fidl::VectorView<::llcpp::fuchsia::shell::NodeDefinition> nodes,
                AddNodesCompleter::Sync _completer) override;

  // Helpers to be able to send events to the client.
  zx_status_t OnError(uint64_t context_id, std::vector<fuchsia::shell::Location>& locations,
                      std::string error_message) {
    fidl::Encoder encoder(fuchsia::shell::internal::kShell_OnError_GenOrdinal);
    auto message = fuchsia::shell::Shell_ResponseEncoder::OnError(&encoder, &context_id, &locations,
                                                                  &error_message);
    return message.Write(handle_, 0);
  }

  zx_status_t OnError(uint64_t context_id, std::string error_message) {
    std::vector<fuchsia::shell::Location> locations;
    return OnError(context_id, locations, error_message);
  }

  zx_status_t OnExecutionDone(uint64_t context_id, fuchsia::shell::ExecuteResult result) {
    fidl::Encoder encoder(fuchsia::shell::internal::kShell_OnExecutionDone_GenOrdinal);
    auto message =
        fuchsia::shell::Shell_ResponseEncoder::OnExecutionDone(&encoder, &context_id, &result);
    return message.Write(handle_, 0);
  }

  zx_status_t OnTextResult(uint64_t context_id, std::string result, bool partial_result) {
    fidl::Encoder encoder(fuchsia::shell::internal::kShell_OnTextResult_GenOrdinal);
    auto message = fuchsia::shell::Shell_ResponseEncoder::OnTextResult(&encoder, &context_id,
                                                                       &result, &partial_result);
    return message.Write(handle_, 0);
  }

 private:
  // Helpers to be able to create AST nodes.
  void AddIntegerLiteral(ServerInterpreterContext* context, uint64_t node_file_id,
                         uint64_t node_node_id, const llcpp::fuchsia::shell::IntegerLiteral& node,
                         bool root_node);

  void AddVariableDefinition(ServerInterpreterContext* context, uint64_t node_file_id,
                             uint64_t node_node_id,
                             const llcpp::fuchsia::shell::VariableDefinition& node, bool root_node);

  // The handle to communicate with the client.
  zx_handle_t handle_;
  // The interpreter associated with this service. An interpreter can only be associated to one
  // service.
  std::unique_ptr<ServerInterpreter> interpreter_;
};

// Class which accept connections from clients. Each time a new connection is accepted, a Service
// object is created.
class Server {
 public:
  Server();

  Service* AddConnection(zx_handle_t handle) {
    auto service = std::make_unique<Service>(handle);
    auto result = service.get();
    services_.emplace_back(std::move(service));
    return result;
  }

  bool Listen();
  void IncommingConnection(zx_handle_t service_request);
  void Run() { loop_.Run(); }

 private:
  async::Loop loop_;
  std::vector<std::unique_ptr<Service>> services_;
};

}  // namespace server
}  // namespace interpreter
}  // namespace shell

#endif  // SRC_DEVELOPER_SHELL_INTERPRETER_SRC_SERVER_H_
