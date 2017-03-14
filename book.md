Fuchsia is Not Linux
====================
_A modular, capability-based operating system_

This "book" is a collection of topics describing the Fuchsia operating system.
Sections will be populated over time.

# Magenta Kernel

 - [Concepts][magenta-concepts]
 - [System Calls][magenta-syscalls] / VDSO (libmagenta)
 - Boot Sequence

# Magenta Core

 - Device Manager & Device Hosts
 - Device Driver Model (DDK)
 - C Library (libc) & POSIX IO (libmxio)
 - Process Start / ELF Loading (liblaunchpad)

# Framework

 - [Core Libraries](core_libraries.md)
 - Application model
   - Interface description language
   - Services
   - Environments
 - [Boot sequence](boot_sequence.md)
 - Device, user, and story runners
 - Components
 - [Story][framework-story]
 - [Module][framework-module]
 - [Agent][framework-agent]
 - Links

# Storage

 - Block devices
 - File systems
 - Directory hierarchy
 - Ledger
 - Document store
 - Application cache

# Networking

 - Ethernet
 - Wifi
 - Bluetooth
 - Sockets
 - HTTP

# Graphics

 - Vulkan driver
 - Physically-based renderer
 - Compositor
 - Input manager
 - View manager
 - Toolkit

# Media

 - Audio
 - Video
 - DRM

# Intelligence

 - Context
 - Agent Framework
 - Suggestions

# User interface

 - Device, user, and story shells
 - Stories and modules

# Backwards compatibility

 - POSIX lite
 - Web runtime

# Update and recovery

 - Verified boot
 - Updater

# Developer tools

 - Building
 - Testing
 - Debugging
 - Tracing
 - [Toolchain](toolchain.md)



[magenta-concepts]: https://fuchsia.googlesource.com/magenta/+/master/docs/concepts.md "Magenta concepts"
[magenta-syscalls]: https://fuchsia.googlesource.com/magenta/+/master/docs/syscalls.md "Magenta syscalls"
[framework-story]: https://fuchsia.googlesource.com/modular/+/master/docs/story.md "Framework story"
[framework-module]: https://fuchsia.googlesource.com/modular/+/master/docs/module.md "Framework module"
[framework-agent]: https://fuchsia.googlesource.com/modular/+/master/docs/agent.md "Framework agent"
