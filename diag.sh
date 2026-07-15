#!/usr/bin/env bash
# TunHub — standalone diagnostics. Run WHILE A TUNNEL IS UP and the internet is broken.
# Everything is local, no internet needed; network probes use a short timeout.
# Output: ~/Desktop/tunhub-diag.txt  — send this file.
OUT="$HOME/Desktop/tunhub-diag.txt"
exec > "$OUT" 2>&1

echo "================ TunHub diag $(date) ================"
echo
echo "########## 1. Processes ##########"
ps aux | grep -E "tunhubd|amneziawg-go|wireguard-go" | grep -v grep

echo; echo "########## 2. utun interfaces ##########"
ifconfig | awk '/^utun[0-9]+:/{i=$1} /inet /{print i, $0}'

echo; echo "########## 3. Routing table (inet) ##########"
netstat -rn -f inet | head -60

echo; echo "########## 4. route get 1.1.1.1 / 8.8.8.8 ##########"
for ip in 1.1.1.1 8.8.8.8; do echo "--- $ip ---"; route -n get "$ip" 2>&1 | grep -E "interface|gateway|destination"; done

echo; echo "########## 5. DNS (scutil) ##########"
scutil --dns | sed -n '1,30p'

echo; echo "########## 6. UAPI handshake of each tunnel ##########"
for s in /var/run/amneziawg/*.sock /var/run/wireguard/*.sock; do
  [ -S "$s" ] || continue
  echo "--- $s ---"
  printf 'get=1\n\n' | nc -U -w2 "$s" 2>/dev/null | \
    grep -E "public_key|endpoint|last_handshake_time_sec|rx_bytes|tx_bytes|errno"
done

echo; echo "########## 7. Pings BY IP (no DNS), 3s timeout ##########"
for ip in 1.1.1.1 8.8.8.8 9.9.9.9; do
  echo "--- ping $ip ---"; ping -c3 -t3 "$ip" 2>&1 | tail -3
done

echo; echo "########## 8. Ping tunnel endpoints ##########"
CFG="$HOME/Library/Application Support/TunHub/tunnels"
for f in "$CFG"/*.json; do
  ep=$(python3 - "$f" <<'PY' 2>/dev/null
import json,sys
c=json.load(open(sys.argv[1]))
for p in c.get("peers",[]):
    e=p.get("endpoint")
    if e: print(c["name"], e)
PY
)
  echo "$ep"
done | while read name hostport; do
  [ -z "$hostport" ] && continue
  host="${hostport%:*}"
  echo "--- $name endpoint $host ---"
  ping -c2 -t3 "$host" 2>&1 | tail -2
done

echo; echo "########## 9. DNS resolution (3s timeout) ##########"
nslookup -timeout=3 google.com 2>&1 | head -8

echo; echo "########## 10. curl example.com (5s) ##########"
curl -s -m5 -o /dev/null -w "by name: http=%{http_code} time=%{time_total}\n" https://example.com 2>&1 || echo "curl by name FAIL"
curl -s -m5 -o /dev/null -w "by IP(cloudflare): http=%{http_code}\n" --resolve example.com:443:104.16.0.0 https://example.com 2>&1 || echo "curl by IP FAIL"

echo; echo "########## 11. Configs: routeMode + AllowedIPs count + presence of 0.0.0.0/0 ##########"
for f in "$CFG"/*.json; do
python3 - "$f" <<'PY' 2>/dev/null
import json,sys
c=json.load(open(sys.argv[1]))
print("=== %s (%s) ===" % (c["name"], c.get("kind")))
print("  routeMode:", c["options"].get("routeMode"))
print("  dnsMode:", c["options"].get("dnsMode"), "dns:", c["interface"].get("dns"))
print("  addresses:", [str(a) for a in c["interface"].get("addresses",[])])
print("  mtu:", c["interface"].get("mtu"))
awg=c.get("awg")
print("  awg:", {k:v for k,v in (awg or {}).items() if v is not None})
for p in c.get("peers",[]):
    aips=[str(a) for a in p.get("allowedIPs",[])]
    print("  peer allowedIPs count:", len(aips), "| default(0.0.0.0/0):", any(a=="0.0.0.0/0" for a in aips))
    print("    first 6:", aips[:6])
PY
done

echo; echo "########## 12. Daemon log tail (last 250 lines) ##########"
tail -250 /var/log/tunhub-daemon.log 2>&1

echo; echo "================ end of diag ================"
echo "File saved: $OUT"
