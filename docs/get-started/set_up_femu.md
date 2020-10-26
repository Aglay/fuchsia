# Set up the Fuchsia emulator (FEMU)

This document describes how to set up the
Fuchsia emulator (FEMU). For more information, see the
[Fuchsia emulator (FEMU) overview](/docs/concepts/emulator/index.md).

## Prerequisites

To run FEMU, you must have

 * [Fuchsia source installed and environment variables created](/docs/get-started/get_fuchsia_source.md)
 * [Configured and built Fuchsia](/docs/get-started/build_fuchsia.md)

Note: When you configure Fuchsia for an emulator, use the `fx set`
command to set an emulator-specific board, either `qemu-x64` or `qemu-arm`.

## Configure network

For Fuchsia's ephemeral software to work with FEMU, you need to configure
an IPv6 network.

  * [Linux configuration](#linux-config)
  * [macOS configuration](#mac-config)

### Linux {#linux-config}

To enable networking in FEMU, run the following commands:

```sh
sudo ip tuntap add dev qemu mode tap user $USER
sudo ip link set qemu up
```

### macOS {#mac-config}

You need to install
[TunTap](http://tuntaposx.sourceforge.net/index.xhtml){:.external}, kernel
extensions that allow macOS to create virtual network interfaces.

For macOS 10.9 (Mavericks) and 10.10 (Yosemite), install TunTap using this
[installation package](http://tuntaposx.sourceforge.net/download.xhtml){:.external}.

For macOS 10.13 (High Sierra) and later versions, do the following:

1.  Install [Homebrew](https://brew.sh){:.external}:

    ```posix-terminal
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    ```

1.  Install TunTap:

    ```posix-terminal
    brew cask install tuntap
    ```

The installation of TunTap may fail at first. In that case, do the following:

1.  Open `System Preferences`.
1.  Open `Security & Privacy` and select the`General` tab.
1.  Next to the `System software from developer "Mattias Nissler" was blocked
    from loading.` message, click **Allow** (see Apple's
    [User-Approved Kernel Extension Loading](https://developer.apple.com/library/archive/technotes/tn2459/_index.html){:.external}
    for details).
1.  Run the install command again:

    ```posix-terminal
    brew cask install tuntap
    ```

After installing TunTap, run the following command:

```posix-terminal
sudo chown $USER /dev/tap0
```

## Start FEMU

The most common way to run FEMU is with networking enabled, using the following command:

```posix-terminal
fx emu -N
```

For more options on running FEMU, see
[Running the Fuchsia Emulator](/docs/development/run/femu.md).

Note: If you need to reach the internet from FEMU, configure IP
forwarding and IPv4 support on the emulator TAP interface.
