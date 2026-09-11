################################################################################
#
# sampler ui
#
################################################################################

SAMPLER_UI_VERSION = 1.0
SAMPLER_UI_SITE = ../package/sampler-ui/src
SAMPLER_UI_SITE_METHOD = local

SAMPLER_UI_DEPENDENCIES = host-sampler-ui host-sampler-libiio \
			  host-nodejs

define HOST_SAMPLER_UI_BUILD_CMDS
	$(MAKE) -C $(@D)

	$(NPM) --prefix $(@D)/react-frontend-webgl2 install

	$(HOST_DIR)/bin/python3 -m venv $(@D)/venv
	source $(@D)/venv/bin/activate && \
	pip install aiohttp
endef

define HOST_SAMPLER_UI_INSTALL_CMDS
	$(INSTALL) -m 0755 -D $(@D)/adc_server.py $(HOST_DIR)/ui/adc_server.py
	$(INSTALL) -m 0755 $(@D)/libiio_wrapper.py $(HOST_DIR)/ui/libiio_wrapper.py
	$(INSTALL) -m 0755 $(@D)/libiio_wrapper_but_python_shouldnt_import_this.so \
			$(HOST_DIR)/ui/libiio_wrapper_but_python_shouldnt_import_this.so
	cp -r $(@D)/react-frontend-webgl2 $(HOST_DIR)/ui/react-frontend-webgl2
	cp -r $(@D)/venv $(HOST_DIR)/ui/venv
endef

$(eval $(host-generic-package))
