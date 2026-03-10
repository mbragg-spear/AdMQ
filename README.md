# **AdMQ (Administrative Message Queue)**

AdMQ is a lightweight, zero-trust, highly concurrent Pub/Sub message broker and edge-agent framework written entirely in C. It is designed for secure, centralized remote administration of distributed Linux devices, IoT nodes, and servers.  
Instead of relying on passwords or vulnerable SSH ports, AdMQ uses **Mutual TLS (mTLS)** to strictly verify the identity of every connecting node via cryptographic certificates before they are allowed onto the message bus.

## **Key Features**

* **Zero-Trust mTLS Architecture:** Both the server and the agents cryptographically verify each other using OpenSSL before a single byte of command data is transmitted.  
* **Dual-Port Enrollment:** Features a secure "Vault" port for active mTLS traffic, and a strict, one-and-done plaintext "Lobby" port to automatically sign Certificate Signing Requests (CSRs) for new agents.  
* **Persistent Audit Logging:** Every command broadcasted and executed is logged permanently into a thread-safe SQLite database (broker\_audit.db).  
* **Ghost Connection Sweeping:** Agents send automated background heartbeats. The broker actively sweeps and ruthlessly severs dropped connections to prevent socket exhaustion.  
* **Live Admin CLI:** A dedicated background thread provides a live interactive prompt to query network status and publish commands without dropping background traffic.  
* **Daemon-Ready:** Automatically detects when it is being run by systemd and safely disables the interactive CLI to run invisibly in the background.  
* **Role-Based Access:** Secure role-based authentication protocols prevent unauthorized access to read/write channels.  
* **Dynamic INI Configuration:** Fully configurable via broker.ini, agent.ini, and rbac.ini files.

## **Prerequisites**

To build AdMQ, you need the standard C build tools, OpenSSL, and SQLite3 development headers installed on your system.  
**Ubuntu / Debian:** 
```
sudo apt-get update  
sudo apt-get install gcc make libssl-dev libsqlite3-dev
```
**RHEL / CentOS:** 
```
sudo yum install gcc make openssl-devel sqlite-devel
```
## **Building the Project**

AdMQ includes a Makefile for streamlined building.  
To compile both the Broker and the Agent:  
make

**Individual Build Targets:**

* `make message_broker` \- Compiles only the server daemon.  
* `make agent` \- Compiles only the edge agent.  
* `make clean` \- Wipes all compiled binaries and object (.o) files.

## **Configuration**

AdMQ relies on standard .ini files for runtime configuration.

### **Broker Configuration (broker.ini)**

Place this in the same directory as the message\_broker executable.
```
[network]  
vault_port = 35565  
lobby_port = 35566

[security]  
cert_path = certs/server.crt  
key_path = certs/server.key  
ca_path = certs/ca.crt

[database]  
db_path = broker_audit.db
```
### **Agent Configuration (agent.ini)**

Place this in the same directory as the agent executable.  
```
[network]  
broker_ip = 127.0.0.1  
broker_port = 35565

[security]  
cert_path = certs/client.crt  
key_path = certs/client.key  
ca_path = certs/ca.crt

[agent]  
command_group = CMD-GRP-1  
action_dir = ./actions
```

### **Role-Based Access Configuration (rbac.ini)**

Place this in the same directory as the message_broker executable.  
```
; AdMQ Role-Based Access Control Configuration

; The DEFAULT role is applied if a device is not explicitly mapped below.
[role:DEFAULT]
SUBSCRIBE = BROADCAST
UNSUBSCRIBE = *
PUBLISH =
SET =

[role:DESKTOP_AGENT]
SUBSCRIBE = BROADCAST, CMD-GRP-1
UNSUBSCRIBE = BROADCAST, CMD-GRP-1
PUBLISH = agent-status
SET = current_user, cpu_alert

[role:ADMIN]
SUBSCRIBE = *
UNSUBSCRIBE =
PUBLISH = *
SET = *

[map]
admin-pc-* = ADMIN
desktop-* = DESKTOP_AGENT
localhost = ADMIN
* = DEFAULT
```
## **Usage Guide**

### **1\. Bootstrapping a New Agent (The Lobby)**

To add a new device to the AdMQ network, generate a local private key and CSR on the edge device, and pipe it to the Broker's Lobby port (default 35566). The broker will validate the request and return a signed certificate.  
```
#!/bin/bash

openssl genrsa -out certs/client.key 2048
CSR=$(openssl req -new -key certs/client.key -subj "/CN=desktop123")

OUTPUT=$((echo "ENROLL desktop123"; echo "$CSR") | nc admqserver 35566)

read -r RESULT <<< $OUTPUT
echo $RESULT
/bin/echo -e "$OUTPUT" | tail -n+2 > certs/client.crt
```
### **2\. Starting the Services**

Start the broker first. If running interactively, you will be dropped into the Admin CLI.  
`./message_broker`

Start the agent on your edge device. It will connect, perform the mTLS handshake, and listen for commands.  
`./agent`

### **3\. The Admin CLI**

From the broker's interactive prompt, you can manage the fleet:

* **View connected agents and active channels:**  
  `admq> STATUS`

* **Send a targeted command to a specific group:**  
  `admq> PUBLISH CMD-GRP-1 UPDATE tonight`

* **Send a global broadcast to all connected agents:**  
  `admq> PUBLISH BROADCAST REBOOT now`

* **Gracefully shut down the server:**  
  `admq> EXIT`

### **4\. Agent Actions**

When an agent receives a command (e.g., `UPDATE tonight`), it looks inside its action\_dir for a matching INI file (e.g., actions/UPDATE.ini). It searches for the `[tonight]` block and safely executes the underlying shell command, reporting the success or failure back to the broker's audit log.  

```
; /etc/mgmt/actions/UPDATE.ini
; Comments start with a semicolon or hash

[now]
cmd = /bin/bash
target = /path/to/update_script.sh
arguments = ""

[tonight]
cmd = /bin/bash
target = /path/to/update_script.sh
arguments = "-t 2100"

[tomorrow_night]
cmd = /bin/bash
target = /path/to/update_script.sh
arguments = "-t 'tomorrow at 2100'"
```
