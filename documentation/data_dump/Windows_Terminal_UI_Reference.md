# Windows Terminal UI Reference for Python CLI Applications

## Overview

This document covers creating cross-platform terminal UIs in Python that work correctly on Windows cmd.exe, PowerShell, and modern terminals.

## Unicode Encoding Issues on Windows

### The Problem

Windows cmd.exe uses code page 1252 (cp1252) by default, which cannot display Unicode characters:

```python
# This FAILS on Windows cmd.exe
print("✓ Success")  # UnicodeEncodeError: 'charmap' codec can't encode character '\u2713'
print("→ Arrow")    # UnicodeEncodeError
```

### The Solution: ASCII Fallbacks

Use ASCII alternatives for cross-platform compatibility:

```python
# Unicode (Unix-only)
CHECK = "✓"
CROSS = "✗"
ARROW = "→"
BOX_TOP = "╔═══════════╗"
BOX_SIDE = "║"

# ASCII (Cross-platform)
CHECK = "[OK]"
CROSS = "[X]"
ARROW = "->"
BOX_TOP = "+===========+"
BOX_SIDE = "|"
```

### Platform Detection

```python
import sys

def get_symbols():
    # Check if we can use Unicode
    try:
        sys.stdout.write("✓")
        sys.stdout.flush()
        # Clear the character we wrote
        sys.stdout.write("\b \b")
        return {"check": "✓", "cross": "✗", "arrow": "→"}
    except (UnicodeEncodeError, UnicodeDecodeError):
        return {"check": "[OK]", "cross": "[X]", "arrow": "->"}
```

## ANSI Escape Codes

### Color Codes (Work on Modern Windows Terminals)

```python
RESET = "\033[0m"
BOLD = "\033[1m"
DIM = "\033[2m"
RED = "\033[31m"
GREEN = "\033[32m"
YELLOW = "\033[33m"
BLUE = "\033[34m"
MAGENTA = "\033[35m"
CYAN = "\033[36m"
WHITE = "\033[37m"
```

### Enable ANSI on Windows

Windows 10+ supports ANSI codes, but may need to be enabled:

```python
import os
import sys

def enable_ansi():
    if sys.platform == 'win32':
        # Enable VT100 escape sequences on Windows
        import ctypes
        kernel32 = ctypes.windll.kernel32
        kernel32.SetConsoleMode(
            kernel32.GetStdHandle(-11),  # STD_OUTPUT_HANDLE
            7  # ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING
        )

# Call at startup
enable_ansi()
```

## Box Drawing with ASCII

### Simple Box
```python
def draw_box(title, content_lines, width):
    lines = []
    lines.append(f"+{'-' * (width - 2)}+")
    lines.append(f"| {title}{' ' * (width - len(title) - 4)} |")
    lines.append(f"+{'-' * (width - 2)}+")

    for line in content_lines:
        truncated = line[:width - 4] if len(line) > width - 4 else line
        padding = ' ' * (width - len(truncated) - 4)
        lines.append(f"| {truncated}{padding} |")

    lines.append(f"+{'-' * (width - 2)}+")
    return lines
```

### Usage Example
```python
box = draw_box("Node 1", ["Status: Running", "Port: 9001"], 30)
for line in box:
    print(line)

# Output:
# +----------------------------+
# | Node 1                     |
# +----------------------------+
# | Status: Running            |
# | Port: 9001                 |
# +----------------------------+
```

## Screen Clearing

### Clear Screen
```python
import os

def clear_screen():
    os.system('cls' if os.name == 'nt' else 'clear')
```

### Get Terminal Size
```python
def get_terminal_size():
    try:
        size = os.get_terminal_size()
        return size.columns, size.lines
    except:
        return 80, 24  # Default fallback
```

## Keyboard Input (Windows)

### Non-blocking Keyboard Check

```python
try:
    import msvcrt
    HAS_MSVCRT = True
except ImportError:
    HAS_MSVCRT = False

def check_keyboard():
    if HAS_MSVCRT and msvcrt.kbhit():
        key = msvcrt.getch().decode('utf-8', errors='ignore').lower()
        return key
    return None

# Usage
while True:
    key = check_keyboard()
    if key == 'q':
        break
    elif key == 'r':
        run_job()
    time.sleep(0.1)
```

### Cross-Platform Alternative

```python
import sys
import select

def check_keyboard_unix():
    """Unix/Linux version using select()"""
    if select.select([sys.stdin], [], [], 0)[0]:
        return sys.stdin.read(1)
    return None

def check_keyboard():
    if sys.platform == 'win32':
        if msvcrt.kbhit():
            return msvcrt.getch().decode('utf-8', errors='ignore')
    else:
        return check_keyboard_unix()
    return None
```

## Live UI Updates

### Continuous Redraw Pattern

```python
import time
import threading

class LiveUI:
    def __init__(self):
        self.running = True
        self.data = {}
        self.lock = threading.Lock()

    def update_data(self, key, value):
        with self.lock:
            self.data[key] = value

    def draw(self):
        clear_screen()
        with self.lock:
            for key, value in self.data.items():
                print(f"{key}: {value}")

    def run(self):
        while self.running:
            self.draw()
            time.sleep(0.3)  # 3 updates per second

# Usage
ui = LiveUI()
ui_thread = threading.Thread(target=ui.run)
ui_thread.start()

# Update from other threads
ui.update_data("Node 1", "Running")
ui.update_data("Node 2", "Computing...")

# Stop
ui.running = False
ui_thread.join()
```

### Print All at Once (Reduces Flicker)

```python
def draw_ui(lines):
    # Build entire output first
    output = "\n".join(lines)

    # Clear and print in one operation
    clear_screen()
    print(output)
```

## Complete Example: Split-Screen Node Monitor

```python
import os
import time
import threading

CYAN = "\033[36m"
YELLOW = "\033[33m"
GREEN = "\033[32m"
RESET = "\033[0m"

def draw_box(title, lines, width, color=CYAN):
    result = []
    result.append(f"{color}+{'-' * (width - 2)}+{RESET}")
    result.append(f"{color}|{RESET} {title}{' ' * (width - len(title) - 4)}{color}|{RESET}")
    result.append(f"{color}+{'-' * (width - 2)}+{RESET}")
    for line in lines[:5]:  # Max 5 lines
        pad = ' ' * (width - len(line) - 4)
        result.append(f"{color}|{RESET} {line}{pad}{color}|{RESET}")
    result.append(f"{color}+{'-' * (width - 2)}+{RESET}")
    return result

def draw_split_screen(node_logs):
    width = os.get_terminal_size().columns
    half = (width - 3) // 2

    output = []

    # Top row: nodes 1 and 2
    box1 = draw_box("Node 1", node_logs.get(1, []), half, YELLOW)
    box2 = draw_box("Node 2", node_logs.get(2, []), half, YELLOW)
    for i in range(len(box1)):
        output.append(f"{box1[i]} {box2[i]}")

    # Bottom row: nodes 3 and 4
    box3 = draw_box("Node 3", node_logs.get(3, []), half, YELLOW)
    box4 = draw_box("Node 4", node_logs.get(4, []), half, YELLOW)
    for i in range(len(box3)):
        output.append(f"{box3[i]} {box4[i]}")

    os.system('cls' if os.name == 'nt' else 'clear')
    print("\n".join(output))

# Example usage
node_logs = {
    1: ["[CONN] Connected", "[SEND] Sending data..."],
    2: ["[WAIT] Waiting for input"],
    3: ["[DONE] Complete!"],
    4: ["[ERR] Connection failed"]
}
draw_split_screen(node_logs)
```

## Best Practices

1. **Always use ASCII for symbols** unless you're certain the terminal supports Unicode
2. **Enable ANSI escape codes** at startup on Windows
3. **Use threading** for live updates to avoid blocking
4. **Clear and redraw entire screen** rather than partial updates (reduces flicker)
5. **Add a small sleep** in update loops (0.1-0.3s) to reduce CPU usage
6. **Use locks** when sharing data between threads
7. **Test on cmd.exe** - it's the most restrictive environment
