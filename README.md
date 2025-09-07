
# OceanWorkspace Daemon (owdaemon)

`owdaemon` is the privileged, native backend service for the Workspace virtualization platform on OceanOS. It is a minimal, security-focused C daemon responsible for the entire lifecycle of KVM-accelerated virtual machines, acting as the secure bridge between the unprivileged Android frontend and the powerful kernel hypervisor. Its purpose is to serve as a trusted, minimal proxy that performs high-privilege operations on behalf of a sandboxed, unprivileged client application, making it possible to leverage hardware virtualization without compromising the foundational security principles of the host operating system.

This repository contains the source code for the daemon itself. The Android frontend application, which provides the user interface and configuration logic, is maintained as a separate project.

## Overview

The goal of Workspace is to bring a secure, high-performance, VM-based desktop experience to OceanOS, effectively turning a mobile device into a portable workstation. `owdaemon` is the cornerstone of this architecture. It is designed from the ground up to operate within the strict confines of the OceanOS security model, providing the necessary hypervisor access that is explicitly and correctly forbidden to regular Android applications. By creating a dedicated, native `init` service, we can safely grant the required permissions to a minimal, audited codebase, ensuring that the vast attack surface of the Android app sandbox remains completely isolated from the host system's hypervisor.

## Technical Architecture

The core of Workspace is a client-server architecture meticulously designed to enforce the principle of least privilege and establish a clean, robust separation between the unprivileged user interface and the high-privilege virtualization backend. This design is not merely a choice but a security imperative. OceanOS's strict SELinux policies contain a non-negotiable `neverallow` rule that forbids any Zygote-spawned process (i.e., every Android app) from directly accessing sensitive drivers like `/dev/kvm`. By isolating all privileged operations into a minimal native daemon, we can grant precise, narrow permissions to that daemon alone without weakening the overall security posture of the system.

### 1. The `owdaemon` (The Privileged Server)

This is the heart of the system. It is a minimal native executable, written in C, that runs as a dedicated system service with a single responsibility: managing the QEMU/KVM lifecycle and associated hardware resources.

-   **✨ Lifecycle:** The daemon is defined as an `init` service in `init.owdaemon.rc`. It is launched directly by the `init` process (PID 1) during boot, placing it entirely outside the Android framework. This is the most crucial aspect of the architecture, as `init`-spawned processes can be placed in bespoke SELinux domains that are not subject to the broad restrictions placed on the Android app sandbox. The service is configured as `disabled` and `oneshot`, meaning it is started on-demand by the frontend app and exits after the VM shuts down.
    
-   **🛡️ SELinux Context:** It runs in a highly restrictive `owdaemon` SELinux domain, which is fundamentally distinct from the `untrusted_app` domain used for the frontend. This domain is granted an extremely narrow and explicit set of permissions defined in `owdaemon.te`:
    
    -   **KVM Access:** `allow owdaemon kvm_device:chr_file {...};` is the primary permission, allowing it to interact with the kernel's KVM hypervisor.
        
    -   **Socket Communication:** Specific rules grant the daemon the ability to create and manage its UNIX domain sockets within the dedicated `/data/ow` directory.
        
    -   **Device Access:** Explicit permissions are granted for direct hardware access, including `usb_device` for USB passthrough and `graphics_device` for the framebuffer mirroring feature.
        
    -   **Storage Access:** Permissions for `media_rw_data_file` enable QEMU's VirtFS (`-virtfs`) feature, allowing a running VM to securely mount directories from the user's shared storage.
        
-   **⚙️ Functionality:** The daemon's logic is simple by design. It creates and listens on its command socket. When the client app connects, the daemon reads a single command string, parses it, and then either launches the full QEMU engine in a new thread or sends a control command to a running QEMU instance via a separate QEMU Monitor Protocol (QMP) socket. This control plane allows for dynamic, stateful management of the VM (pausing, resuming, hotplugging devices).
    

### 2. The OceanWorkspace App (The Unprivileged Client)
Documentation for the OW frontend, display handler, and USB handler is not  currently a Helio Open Source Project candidate.
    

### 3. The IPC & Streaming Channels

Communication is handled by secure and efficient UNIX domain sockets, protected by SELinux.

-   **🔐 Command Channel (`/data/ow/owdaemon.socket`):** This is a low-bandwidth channel protected by the `allow untrusted_app owdaemon:unix_stream_socket connectto;` rule. This kernel-enforced rule ensures that only the official frontend app can send commands to the daemon. The protocol is a simple, text-based command set:
    
    -   `start [qemu_args...]`: Launches a new VM.
        
    -   `pause` / `resume` / `shutdown`: Controls the VM state via QMP.
        
    -   `attach_usb` / `detach_usb`: Manages USB device hotplugging.
        
    -   `start_fb_stream [key]` / `stop_fb_stream`: Controls the screen mirroring feature.
        
-   **🔒 Encrypted Framebuffer Stream (`/data/ow/owfb.socket`):** This is a high-bandwidth, one-way channel for the screen mirroring feature. It is not a traditional display protocol like VNC. Instead, `owdaemon` reads the raw pixel data from the host's framebuffer (`/dev/graphics/fb0`), encrypts it frame-by-frame using **AES-256-GCM**, and streams it to this socket. The Workspace app _inside the guest VM_ connects to this stream, decrypts it, and renders the host's screen within its own window. This provides a secure, near-native performance mirroring solution inside the guest OS.
    

This strict separation of privilege allows Workspace to provide powerful virtualization in a way that is fully compliant with the OceanOS security architecture.

## Building `owdaemon`

`owdaemon` is not a standalone project and must be compiled as part of a full OceanOS build. The source code is placed in `system/core/owdaemon/` within the AOSP source tree, and the AOSP Soong build system compiles it using the provided `Android.bp` file.

Warning: You need selinux patches in order for owdaemon to function if this is being built for any other platform other than OceanOS official. We've provided some examples in the technical breakdown above.

## Contributing

We welcome contributions to `owdaemon`. Please feel free to fork the repository and submit pull requests.

### Reporting Bugs

General bugs, feature requests, and build issues should be reported by opening a new issue on the [GitHub Issues](https://www.google.com/search?q=https://github.com/helio-mobile/owdaemon/issues "null") page.

### Reporting Security Vulnerabilities

If you believe you have found a security vulnerability, please **do not** open a public GitHub issue. Instead, please send a detailed report directly to our security team at **security@heliomobile.com**.

## Project Owners

`owdaemon` is proudly developed and maintained by Helio Mobile LLC.

| Member 	| Profile 	| Contact 	|
|---	|---	|---	|
| ![Ra's al Ghul](https://avatars.githubusercontent.com/u/2022965?v=4 "Ra's al Ghul") 	| https://github.com/imrasalghul 	| r.alghul@heliomobile.com 	|

## License

`owdaemon` is a derivative work of QEMU and is therefore licensed under the **GNU General Public License, version 2 (GPLv2)**. This is a legal requirement due to the static linking of the QEMU source code into the final daemon binary. The full license text is available in the `COPYING` file.

## Credits and Acknowledgements

-   **Limbo Tensor & Limbo PC Emulator:** For providing the foundational, Android-patched QEMU source and build logic.
    
-   **QEMU:** For the powerful and flexible open-source machine emulator and virtualizer.
    
-   **GrapheneOS:** For pioneering the hardened security model that inspired the secure architecture of this project.
    
-   **The Android Open Source Project (AOSP):** For the mobile operating system on which OceanOS is based.
