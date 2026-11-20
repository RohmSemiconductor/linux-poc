################################################################################
#
# sampler pylibiio
#
################################################################################

SAMPLER_PYLIBIIO_VERSION = 1.0
SAMPLER_PYLIBIIO_SOURCE = pylibiio-$(SAMPLER_PYLIBIIO_VERSION).tar.gz
SAMPLER_PYLIBIIO_SITE = https://pypi.python.org/packages/1d/dc/125e1c3fbf7c829974795fee636853ee41eb6d502ab3bed7dd47a45735bf
SAMPLER_PYLIBIIO_LICENSE = LGPL-2.1+
SAMPLER_PYLIBIIO_LICENSE_FILE = COPYING
SAMPLER_PYLIBIIO_SETUP_TYPE = setuptools

$(eval $(python-package))
