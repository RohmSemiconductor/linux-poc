################################################################################
#
# beaglevfire adcdemo
#
################################################################################

BEAGLEVFIRE_ADCDEMO_VERSION = 1.0
BEAGLEVFIRE_ADCDEMO_SITE = ../package/beagleVFire_adcdemo
BEAGLEVFIRE_ADCDEMO_SITE_METHOD = local

define BEAGLEVFIRE_ADCDEMO_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) CC="$(TARGET_CC)" AR="$(TARGET_AR)" CFLAGS="$(TARGET_CFLAGS) -Wall" \
		LDFLAGS="$(TARGET_LDFLAGS)" -C $(@D) bin/bvf_adc_stream bin/bvf_seq_check
endef

define BEAGLEVFIRE_ADCDEMO_INSTALL_TARGET_CMDS
	$(INSTALL) -m 0755 -D $(@D)/bin/bvf_adc_stream $(TARGET_DIR)/usr/bin/bvf_adc_stream
	$(INSTALL) -m 0755 -D $(@D)/bin/bvf_seq_check $(TARGET_DIR)/usr/bin/bvf_seq_check
endef

$(eval $(generic-package))
