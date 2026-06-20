# Task 07a: Demo 1 Improvements

**Status**: Completed
**Output**: `documentation/task_outputs/07a_Demo1_Reliability_Improvements.md`

## Goal
Improve the reliability and robustness of the distributed Map-Reduce demo (C2 system).

## Issues Addressed
1.  **Startup Reliability**: Master controller hangs waiting for nodes.
2.  **Data Transfer**: Large payload transfers fail or time out.
3.  **Performance**: Worker execution is slow due to inefficient I/O.
4.  **Flash Space**: "No space left on device" errors when building.

## Implementation Details
-   **Simpler Startup**: Removed brittle log parsing; switched to fixed stabilization wait (10s).
-   **Throttled Network**: Implemented `send_chunked()` (1024B/128B chunks with 50ms delay) to prevent LwIP exhaustion.
-   **Buffered I/O**: Refactored worker `read()` to use 1KB buffer instead of byte-by-byte reads.
-   **Build Optimization**: Auto-clean `data/` folder and exclude worker ELF from flash image to save space.

## Verification
-   **Automated Test**: `python tools/c2_master.py --auto` passes consistently with 4 nodes.
-   **Manual Test**: Interactive mode runs reliably.

---

## Original Requirements
(Kept for reference)
... Here, have more realistic payloads that will actually transmit some information to each ESP32. Perhaps, rewrite C2 payload to perform a map reduce calculation. Use the C2 master py file to build payload apps, build ESP unikernel, spin up 4 concurrent instances of qemu, grabbing different ports. Use the master to do distributed computing on these nodes and relay info back to the master. Use some demo for this that is realistic and has a REAL application for distributed IoT computing. Think of how to make the CLI very interactive and nice to use for this demo, sending payload only to all 4 nodes on user input (and it should have a noninteractive commandline arg so you can test it autonomously) and should show the QEMU output of all 4 in a user friendly way (dividing window into 5 parts). On completing this, go again to main menu where you can again press a button to send a payload with some other set of data. Exiting will forcibly destroy all qemu instances and exit. This should be the new structure of c2_master.