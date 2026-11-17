LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE    := HandlingTweak
LOCAL_SRC_FILES := HandlingTweak.cpp
LOCAL_C_INCLUDES += $(LOCAL_PATH)/AndroidModLoader $(LOCAL_PATH)/AndroidModLoader/include
LOCAL_CPPFLAGS  += -std=c++17 -O2 -fno-exceptions -fno-rtti
LOCAL_LDLIBS    := -llog -ldl
include $(BUILD_SHARED_LIBRARY)
