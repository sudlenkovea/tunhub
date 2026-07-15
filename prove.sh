#!/usr/bin/env bash
# Proof: whether a TunHub tunnel works, isolated from any other VPN.
# The probe binds DIRECTLY to TunHub's utun interface (--interface / ping -b),
# so the result doesn't depend on another VPN or the system default route.
#
# HOW TO RUN:
#   1. Turn on ONE TunHub tunnel (e.g. latvia). You can leave your other VPN as-is.
#   2. ./prove.sh
#   3. Send the output (it's short).

echo "===== TunHub PROVE $(date '+%H:%M:%S') ====="

# Find the utun interfaces managed by TunHub (via UAPI sockets).
mapfile -t SOCKS < <(ls /var/run/amneziawg/*.sock /var/run/wireguard/*.sock 2>/dev/null)
if [ ${#SOCKS[@]} -eq 0 ]; then
  echo "NO active TunHub tunnels (no UAPI sockets found). Turn on a tunnel and run again."
  exit 1
fi

for s in "${SOCKS[@]}"; do
  utun="$(basename "$s" .sock)"
  echo
  echo "######## Tunnel on $utun ########"

  # 1) Handshake / traffic from the core
  resp="$(printf 'get=1\n\n' | nc -U -w2 "$s" 2>/dev/null)"
  hs="$(echo "$resp" | awk -F= '/last_handshake_time_sec/{print $2}')"
  rx="$(echo "$resp" | awk -F= '/rx_bytes/{s+=$2} END{print s+0}')"
  tx="$(echo "$resp" | awk -F= '/tx_bytes/{s+=$2} END{print s+0}')"
  if [ -z "$hs" ] || [ "$hs" = "0" ]; then
    echo "  HANDSHAKE: NONE  → tunnel did not connect to the server"
  else
    now=$(date +%s); age=$((now - hs))
    echo "  HANDSHAKE: ${age}s ago"
  fi
  echo "  rx (received from server): ${rx} bytes | tx (sent): ${tx} bytes"
  if [ "${tx:-0}" -gt 0 ] && [ "${rx:-0}" -lt 1000 ]; then
    echo "  ⚠ sending, but receiving almost nothing ← data is NOT coming back"
  fi

  # 2) IP address of this utun (for binding probes)
  addr="$(ifconfig "$utun" 2>/dev/null | awk '/inet /{print $2; exit}')"
  echo "  interface address: ${addr:-none}"

  # 3) Ping 1.1.1.1 FORCED through this interface (-b), 3s
  echo -n "  ping 1.1.1.1 via $utun: "
  ping -b "$utun" -c3 -t3 1.1.1.1 >/tmp/p.$$ 2>&1
  loss="$(awk -F', ' '/packet loss/{print $3}' /tmp/p.$$)"
  echo "${loss:-error}"; rm -f /tmp/p.$$

  # 4) External IP VIA this tunnel (--interface). This is the decisive check.
  echo -n "  EXTERNAL IP via $utun: "
  ip="$(curl -s -m8 --interface "$utun" https://api.ipify.org 2>/dev/null)"
  echo "${ip:-NOT OBTAINED (tunnel isn't passing traffic)}"
done

echo
echo "===== VERDICT ====="
echo "If a tunnel has: handshake PRESENT, ping 0% loss, EXTERNAL IP obtained and it's the VPN server's IP — the tunnel works."
echo "If: no handshake / rx≈0 / ping 100% loss / no external IP — the tunnel does NOT work (send me this)."
