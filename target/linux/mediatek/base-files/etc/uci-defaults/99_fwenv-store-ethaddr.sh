. /lib/functions/system.sh

board="$(board_name)"

if [ ! -e /etc/fw_env.config ]; then
	[ "$board" = "bananapi,bpi-r4-pro-8x" ] && exit 1
	exit 0
fi

mac_is_valid() {
	[ -n "$1" ] &&
		[ "$1" != "00:00:00:00:00:00" ] &&
		[ "$1" != "ff:ff:ff:ff:ff:ff" ] &&
		[ $((0x${1%%:*} & 1)) -eq 0 ]
}

case "$board" in
bananapi,bpi-r2|\
bananapi,bpi-r64|\
unielec,u7623-02)
	[ -z "$(fw_printenv -n ethaddr 2>/dev/null)" ] &&
		fw_setenv ethaddr "$(cat /sys/class/net/eth0/address)"
	;;
bananapi,bpi-r3|\
bananapi,bpi-r3-mini|\
bananapi,bpi-r4|\
bananapi,bpi-r4-2g5|\
bananapi,bpi-r4-lite|\
bananapi,bpi-r4-poe)
	[ -z "$(fw_printenv -n ethaddr 2>/dev/null)" ] &&
		fw_setenv ethaddr "$(cat /sys/class/net/eth0/address)"
	[ -z "$(fw_printenv -n eth1addr 2>/dev/null)" ] &&
		fw_setenv eth1addr "$(macaddr_add $(cat /sys/class/net/eth0/address) 1)"
	;;
bananapi,bpi-r4-pro-8x)
	bootcmd="$(fw_printenv -n bootcmd 2>/dev/null)" || exit 1
	bootconf="$(fw_printenv -n bootconf 2>/dev/null)" || exit 1
	part_default="$(fw_printenv -n part_default 2>/dev/null)" || exit 1
	part_recovery="$(fw_printenv -n part_recovery 2>/dev/null)" || exit 1

	[ -n "$bootcmd" ] || exit 1
	[ "$bootconf" = "config-mt7988a-bananapi-bpi-r4-pro-8x" ] || exit 1
	[ "$part_default" = "production" ] || exit 1
	[ "$part_recovery" = "recovery" ] || exit 1

	ethaddr="$(macaddr_canonicalize "$(fw_printenv -n ethaddr 2>/dev/null)")"
	eth1addr="$(macaddr_canonicalize "$(fw_printenv -n eth1addr 2>/dev/null)")"
	eth2addr="$(macaddr_canonicalize "$(fw_printenv -n eth2addr 2>/dev/null)")"

	if mac_is_valid "$ethaddr" &&
		mac_is_valid "$eth1addr" &&
		mac_is_valid "$eth2addr" &&
		[ "$ethaddr" != "$eth1addr" ] &&
		[ "$ethaddr" != "$eth2addr" ] &&
		[ "$eth1addr" != "$eth2addr" ]; then
		exit 0
	fi

	mac_is_valid "$ethaddr" || ethaddr="$(macaddr_random)"
	eth1addr="$(macaddr_add "$ethaddr" 1)"
	eth2addr="$(macaddr_add "$ethaddr" 2)"

	mac_is_valid "$ethaddr" &&
		mac_is_valid "$eth1addr" &&
		mac_is_valid "$eth2addr" &&
		[ "$ethaddr" != "$eth1addr" ] &&
		[ "$ethaddr" != "$eth2addr" ] &&
		[ "$eth1addr" != "$eth2addr" ] || exit 1

	fw_setenv -s - <<-EOF || exit 1
		ethaddr $ethaddr
		eth1addr $eth1addr
		eth2addr $eth2addr
	EOF

	[ "$(fw_printenv -n ethaddr 2>/dev/null)" = "$ethaddr" ] &&
		[ "$(fw_printenv -n eth1addr 2>/dev/null)" = "$eth1addr" ] &&
		[ "$(fw_printenv -n eth2addr 2>/dev/null)" = "$eth2addr" ] || exit 1
	;;
esac

exit 0
