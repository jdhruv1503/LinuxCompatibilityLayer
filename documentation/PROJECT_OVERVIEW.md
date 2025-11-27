
# **Architecting a POSIX-Compliant Runtime Environment on ESP32: A Unikernel Approach to Dynamic ELF Execution**

## **1\. Introduction: The Convergence of RTOS and High-Level Application Models**

The embedded systems landscape has historically been bisected into two distinct paradigms: the deterministic, resource-constrained world of Real-Time Operating Systems (RTOS) and the dynamic, feature-rich environment of Embedded Linux. However, modern application requirements for Internet of Things (IoT) devices—specifically in domains requiring Command and Control (C2) flexibility, dynamic payload execution, and sophisticated sensor fusion algorithms like collision avoidance—are increasingly demanding a hybrid approach. The objective of this report is to detail the architectural design, implementation strategy, and theoretical underpinnings of a "Thin Linux Compatibility Layer" (TLCL) for the Espressif ESP32 System-on-Chip (SoC).

This proposed system functions effectively as a Unikernel or Library OS, abstracting the underlying primitives of the ESP-IDF (Espressif IoT Development Framework) and FreeRTOS into a POSIX-compliant system call interface. By doing so, it enables the execution of standard Linux ELF (Executable and Linkable Format) binaries on a microcontroller that lacks the Virtual Memory Management Unit (MMU) capabilities typically required for a full Linux kernel. This report synthesizes technical methodologies for syscall wrapping, dynamic binary loading without an MMU, virtual filesystem extension, and a seamless simulation workflow using QEMU, ensuring that the rigorous requirements of ephemeral execution and extensibility are met with precision.

### **1.1 The "No-MMU" Architectural Constraint**

The primary divergence between a standard Linux environment and the ESP32 execution model lies in memory management. Standard Linux applications rely on an MMU to provide each process with a virtual address space starting at 0x00, guaranteeing address isolation and enabling features like Copy-on-Write (CoW) for efficient fork() implementation. The ESP32, while possessing a memory controller that maps external flash to the CPU's address space, does not support the arbitrary remapping of RAM pages to create isolated virtual address spaces for dynamic processes in the manner of a Cortex-A or x86 processor.2

Consequently, the compatibility layer must adopt an operational model similar to uClinux. In this "Single Address Space" architecture, the kernel (FreeRTOS) and all loaded user applications share the same physical RAM. This implies that "processes" are effectively FreeRTOS tasks, and memory protection is virtually non-existent; a pointer error in a loaded application can destabilize the entire system. Furthermore, binaries cannot assume fixed load addresses. They must be compiled as Position Independent Executables (PIE) or relocatable object files, requiring the loader to perform runtime patching of memory references—a complex process on the Xtensa architecture used by the ESP32.4

### **1.2 The Unikernel Paradigm**

The proposed solution shifts the operating system structure from a monolithic kernel managing distinct user-space processes to a Unikernel model. In this model, the "application" is linked (dynamically at runtime) against a shim library that translates its requests directly into kernel functions. There is no context switch overhead associated with a software interrupt (like the int 0x80 instruction in x86 Linux) because the syscall interface is implemented as a function call table. This reduces latency, a critical factor for the real-time requirements of collision avoidance systems, but imposes strict requirements on the symbol resolution mechanism during binary loading.2

---

## **2\. Dynamic Execution: The ELF Loader Engine**

The cornerstone of this compatibility layer is the ELF Loader. To satisfy the requirement for executing "ephemeral binaries" that can be loaded, executed, and discarded (simulating the lifecycle of a forked process), the system must implement a mechanism to parse an ELF file from storage, allocate executable memory, and transfer control to it. This replaces the standard fork() and exec() mechanism of Linux, which is unsupported due to the lack of an MMU.4

### **2.1 ELF Binary Structure and Parsing**

The ELF format serves as the standard container for binary executables in Linux. It is a self-describing format containing headers that define the target architecture (Xtensa), entry point address, and memory layout. The loader's first task is to validate the ELF Header to ensure the binary is compatible with the ESP32's specific Xtensa core (LX6 for ESP32, LX7 for S3).

Following validation, the loader iterates through the **Program Header Table**. This table describes the "segments" of the binary that must be loaded into memory. Of particular interest are segments marked PT\_LOAD. These contain the actual code (.text) and data (.data, .rodata) of the application. Unlike a Linux kernel which maps these segments to virtual pages, the ESP32 loader must physically copy these bytes from the filesystem (LittleFS) into allocated heap memory.5

The **Section Header Table** provides a more granular view, identifying specific sections like the symbol table (.symtab) and relocation tables (.rel.text). These are crucial for the linking process. The loader must identify the size requirements for instruction memory and data memory separately, as the ESP32 employs a modified Harvard architecture where instruction fetch and data access occur on different buses and memory regions.5

### **2.2 Memory Allocation Strategy: IRAM vs. DRAM**

The ESP32's memory architecture imposes strict placement rules. Executable code must reside in Instruction RAM (IRAM) to be fetched by the CPU pipeline, while variables and stack must reside in Data RAM (DRAM). The loader cannot simply malloc a large block for the whole binary. It must perform a segmented allocation:

1. **Text Segment (.text):** This contains the machine instructions. The loader must use heap\_caps\_malloc(size, MALLOC\_CAP\_EXEC) to allocate memory from the IRAM pool. Failure to use the MALLOC\_CAP\_EXEC capability flag will result in the code being placed in DRAM, where the instruction fetch unit cannot access it, leading to an immediate crash upon execution.6  
2. **Data Segments (.data, .bss):** These contain initialized and uninitialized global variables. They are allocated using heap\_caps\_malloc(size, MALLOC\_CAP\_8BIT) to ensure they reside in DRAM. The .bss section requires special handling; it is not present in the file but occupies space in memory, so the loader must zero-initialize this region.10

### **2.3 Relocation and The Xtensa Architecture**

Because the ESP32 lacks an MMU to remap virtual addresses to physical ones, the application binary cannot know its runtime address at compile time. This necessitates the use of Position Independent Code (PIC) or load-time relocation. When the compiler generates the ELF, it creates a **Relocation Table** containing instructions on how to patch the code once the final load address is known.

For the Xtensa architecture, this is particularly complex due to its instruction format and windowed register file. The loader must handle specific relocation types, such as R\_XTENSA\_SLOT0\_OP, which modifies the immediate value of an instruction. The loader calculates the "delta" (the difference between the preferred link address—usually 0—and the actual allocated address in IRAM) and applies this delta to every reference listed in the relocation table. If this step is skipped or miscalculated, jump instructions will land in invalid memory, causing an InstrFetchProhibited panic.5

### **2.4 Cache Coherency and Flushing**

A critical, often overlooked aspect of dynamic loading on microcontrollers is cache coherency. The ESP32 uses a flash cache to speed up instruction fetching. When the loader writes new machine code into IRAM via the data bus, these writes may sit in a write buffer or be cached in the data cache, while the instruction cache effectively holds "stale" information (or nothing) for that address range.

If the processor attempts to execute the newly loaded code immediately, the instruction fetch unit might pull invalid data, leading to an IllegalInstruction exception. To prevent this, the loader must explicitly flush the data cache and invalidate the instruction cache for the modified memory range before transferring control to the loaded application. ESP-IDF provides ROM functions and cache management APIs (e.g., spi\_flash\_cache\_enabled) to handle this synchronization, ensuring that the instruction path sees the newly written bytes.9

### **2.5 Symbol Resolution and The Jump Table**

The loaded binary is "ephemeral" and "thin," meaning it does not contain the C standard library (libc) or FreeRTOS functions. It relies on the host firmware to provide these. This requires a **Dynamic Linking** step.

The host firmware must maintain a **Symbol Export Table**—a mapped array linking symbol names (strings like "printf", "socket", "vTaskDelay") to their actual memory addresses in the running firmware. When the ELF loader parses the guest binary's .symtab, it identifies undefined symbols. It then scans the host's export table to find the matching address. Once found, the loader patches the guest binary's call sites (using the relocation mechanism) to point directly to the function in the firmware.

This mechanism effectively recreates the behavior of ld.so in Linux, but statically defined within the firmware image. Tools like nm or custom Python scripts (e.g., symbols.py) can be used during the build process to automatically generate this symbol table from the firmware's .elf file, ensuring that every available API in the ESP-IDF is accessible to the guest application.14

---

## **3\. The System Call Shim Layer**

To run unmodified or minimally modified Linux code, the ESP32 must present a POSIX-compliant API. Since ESP-IDF is already largely POSIX-compliant (thanks to the Newlib C library), the task is not implementing these functions from scratch, but rather *wrapping* and *exposing* them correctly to the dynamic loader.

### **3.1 Filesystem (FS) Syscalls**

The Virtual Filesystem (VFS) in ESP-IDF allows mapping drivers to paths (e.g., /spiffs, /dev/uart). The shim layer wraps standard calls:

* **open(path, flags, mode):** The shim receives the call from the guest. It must handle path translation if necessary. For instance, mapping a Linux expectation of /var/log to a location in LittleFS. It calls esp\_vfs\_open and returns the file descriptor (FD).16  
* **read / write:** These map directly to esp\_vfs\_read and esp\_vfs\_write.  
* **close:** Maps to esp\_vfs\_close.  
* **lseek:** Maps to esp\_vfs\_lseek.  
* **stat / fstat:** Essential for ls style commands. The shim must populate the struct stat correctly, particularly the st\_mode field to indicate if a node is a file or directory, as some embedded filesystems have limited metadata support.16

### **3.2 Networking (Net) Syscalls via LwIP**

ESP-IDF uses LwIP (Lightweight IP) as its networking stack, which provides a BSD Sockets API. However, a direct mapping is insufficient due to **Error Code Divergence**.

* **errno Translation:** LwIP uses its own internal error codes. When a socket function fails, it sets an internal LwIP error. The Linux app expects standard POSIX errno values (e.g., EAGAIN, ECONNRESET). The shim layer must capture the LwIP error, translate it to the corresponding Newlib errno value, and set the global errno variable for the task. Without this, the Linux app might misinterpret a "Buffer Full" error as a fatal crash.19  
* **socket, bind, connect, accept:** These are passed through to the LwIP implementations.  
* **select / poll:** The shim must ensure that the fd\_set passed by the Linux app is compatible with the FD set size configured in LwIP. Crucially, ESP-IDF supports select() on both VFS file descriptors (UART) and LwIP socket descriptors, allowing the application to block on both network and serial events simultaneously—a critical feature for C2 payloads.16

### **3.3 Process (Proc) and Threading**

As established, "processes" are emulated as FreeRTOS tasks.

* **fork():** This is explicitly **not supported** due to the lack of MMU (no CoW). The shim should return \-1 and set errno to ENOSYS or ENOMEM.  
* **execve(path, argv, envp):** This is the trigger for the ELF Loader. When the app calls execve, the shim pauses the current execution, parses the binary at path, loads it, and effectively replaces the current task's execution context with the new entry point. In a "spawn" model, it might instead create a *new* task and wait for it.  
* **getpid():** Returns the TaskHandle\_t cast to an integer.  
* **kill(pid, sig):** Maps to vTaskDelete (for SIGKILL). Signal handling (SIGINT, SIGTERM) can be emulated using FreeRTOS Task Notifications. The shim registers a callback that checks task notifications at safe points and invokes the app's signal handler if one is pending.2

### **3.4 Memory (Mem) Syscalls**

* **malloc / free:** Map directly to pvPortMalloc / vPortFree or the standard C library wrappers provided by ESP-IDF.  
* **sbrk:** Used by older allocators. The shim can support this by reserving a large static buffer as the "heap" for the process, but generally, redirecting to the system heap is preferred for efficiency.  
* **mmap:** POSIX mmap is generally unsupported for writeable memory. However, for read-only access (e.g., reading a large constant database), the shim can wrap esp\_partition\_mmap. This function maps a partition from the SPI flash into the data address space using the MMU, allowing the application to read flash contents as if they were in RAM, significantly saving DRAM resources.13

---

## **4\. Extensible Driver Architecture: The /dev Model**

A key requirement is the ability to add drivers (e.g., for collision avoidance sensors) via a file structure, mimicking the UNIX "everything is a file" philosophy. This requires extending the ESP-IDF Virtual Filesystem.

### **4.1 Custom VFS Implementation**

The ESP-IDF allows registering custom VFS drivers using the esp\_vfs\_register function. A driver is defined by a esp\_vfs\_t structure containing function pointers for system calls.21

C

// Conceptual structure of a VFS driver  
esp\_vfs\_t my\_driver \= {  
   .flags \= ESP\_VFS\_FLAG\_DEFAULT,  
   .write \= \&driver\_write,  
   .read \= \&driver\_read,  
   .open \= \&driver\_open,  
   .close \= \&driver\_close,  
   .ioctl \= \&driver\_ioctl, // Critical for hardware control  
};

By registering this structure under the path base /dev/collision, any call to open("/dev/collision",...) in the Linux app will be routed by the VFS layer to driver\_open in the firmware.16

### **4.2 The ioctl Dispatcher**

For hardware that cannot be controlled by simple read/write operations (e.g., setting the frequency of a PWM pin, or configuring I2C address), ioctl is the standard interface. The shim implementation of ioctl acts as a dispatcher.

1. The Linux app defines request codes (e.g., COLLISION\_SET\_RANGE).  
2. The app calls ioctl(fd, COLLISION\_SET\_RANGE, \&range).  
3. The VFS routes this to driver\_ioctl.  
4. Inside driver\_ioctl, a switch statement handles the command, translating it into ESP-IDF driver calls like gpio\_set\_level, i2c\_master\_write, or ledc\_set\_duty.23

### **4.3 Case Study: Collision Avoidance Driver**

For the Collision Avoidance demo, we assume an ultrasonic distance sensor (trigger/echo) or a LiDAR module.

* **Hardware Interface:** GPIO trigger pin, GPIO echo pin (interrupt driven).  
* **Driver Logic:**  
  * open: Initialize GPIOs.  
  * read: Trigger a pulse, wait for the interrupt (blocking the task with a semaphore), calculate duration, compute distance, copy distance (in bytes) to the user buffer.  
  * write: Could be used to set parameters like max distance threshold.  
* **App Logic:** The Linux application logic is purely algorithmic. It opens /dev/collision, loops on read(), and if the distance is below a threshold, it executes avoidance logic. This cleanly separates the hardware driver (Firmware) from the business logic (ELF Binary).18

---

## **5\. Advanced I/O: Stdout Redirection and C2 Systems**

The Command and Control (C2) requirement dictates that the output of the running binary (standard output/error) must be redirectable to a network socket so a remote operator can view it. In Linux, this is trivially achieved via dup2(socket\_fd, STDOUT\_FILENO). On the ESP32, this presents significant architectural challenges.25

### **5.1 The dup2 Challenge in ESP-IDF**

In ESP-IDF, file descriptors are indices in a global table. FDs 0 (stdin), 1 (stdout), and 2 (stderr) are initialized at boot and mapped to the UART driver. The LwIP stack manages socket FDs separately in older versions, though newer ESP-IDF versions unify them under the VFS. However, simply overwriting the VFS entry at index 1 with a socket driver is risky and not officially supported via a public dup2 API in all versions.26

Furthermore, stdout is often buffered by the C library (Newlib). Simply changing the file descriptor might not flush the buffer, leading to lost data.

### **5.2 The "Shim Dup2" Solution**

To implement a robust dup2 for C2:

1. **Virtual Driver Shim:** We create a specialized VFS driver called the "C2 Pipe."  
2. **Registration:** This driver is registered at /dev/c2.  
3. **Shim dup2 Logic:**  
   * When dup2(sock\_fd, 1\) is called:  
   * The shim closes the existing FD 1 (UART).  
   * It maps FD 1 to the "C2 Pipe" driver.  
   * It associates the sock\_fd with this pipe instance.  
4. **Write Handler:** When printf (and thus write(1,...)) is called:  
   * The C library calls the write function of the C2 Pipe.  
   * The C2 Pipe's write function retrieves the associated sock\_fd.  
   * It calls send(sock\_fd, buffer, length, 0\) to push the data over the network.

This approach ensures that the application simply uses printf, while the underlying "plumbing" transparently routes text to the C2 server.26

### **5.3 C2 Payload Workflow**

The execution flow for a C2 binary payload is as follows:

1. **Connect:** The payload calls socket() and connect() to reach the C2 server.  
2. **Redirect:** It calls dup2(socket, STDOUT\_FILENO) and dup2(socket, STDERR\_FILENO).  
3. **Execute:** It enters a loop (e.g., a shell or status reporter), printing output.  
4. **Transport:** The C2 Pipe shim intercepts these prints and transmits them via TCP/UDP.  
5. **Control:** The payload calls read(STDIN\_FILENO), which the shim translates to recv(socket), allowing the attacker to send commands back to the ESP32.

---

## **6\. Simulation and Development Workflow: QEMU & LittleFS**

Developing ephemeral binaries on hardware requires constant flashing, which is slow. A robust workflow utilizes the QEMU ESP32 emulator to allow rapid iteration of both the firmware and the Linux payloads.

### **6.1 QEMU Configuration**

QEMU (Espressif's fork) simulates the ESP32 hardware including Flash and networking. QEMU requires a merged flash binary containing all partitions.

**Creating Merged Flash Binary:**
```bash
python -m esptool --chip esp32 merge_bin \
    -o build/merged-flash.bin \
    --flash_mode dio \
    --flash_size 4MB \
    0x1000 build/bootloader/bootloader.bin \
    0x8000 build/partition_table/partition-table.bin \
    0xd000 build/ota_data_initial.bin \
    0x10000 build/<app>.bin \
    0x190000 build/linux_fs.bin

# Pad to exactly 4MB (QEMU requirement)
dd if=/dev/zero bs=1 count=$((4194304 - $(stat -c%s build/merged-flash.bin))) >> build/merged-flash.bin
```

**Running QEMU:**
```bash
qemu-system-xtensa -nographic -machine esp32 \
    -drive file=build/merged-flash.bin,if=mtd,format=raw \
    -no-reboot
```

### **6.2 Filesystem Image Generation**

The Linux binaries (.elf files) must reside in the LittleFS partition. The workflow involves:

1. **Payload Compilation:** Compile the Linux app using xtensa-esp32-elf-gcc to produce payload.elf.  
2. **Directory Staging:** Place payload.elf into a data/ directory on the host machine.  
3. **Image Creation:** Use the mklittlefs tool (or littlefs-python via CMake) to pack the data/ directory into a littlefs.bin image.31  
   CMake  
   littlefs\_create\_partition\_image(storage data FLASH\_IN\_PROJECT)

4. **Partition Table:** Ensure partitions.csv defines a partition named storage of type data and subtype littlefs.33

### **6.3 The Seamless Cycle**

With this setup, the developer simply runs `idf.py build`. The build system compiles the firmware, compiles the payload (via a custom CMake target), and packs the filesystem. After building, create the merged flash binary using esptool merge_bin (see 6.1) and run in QEMU. The firmware boots, mounts LittleFS, and the user can immediately test exec("payload.elf") via the serial console.

---

## **7\. Performance Considerations: The Floating Point Unit (FPU)**

A critical detail for computational payloads (like Collision Avoidance using Kalman filters) is the handling of floating-point math. The ESP32 (Xtensa LX6) has a hardware FPU for single-precision (float) arithmetic, but double is emulated in software (slow).

### **7.1 Context Switching Latency**

FreeRTOS on ESP32 implements **Lazy Context Switching** for the FPU. When a task is swapped out, the FPU registers are *not* automatically saved to the stack unless the task has explicitly used the FPU. This optimization saves time but introduces a trap/exception overhead the first time a task attempts a float operation.

For our Linux compatibility layer, this implies:

* **Interrupt Handling:** Code within the "shim" layer (especially driver ISRs for sensors) must be extremely careful using float operations. If an ISR uses the FPU while a task was using it, it triggers a register save/restore that significantly increases interrupt latency, potentially missing sensor deadlines.36  
* **Task Priority:** Heavy computational payloads should be assigned lower priority than the critical "kernel" tasks (networking, driver handling) to prevent starvation, but high enough to meet their own deadlines. Using float (single precision) instead of double is mandatory for performance.38

---

## **8\. Implementation Plan**

### **Phase 1: Foundation**

1. **Project Setup:** Initialize ESP-IDF project with main, components, and data directories.  
2. **Partition Table:** Edit partitions.csv to include a littlefs partition (e.g., 1MB).  
3. **Filesystem:** Enable CONFIG\_LITTLEFS and mount it in app\_main.

### **Phase 2: The Shim Core**

1. **Symbol Table:** Implement the tools/gen\_symbols.py script to parse the firmware ELF and generate esp\_all\_symbol.c.  
2. **ELF Loader:** Port esp32-elfloader logic. Implement parsing of ELF headers and basic relocation (R\_XTENSA\_32, R\_XTENSA\_SLOT0\_OP).  
3. **Syscalls:** Create shim\_unistd.c. Implement open, read, write wrapping esp\_vfs calls.

### **Phase 3: Advanced Features**

1. **Network Wrapper:** Implement shim\_socket, shim\_bind, shim\_accept. Add errno translation helper.  
2. **Stdio Redirection:** Implement the "C2 Pipe" VFS driver and the shim\_dup2 logic.  
3. **Driver Extension:** Create the collision sensor driver frame using esp\_vfs\_t and register it at /dev/collision.

### **Phase 4: Validation**

1. **Payload Creation:** Write a "Hello World" C program. Compile with \-fno-common \-mlongcalls \-nostdlib.
2. **Simulation:** Build firmware, create merged flash binary, and run in QEMU. Verify "Hello World" output.
3. **C2 Test:** Write a socket-based payload. Run in QEMU with networking enabled. Verify output appears on a netcat listener on the host.

---

## **9\. Conclusion**

This architectural blueprint demonstrates that while the ESP32 lacks the hardware features (MMU) for a native Linux kernel, a functional UNIX-like environment can be synthesized via a Unikernel approach. By combining an ELF loader with a comprehensive system call shim and leveraging the ESP-IDF's extensible VFS, we can achieve the dynamic execution capabilities required for modern C2 and algorithmic payloads. The integration of QEMU into the development loop ensures that this complex software stack can be verified efficiently, bridging the gap between embedded constraints and high-level software flexibility.

### **Appendix A: Recommended File Structure**

project\_root/
├── CMakeLists.txt \# Top-level build script
├── sdkconfig.defaults \# Default config (LittleFS enabled, etc.)
├── partitions.csv \# Partition table (NVS, Factory, LittleFS)  
├── main/  
│ ├── main.c \# Firmware entry, VFS mount, WiFi init  
│ ├── elf\_loader.c \# ELF parsing & IRAM allocation logic  
│ ├── syscall\_shim.c \# Implementation of open, read, socket, etc.  
│ ├── symbol\_table.c \# Auto-generated exported symbols  
│ └── vfs\_drivers/  
│ ├── c2\_pipe.c \# Custom VFS for stdout redirection  
│ └── dev\_collision.c \# Driver for collision sensor  
├── components/  
│ └── lib\_linux\_compat/ \# The core library component  
├── apps/ \# Source code for Guest Linux Apps  
│ ├── c2\_payload/  
│ │ ├── main.c  
│ │ └── CMakeLists.txt \# Builds to.elf with \-nostdlib  
│ └── collision\_algo/  
└── data/ \# Staging area for LittleFS  
└── bin/ \# Compiled ELFs are placed here by build system

### **Appendix B: Critical Data Tables**

**Table 1: ELF Section Mapping Strategy**

| ELF Section | Content Type | ESP32 Memory Region | Allocation Function | Access Attributes |
| :---- | :---- | :---- | :---- | :---- |
| .text | Machine Instructions | IRAM (Instruction RAM) | heap\_caps\_malloc(..., MALLOC\_CAP\_EXEC) | Read-Only, Executable (32-bit access only) |
| .rodata | Constants / Strings | DRAM (Data RAM) | heap\_caps\_malloc(..., MALLOC\_CAP\_8BIT) | Read-Only |
| .data | Initialized Globals | DRAM (Data RAM) | heap\_caps\_malloc(..., MALLOC\_CAP\_8BIT) | Read-Write |
| .bss | Uninitialized Globals | DRAM (Data RAM) | heap\_caps\_malloc(..., MALLOC\_CAP\_8BIT) | Read-Write (Zero Initialized) |

**Table 2: Syscall Implementation Status Matrix**

| Subsystem | Syscall | ESP-IDF Primitive | Shim Complexity | Notes |
| :---- | :---- | :---- | :---- | :---- |
| **FS** | open | esp\_vfs\_open | Low | Path translation required. |
| **FS** | ioctl | esp\_vfs\_ioctl | High | Requires dispatcher for custom hardware. |
| **Net** | socket | lwip\_socket | Medium | Must translate return errors to errno. |
| **Net** | dup2 | **None** | Very High | Requires custom VFS pipe implementation. |
| **Proc** | fork | **None** | N/A | Not supported. Use spawn model. |
| **Proc** | execve | elf\_loader\_run | High | Handles loading, linking, and task creation. |
| **Mem** | sbrk | pvPortMalloc | Low | Map to system heap. |

#### **Works cited**

1. gautam-dev-maker/FreeRTOS-esp-idf \- GitHub, accessed November 27, 2025, [https://github.com/gautam-dev-maker/FreeRTOS-esp-idf](https://github.com/gautam-dev-maker/FreeRTOS-esp-idf)  
2. Running another kernel on top of FreeRTOS vs adding a compatibility layer for tasks into FreeRTOS's task scheduler and IPC routines, accessed November 27, 2025, [https://forums.freertos.org/t/running-another-kernel-on-top-of-freertos-vs-adding-a-compatibility-layer-for-tasks-into-freertoss-task-scheduler-and-ipc-routines/15214](https://forums.freertos.org/t/running-another-kernel-on-top-of-freertos-vs-adding-a-compatibility-layer-for-tasks-into-freertoss-task-scheduler-and-ipc-routines/15214)  
3. Guide to Building Embedded Linux Systems with Containers \- Pantacor, accessed November 27, 2025, [https://pantacor.com/embedded-linux/guide-to-building-embedded-linux-systems-for-iot-with-containers/](https://pantacor.com/embedded-linux/guide-to-building-embedded-linux-systems-for-iot-with-containers/)  
4. ESP32 library to load in ram and relocate elf file, accessed November 27, 2025, [https://esp32.com/viewtopic.php?t=10352](https://esp32.com/viewtopic.php?t=10352)  
5. Elf Loader \- Embedded, accessed November 27, 2025, [https://ourembeddeds.github.io/blog/2020/08/16/elf-loader/](https://ourembeddeds.github.io/blog/2020/08/16/elf-loader/)  
6. Building and running a module beyond the original image file \- ESP32 Forum, accessed November 27, 2025, [https://esp32.com/viewtopic.php?t=4769](https://esp32.com/viewtopic.php?t=4769)  
7. Ultra-fast Wasm3 interpreter brings WebAssembly to ESP32, ESP8266, Raspberry Pi, Arduino and other embedded platforms \- Reddit, accessed November 27, 2025, [https://www.reddit.com/r/esp32/comments/duwthm/ultrafast\_wasm3\_interpreter\_brings\_webassembly\_to/](https://www.reddit.com/r/esp32/comments/duwthm/ultrafast_wasm3_interpreter_brings_webassembly_to/)  
8. elf-loader · GitHub Topics, accessed November 27, 2025, [https://github.com/topics/elf-loader?l=c](https://github.com/topics/elf-loader?l=c)  
9. Basic Commands \- ESP32 \- — esptool latest documentation \- Espressif Systems, accessed November 27, 2025, [https://docs.espressif.com/projects/esptool/en/latest/esp32/esptool/basic-commands.html](https://docs.espressif.com/projects/esptool/en/latest/esp32/esptool/basic-commands.html)  
10. Experiement to load/execute compiled C code into RAM \- ESP32 Forum, accessed November 27, 2025, [https://esp32.com/viewtopic.php?t=10039](https://esp32.com/viewtopic.php?t=10039)  
11. Linker/ESPTOOL issue--can't generate bin from elf \- ESP32 Forum, accessed November 27, 2025, [https://esp32.com/viewtopic.php?t=21442](https://esp32.com/viewtopic.php?t=21442)  
12. Implementing A Custom ESP32 Runtime Linker, accessed November 27, 2025, [https://esp32.com/viewtopic.php?t=4293](https://esp32.com/viewtopic.php?t=4293)  
13. Maximizing Execution Speed \- ESP32 \- — ESP-IDF Programming Guide v5.1 documentation \- Espressif Systems, accessed November 27, 2025, [https://docs.espressif.com/projects/esp-idf/en/v5.1/esp32/api-guides/performance/speed.html](https://docs.espressif.com/projects/esp-idf/en/v5.1/esp32/api-guides/performance/speed.html)  
14. espressif/elf\_loader \- 1.1.0 \- Example elf\_loader\_example \- ESP Component Registry, accessed November 27, 2025, [https://components.espressif.com/components/espressif/elf\_loader/versions/1.1.0/examples/elf\_loader\_example?language=en](https://components.espressif.com/components/espressif/elf_loader/versions/1.1.0/examples/elf_loader_example?language=en)  
15. niicoooo/esp32-elfloader: esp32 component to load in ram and relocate elf file \- GitHub, accessed November 27, 2025, [https://github.com/niicoooo/esp32-elfloader](https://github.com/niicoooo/esp32-elfloader)  
16. Virtual Filesystem Component \- ESP32 \- — ESP-IDF Programming Guide v5.5.1 documentation \- Espressif Systems, accessed November 27, 2025, [https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/vfs.html](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/vfs.html)  
17. Virtual filesystem component \- ESP-IDF Programming Guide \- Read the Docs, accessed November 27, 2025, [https://demo-dijiudu.readthedocs.io/en/latest/api-reference/storage/vfs.html](https://demo-dijiudu.readthedocs.io/en/latest/api-reference/storage/vfs.html)  
18. ESP 32 VFS Integration | Random thoughts on everything, accessed November 27, 2025, [http://www.gnilk.com/esp32\_vfs\_integration/](http://www.gnilk.com/esp32_vfs_integration/)  
19. lwIP \- ESP32-S2 \- — ESP-IDF Programming Guide v4.4.2 documentation, accessed November 27, 2025, [https://docs.espressif.com/projects/esp-idf/en/v4.4.2/esp32s2/api-guides/lwip.html](https://docs.espressif.com/projects/esp-idf/en/v4.4.2/esp32s2/api-guides/lwip.html)  
20. lwIP \- ESP32 \- — ESP-IDF Programming Guide v5.5.1 documentation \- Espressif Systems, accessed November 27, 2025, [https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/lwip.html](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/lwip.html)  
21. Virtual filesystem component \- ESP32 \- — ESP-IDF Programming Guide v4.3.1 documentation \- Espressif Systems, accessed November 27, 2025, [https://docs.espressif.com/projects/esp-idf/en/v4.3.1/esp32/api-reference/storage/vfs.html](https://docs.espressif.com/projects/esp-idf/en/v4.3.1/esp32/api-reference/storage/vfs.html)  
22. Virtual filesystem component \- ESP32 \- — ESP-IDF Programming Guide v5.1 documentation, accessed November 27, 2025, [https://docs.espressif.com/projects/esp-idf/en/v5.1/esp32/api-reference/storage/vfs.html](https://docs.espressif.com/projects/esp-idf/en/v5.1/esp32/api-reference/storage/vfs.html)  
23. Network ioctls (IDFGH-4381) · Issue \#6215 · espressif/esp-idf \- GitHub, accessed November 27, 2025, [https://github.com/espressif/esp-idf/issues/6215](https://github.com/espressif/esp-idf/issues/6215)  
24. ESP NETIF Custom I/O Driver with vfs\_l2tap \- ESP32 Forum, accessed November 27, 2025, [https://esp32.com/viewtopic.php?t=41078](https://esp32.com/viewtopic.php?t=41078)  
25. Standard I/O and Console Output \- ESP32 \- — ESP-IDF Programming Guide v5.5.1 documentation \- Espressif Systems, accessed November 27, 2025, [https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/stdio.html](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/stdio.html)  
26. Redircting STDOUT \- ESP32 Forum, accessed November 27, 2025, [https://esp32.com/viewtopic.php?t=14279](https://esp32.com/viewtopic.php?t=14279)  
27. Redirecting stdout to socket \- Stack Overflow, accessed November 27, 2025, [https://stackoverflow.com/questions/15102680/redirecting-stdout-to-socket](https://stackoverflow.com/questions/15102680/redirecting-stdout-to-socket)  
28. esp\_console\_set\_vprintf feature request (IDFGH-10885) · Issue \#12087 · espressif/esp-idf, accessed November 27, 2025, [https://github.com/espressif/esp-idf/issues/12087](https://github.com/espressif/esp-idf/issues/12087)  
29. Configuring Your Project (wokwi.toml), accessed November 27, 2025, [https://docs.wokwi.com/vscode/project-config](https://docs.wokwi.com/vscode/project-config)  
30. Specific partitions.csv in vscode with wokwi.toml · Issue \#523 \- GitHub, accessed November 27, 2025, [https://github.com/wokwi/wokwi-features/issues/523](https://github.com/wokwi/wokwi-features/issues/523)  
31. How to define LittleFS partition, build image and flash it on ESP32? \- PlatformIO Community, accessed November 27, 2025, [https://community.platformio.org/t/how-to-define-littlefs-partition-build-image-and-flash-it-on-esp32/11333](https://community.platformio.org/t/how-to-define-littlefs-partition-build-image-and-flash-it-on-esp32/11333)  
32. joltwallet/littlefs • v1.20.3 \- ESP Component Registry \- Espressif Systems, accessed November 27, 2025, [https://components.espressif.com/components/joltwallet/littlefs/versions/1.20.3/readme](https://components.espressif.com/components/joltwallet/littlefs/versions/1.20.3/readme)  
33. Partition Tables \- ESP32 \- — ESP-IDF Programming Guide v5.5.1 documentation \- Espressif Systems, accessed November 27, 2025, [https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/partition-tables.html](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/partition-tables.html)  
34. Partition Tables \- ESP32-S3 \- — ESP-IDF Programming Guide v5.2 documentation, accessed November 27, 2025, [https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32s3/api-guides/partition-tables.html](https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32s3/api-guides/partition-tables.html)  
35. Nano \+ Json \+ LittleFS \- Wokwi \- Online ESP32, STM32, Arduino Simulator, accessed November 27, 2025, [https://wokwi.com/projects/387204964984172545](https://wokwi.com/projects/387204964984172545)  
36. FreeRTOS (IDF) \- ESP32 \- — ESP-IDF Programming Guide v5.5.1 documentation \- Espressif Systems, accessed November 27, 2025, [https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/freertos\_idf.html](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/freertos_idf.html)  
37. Unexpectedly low floating-point performance in C \- ESP32 Forum, accessed November 27, 2025, [https://esp32.com/viewtopic.php?t=800](https://esp32.com/viewtopic.php?t=800)  
38. ESP32 \- floating point performance \- Reddit, accessed November 27, 2025, [https://www.reddit.com/r/esp32/comments/1la8fob/esp32\_floating\_point\_performance/](https://www.reddit.com/r/esp32/comments/1la8fob/esp32_floating_point_performance/)  
39. Floating-Point Units on Espressif SoCs: Why (and when) they matter · Developer Portal, accessed November 27, 2025, [https://developer.espressif.com/blog/2025/10/cores\_with\_fpu/](https://developer.espressif.com/blog/2025/10/cores_with_fpu/)