#Android makefile to build kernel as a part of Android Build
PERL		= perl

ifeq ($(TARGET_PREBUILT_KERNEL),)

KERNEL_OUT := $(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ
KERNEL_CONFIG := $(KERNEL_OUT)/.config
ifeq ($(TARGET_KERNEL_APPEND_DTB), true)
TARGET_PREBUILT_INT_KERNEL := $(KERNEL_OUT)/arch/arm/boot/zImage-dtb
else
TARGET_PREBUILT_INT_KERNEL := $(KERNEL_OUT)/arch/arm/boot/zImage
endif
KERNEL_HEADERS_INSTALL := $(KERNEL_OUT)/usr
KERNEL_MODULES_INSTALL := system
KERNEL_MODULES_OUT := $(TARGET_OUT)/lib/modules
KERNEL_IMG=$(KERNEL_OUT)/arch/arm/boot/Image

DTS_NAMES ?= $(shell $(PERL) -e 'while (<>) {$$a = $$1 if /CONFIG_ARCH_((?:MSM|QSD|MPQ)[a-zA-Z0-9]+)=y/; $$r = $$1 if /CONFIG_MSM_SOC_REV_(?!NONE)(\w+)=y/; $$arch = $$arch.lc("$$a$$r ") if /CONFIG_ARCH_((?:MSM|QSD|MPQ)[a-zA-Z0-9]+)=y/} print $$arch;' $(KERNEL_CONFIG))
KERNEL_USE_OF ?= $(shell $(PERL) -e '$$of = "n"; while (<>) { if (/CONFIG_USE_OF=y/) { $$of = "y"; break; } } print $$of;' kernel/arch/arm/configs/$(KERNEL_DEFCONFIG))

ifeq "$(KERNEL_USE_OF)" "y"
DTS_FILES = $(wildcard $(TOP)/kernel/arch/arm/boot/dts/$(DTS_NAME)*.dts)
DTS_FILE = $(lastword $(subst /, ,$(1)))
DTB_FILE = $(addprefix $(KERNEL_OUT)/arch/arm/boot/,$(patsubst %.dts,%.dtb,$(call DTS_FILE,$(1))))
ZIMG_FILE = $(addprefix $(KERNEL_OUT)/arch/arm/boot/,$(patsubst %.dts,%-zImage,$(call DTS_FILE,$(1))))
KERNEL_ZIMG = $(KERNEL_OUT)/arch/arm/boot/zImage
DTC = $(KERNEL_OUT)/scripts/dtc/dtc

define append-dtb
mkdir -p $(KERNEL_OUT)/arch/arm/boot;\
$(foreach DTS_NAME, $(DTS_NAMES), \
   $(foreach d, $(DTS_FILES), \
      $(DTC) -p 1024 -O dtb -o $(call DTB_FILE,$(d)) $(d); \
      cat $(KERNEL_ZIMG) $(call DTB_FILE,$(d)) > $(call ZIMG_FILE,$(d));))
endef
else

define append-dtb
endef
endif

#S:LO for sim detection
ifeq ($(TARGET_PRODUCT),eagle_ds)
  CCI_SIM_DET_EAGLE_DS := 1
endif
#E:LO for sim detection

#[VY5x] ==> CCI KLog, added by Jimmy@CCI
ifeq ($(CCI_TARGET_KLOG),true)
  CCI_CUSTOMIZE := 1
  CCI_KLOG := 1
  CCI_KLOG_START_ADDR_PHYSICAL := $(CCI_TARGET_KLOG_START_ADDR_PHYSICAL)
  CCI_KLOG_SIZE := $(CCI_TARGET_KLOG_SIZE)
  CCI_KLOG_HEADER_SIZE := $(CCI_TARGET_KLOG_HEADER_SIZE)
  CCI_KLOG_CRASH_SIZE := $(CCI_TARGET_KLOG_CRASH_SIZE)
  CCI_KLOG_APPSBL_SIZE := $(CCI_TARGET_KLOG_APPSBL_SIZE)
  CCI_KLOG_KERNEL_SIZE := $(CCI_TARGET_KLOG_KERNEL_SIZE)
  CCI_KLOG_ANDROID_MAIN_SIZE := $(CCI_TARGET_KLOG_ANDROID_MAIN_SIZE)
  CCI_KLOG_ANDROID_SYSTEM_SIZE := $(CCI_TARGET_KLOG_ANDROID_SYSTEM_SIZE)
  CCI_KLOG_ANDROID_RADIO_SIZE := $(CCI_TARGET_KLOG_ANDROID_RADIO_SIZE)
  CCI_KLOG_ANDROID_EVENTS_SIZE := $(CCI_TARGET_KLOG_ANDROID_EVENTS_SIZE)
ifeq ($(CCI_TARGET_KLOG_SUPPORT_CCI_ENGMODE),true)
  CCI_KLOG_SUPPORT_CCI_ENGMODE := 1
endif # ifeq ($(CCI_TARGET_KLOG_SUPPORT_CCI_ENGMODE),true)
ifneq ($(TARGET_BUILD_VARIANT),user)
  CCI_KLOG_ALLOW_FORCE_PANIC := 1
ifeq ($(CCI_TARGET_KLOG_SUPPORT_RESTORATION),true)
    CCI_KLOG_SUPPORT_RESTORATION := 1
endif # ifeq ($(CCI_TARGET_KLOG_SUPPORT_RESTORATION),true)
endif # ifneq ($(TARGET_BUILD_VARIANT),user)
endif # ifeq ($(CCI_TARGET_KLOG),true)
#[VY5x] <== CCI KLog, added by Jimmy@CCI

#/* KevinA_Lin, 20130906 */
ifneq ($(TARGET_BUILD_VARIANT),user) 
  CCI_FORCE_RAMDUMP := 1
endif
#/* KevinA_Lin, 20130906 */

ifeq ($(TARGET_USES_UNCOMPRESSED_KERNEL),true)
$(info Using uncompressed kernel)
TARGET_PREBUILT_KERNEL := $(KERNEL_OUT)/piggy
else
TARGET_PREBUILT_KERNEL := $(TARGET_PREBUILT_INT_KERNEL)
endif

define mv-modules
mdpath=`find $(KERNEL_MODULES_OUT) -type f -name modules.dep`;\
if [ "$$mdpath" != "" ];then\
mpath=`dirname $$mdpath`;\
ko=`find $$mpath/kernel -type f -name *.ko`;\
for i in $$ko; do mv $$i $(KERNEL_MODULES_OUT)/; done;\
fi
endef

define clean-module-folder
mdpath=`find $(KERNEL_MODULES_OUT) -type f -name modules.dep`;\
if [ "$$mdpath" != "" ];then\
mpath=`dirname $$mdpath`; rm -rf $$mpath;\
fi
endef

$(KERNEL_OUT):
	mkdir -p $(KERNEL_OUT)

$(KERNEL_CONFIG): $(KERNEL_OUT)
	$(MAKE) -C kernel O=../$(KERNEL_OUT) ARCH=arm CROSS_COMPILE=arm-eabi- $(KERNEL_DEFCONFIG)

$(KERNEL_OUT)/piggy : $(TARGET_PREBUILT_INT_KERNEL)
	$(hide) gunzip -c $(KERNEL_OUT)/arch/arm/boot/compressed/piggy.gzip > $(KERNEL_OUT)/piggy

$(TARGET_PREBUILT_INT_KERNEL): CONFIG_SECURE $(KERNEL_OUT) $(KERNEL_CONFIG) $(KERNEL_HEADERS_INSTALL)
#[VY5x] ==> CCI KLog, added by Jimmy@CCI
ifeq ($(CCI_CUSTOMIZE),1)
	$(MAKE) -C kernel O=../$(KERNEL_OUT) ARCH=arm CROSS_COMPILE=arm-eabi- CCI_KLOG=$(CCI_KLOG) CCI_KLOG_START_ADDR_PHYSICAL=$(CCI_KLOG_START_ADDR_PHYSICAL) CCI_KLOG_SIZE=$(CCI_KLOG_SIZE) CCI_KLOG_HEADER_SIZE=$(CCI_KLOG_HEADER_SIZE) CCI_KLOG_CRASH_SIZE=$(CCI_KLOG_CRASH_SIZE) CCI_KLOG_APPSBL_SIZE=$(CCI_KLOG_APPSBL_SIZE) CCI_KLOG_KERNEL_SIZE=$(CCI_KLOG_KERNEL_SIZE) CCI_KLOG_ANDROID_MAIN_SIZE=$(CCI_KLOG_ANDROID_MAIN_SIZE) CCI_KLOG_ANDROID_SYSTEM_SIZE=$(CCI_KLOG_ANDROID_SYSTEM_SIZE) CCI_KLOG_ANDROID_RADIO_SIZE=$(CCI_KLOG_ANDROID_RADIO_SIZE) CCI_KLOG_ANDROID_EVENTS_SIZE=$(CCI_KLOG_ANDROID_EVENTS_SIZE) CCI_KLOG_SUPPORT_CCI_ENGMODE=$(CCI_KLOG_SUPPORT_CCI_ENGMODE) CCI_KLOG_ALLOW_FORCE_PANIC=$(CCI_KLOG_ALLOW_FORCE_PANIC) CCI_KLOG_SUPPORT_RESTORATION=$(CCI_KLOG_SUPPORT_RESTORATION) CCI_FORCE_RAMDUMP=$(CCI_FORCE_RAMDUMP) CCI_SIM_DET_EAGLE_DS=$(CCI_SIM_DET_EAGLE_DS)
else # ifeq ($(CCI_CUSTOMIZE),1)
	$(MAKE) -C kernel O=../$(KERNEL_OUT) ARCH=arm CROSS_COMPILE=arm-eabi-
endif # ifeq ($(CCI_CUSTOMIZE),1)
#[VY5x] <== CCI KLog, added by Jimmy@CCI
	$(MAKE) -C kernel O=../$(KERNEL_OUT) ARCH=arm CROSS_COMPILE=arm-eabi- modules
	$(MAKE) -C kernel O=../$(KERNEL_OUT) INSTALL_MOD_PATH=../../$(KERNEL_MODULES_INSTALL) INSTALL_MOD_STRIP=1 ARCH=arm CROSS_COMPILE=arm-eabi- modules_install
	$(mv-modules)
	$(clean-module-folder)
	$(append-dtb)

$(KERNEL_HEADERS_INSTALL): $(KERNEL_OUT) $(KERNEL_CONFIG)
	$(MAKE) -C kernel O=../$(KERNEL_OUT) ARCH=arm CROSS_COMPILE=arm-eabi- headers_install

kerneltags: $(KERNEL_OUT) $(KERNEL_CONFIG)
	$(MAKE) -C kernel O=../$(KERNEL_OUT) ARCH=arm CROSS_COMPILE=arm-eabi- tags

kernelconfig: $(KERNEL_OUT) $(KERNEL_CONFIG)
	env KCONFIG_NOTIMESTAMP=true \
	     $(MAKE) -C kernel O=../$(KERNEL_OUT) ARCH=arm CROSS_COMPILE=arm-eabi- menuconfig
	env KCONFIG_NOTIMESTAMP=true \
	     $(MAKE) -C kernel O=../$(KERNEL_OUT) ARCH=arm CROSS_COMPILE=arm-eabi- savedefconfig
	cp $(KERNEL_OUT)/defconfig kernel/arch/arm/configs/$(KERNEL_DEFCONFIG)

#[VY5x]Bato add secure mode and disable UART in secure mode.
KERNEL_DEFCONFIG_FILE := kernel/arch/arm/configs/$(KERNEL_DEFCONFIG)

CONFIG_SECURE:
ifeq ($(CCI_SECURE_MODE),true)
	echo "$(KERNEL_DEFCONFIG): set CONFIG_CCI_SECURE_MODE"
	sed -i '/SERIAL_MSM_HSL/d' $(KERNEL_DEFCONFIG_FILE)
	echo "# CONFIG_SERIAL_MSM_HSL is not set" >> $(KERNEL_DEFCONFIG_FILE)
else
	echo "$(KERNEL_DEFCONFIG): unset CONFIG_CCI_SECURE_MODE"
	sed -i '/SERIAL_MSM_HSL/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_SERIAL_MSM_HSL=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/SERIAL_MSM_HSL_CONSOLE/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_SERIAL_MSM_HSL_CONSOLE=y" >> $(KERNEL_DEFCONFIG_FILE)
endif
#[VY5x] ==> disable some debug options in user-build, added by Jimmy@CCI
ifeq ($(TARGET_BUILD_VARIANT),user)
	sed -i '/CORESIGHT/d' $(KERNEL_DEFCONFIG_FILE)
	echo "# CONFIG_CORESIGHT is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/MSM_JTAG_MM/d' $(KERNEL_DEFCONFIG_FILE)
	echo "# CONFIG_MSM_JTAG_MM is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/INPUT_EVBUG/d' $(KERNEL_DEFCONFIG_FILE)
	echo "# CONFIG_INPUT_EVBUG is not set" >> $(KERNEL_DEFCONFIG_FILE)
else
	sed -i '/CORESIGHT/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_CORESIGHT=y" >> $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_CORESIGHT_FUSE=y" >> $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_CORESIGHT_TMC=y" >> $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_CORESIGHT_TPIU=y" >> $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_CORESIGHT_FUNNEL=y" >> $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_CORESIGHT_REPLICATOR=y" >> $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_CORESIGHT_STM=y" >> $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_CORESIGHT_HWEVENT=y" >> $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_CORESIGHT_ETM=y" >> $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_CORESIGHT_AUDIO_ETM=y" >> $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_CORESIGHT_MODEM_ETM=y" >> $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_CORESIGHT_WCN_ETM=y" >> $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_CORESIGHT_RPM_ETM=y" >> $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_CORESIGHT_EVENT=m" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/MSM_JTAG_MM/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_MSM_JTAG_MM=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/INPUT_EVBUG/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_INPUT_EVBUG=m" >> $(KERNEL_DEFCONFIG_FILE)
endif
ifeq ($(TARGET_BUILD_VARIANT),user)
	sed -i '/DEBUG_MUTEXES/d' $(KERNEL_DEFCONFIG_FILE)
	echo "# CONFIG_DEBUG_MUTEXES is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/CONFIG_DEBUG_PAGEALLOC/d' $(KERNEL_DEFCONFIG_FILE)
	echo "# CONFIG_DEBUG_PAGEALLOC is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/PAGE_POISONING/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_PAGE_POISONING is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/CGROUP_DEBUG/d' $(KERNEL_DEFCONFIG_FILE)
	echo "# CONFIG_CGROUP_DEBUG is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/SLUB_DEBUG/d' $(KERNEL_DEFCONFIG_FILE)
	echo "# CONFIG_SLUB_DEBUG is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/MMC_BLOCK_TEST/d' $(KERNEL_DEFCONFIG_FILE)
	echo "# CONFIG_MMC_BLOCK_TEST is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/DEBUG_KMEMLEAK/d' $(KERNEL_DEFCONFIG_FILE)
	echo "# CONFIG_DEBUG_KMEMLEAK is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/DEBUG_SPINLOCK/d' $(KERNEL_DEFCONFIG_FILE)
	echo "# CONFIG_DEBUG_SPINLOCK is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/DEBUG_ATOMIC_SLEEP/d' $(KERNEL_DEFCONFIG_FILE)
	echo "# CONFIG_DEBUG_ATOMIC_SLEEP is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/DEBUG_STACK_USAGE/d' $(KERNEL_DEFCONFIG_FILE)
	echo "# CONFIG_DEBUG_STACK_USAGE is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/DEBUG_LIST/d' $(KERNEL_DEFCONFIG_FILE)
	echo "# CONFIG_DEBUG_LIST is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/FAULT_INJECTION/d' $(KERNEL_DEFCONFIG_FILE)
	echo "# CONFIG_FAULT_INJECTION is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/CONFIG_DEBUG_FS/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_DEBUG_FS=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/FAIL_PAGE_ALLOC/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_FAIL_PAGE_ALLOC is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/RCU_CPU_STALL_VERBOSE/d' $(KERNEL_DEFCONFIG_FILE)
	echo "# CONFIG_RCU_CPU_STALL_VERBOSE is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/CONFIG_FTRACE=\|CONFIG_FTRACE /d' $(KERNEL_DEFCONFIG_FILE)
	echo "# CONFIG_FTRACE is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/EVENT_POWER_TRACING_DEPRECATED/d' $(KERNEL_DEFCONFIG_FILE)
	echo "# CONFIG_EVENT_POWER_TRACING_DEPRECATED is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/CONFIG_STACKTRACE=\|CONFIG_STACKTRACE /d' $(KERNEL_DEFCONFIG_FILE)
	echo "# CONFIG_STACKTRACE is not set" >> $(KERNEL_DEFCONFIG_FILE)
else
	sed -i '/DEBUG_MUTEXES/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_DEBUG_MUTEXES=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/CONFIG_DEBUG_PAGEALLOC/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_DEBUG_PAGEALLOC=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/PAGE_POISONING/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_PAGE_POISONING=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/CGROUP_DEBUG/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_CGROUP_DEBUG=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/CONFIG_SLUB_DEBUG=\|CONFIG_SLUB_DEBUG /d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_SLUB_DEBUG=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/MMC_BLOCK_TEST/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_MMC_BLOCK_TEST=m" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/DEBUG_KMEMLEAK/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_DEBUG_KMEMLEAK=y" >> $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_DEBUG_KMEMLEAK_EARLY_LOG_SIZE=400" >> $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_DEBUG_KMEMLEAK_DEFAULT_OFF=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/DEBUG_SPINLOCK/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_DEBUG_SPINLOCK=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/DEBUG_ATOMIC_SLEEP/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_DEBUG_ATOMIC_SLEEP=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/DEBUG_STACK_USAGE/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_DEBUG_STACK_USAGE=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/DEBUG_LIST/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_DEBUG_LIST=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/CONFIG_FAULT_INJECTION/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_FAULT_INJECTION=y" >> $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_FAULT_INJECTION_DEBUG_FS=y" >> $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_FAULT_INJECTION_STACKTRACE_FILTER=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/CONFIG_DEBUG_FS/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_DEBUG_FS=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/FAIL_PAGE_ALLOC/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_FAIL_PAGE_ALLOC=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/RCU_CPU_STALL_VERBOSE/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_RCU_CPU_STALL_VERBOSE=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/CONFIG_FTRACE=\|CONFIG_FTRACE /d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_FTRACE=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/ENABLE_DEFAULT_TRACERS/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_ENABLE_DEFAULT_TRACERS=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/BRANCH_PROFILE_NONE/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_BRANCH_PROFILE_NONE=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/KPROBE_EVENT/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_KPROBE_EVENT=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/EVENT_POWER_TRACING_DEPRECATED/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_EVENT_POWER_TRACING_DEPRECATED=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/CONFIG_STACKTRACE=\|CONFIG_STACKTRACE /d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_STACKTRACE=y" >> $(KERNEL_DEFCONFIG_FILE)
endif
ifeq ($(TARGET_BUILD_VARIANT),user)
	sed -i '/CONFIG_TRACING=\|CONFIG_TRACING /d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_TRACING is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/CONFIG_TRACEPOINTS/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_TRACEPOINTS is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/NOP_TRACER/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_NOP_TRACER is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/EVENT_TRACING/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_EVENT_TRACING is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/CONTEXT_SWITCH_TRACER/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_CONTEXT_SWITCH_TRACER is not set" >> $(KERNEL_DEFCONFIG_FILE)
else
	sed -i '/CONFIG_TRACING=\|CONFIG_TRACING /d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_TRACING=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/CONFIG_TRACEPOINTS/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_TRACEPOINTS=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/NOP_TRACER/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_NOP_TRACER=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/EVENT_TRACING/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_EVENT_TRACING=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/CONTEXT_SWITCH_TRACER/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_CONTEXT_SWITCH_TRACER=y" >> $(KERNEL_DEFCONFIG_FILE)
endif
ifeq ($(TARGET_BUILD_VARIANT),user)
	sed -i '/DETECT_HUNG_TASK/d' $(KERNEL_DEFCONFIG_FILE)
	echo "# CONFIG_DETECT_HUNG_TASK is not set" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/CONFIG_LOCKUP_DETECTOR/d' $(KERNEL_DEFCONFIG_FILE)
	echo "# CONFIG_LOCKUP_DETECTOR is not set" >> $(KERNEL_DEFCONFIG_FILE)
else
	sed -i '/DETECT_HUNG_TASK/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_DETECT_HUNG_TASK=y" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/DEFAULT_HUNG_TASK_TIMEOUT/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_DEFAULT_HUNG_TASK_TIMEOUT=120" >> $(KERNEL_DEFCONFIG_FILE)
	sed -i '/CONFIG_LOCKUP_DETECTOR/d' $(KERNEL_DEFCONFIG_FILE)
	echo "CONFIG_LOCKUP_DETECTOR=y" >> $(KERNEL_DEFCONFIG_FILE)
endif
#[VY5x] <== disable some debug options in user-build, added by Jimmy@CCI

endif
