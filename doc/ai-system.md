# AlJefra OS -- AI System

## Overview

AlJefra OS is an AI-native operating system where artificial intelligence is embedded at the kernel level, not bolted on as an application layer. The AI subsystem provides natural language interaction, automatic hardware configuration, and intelligent system management. Users communicate with the OS in plain English or Arabic, and the AI translates intent into system actions.

The AI system consists of three major components: the AI Chat Engine for interactive use, the AI Bootstrap for zero-configuration hardware setup, and the LLM Backend Router for intelligent model selection.

## AI Chat Engine

**Source**: `kernel/ai_chat.c` (1,815 lines)

### Architecture

The chat engine follows a five-stage pipeline:

```
User Input
    |
    v
NLP Parser          -- Pattern matching against local grammar rules
    |
    v
Action Resolver     -- Maps parsed intent to a concrete action type
    |
    v
Executor            -- Calls the appropriate kernel subsystem
    |
    v
Response Builder    -- Formats output for the user (English or Arabic)
```

Each stage is modular. The current implementation uses a local pattern-matching NLP engine for all command processing. The architecture supports future integration with external LLM backends when online, but the v1.0 release operates entirely offline via pattern matching. When offline, the local pattern matcher handles the most common operations without any network dependency.

### Local NLP

The built-in NLP engine uses hand-crafted pattern rules for offline operation:

| Language | Pattern Count | Coverage |
|----------|--------------|----------|
| English  | 37 patterns  | Common file, network, driver, and system commands |
| Arabic   | 32 patterns  | Same coverage with Arabic grammar structures |

Patterns are matched using keyword extraction and phrase templates. No machine learning model is required for basic command recognition -- the pattern matcher runs entirely on the CPU with zero external dependencies.

Example patterns:
- `"show|list|display" + "files|documents|directory"` -> `FS_LIST`
- `"connect|join" + "wifi|wireless|network"` -> `WIFI_CONNECT`
- `"install" + "driver"` -> `DRIVER_INSTALL`

### Action Types

The engine supports 20 distinct action types:

| Action | Description | Subsystem |
|--------|-------------|-----------|
| `FS_LIST` | List directory contents | Filesystem |
| `FS_READ` | Read file contents | Filesystem |
| `FS_WRITE` | Write data to a file | Filesystem |
| `FS_CREATE` | Create a new file | Filesystem |
| `FS_DELETE` | Delete a file | Filesystem |
| `NET_STATUS` | Show network interface status | Network |
| `NET_CONNECT` | Establish network connection | Network |
| `WIFI_SCAN` | Scan for available WiFi networks | WiFi |
| `WIFI_CONNECT` | Connect to a WiFi network | WiFi |
| `DRIVER_LIST` | List installed drivers | Driver loader |
| `DRIVER_INSTALL` | Install a driver from marketplace | Driver store |
| `DRIVER_SEARCH` | Search marketplace for drivers | Driver store |
| `SYS_INFO` | Display system information | Kernel |
| `SYS_REBOOT` | Reboot the system | Kernel |
| `SYS_UPDATE` | Check for and apply OS updates | OTA |
| `DISPLAY_SET` | Change display settings | Display |
| `SETTINGS` | Open or modify system settings | Settings |
| `MEMORY_INFO` | Show memory usage statistics | Memory manager |
| `TASK_LIST` | List running tasks/processes | Scheduler |
| `HELP` | Show help and available commands | Chat engine |

### Conversation History

The chat engine maintains a 16-entry ring buffer of conversation history. This allows the AI to resolve contextual references across turns:

```c
#define AI_CHAT_HISTORY_SIZE 16

typedef struct {
    char user_message[512];
    char ai_response[1024];
    ai_action_type_t action;
    uint8_t confidence;
} ai_chat_entry_t;

static ai_chat_entry_t history[AI_CHAT_HISTORY_SIZE];
static int history_head;
```

When the buffer is full, the oldest entry is overwritten. The history is passed to the LLM backend (when available) as context, enabling multi-turn conversations such as:

```
User: "show my files"
AI:   [lists files: config.txt, notes.txt, data.bin]
User: "open the first one"
AI:   [opens config.txt, using history to resolve "the first one"]
```

### Safety

All actions that modify system state carry a `needs_confirm` flag:

```c
typedef struct {
    ai_action_type_t type;
    char arguments[256];
    uint8_t confidence;     // 0-100
    bool needs_confirm;     // true for destructive actions
} ai_action_t;
```

- **Destructive actions** (`FS_DELETE`, `SYS_REBOOT`, `SYS_UPDATE`, `FS_WRITE`) always require explicit user confirmation before execution.
- **Confidence scoring** ranges from 0 to 100. Actions with confidence below a configurable threshold (default: 60) prompt the user for clarification rather than executing.
- **Read-only actions** (`FS_LIST`, `NET_STATUS`, `SYS_INFO`, `MEMORY_INFO`, `TASK_LIST`, `HELP`) execute immediately without confirmation.

### Bilingual Support

The chat engine operates natively in both English and Arabic. Language detection is automatic based on character set analysis of the input. All system responses, error messages, and confirmations are generated in the detected language. The user can switch languages mid-conversation.

## AI Bootstrap

**Source**: `kernel/ai_bootstrap.c`

The AI bootstrap system provides zero-configuration hardware setup. When the OS boots, it automatically detects all hardware, contacts the AlJefra marketplace, and installs the appropriate drivers -- without any user intervention.

### Bootstrap Flow

```
1. Hardware Manifest Generation
   +-- Enumerate all PCIe devices (vendor:device IDs)
   +-- Detect CPU model and feature flags
   +-- Query total RAM size
   +-- Identify display adapter (if present)

2. Marketplace Communication
   +-- Establish HTTPS connection to api.aljefra.com
   +-- POST /v1/manifest with JSON hardware manifest
   +-- Receive driver recommendation list

3. Driver Installation
   +-- Download each recommended .ajdrv package
   +-- Verify Ed25519 signature
   +-- Load driver into kernel
   +-- Initialize driver for detected hardware

4. System Ready
   +-- All hardware operational
   +-- User notified of installed drivers
```

### Hardware Manifest Format

The manifest sent to the marketplace is a JSON document:

```json
{
    "arch": "x86_64",
    "cpu": "Intel Core i7-12700K",
    "ram_mb": 32768,
    "pcie_devices": [
        {"vendor": "0x8086", "device": "0x1533", "class": "0x0200"},
        {"vendor": "0x144d", "device": "0xa808", "class": "0x0108"}
    ],
    "display": {
        "detected": true,
        "framebuffer": "EFI GOP",
        "resolution": "1920x1080"
    }
}
```

### Zero-Configuration Design

The user does not need to know what hardware they have, what drivers are required, or how to install them. The entire process is automatic:

1. The OS detects hardware at the bus level (PCIe enumeration, ACPI tables).
2. The marketplace AI analyzes the manifest and returns the optimal driver set.
3. Drivers are downloaded, verified, and installed in the correct order.
4. If the display adapter is detected, the OS offers to download the GUI plugin (`.ajdrv`).

## LLM Backends

### Online Backends (Planned)

The architecture is designed to support external LLM backends when internet connectivity is available. These are not yet implemented in v0.7.2:

| Backend | Endpoint | Purpose | Status |
|---------|----------|---------|--------|
| AlJefra AI Cloud | `api.aljefra.com` | Primary backend, optimized for OS operations | Planned |
| Claude API | `api.anthropic.com` | Fallback backend for advanced reasoning | Planned |

When implemented, online backends would receive the full conversation history, hardware context, and system state, returning structured action descriptions that the action resolver can execute directly.

### Current Backend (v0.7.2)

The v0.7.2 release uses a fully local NLP pattern matcher:

- Handles command classification without internet access using 69 hand-crafted patterns (37 English, 32 Arabic)
- Supports the 20 core action types
- Pattern-matched responses (no free-form generation)
- Zero external dependencies — runs entirely on the CPU with static buffers

### AI Router

The AI router automatically selects the appropriate backend based on connectivity status:

```
ai_chat_process(input)
    |
    +-- Check network status
    |
    +-- Online?
    |     +-- Yes: Route to AlJefra Cloud (primary)
    |     |         +-- Timeout/error? Route to Claude API (fallback)
    |     +-- No:  Route to offline SLM
    |               +-- SLM insufficient? Fall back to local NLP patterns
    |
    +-- Parse LLM/NLP response into ai_action_t
    +-- Execute action
    +-- Build response
```

The routing is transparent to the user. The same natural language input works regardless of which backend processes it -- only the quality and depth of responses varies.

## API Reference

### Initialization

```c
/**
 * Initialize the AI chat engine.
 * Must be called once during kernel startup, after filesystem
 * and network subsystems are available.
 */
void ai_chat_init(void);
```

### LLM Backend Registration

```c
/**
 * Register an LLM backend callback.
 * The callback receives user input and conversation history,
 * and returns a structured action recommendation.
 *
 * @param callback  Function pointer to the LLM backend
 */
void ai_chat_set_llm_callback(ai_llm_callback_t callback);
```

### Message Processing

```c
/**
 * Process a user message through the AI pipeline.
 * Handles NLP parsing, action resolution, execution,
 * and response generation.
 *
 * @param input          User's natural language input (UTF-8)
 * @param response       Buffer to receive the AI's response
 * @param response_size  Size of the response buffer in bytes
 * @return               0 on success, negative error code on failure
 */
int ai_chat_process(const char *input, char *response, size_t response_size);
```

### Action Execution

```c
/**
 * Execute a resolved action directly.
 * Used by the chat pipeline internally, but also available
 * for programmatic action execution.
 *
 * @param action  Pointer to a fully resolved ai_action_t
 * @return        0 on success, negative error code on failure
 */
int ai_execute_action(const ai_action_t *action);
```

## Example Interactions

### File Listing

```
User: "show my files"
  -> NLP parser: matches "show" + "files" pattern
  -> Action resolver: FS_LIST, confidence 95
  -> Executor: calls fs_list()
  -> Response: "Here are your files:\n  config.txt (1.2 KB)\n  notes.txt (340 B)\n  data.bin (2.0 MB)"
```

### WiFi Connection

```
User: "connect to wifi"
  -> NLP parser: matches "connect" + "wifi" pattern
  -> Action resolver: WIFI_CONNECT, confidence 90
  -> Executor: calls wifi_scan(), displays available networks
  -> Follow-up: "Which network? Found: HomeNet, CoffeeShop, 5G-Guest"
  -> User: "HomeNet"
  -> Executor: calls wifi_connect("HomeNet"), prompts for password
```

### Driver Installation

```
User: "install the NVMe driver"
  -> NLP parser: matches "install" + "driver" pattern
  -> Action resolver: DRIVER_INSTALL, arguments="NVMe", confidence 88
  -> Executor: searches marketplace for NVMe driver
  -> Confirmation: "Found NVMe driver v2.1 (signed). Install? [Y/n]"
  -> User: "yes"
  -> Executor: downloads, verifies signature, loads driver
  -> Response: "NVMe driver installed. Detected 1TB Samsung 990 Pro."
```
