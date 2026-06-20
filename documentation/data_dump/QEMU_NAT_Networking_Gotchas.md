# QEMU User-Mode NAT Networking Gotchas

## Overview

QEMU's user-mode networking (`-nic user,model=open_eth`) provides NAT-based network access without requiring host privileges. However, it has several limitations and gotchas when used with ESP32 simulations.

---

## Key Gotchas

### 1. Idle Connection Timeouts & Buffer Exhaustion

**Problem:** 
1. Connections that appear idle may be closed by QEMU's NAT stack.
2. Sending too much data too fast overwhelms the ESP32 LwIP stack (Out of Memory/PBUF exhaustion).

**Solution:** Use a "Slow and Steady" chunked approach.

**Tested Optimal Values for ESP32 QEMU:**
*   **ELF Binary Transfer:** 1024-byte chunks with 50ms delay.
*   **Text Data Transfer:** 128-byte chunks with 50ms delay.
*   **Post-ELF Load Wait:** Wait at least 2.0s after sending ELF before sending data (allows relocation/startup).

```python
def send_chunked(sock, data, chunk_size=1024, delay=0.05):
    total_len = len(data)
    bytes_sent = 0
    while bytes_sent < total_len:
        chunk = data[bytes_sent : bytes_sent + chunk_size]
        sock.sendall(chunk)
        bytes_sent += len(chunk)
        time.sleep(delay)
```

### 2. Limited Concurrent Connections

**Problem:** QEMU NAT can only handle a limited number of concurrent connections.

**Symptoms:** Later connections fail when multiple clients connect simultaneously.

**Solution:** Stagger connection attempts:
```python
for i, node in enumerate(nodes):
    time.sleep(i * 0.4)  # Stagger by 400ms
    connect_to_node(node)
```

### 3. Port Forwarding Syntax

**Correct syntax:**
```
-nic user,model=open_eth,hostfwd=tcp::<HOST_PORT>-:<GUEST_PORT>
```

**Examples:**
```bash
# Forward host:9001 to guest:9000
-nic user,model=open_eth,hostfwd=tcp::9001-:9000

# Multiple forwards (comma-separated in one -nic)
-nic user,model=open_eth,hostfwd=tcp::9001-:9000,hostfwd=tcp::8080-:80
```

### 4. No Inbound Connections Without Port Forwarding

**Problem:** Without explicit port forwarding, the guest cannot accept incoming connections.

**Solution:** Always specify `hostfwd` for any services the guest needs to expose.

### 5. No Outbound to Arbitrary External IPs

**Problem:** QEMU NAT doesn't route to external IPs reliably. The guest can only connect to:
- localhost (via host's loopback)
- Other QEMU instances on the same host

**Solution:** For testing, use port forwarding to localhost instead of external connections.

### 6. DNS Resolution

**Note:** QEMU user-mode provides a built-in DNS server at 10.0.2.3 (default). The guest can resolve hostnames, but connections may fail due to routing limitations.

---

## Multi-Instance Considerations

When running multiple QEMU instances:

### Stagger Startup
```python
for node in nodes:
    start_qemu_instance(node)
    time.sleep(0.5)  # Allow each instance to initialize
```

### Unique Port Forwarding
Each instance needs unique host ports:
```bash
# Instance 1: host:9001 -> guest:9000
# Instance 2: host:9002 -> guest:9000
# Instance 3: host:9003 -> guest:9000
# Instance 4: host:9004 -> guest:9000
```

### Resource Contention
- 4 QEMU instances = significant CPU/memory usage
- Use longer timeouts (40+ seconds) for node readiness
- Stagger operations to reduce contention

---

## Recommended Timeout Values

| Operation | Recommended Timeout |
|-----------|-------------------|
| Node startup (4 instances) | 40+ seconds |
| Individual connection | 60 seconds |
| Response read | 30 seconds |
| Node readiness check | Stagger by 0.5s |
| Job execution | Stagger by 0.4s |

---

## Debugging Tips

1. **Check if port is listening:**
   ```python
   def wait_for_node_ready(port, timeout=30):
       start = time.time()
       while time.time() - start < timeout:
           try:
               s = socket.socket()
               s.settimeout(1)
               s.connect(('localhost', port))
               s.close()
               return True
           except:
               time.sleep(0.5)
       return False
   ```

2. **Enable TCP keepalive:**
   ```python
   s.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
   ```

3. **Log connection states:**
   ```python
   log_node(node_id, "[CONN] Connecting...")
   log_node(node_id, "[CONN] Connected!")
   log_node(node_id, "[SEND] Sending data...")
   ```

---

## Alternative: TAP Networking

For more reliable networking, consider TAP mode (requires admin privileges):
```bash
-nic tap,model=open_eth,ifname=tap0
```

TAP provides:
- Real network interface on host
- No NAT limitations
- Better performance
- Full bidirectional connectivity

However, TAP requires:
- Root/admin privileges
- Network bridge configuration
- More complex setup
