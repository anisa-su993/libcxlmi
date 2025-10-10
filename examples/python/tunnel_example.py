#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""
Example: Demonstrate tunneling commands through CXL topology

This example shows how to:
1. Create tunnel info structures
2. Send commands through CXL Switches
3. Send commands to Logical Devices in MLDs
4. Use multi-level tunneling
"""

import sys
import cxlmi


def example_tunnel_to_mld(ep, ld_id):
    """Example: Tunnel identify command to an LD in an MLD"""
    print(f"\nTunneling to LD {ld_id} in MLD:")

    # Create MLD tunnel info
    ti = cxlmi.cxlmi_tunnel_mld(ld_id)

    try:
        ident = cxlmi.cxlmi_cmd_identify_rsp()
        ret = cxlmi.cxlmi_cmd_identify(ep, ti, ident)

        print(f"  Vendor ID:    0x{ident.vendor_id:04x}")
        print(f"  Device ID:    0x{ident.device_id:04x}")
        print(f"  Serial:       0x{ident.serial_num:016x}")

    except RuntimeError as e:
        print(f"  Error: {e}")
    finally:
        cxlmi.cxlmi_tunnel_free(ti)


def example_tunnel_to_switch_port(ep, port):
    """Example: Tunnel identify command through a switch port"""
    print(f"\nTunneling through Switch port {port}:")

    # Create Switch tunnel info
    ti = cxlmi.cxlmi_tunnel_switch(port)

    try:
        ident = cxlmi.cxlmi_cmd_identify_rsp()
        ret = cxlmi.cxlmi_cmd_identify(ep, ti, ident)

        print(f"  Vendor ID:    0x{ident.vendor_id:04x}")
        print(f"  Device ID:    0x{ident.device_id:04x}")

    except RuntimeError as e:
        print(f"  Error: {e}")
    finally:
        cxlmi.cxlmi_tunnel_free(ti)


def example_tunnel_switch_to_mld(ep, port, ld_id):
    """Example: Multi-level tunnel through switch to MLD"""
    print(f"\nTunneling through Switch port {port} to LD {ld_id}:")

    # Create Switch+MLD tunnel info (2-level tunneling)
    ti = cxlmi.cxlmi_tunnel_switch_mld(port, ld_id)

    try:
        ident = cxlmi.cxlmi_cmd_identify_rsp()
        ret = cxlmi.cxlmi_cmd_identify(ep, ti, ident)

        print(f"  Vendor ID:    0x{ident.vendor_id:04x}")
        print(f"  Device ID:    0x{ident.device_id:04x}")

    except RuntimeError as e:
        print(f"  Error: {e}")
    finally:
        cxlmi.cxlmi_tunnel_free(ti)


def example_tunnel_to_mhd(ep):
    """Example: Tunnel to LD Pool CCI in Multi-Headed Device"""
    print(f"\nTunneling to MHD LD Pool:")

    # Create MHD tunnel info
    ti = cxlmi.cxlmi_tunnel_mhd()

    try:
        ident = cxlmi.cxlmi_cmd_identify_rsp()
        ret = cxlmi.cxlmi_cmd_identify(ep, ti, ident)

        print(f"  Vendor ID:    0x{ident.vendor_id:04x}")
        print(f"  Device ID:    0x{ident.device_id:04x}")

    except RuntimeError as e:
        print(f"  Error: {e}")
    finally:
        cxlmi.cxlmi_tunnel_free(ti)


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <cxl_device>")
        print()
        print("This example demonstrates different tunneling scenarios.")
        print("Note: Most commands will fail unless you have the appropriate")
        print("      CXL topology (MLD, Switch, etc.)")
        print()
        print(f"Example: {sys.argv[0]} mem0")
        return 1

    device_name = sys.argv[1]

    ctx = cxlmi.cxlmi_new_ctx(None, 6)
    if not ctx:
        print("Failed to create context")
        return 1

    try:
        ep = cxlmi.cxlmi_open(ctx, device_name)
        if not ep:
            print(f"Failed to open device: {device_name}")
            return 1

        try:
            print("CXL Command Tunneling Examples")
            print("=" * 50)

            # Example 1: Tunnel to LD in MLD
            example_tunnel_to_mld(ep, ld_id=0)
            example_tunnel_to_mld(ep, ld_id=1)

            # Example 2: Tunnel through Switch
            example_tunnel_to_switch_port(ep, port=0)
            example_tunnel_to_switch_port(ep, port=1)

            # Example 3: Multi-level tunneling
            example_tunnel_switch_to_mld(ep, port=0, ld_id=0)

            # Example 4: MHD tunneling
            example_tunnel_to_mhd(ep)

            print("\nNote: Errors are expected if the device doesn't support")
            print("      the tunneling configuration being tested.")

        finally:
            cxlmi.cxlmi_close(ep)

    finally:
        cxlmi.cxlmi_free_ctx(ctx)

    return 0


if __name__ == '__main__':
    sys.exit(main())
