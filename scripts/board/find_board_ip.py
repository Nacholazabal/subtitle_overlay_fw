#!/usr/bin/env python3
"""Locate the board's current DHCP address by its Ethernet MAC address."""

from __future__ import annotations

import argparse
import concurrent.futures
import ipaddress
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path


DEFAULT_MAC = "00:0a:35:00:1e:53"
MAX_SCAN_HOSTS = 1022
SCAN_WORKERS = 64
CONNECT_TIMEOUT_SEC = 0.20


def normalize_mac(value: str) -> str:
    normalized = re.sub(r"[^0-9a-fA-F]", "", value).lower()
    if len(normalized) != 12:
        raise ValueError(f"invalid MAC address: {value}")
    return normalized


def run_output(command: list[str]) -> str:
    try:
        result = subprocess.run(
            command,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=15,
        )
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return result.stdout if result.returncode == 0 else ""


def windows_tool(name: str) -> str | None:
    located = shutil.which(name)
    if located:
        return located
    candidate = Path("/mnt/c/Windows/System32") / name
    return str(candidate) if candidate.exists() else None


def parse_windows_arp(text: str, target_mac: str) -> list[str]:
    matches: list[str] = []
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 2:
            continue
        try:
            address = str(ipaddress.IPv4Address(fields[0]))
        except ipaddress.AddressValueError:
            continue
        if normalize_mac_if_valid(fields[1]) == target_mac:
            matches.append(address)
    return matches


def normalize_mac_if_valid(value: str) -> str | None:
    try:
        return normalize_mac(value)
    except ValueError:
        return None


def parse_linux_neighbors(text: str, target_mac: str) -> list[str]:
    matches: list[str] = []
    for line in text.splitlines():
        fields = line.split()
        if "lladdr" not in fields:
            continue
        index = fields.index("lladdr")
        if index + 1 >= len(fields) or normalize_mac_if_valid(fields[index + 1]) != target_mac:
            continue
        try:
            matches.append(str(ipaddress.IPv4Address(fields[0])))
        except ipaddress.AddressValueError:
            continue
    return matches


def cached_addresses(target_mac: str) -> list[str]:
    matches: list[str] = []
    arp = windows_tool("ARP.EXE") or windows_tool("arp.exe")
    if arp:
        matches.extend(parse_windows_arp(run_output([arp, "-a"]), target_mac))
    ip_tool = shutil.which("ip")
    if ip_tool:
        matches.extend(parse_linux_neighbors(run_output([ip_tool, "neigh", "show"]), target_mac))
    return list(dict.fromkeys(matches))


def parse_windows_routes(text: str) -> list[ipaddress.IPv4Network]:
    default_interfaces: set[ipaddress.IPv4Address] = set()
    route_rows: list[tuple[str, str, str, str]] = []

    for line in text.splitlines():
        fields = line.split()
        if len(fields) != 5:
            continue
        destination, netmask, gateway, interface, _metric = fields
        try:
            interface_ip = ipaddress.IPv4Address(interface)
            ipaddress.IPv4Address(destination)
            ipaddress.IPv4Address(netmask)
        except ipaddress.AddressValueError:
            continue
        route_rows.append((destination, netmask, gateway, interface))
        if destination == "0.0.0.0" and netmask == "0.0.0.0":
            default_interfaces.add(interface_ip)

    networks: list[ipaddress.IPv4Network] = []
    for destination, netmask, _gateway, interface in route_rows:
        interface_ip = ipaddress.IPv4Address(interface)
        # Do not inspect Windows' localized "On-link" label. A route whose
        # interface owns an address inside the destination network is enough
        # to identify the directly connected LAN.
        if interface_ip not in default_interfaces:
            continue
        try:
            network = ipaddress.IPv4Network((destination, netmask))
        except (ipaddress.AddressValueError, ipaddress.NetmaskValueError):
            continue
        if interface_ip in network and 0 < network.prefixlen < 32 and network.is_private:
            networks.append(network)
    return list(dict.fromkeys(networks))


def parse_linux_routes(text: str) -> list[ipaddress.IPv4Network]:
    default_devices: set[str] = set()
    route_rows: list[tuple[str, list[str]]] = []

    for line in text.splitlines():
        fields = line.split()
        if not fields:
            continue
        if fields[0] == "default" and "dev" in fields:
            index = fields.index("dev")
            if index + 1 < len(fields):
                default_devices.add(fields[index + 1])
        elif "/" in fields[0]:
            route_rows.append((fields[0], fields[1:]))

    networks: list[ipaddress.IPv4Network] = []
    for destination, fields in route_rows:
        if "dev" not in fields:
            continue
        index = fields.index("dev")
        if index + 1 >= len(fields) or fields[index + 1] not in default_devices:
            continue
        try:
            network = ipaddress.IPv4Network(destination, strict=False)
        except (ipaddress.AddressValueError, ValueError):
            continue
        if network.prefixlen < 32 and network.is_private:
            networks.append(network)
    return list(dict.fromkeys(networks))


def candidate_networks() -> list[ipaddress.IPv4Network]:
    route = windows_tool("ROUTE.EXE") or windows_tool("route.exe")
    if route:
        networks = parse_windows_routes(run_output([route, "PRINT", "-4"]))
        if networks:
            return networks
    ip_tool = shutil.which("ip")
    if ip_tool:
        return parse_linux_routes(run_output([ip_tool, "-4", "route", "show"]))
    return []


def probe_ssh(address: str) -> None:
    try:
        with socket.create_connection((address, 22), timeout=CONNECT_TIMEOUT_SEC):
            pass
    except OSError:
        # A refused or timed-out connection still forces the host to resolve ARP.
        pass


def populate_neighbor_cache(networks: list[ipaddress.IPv4Network]) -> None:
    addresses: list[str] = []
    for network in networks:
        if network.num_addresses - 2 > MAX_SCAN_HOSTS:
            continue
        addresses.extend(str(address) for address in network.hosts())
    if not addresses:
        return
    with concurrent.futures.ThreadPoolExecutor(max_workers=SCAN_WORKERS) as executor:
        list(executor.map(probe_ssh, addresses))


def address_accepts_ssh(address: str) -> bool:
    try:
        with socket.create_connection((address, 22), timeout=0.75):
            return True
    except OSError:
        return False


def find_board_ip(mac: str) -> tuple[str | None, list[ipaddress.IPv4Network]]:
    target_mac = normalize_mac(mac)
    matches = cached_addresses(target_mac)
    networks: list[ipaddress.IPv4Network] = []

    if not matches:
        networks = candidate_networks()
        populate_neighbor_cache(networks)
        time.sleep(0.2)
        matches = cached_addresses(target_mac)

    if not matches:
        return None, networks
    for address in matches:
        if address_accepts_ssh(address):
            return address, networks
    return matches[0], networks


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mac", default=DEFAULT_MAC, help=f"board MAC (default: {DEFAULT_MAC})")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        address, networks = find_board_ip(args.mac)
    except ValueError as exc:
        print(f"board discovery failed: {exc}", file=sys.stderr)
        return 2
    if address:
        print(address)
        return 0

    scanned = ", ".join(str(network) for network in networks) or "no Windows LAN subnet detected"
    print(
        f"board discovery failed: MAC {args.mac} was not found (scanned: {scanned}).\n"
        "Check that the board is powered on and connected to the same router, "
        "or set BOARD_IP manually.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
