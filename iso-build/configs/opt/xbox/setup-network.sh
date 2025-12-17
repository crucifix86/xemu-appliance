#!/bin/bash
# Create TAP interface
ip tuntap add tap0 mode tap user xbox
ip link set tap0 up
ip addr add 192.168.100.1/24 dev tap0

# Enable IP forwarding
sysctl -w net.ipv4.ip_forward=1

# NAT for internet access (adjust eth0 to actual interface)
iptables -t nat -A POSTROUTING -s 192.168.100.0/24 -j MASQUERADE
