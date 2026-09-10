# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=qdx-nss-firmware
# Numeric APK version for the unchanged NSS.HK.11.4.0.5-6-R release.
PKG_VERSION:=11.4.0.5.6
PKG_RELEASE:=1
PKG_LICENSE:=LicenseRef-Qualcomm-NSS-Firmware
PKG_LICENSE_FILES:=README.md

include $(INCLUDE_DIR)/package.mk

RSTRIP:=:
STRIP:=:

define Package/qdx-nss-firmware
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=Qualcomm IPQ8074 NSS 11.4 firmware
  DEPENDS:=@TARGET_qualcommax_ipq807x
  PKGARCH:=all
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./README.md $(PKG_BUILD_DIR)/README.md
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/qdx-nss-firmware/install
	$(INSTALL_DIR) $(1)/lib/firmware/qdx/11.4
	$(INSTALL_DATA) ./files/retail_router0.bin $(1)/lib/firmware/qdx/11.4/
	$(INSTALL_DATA) ./files/retail_router1.bin $(1)/lib/firmware/qdx/11.4/
	$(INSTALL_DIR) $(1)/usr/share/doc/qdx-nss-firmware
	$(INSTALL_DATA) ./README.md $(1)/usr/share/doc/qdx-nss-firmware/
endef

$(eval $(call BuildPackage,qdx-nss-firmware))
