#
#
# Copyright (C) Intel 2013-2017
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#


LOCAL_PATH := $(call my-dir)

# Component build
#######################################################################
# Common variables

component_src_files :=  \
    src/Pfw.cpp \
    src/VolumeKeys.cpp \
    src/AudioPlatformState.cpp

component_export_includes := \
    $(LOCAL_PATH)/include \
    $(LOCAL_PATH)/src

component_includes_common := \
    $(component_export_includes) \
    external/tinyalsa/include \
    frameworks/av/include/media

component_includes_dir := \
    hw \
    parameter

component_includes_dir_host := \
    $(component_includes_common) \
    $(foreach inc, $(component_includes_dir), $(HOST_OUT_HEADERS)/$(inc))

component_includes_dir_target := \
    $(component_includes_common) \
    $(foreach inc, $(component_includes_dir), $(TARGET_OUT_HEADERS)/$(inc))

component_static_lib := \
    libsamplespec_static \
    libstream_static \
    libparametermgr_static \
    libaudio_comms_utilities \
    libaudio_comms_convert \
    libaudio_hal_utilities \
    liblpepreprocessinghelper \
    libaudiocomms_naive_tokenizer \
    libproperty \
    audio.routemanager.includes \
    libaudioparameters \
    libevent-listener_static \
    libboost

component_static_lib_host := \
    $(foreach lib, $(component_static_lib), $(lib)_host)

component_cflags := $(HAL_COMMON_CFLAGS)

#######################################################################

include $(OPTIONAL_QUALITY_ENV_SETUP)

#######################################################################
# Component Target Build

include $(CLEAR_VARS)

LOCAL_MODULE := libaudioplatformstate
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MODULE_OWNER := intel

LOCAL_EXPORT_C_INCLUDE_DIRS := $(component_export_includes)
LOCAL_C_INCLUDES := $(component_includes_dir_target)
LOCAL_SRC_FILES := $(component_src_files)
LOCAL_STATIC_LIBRARIES := $(component_static_lib)
LOCAL_CFLAGS := $(component_cflags)

LOCAL_SHARED_LIBRARIES := \
    libparameter

ifeq ($(USE_ALSA_LIB), 1)
LOCAL_SHARED_LIBRARIES += libasound
endif


ifneq ($(strip $(PFW_CONFIGURATION_FOLDER)),)
LOCAL_CFLAGS += -DPFW_CONF_FILE_PATH=\"$(PFW_CONFIGURATION_FOLDER)\"
endif

LOCAL_MODULE_TAGS := optional
include $(OPTIONAL_QUALITY_COVERAGE_JUMPER)

include $(BUILD_STATIC_LIBRARY)


#######################################################################
# Component Host Build
ifeq (ENABLE_HOST_VERSION,1)
include $(CLEAR_VARS)

LOCAL_MODULE := libaudioplatformstate_host
LOCAL_MODULE_OWNER := intel
LOCAL_MODULE_TAGS := optional
LOCAL_STRIP_MODULE := false

LOCAL_EXPORT_C_INCLUDE_DIRS := $(component_export_includes)
LOCAL_C_INCLUDES := $(component_includes_dir_host)
LOCAL_SRC_FILES := $(component_src_files)
LOCAL_STATIC_LIBRARIES := $(component_static_lib_host)
LOCAL_CFLAGS := \
    $(component_cflags) \
     -O0 -ggdb \
    -DPFW_CONF_FILE_PATH=\"$(HOST_OUT)\"'"/etc/parameter-framework/"' \
    -DHAL_CONF_FILE_PATH=\"$(HOST_OUT)\"'"/etc"'

LOCAL_SHARED_LIBRARIES := \
    libparameter_host

include $(OPTIONAL_QUALITY_COVERAGE_JUMPER)

include $(BUILD_HOST_STATIC_LIBRARY)
endif
#######################################################################

include $(OPTIONAL_QUALITY_ENV_TEARDOWN)
