# jack_conosc

Small CLI tool in C for Debian 11 that controls JACK connections over OSC.

## Requirements

- JACK (JACK1 or JACK2)
- `libjack` development package
- `liblo` development package
- `make`, `gcc`

Example (Debian 11):

```bash
sudo apt install build-essential libjack-jackd2-dev liblo-dev
```

## Build

```bash
make
```

## Run

```bash
./jack_conosc
```

Custom OSC port:

```bash
./jack_conosc --port 50420
```

The tool listens on UDP/OSC port `50420` by default.
Responses are always sent to the sender IP on the currently configured UDP port.

Show help:

```bash
./jack_conosc --help
```

## OSC Protocol

- Request: `/get_clients_all`
  - Response per client: `/client <jackclientname> <num_inputs> <num_outputs>`
  - Final marker: `/done get_clients_all`

- Request: `/get_connections_all`
  - Response per connection: `/connection <fromname> <fromchannum> <toname> <tochannum>`
  - Final marker: `/done get_connections_all`

- Request: `/connection <clientname>`
  - Response per outgoing connection of that client:
    `/connection <fromname> <fromchannum> <toname> <tochannum>`
  - Final marker: `/done connection`

- Request: `/connection <fromname> <fromchannum> <toname> <tochannum>`
  - Creates a connection from output channel to input channel
  - Success: `/ok connection <fromname> <fromchannum> <toname> <tochannum>`
  - Error: `/error <message>`

- Request: `/disconnect <fromname> <fromchannum> <toname> <tochannum>`
  - Removes an existing connection from output channel to input channel
  - Success: `/ok disconnect <fromname> <fromchannum> <toname> <tochannum>`
  - Error: `/error <message>`

## OSC Usage Examples

Using `oscsend` (from liblo-tools):

```bash
# Query all JACK clients and channel counts
oscsend localhost 50420 /get_clients_all

# Query all active JACK connections
oscsend localhost 50420 /get_connections_all

# Query all outgoing connections for one client
oscsend localhost 50420 /connection s system

# Connect system:out_1 -> myclient:in_1 (by channel index)
oscsend localhost 50420 /connection sisi system 1 myclient 1

# Disconnect system:out_1 -> myclient:in_1
oscsend localhost 50420 /disconnect sisi system 1 myclient 1
```
