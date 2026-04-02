# AAuto App & Core Modules Configuration
# 
# Usage:
# In your device.mk or product makefile, simply include this file:
# $(call inherit-product, packages/apps/aa/aauto.mk)
# (or: include packages/apps/aa/aauto.mk)

PRODUCT_PACKAGES += \
    AAutoApp \
    libaauto \
    libaauto_jni \
    privapp-permissions-com.aauto.app.xml
