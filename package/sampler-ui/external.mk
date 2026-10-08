################################################################################
#
# sampler ui
#
################################################################################

SAMPLER_UI_VERSION = 1.0
SAMPLER_UI_SITE = ../package/sampler-ui/src
SAMPLER_UI_SITE_METHOD = local

SAMPLER_UI_DEPENDENCIES = sampler-libiio host-nodejs

define SAMPLER_UI_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) CC=$(TARGET_CC) -C $(@D)

	$(NPM) --prefix $(@D)/react-frontend-webgl2 install
	$(NPM) --prefix $(@D)/react-frontend-webgl2 run build
endef

define SAMPLER_UI_INSTALL_TARGET_CMDS
	$(INSTALL) -m 0755 -D $(@D)/adc_server.py $(TARGET_DIR)/ui/adc_server.py
	$(INSTALL) -m 0755 $(@D)/libiio_wrapper.py $(TARGET_DIR)/ui/libiio_wrapper.py

	$(INSTALL) -m 0755 $(@D)/libiio_wrapper_but_python_shouldnt_import_this.so \
		$(TARGET_DIR)/usr/lib/libiio_wrapper_but_python_shouldnt_import_this.so

	rsync -au $(@D)/react-frontend-webgl2/dist/* $(TARGET_DIR)/var/www/
	$(INSTALL) -m 0755 -D $(@D)/S50adc_server $(TARGET_DIR)/etc/init.d/S50adc_server
endef

$(eval $(generic-package))
