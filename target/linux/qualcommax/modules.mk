# SPDX-License-Identifier: GPL-2.0-only

define KernelPackage/qdx-drv
  SUBMENU:=$(NETWORK_DEVICES_MENU)
  TITLE:=IPQ8074 NSS execution driver
  DEPENDS:=@TARGET_qualcommax_ipq807x +qdx-nss-firmware
  KCONFIG:=CONFIG_QDX
  FILES:=$(LINUX_DIR)/drivers/net/ethernet/qualcomm/qdx/qdx-drv.ko
  AUTOLOAD:=$(call AutoProbe,qdx-drv)
endef

define KernelPackage/qdx-drv/description
  Dual-core NSS 11.4 firmware, communication and wired packet transport.
  Linux and the native EDMA/PPE drivers retain network configuration.
endef

$(eval $(call KernelPackage,qdx-drv))

define KernelPackage/qca-edma
  SUBMENU:=$(NETWORK_DEVICES_MENU)
  TITLE:=Qualcomm IPQ807x EDMA Ethernet controller
  DEPENDS:=@TARGET_qualcommax_ipq807x +PACKAGE_kmod-qdx-drv:kmod-qdx-drv
  KCONFIG:=CONFIG_QCOM_EDMA
  FILES:=$(LINUX_DIR)/drivers/net/ethernet/qualcomm/qca_edma.ko
  AUTOLOAD:=$(call AutoProbe,qca_edma)
endef

$(eval $(call KernelPackage,qca-edma))

define KernelPackage/qca-ppe
  SUBMENU:=$(NETWORK_DEVICES_MENU)
  TITLE:=Qualcomm IPQ807x 802.11ax PPE switch
  DEPENDS:=@TARGET_qualcommax_ipq807x +kmod-libphy +PACKAGE_kmod-qdx-drv:kmod-qdx-drv
  KCONFIG:=CONFIG_QCOM_80211AX_PPE
  FILES:=$(LINUX_DIR)/drivers/net/ethernet/qualcomm/qca_ppe.ko
  AUTOLOAD:=$(call AutoProbe,qca_ppe)
endef

$(eval $(call KernelPackage,qca-ppe))
