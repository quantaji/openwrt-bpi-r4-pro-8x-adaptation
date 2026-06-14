. /lib/functions/system.sh

fwenv_bpi8x_log()
{
	logger -t bpi-r4-pro-8x-mac "$*"
}

fwenv_macaddr_valid_unicast()
{
	local mac

	mac=$(macaddr_canonicalize "$1")
	[ -n "$mac" ] || return 1
	[ "$mac" != "00:00:00:00:00:00" ] || return 1
	[ "$mac" != "ff:ff:ff:ff:ff:ff" ] || return 1
	[ $((0x${mac%%:*} & 1)) -eq 0 ] || return 1

	echo "$mac"
}

fwenv_get_valid_env_mac()
{
	local mac

	mac=$(fw_printenv -n "$1" 2>/dev/null) || return 1
	fwenv_macaddr_valid_unicast "$mac"
}

fwenv_get_env()
{
	fw_printenv -n "$1" 2>/dev/null
}

fwenv_env_contains()
{
	local name="$1"
	local needle="$2"
	local value

	value=$(fwenv_get_env "$name") || return 1
	case "$value" in
	*"$needle"*) return 0 ;;
	esac

	return 1
}

fwenv_bpi8x_boot_env_complete()
{
	local bootcmd

	bootcmd=$(fwenv_get_env bootcmd) || return 1
	case "$bootcmd" in
	*boot_recovery*boot_sdmmc*|*boot_sdmmc*boot_recovery*) ;;
	*) return 1 ;;
	esac

	[ "$(fwenv_get_env bootconf)" = "config-mt7988a-bananapi-bpi-r4-pro-8x" ] || return 1
	[ "$(fwenv_get_env bootconf_sd)" = "mt7988a-bananapi-bpi-r4-pro-sd" ] || return 1
	[ "$(fwenv_get_env bootconf_emmc)" = "mt7988a-bananapi-bpi-r4-pro-emmc" ] || return 1
	[ "$(fwenv_get_env part_default)" = "production" ] || return 1
	[ "$(fwenv_get_env part_recovery)" = "recovery" ] || return 1

	fwenv_env_contains bootmenu_0 "run boot_default" || return 1
	fwenv_env_contains boot_default "run bootcmd" || return 1
	fwenv_env_contains boot_default "run boot_recovery" || return 1
	fwenv_env_contains bootmenu_1 "run boot_production" || return 1
	fwenv_env_contains bootmenu_2 "run boot_recovery" || return 1
	fwenv_env_contains boot_production "sdmmc_read_production" || return 1
	fwenv_env_contains boot_production "bootconf_sd" || return 1
	fwenv_env_contains boot_recovery "sdmmc_read_recovery" || return 1
	fwenv_env_contains boot_recovery "bootconf_emmc" || return 1
	fwenv_env_contains boot_sdmmc "boot_production" || return 1
	fwenv_env_contains boot_sdmmc "boot_recovery" || return 1
	fwenv_env_contains mmc_read_vol "mmc read" || return 1
	fwenv_env_contains sdmmc_read_production "part start mmc 0" || return 1
	fwenv_env_contains sdmmc_read_production "part_default" || return 1
	fwenv_env_contains sdmmc_read_recovery "part start mmc 0" || return 1
	fwenv_env_contains sdmmc_read_recovery "part_recovery" || return 1
}

fwenv_macaddr_triplet_valid()
{
	local ethaddr eth1addr eth2addr

	ethaddr=$(fwenv_macaddr_valid_unicast "$1") || return 1
	eth1addr=$(fwenv_macaddr_valid_unicast "$2") || return 1
	eth2addr=$(fwenv_macaddr_valid_unicast "$3") || return 1
	[ "$ethaddr" != "$eth1addr" ] || return 1
	[ "$ethaddr" != "$eth2addr" ] || return 1
	[ "$eth1addr" != "$eth2addr" ] || return 1
}

fwenv_get_eth0_mac()
{
	[ -r /sys/class/net/eth0/address ] || return 1

	fwenv_macaddr_valid_unicast "$(cat /sys/class/net/eth0/address)"
}

fwenv_get_factory_triplet()
{
	local ethaddr eth1addr eth2addr

	ethaddr=$(fwenv_macaddr_valid_unicast "$(mtd_get_mac_binary "Factory" 0xffff4 2>/dev/null)") || return 1
	eth1addr=$(fwenv_macaddr_valid_unicast "$(mtd_get_mac_binary "Factory" 0xffffa 2>/dev/null)") || return 1
	eth2addr=$(fwenv_macaddr_valid_unicast "$(mtd_get_mac_binary "Factory" 0xfffee 2>/dev/null)") || return 1
	fwenv_macaddr_triplet_valid "$ethaddr" "$eth1addr" "$eth2addr" || return 1

	FWENV_FACTORY_ETHADDR="$ethaddr"
	FWENV_FACTORY_ETH1ADDR="$eth1addr"
	FWENV_FACTORY_ETH2ADDR="$eth2addr"
}

fwenv_store_mac()
{
	local name="$1"
	local value="$2"

	if fw_setenv "$name" "$value"; then
		fwenv_bpi8x_log "stored $name=$value in active U-Boot env"
		return 0
	fi

	fwenv_bpi8x_log "failed to store $name in active U-Boot env"
	return 1
}

fwenv_store_mac_if_changed()
{
	local name="$1"
	local current="$2"
	local value="$3"

	[ "$current" = "$value" ] && return 0
	fwenv_store_mac "$name" "$value"
}

fwenv_repair_derived_triplet()
{
	local base="$1"
	local current1="$2"
	local current2="$3"
	local desired1 desired2
	local final1 final2

	desired1=$(macaddr_add "$base" 1)
	desired2=$(macaddr_add "$base" 2)
	fwenv_macaddr_triplet_valid "$base" "$desired1" "$desired2" || {
		fwenv_bpi8x_log "derived ethaddr/eth1addr/eth2addr triplet invalid, cannot persist MAC"
		return 1
	}

	final1="$current1"
	final2="$current2"
	[ -n "$final1" ] && [ "$final1" != "$base" ] || final1=""
	[ -n "$final2" ] && [ "$final2" != "$base" ] || final2=""
	[ -n "$final1" ] && [ "$final1" = "$final2" ] && final2=""

	if [ -z "$final1" ]; then
		final1="$desired1"
		fwenv_store_mac_if_changed eth1addr "$current1" "$final1" || return 1
	fi

	if [ -z "$final2" ] || [ "$final2" = "$final1" ]; then
		final2="$desired2"
		fwenv_store_mac_if_changed eth2addr "$current2" "$final2" || return 1
	fi

	if fwenv_macaddr_triplet_valid "$base" "$final1" "$final2"; then
		return 0
	fi

	final1="$desired1"
	final2="$desired2"
	fwenv_store_mac_if_changed eth1addr "$current1" "$final1" || return 1
	fwenv_store_mac_if_changed eth2addr "$current2" "$final2" || return 1
	fwenv_macaddr_triplet_valid "$base" "$final1" "$final2" || {
		fwenv_bpi8x_log "derived ethaddr/eth1addr/eth2addr triplet invalid, cannot persist MAC"
		return 1
	}
}

fwenv_store_factory_triplet()
{
	fwenv_bpi8x_log "active U-Boot env MAC triplet incomplete, persisting Factory MAC triplet to active U-Boot env"
	fwenv_store_mac_if_changed ethaddr "$ethaddr" "$FWENV_FACTORY_ETHADDR" || return 1
	fwenv_store_mac_if_changed eth1addr "$eth1addr" "$FWENV_FACTORY_ETH1ADDR" || return 1
	fwenv_store_mac_if_changed eth2addr "$eth2addr" "$FWENV_FACTORY_ETH2ADDR" || return 1
}

case "$(board_name)" in
bananapi,bpi-r4-pro-8x)
	;;
*)
	[ ! -e /etc/fw_env.config ] && exit 0
	;;
esac

case "$(board_name)" in
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
	[ -s /etc/fw_env.config ] || {
		fwenv_bpi8x_log "active U-Boot env config missing, cannot persist MAC"
		exit 1
	}
	(command -v fw_printenv >/dev/null 2>&1 &&
		command -v fw_setenv >/dev/null 2>&1) || {
		fwenv_bpi8x_log "fw_printenv/fw_setenv unavailable, cannot persist MAC"
		exit 1
	}
	fw_printenv >/dev/null 2>&1 || {
		fwenv_bpi8x_log "active U-Boot env unreadable, cannot persist MAC"
		exit 1
	}
	fwenv_bpi8x_boot_env_complete || {
		fwenv_bpi8x_log "active U-Boot env lacks complete 8X SD boot variables; refusing automatic MAC persistence"
		exit 1
	}

	ethaddr=$(fwenv_get_valid_env_mac ethaddr)
	eth1addr=$(fwenv_get_valid_env_mac eth1addr)
	eth2addr=$(fwenv_get_valid_env_mac eth2addr)
	if fwenv_macaddr_triplet_valid "$ethaddr" "$eth1addr" "$eth2addr"; then
		fwenv_bpi8x_log "active U-Boot env already has valid ethaddr/eth1addr/eth2addr"
		exit 0
	fi

	if fwenv_get_factory_triplet; then
		fwenv_store_factory_triplet || exit 1
		exit 0
	fi

	if [ -z "$ethaddr" ]; then
		fwenv_bpi8x_log "ethaddr missing or invalid, persisting runtime eth0 MAC to active U-Boot env"
		ethaddr=$(fwenv_get_eth0_mac) || {
			fwenv_bpi8x_log "runtime eth0 MAC invalid, cannot persist MAC"
			exit 1
		}
		fwenv_store_mac ethaddr "$ethaddr" || exit 1
	fi

	fwenv_repair_derived_triplet "$ethaddr" "$eth1addr" "$eth2addr" || exit 1
	;;
esac

exit 0
