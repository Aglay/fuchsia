// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/guest/pkg/biscotti_guest/bin/guest.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>
#include <memory>

#include <grpc++/grpc++.h>
#include <grpc++/server_posix.h>
#include <lib/async/default.h>
#include <lib/fdio/util.h>
#include <lib/fidl/cpp/vector.h>
#include <lib/fxl/logging.h>
#include <lib/fzl/fdio.h>
#include <zircon/processargs.h>

#include "garnet/bin/guest/pkg/biscotti_guest/third_party/protos/vm_guest.grpc.pb.h"

namespace biscotti {

// If this is true, a container shell is spawned on /dev/hvc0 logged into the
// default 'machina' user. If this is false then the shell on /dev/hvc0 will
// be a root shell for the VM.
//
// Generally 'true' here will be more useful but we'll keep it around to enable
// debugging any issues with container startup.
static constexpr bool kBootToContainer = true;

static constexpr const char* kLinuxEnvirionmentName = "biscotti";
static constexpr const char* kLinuxGuestPackage = "fuchsia-pkg://fuchsia.com/biscotti_guest#meta/biscotti_guest.cmx";
static constexpr uint32_t kStartupListenerPort = 7777;
static constexpr uint32_t kTremplinListenerPort = 7778;
static constexpr uint32_t kMaitredPort = 8888;
static constexpr uint32_t kGarconPort = 8889;
static constexpr uint32_t kTremplinPort = 8890;
static constexpr uint32_t kLogCollectorPort = 9999;
static constexpr const char* kVmShellCommand = "/bin/sh";
static constexpr const char* kContainerName = "stretch";
static constexpr const char* kContainerImageAlias = "debian/stretch";
static constexpr const char* kContainerImageServer =
    "https://storage.googleapis.com/cros-containers";
static constexpr const char* kDefaultContainerUser = "machina";

// Minfs max file size is currently just under 4GB.
static constexpr off_t kStatefulImageSize = 4000ul * 1024 * 1024;
static constexpr const char* kStatefulImagePath = "/data/stateful.img";

static fidl::InterfaceHandle<fuchsia::io::File> GetOrCreateStatefulPartition() {
  int fd = open(kStatefulImagePath, O_RDWR | O_CREAT);
  if (fd < 0) {
    FXL_LOG(ERROR) << "Failed to open image: " << strerror(errno);
    return nullptr;
  }
  if (ftruncate(fd, kStatefulImageSize) < 0) {
    FXL_LOG(ERROR) << "Failed to truncate image: " << strerror(errno);
    return nullptr;
  }

  zx_handle_t handle = ZX_HANDLE_INVALID;
  zx_status_t status = fdio_get_service_handle(fd, &handle);
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "Failed to get service handle: " << status;
    return nullptr;
  }
  return fidl::InterfaceHandle<fuchsia::io::File>(zx::channel(handle));
}

static fidl::VectorPtr<fuchsia::guest::BlockDevice> GetBlockDevices() {
  auto file_handle = GetOrCreateStatefulPartition();
  FXL_CHECK(file_handle) << "Failed to open stateful file";
  fidl::VectorPtr<fuchsia::guest::BlockDevice> devices;
  devices.push_back({
    "stateful",
    fuchsia::guest::BlockMode::READ_WRITE,
    fuchsia::guest::BlockFormat::RAW,
    std::move(file_handle),
  });
  return devices;
}

static int convert_socket_to_fd(zx::socket socket) {
  int fd;
  uint32_t type = PA_FDIO_SOCKET;
  auto handle = socket.release();
  zx_status_t status = fdio_create_fd(&handle, &type, 1, &fd);
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "Could not get client fdio endpoint";
    return -1;
  }

  auto flags = fcntl(fd, F_GETFL);
  if (flags == -1) {
    FXL_LOG(ERROR) << "fcntl(F_GETFL) failed: " << strerror(errno);
    return -1;
  }

  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    FXL_LOG(ERROR) << "fcntl(F_SETFL) failed: " << strerror(errno);
    return -1;
  }
  return fd;
}

// static
zx_status_t Guest::CreateAndStart(component::StartupContext* context,
                                  fxl::CommandLine cl,
                                  std::unique_ptr<Guest>* guest) {
  FXL_LOG(INFO) << "Creating Guest Environment...";
  fuchsia::guest::EnvironmentManagerPtr guestmgr;
  context->ConnectToEnvironmentService(guestmgr.NewRequest());
  fuchsia::guest::EnvironmentControllerPtr guest_env;
  guestmgr->Create(kLinuxEnvirionmentName, guest_env.NewRequest());

  *guest = std::make_unique<Guest>(context, std::move(guest_env), std::move(cl));
  return ZX_OK;
}

Guest::Guest(component::StartupContext* context,
             fuchsia::guest::EnvironmentControllerPtr env,
             fxl::CommandLine cl)
    : guest_env_(std::move(env)), cl_(std::move(cl)), wayland_dispatcher_(context) {
  guest_env_->GetHostVsockEndpoint(socket_endpoint_.NewRequest());
  async_ = async_get_default_dispatcher();
  Start();
}

void Guest::Start() {
  StartGrpcServer();
  StartGuest();
}

// A thin wrapper around |grpc::ServerBuilder| that also registers the service
// ports with the |HostVsockEndpoint|.
class GrpcServerBuilder {
 public:
  // The BindingFactory is a function that returns an |InterfaceHandle| to use
  // for a new binding.
  using BindingFactory =
      fit::function<fidl::InterfaceHandle<fuchsia::guest::HostVsockAcceptor>()>;

  GrpcServerBuilder(
      const fuchsia::guest::HostVsockEndpointSyncPtr& socket_endpoint,
      BindingFactory binding_factory)
      : binding_factory_(std::move(binding_factory)),
        socket_endpoint_(socket_endpoint) {}

  // Registers the service on the provided vsock port.
  //
  // Note that this actually makes all services available on all ports. Ex,
  // if you register 'service A' on 'port A' and 'service B' on 'port B',
  // requests for 'service B' that are sent to 'port A' would still be handled.
  // This is because all the services are backed by the same gRPC server
  // instance.
  zx_status_t RegisterService(uint32_t vsock_port, grpc::Service* service) {
    builder_.RegisterService(service);
    zx_status_t listen_status, fidl_status;
    fidl_status = socket_endpoint_->Listen(vsock_port, binding_factory_(),
                                           &listen_status);
    if (fidl_status != ZX_OK) {
      FXL_LOG(ERROR) << "Failed to perform vsock Listen RCP";
      return fidl_status;
    }
    return listen_status;
  }

  // Constructs the |grpc::Server| and starts processing any in-bound requests
  // on the sockets.
  std::unique_ptr<grpc::Server> Build() { return builder_.BuildAndStart(); }

 private:
  BindingFactory binding_factory_;
  const fuchsia::guest::HostVsockEndpointSyncPtr& socket_endpoint_;
  grpc::ServerBuilder builder_;
};

void Guest::StartGrpcServer() {
  FXL_LOG(INFO) << "Starting GRPC server...";
  zx_status_t status;
  GrpcServerBuilder builder(socket_endpoint_, [this]() {
    return acceptor_bindings_.AddBinding(this);
  });

  status = builder.RegisterService(kLogCollectorPort, &log_collector_);
  FXL_CHECK(status == ZX_OK) << "Failed to register LogCollector service.";

  status = builder.RegisterService(
      kStartupListenerPort,
      static_cast<vm_tools::StartupListener::Service*>(this));
  FXL_CHECK(status == ZX_OK) << "Failed to register StartupListener service.";

  status = builder.RegisterService(
      kTremplinListenerPort,
      static_cast<vm_tools::tremplin::TremplinListener::Service*>(this));
  FXL_CHECK(status == ZX_OK) << "Failed to register TremplinListener service.";

  status = builder.RegisterService(
      kGarconPort,
      static_cast<vm_tools::container::ContainerListener::Service*>(this));
  FXL_CHECK(status == ZX_OK) << "Failed to register ContainerListener service.";

  grpc_server_ = builder.Build();
}

void Guest::StartGuest() {
  FXL_CHECK(!guest_controller_)
      << "Called StartGuest with an existing instance";
  FXL_LOG(INFO) << "Launching guest...";

  fuchsia::guest::LaunchInfo launch_info;
  launch_info.url = kLinuxGuestPackage;
  launch_info.args.push_back("--virtio-gpu=false");
  launch_info.block_devices = GetBlockDevices();
  launch_info.wayland_device = fuchsia::guest::WaylandDevice::New();
  launch_info.wayland_device->dispatcher = wayland_dispatcher_.NewBinding();
  guest_env_->LaunchInstance(
      std::move(launch_info), guest_controller_.NewRequest(),
      [this](uint32_t cid) {
        FXL_LOG(INFO) << "Guest launched with CID " << cid;
        guest_cid_ = cid;
      });
}

void Guest::ConfigureNetwork() {
  FXL_CHECK(maitred_)
      << "Called ConfigureNetwork without a maitre'd connection";
  std::string arg;
  struct in_addr addr;

  uint32_t ip_addr = 0;
  if (!cl_.GetOptionValue("ip", &arg)) {
    arg = BISCOTTI_IP_DEFAULT;
  }
  FXL_LOG(INFO) << "Using ip: " << arg;
  FXL_CHECK(inet_aton(arg.c_str(), &addr) != 0)
      << "Failed to parse address string";
  ip_addr = addr.s_addr;

  uint32_t netmask = 0;
  if (!cl_.GetOptionValue("netmask", &arg)) {
    arg = BISCOTTI_NETMASK_DEFAULT;
  }
  FXL_LOG(INFO) << "Using netmask: " << arg;
  FXL_CHECK(inet_aton(arg.c_str(), &addr) != 0)
      << "Failed to parse address string";
  netmask = addr.s_addr;

  uint32_t gateway = 0;
  if (!cl_.GetOptionValue("gateway", &arg)) {
    arg = BISCOTTI_GATEWAY_DEFAULT;
  }
  FXL_LOG(INFO) << "Using gateway: " << arg;
  FXL_CHECK(inet_aton(arg.c_str(), &addr) != 0)
      << "Failed to parse address string";
  gateway = addr.s_addr;
  FXL_LOG(INFO) << "Configuring Guest Network...";

  grpc::ClientContext context;
  vm_tools::NetworkConfigRequest request;
  vm_tools::EmptyMessage response;

  vm_tools::IPv4Config* config = request.mutable_ipv4_config();
  config->set_address(ip_addr);
  config->set_gateway(gateway);
  config->set_netmask(netmask);

  auto grpc_status = maitred_->ConfigureNetwork(&context, request, &response);
  FXL_CHECK(grpc_status.ok())
      << "Failed to configure guest network: " << grpc_status.error_message();
  FXL_LOG(INFO) << "Network configured.";
}

void Guest::StartTermina() {
  FXL_CHECK(maitred_) << "Called StartTermina without a maitre'd connection";
  FXL_LOG(INFO) << "Starting Termina...";

  grpc::ClientContext context;
  vm_tools::StartTerminaRequest request;
  vm_tools::StartTerminaResponse response;
  std::string lxd_subnet = "100.115.92.1/24";
  request.mutable_lxd_ipv4_subnet()->swap(lxd_subnet);

  auto grpc_status = maitred_->StartTermina(&context, request, &response);
  FXL_CHECK(grpc_status.ok())
      << "Failed to start Termina: " << grpc_status.error_message();
}

// This exposes a shell on /dev/hvc0 that can be used to interact with the
// VM.
void Guest::LaunchVmShell() {
  FXL_CHECK(maitred_) << "Called LaunchShell without a maitre'd connection";
  FXL_LOG(INFO) << "Launching '" << kVmShellCommand << "'...";

  grpc::ClientContext context;
  vm_tools::LaunchProcessRequest request;
  vm_tools::LaunchProcessResponse response;

  request.add_argv()->assign(kVmShellCommand);
  request.set_respawn(true);
  request.set_use_console(true);
  request.set_wait_for_exit(false);
  {
    auto env = request.mutable_env();
    // These make the lxd/lxc commands behave as expected from the shell.
    env->insert({"LXD_DIR", "/mnt/stateful/lxd"});
    env->insert({"LXD_CONF", "/mnt/stateful/lxd_conf"});
    env->insert({"LXD_UNPRIVILEGED_ONLY", "true"});
  }

  auto status = maitred_->LaunchProcess(&context, request, &response);
  FXL_CHECK(status.ok()) << "Failed to launch '" << kVmShellCommand
                         << "': " << status.error_message();
}

// This exposes a shell on /dev/hvc0 that can be used to interact with the
// VM.
void Guest::LaunchContainerShell() {
  FXL_CHECK(maitred_) << "Called LaunchShell without a maitre'd connection";
  FXL_LOG(INFO) << "Launching container shell...";

  grpc::ClientContext context;
  vm_tools::LaunchProcessRequest request;
  vm_tools::LaunchProcessResponse response;

  request.add_argv()->assign("/usr/bin/lxc");
  request.add_argv()->assign("exec");
  request.add_argv()->assign(kContainerName);
  request.add_argv()->assign("--");
  request.add_argv()->assign("/bin/login");
  request.add_argv()->assign("-f");
  request.add_argv()->assign(kDefaultContainerUser);

  request.set_respawn(true);
  request.set_use_console(true);
  request.set_wait_for_exit(false);
  {
    auto env = request.mutable_env();
    env->insert({"LXD_DIR", "/mnt/stateful/lxd"});
    env->insert({"LXD_CONF", "/mnt/stateful/lxd_conf"});
    env->insert({"LXD_UNPRIVILEGED_ONLY", "true"});
  }

  auto status = maitred_->LaunchProcess(&context, request, &response);
  FXL_CHECK(status.ok()) << "Failed to launch container shell: "
                         << status.error_message();
}

void Guest::CreateContainer() {
  FXL_CHECK(tremplin_)
      << "CreateContainer called without a Tremplin connection";
  FXL_LOG(INFO) << "Creating Container...";

  grpc::ClientContext context;
  vm_tools::tremplin::CreateContainerRequest request;
  vm_tools::tremplin::CreateContainerResponse response;

  request.mutable_container_name()->assign(kContainerName);
  request.mutable_image_alias()->assign(kContainerImageAlias);
  request.mutable_image_server()->assign(kContainerImageServer);

  auto status = tremplin_->CreateContainer(&context, request, &response);
  FXL_CHECK(status.ok()) << "Failed to create container: "
                         << status.error_message();
  switch (response.status()) {
    case vm_tools::tremplin::CreateContainerResponse::CREATING:
      break;
    case vm_tools::tremplin::CreateContainerResponse::EXISTS:
      FXL_LOG(INFO) << "Container already exists";
      StartContainer();
      break;
    case vm_tools::tremplin::CreateContainerResponse::FAILED:
      FXL_LOG(ERROR) << "Failed to create container: "
                     << response.failure_reason();
      break;
    case vm_tools::tremplin::CreateContainerResponse::UNKNOWN:
    default:
      FXL_LOG(ERROR) << "Unknown status: " << response.status();
      break;
  }
}

void Guest::StartContainer() {
  FXL_CHECK(tremplin_) << "StartContainer called without a Tremplin connection";
  FXL_LOG(INFO) << "Starting Container...";

  grpc::ClientContext context;
  vm_tools::tremplin::StartContainerRequest request;
  vm_tools::tremplin::StartContainerResponse response;

  request.mutable_container_name()->assign(kContainerName);
  request.mutable_host_public_key()->assign("");
  request.mutable_container_private_key()->assign("");
  request.mutable_token()->assign("container_token");

  auto status = tremplin_->StartContainer(&context, request, &response);
  FXL_CHECK(status.ok()) << "Failed to start container: "
                         << status.error_message();

  switch (response.status()) {
    case vm_tools::tremplin::StartContainerResponse::RUNNING:
    case vm_tools::tremplin::StartContainerResponse::STARTED:
      FXL_LOG(INFO) << "Container started";
      SetupUser();
      break;
    case vm_tools::tremplin::StartContainerResponse::FAILED:
      FXL_LOG(ERROR) << "Failed to start container: "
                     << response.failure_reason();
      break;
    case vm_tools::tremplin::StartContainerResponse::UNKNOWN:
    default:
      FXL_LOG(ERROR) << "Unknown status: " << response.status();
      break;
  }
}

void Guest::SetupUser() {
  FXL_CHECK(tremplin_) << "SetupUser called without a Tremplin connection";
  FXL_LOG(INFO) << "Creating user '" << kDefaultContainerUser << "'...";

  grpc::ClientContext context;
  vm_tools::tremplin::SetUpUserRequest request;
  vm_tools::tremplin::SetUpUserResponse response;

  request.mutable_container_name()->assign(kContainerName);
  request.mutable_container_username()->assign(kDefaultContainerUser);
  auto status = tremplin_->SetUpUser(&context, request, &response);
  FXL_CHECK(status.ok()) << "Failed to setup user '" << kDefaultContainerUser
                         << "': " << status.error_message();

  switch (response.status()) {
    case vm_tools::tremplin::SetUpUserResponse::EXISTS:
    case vm_tools::tremplin::SetUpUserResponse::SUCCESS:
      FXL_LOG(INFO) << "User created.";
      if (kBootToContainer) {
        LaunchContainerShell();
      }
      break;
    case vm_tools::tremplin::SetUpUserResponse::FAILED:
      FXL_LOG(ERROR) << "Failed to create user: " << response.failure_reason();
      break;
    case vm_tools::tremplin::SetUpUserResponse::UNKNOWN:
    default:
      FXL_LOG(ERROR) << "Unknown status: " << response.status();
      break;
  }
}

// We've received a new vsock connection from a guest. We need to create a
// socket for this client and hand one end over to the |grpc::Server|.
void Guest::Accept(uint32_t src_cid, uint32_t src_port, uint32_t port,
                   AcceptCallback callback) {
  FXL_CHECK(grpc_server_);
  FXL_LOG(INFO) << "Inbound connection request from CID " << src_cid
                << " on port " << src_port;
  zx::socket h1, h2;
  zx_status_t status = zx::socket::create(ZX_SOCKET_STREAM, &h1, &h2);
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "Failed to create socket " << status;
    callback(ZX_ERR_CONNECTION_REFUSED, zx::handle());
    return;
  }
  int fd = convert_socket_to_fd(std::move(h1));
  if (fd < 0) {
    FXL_LOG(ERROR) << "Failed get file descriptor for socket";
    callback(ZX_ERR_INTERNAL, zx::socket());
    return;
  }
  grpc::AddInsecureChannelFromFd(grpc_server_.get(), fd);
  callback(status, std::move(h2));
}

// Creates a new GRPC stub for a service.
template <typename T>
std::unique_ptr<typename T::Stub> Guest::NewVsockStub(uint32_t cid,
                                                      uint32_t port) {
  // Create the socket for the connection.
  zx::socket h1, h2;
  zx_status_t status = zx::socket::create(ZX_SOCKET_STREAM, &h1, &h2);
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "Failed to create socket";
    return nullptr;
  }

  // Establish connection, hand first socket endpoint over to the guest.
  zx_status_t fidl_status =
      socket_endpoint_->Connect(cid, port, std::move(h1), &status);
  if (fidl_status != ZX_OK) {
    FXL_LOG(ERROR) << "Failed to perform vsock Connect RPC for "
                   << T::service_full_name() << ": " << status;
    return nullptr;
  }
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "Failed to connect to " << T::service_full_name() << ": "
                   << status;
    return nullptr;
  }

  // Hand the second socket endpoint to GRPC. We need to use a FDIO interface
  // to the socket for gRPC.
  int fd = convert_socket_to_fd(std::move(h2));
  if (fd < 0) {
    FXL_LOG(ERROR) << "Failed to get socket FD";
    return nullptr;
  }
  auto chan = grpc::CreateInsecureChannelFromFd("vsock", fd);
  return T::NewStub(std::move(chan));
}

grpc::Status Guest::VmReady(grpc::ServerContext* context,
                            const vm_tools::EmptyMessage* request,
                            vm_tools::EmptyMessage* response) {
  FXL_LOG(INFO) << "VM Ready -- Connecting to Maitre'd...";
  maitred_ = NewVsockStub<vm_tools::Maitred>(guest_cid_, kMaitredPort);
  FXL_CHECK(maitred_) << "Failed to connect to Maitre'd";

  // If we're not booting to a container; we'll drop the VM inside a root shell.
  const bool vm_only = cl_.HasOption("vm");
  if (!kBootToContainer || vm_only) {
    LaunchVmShell();
  }
  if (!vm_only) {
    ConfigureNetwork();
    StartTermina();
  }
  return grpc::Status::OK;
}

grpc::Status Guest::ContainerStartupFailed(
    grpc::ServerContext* context, const vm_tools::ContainerName* request,
    vm_tools::EmptyMessage* response) {
  FXL_LOG(ERROR) << "Container Startup Failed";
  return grpc::Status::OK;
}

grpc::Status Guest::TremplinReady(
    grpc::ServerContext* context,
    const ::vm_tools::tremplin::TremplinStartupInfo* request,
    vm_tools::tremplin::EmptyMessage* response) {
  FXL_LOG(INFO) << "Tremplin Ready.";
  tremplin_ =
      NewVsockStub<vm_tools::tremplin::Tremplin>(guest_cid_, kTremplinPort);
  FXL_CHECK(tremplin_) << "Failed to connect to tremplin";
  // The post is important here because the guest won't process requests until
  // this RPC has completed.
  async::PostTask(async_, [this]() { CreateContainer(); });
  return grpc::Status::OK;
}

grpc::Status Guest::UpdateCreateStatus(
    grpc::ServerContext* context,
    const vm_tools::tremplin::ContainerCreationProgress* request,
    vm_tools::tremplin::EmptyMessage* response) {
  switch (request->status()) {
    case vm_tools::tremplin::ContainerCreationProgress::CREATED:
      FXL_LOG(INFO) << "Container created: " << request->container_name();
      StartContainer();
      break;
    case vm_tools::tremplin::ContainerCreationProgress::DOWNLOADING:
      FXL_LOG(INFO) << "Downloading " << request->container_name() << ": "
                    << request->download_progress() << "%";
      break;
    case vm_tools::tremplin::ContainerCreationProgress::DOWNLOAD_TIMED_OUT:
      FXL_LOG(INFO) << "Download timed out for " << request->container_name();
      break;
    case vm_tools::tremplin::ContainerCreationProgress::CANCELLED:
      FXL_LOG(INFO) << "Download cancelled for " << request->container_name();
      break;
    case vm_tools::tremplin::ContainerCreationProgress::FAILED:
      FXL_LOG(INFO) << "Download failed for " << request->container_name()
                    << ": " << request->failure_reason();
      break;
    case vm_tools::tremplin::ContainerCreationProgress::UNKNOWN:
    default:
      FXL_LOG(INFO) << "Unknown download status: " << request->status();
      break;
  }
  return grpc::Status::OK;
}

grpc::Status Guest::ContainerReady(
    grpc::ServerContext* context,
    const vm_tools::container::ContainerStartupInfo* request,
    vm_tools::EmptyMessage* response) {
  // TODO(tjdetwiler): validate token.
  auto garcon_port = request->garcon_port();
  FXL_LOG(INFO) << "Container Ready; Garcon listening on port " << garcon_port;
  garcon_ = NewVsockStub<vm_tools::container::Garcon>(guest_cid_, garcon_port);

  DumpContainerDebugInfo();

  return grpc::Status::OK;
}

grpc::Status Guest::ContainerShutdown(
    grpc::ServerContext* context,
    const vm_tools::container::ContainerShutdownInfo* request,
    vm_tools::EmptyMessage* response) {
  FXL_LOG(INFO) << "Container Shutdown";
  return grpc::Status::OK;
}

grpc::Status Guest::UpdateApplicationList(
    grpc::ServerContext* context,
    const vm_tools::container::UpdateApplicationListRequest* request,
    vm_tools::EmptyMessage* response) {
  FXL_LOG(INFO) << "Update Application List";
  for (const auto& application : request->application()) {
    FXL_LOG(INFO) << "ID: " << application.desktop_file_id();
    const auto& name = application.name().values().begin();
    if (name != application.name().values().end()) {
      FXL_LOG(INFO) << "\tname:             " << name->value();
    }
    const auto& comment = application.comment().values().begin();
    if (comment != application.comment().values().end()) {
      FXL_LOG(INFO) << "\tcomment:          " << comment->value();
    }
    FXL_LOG(INFO) << "\tno_display:       " << application.no_display();
    FXL_LOG(INFO) << "\tstartup_wm_class: " << application.startup_wm_class();
    FXL_LOG(INFO) << "\tstartup_notify:   " << application.startup_notify();
    FXL_LOG(INFO) << "\tpackage_id:       " << application.package_id();
  }
  return grpc::Status::OK;
}

grpc::Status Guest::OpenUrl(
    grpc::ServerContext* context,
    const vm_tools::container::OpenUrlRequest* request,
    vm_tools::EmptyMessage* response) {
  FXL_LOG(INFO) << "Open URL";
  return grpc::Status::OK;
}

grpc::Status Guest::InstallLinuxPackageProgress(
    grpc::ServerContext* context,
    const vm_tools::container::InstallLinuxPackageProgressInfo* request,
    vm_tools::EmptyMessage* response) {
  FXL_LOG(INFO) << "Install Linux Package Progress";
  return grpc::Status::OK;
}

grpc::Status Guest::UninstallPackageProgress(
    grpc::ServerContext* context,
    const vm_tools::container::UninstallPackageProgressInfo* request,
    vm_tools::EmptyMessage* response) {
  FXL_LOG(INFO) << "Uninstall Package Progress";
  return grpc::Status::OK;
}

grpc::Status Guest::OpenTerminal(
    grpc::ServerContext* context,
    const vm_tools::container::OpenTerminalRequest* request,
    vm_tools::EmptyMessage* response) {
  FXL_LOG(INFO) << "Open Terminal";
  return grpc::Status::OK;
}

grpc::Status Guest::UpdateMimeTypes(
    grpc::ServerContext* context,
    const vm_tools::container::UpdateMimeTypesRequest* request,
    vm_tools::EmptyMessage* response) {
  FXL_LOG(INFO) << "Update Mime Types";
  size_t i = 0;
  for (const auto& pair : request->mime_type_mappings()) {
    FXL_LOG(INFO) << "\t" << pair.first << ": " << pair.second;
    if (++i > 10) {
      FXL_LOG(INFO) << "\t..." << (request->mime_type_mappings_size() - i) << " more.";
      break;
    }
  }
  return grpc::Status::OK;
}

void Guest::DumpContainerDebugInfo() {
  FXL_CHECK(garcon_)
      << "Called DumpContainerDebugInfo without a garcon connection";
  FXL_LOG(INFO) << "Dumping Container Debug Info...";

  grpc::ClientContext context;
  vm_tools::container::GetDebugInformationRequest request;
  vm_tools::container::GetDebugInformationResponse response;

  auto grpc_status = garcon_->GetDebugInformation(&context, request, &response);
  if (!grpc_status.ok()) {
      FXL_LOG(ERROR) << "Failed to read container debug information: " << grpc_status.error_message();
      return;
  }

  FXL_LOG(INFO) << "Container debug information:";
  FXL_LOG(INFO) << response.debug_information();
}

}  // namespace biscotti
