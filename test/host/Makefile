# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The debugprobe-wifi authors
DP   ?= ../..
CC   ?= cc
CFLAGS = -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -fsanitize=address,undefined \
         -DDAP_TCP_HOST -DPROBE_NET_WIFI=1 -D__CC_ARM \
         -Istubs -I$(DP)/src/net -I$(DP)/include -I$(DP)/src -I$(DP)/CMSIS_DAP/CMSIS/DAP/Firmware/Include -I.
DAPSRC = $(DP)/CMSIS_DAP/CMSIS/DAP/Firmware/Source/DAP.c $(DP)/CMSIS_DAP/CMSIS/DAP/Firmware/Source/DAP_vendor.c $(DP)/src/sw_dp_pio.c
NETSRC = $(DP)/src/net/dap_tcp_proto.c $(DP)/src/net/dap_tcp_server.c $(DP)/src/net/netcfg.c $(DP)/src/net/netcfg_console.c $(DP)/src/net/plog.c $(DP)/src/net/diag.c

all: daptcp-sim unit console_pty

daptcp-sim: host_main.c swd_sim.c $(DAPSRC) $(NETSRC)
	$(CC) $(CFLAGS) -o $@ $^

unit: unit.c $(NETSRC)
	$(CC) $(CFLAGS) -o $@ $^

console_pty: console_pty.c $(DP)/src/net/netcfg.c $(DP)/src/net/netcfg_console.c
	$(CC) -O1 -g -Wall -I$(DP)/src/net -o $@ $^

clean:
	rm -f daptcp-sim unit console_pty
