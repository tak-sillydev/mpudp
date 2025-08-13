#!/bin/bash
ip tuntap add tun_test mode tun
ip address add 10.255.0.1/24 dev tun_test
ip link set tun_test up
