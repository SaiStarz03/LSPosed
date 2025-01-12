/*
 * This file is part of LSPosed.
 *
 * LSPosed is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * LSPosed is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LSPosed.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2021 LSPosed Contributors
 */

//
// Created by loves on 2/7/2021.
//

#include <dobby.h>
#include <thread>
#include "base/object.h"
#include "service.h"
#include "context.h"
#include "jni_helper.h"
#include "symbol_cache.h"

namespace lspd {
    jboolean
    Service::exec_transact_replace(jboolean *res, JNIEnv *env, [[maybe_unused]] jobject obj,
                                   va_list args) {
        va_list copy;
        va_copy(copy, args);
        auto code = va_arg(copy, jint);
        auto data_obj = va_arg(copy, jlong);
        auto reply_obj = va_arg(copy, jlong);
        auto flags = va_arg(copy, jint);
        va_end(copy);

        if (code == BRIDGE_TRANSACTION_CODE) [[unlikely]] {
            *res = JNI_CallStaticBooleanMethod(env, instance()->bridge_service_class_,
                                               instance()->exec_transact_replace_methodID_,
                                               obj, code, data_obj, reply_obj, flags);
            return true;
        } else if (SET_ACTIVITY_CONTROLLER_CODE != -1 &&
                   code == SET_ACTIVITY_CONTROLLER_CODE) [[unlikely]] {
            va_copy(copy, args);
            if (instance()->replace_activity_controller_methodID_) {
                *res = JNI_CallStaticBooleanMethod(env, instance()->bridge_service_class_,
                                                   instance()->replace_activity_controller_methodID_,
                                                   obj, code, data_obj, reply_obj, flags);
            }
            va_end(copy);
            // fallback the backup
        } else if (code == (('_' << 24) | ('C' << 16) | ('M' << 8) | 'D')) {
            va_copy(copy, args);
            if (instance()->replace_shell_command_methodID_) {
                *res = JNI_CallStaticBooleanMethod(env, instance()->bridge_service_class_,
                                                   instance()->replace_shell_command_methodID_,
                                                   obj, code, data_obj, reply_obj, flags);
            }
            va_end(copy);
            return *res;
        }
        return false;
    }

    jboolean
    Service::call_boolean_method_va_replace(JNIEnv *env, jobject obj, jmethodID methodId,
                                            va_list args) {
        if (methodId == instance()->exec_transact_backup_methodID_) [[unlikely]] {
            jboolean res = false;
            if (exec_transact_replace(&res, env, obj, args)) [[unlikely]] return res;
            // else fallback to backup
        }
        return instance()->call_boolean_method_va_backup_(env, obj, methodId, args);
    }

    void Service::InitService(JNIEnv *env) {
        if (initialized_) [[unlikely]] return;

        // ServiceManager
        if (auto service_manager_class = JNI_FindClass(env, "android/os/ServiceManager")) {
            service_manager_class_ = JNI_NewGlobalRef(env, service_manager_class);
        } else return;
        get_service_method_ = JNI_GetStaticMethodID(env, service_manager_class_, "getService",
                                                    "(Ljava/lang/String;)Landroid/os/IBinder;");
        if (!get_service_method_) return;

        // IBinder
        if (auto ibinder_class = JNI_FindClass(env, "android/os/IBinder")) {
            transact_method_ = JNI_GetMethodID(env, ibinder_class, "transact",
                                               "(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z");
        } else return;

        if (auto binder_class = JNI_FindClass(env, "android/os/Binder")) {
            binder_class_ = JNI_NewGlobalRef(env, binder_class);
        } else return;
        binder_ctor_ = JNI_GetMethodID(env, binder_class_, "<init>", "()V");

        // Parcel
        if (auto parcel_class = JNI_FindClass(env, "android/os/Parcel")) {
            parcel_class_ = JNI_NewGlobalRef(env, parcel_class);
        } else return;
        obtain_method_ = JNI_GetStaticMethodID(env, parcel_class_, "obtain",
                                               "()Landroid/os/Parcel;");
        recycleMethod_ = JNI_GetMethodID(env, parcel_class_, "recycle", "()V");
        write_interface_token_method_ = JNI_GetMethodID(env, parcel_class_, "writeInterfaceToken",
                                                        "(Ljava/lang/String;)V");
        write_int_method_ = JNI_GetMethodID(env, parcel_class_, "writeInt", "(I)V");
        write_string_method_ = JNI_GetMethodID(env, parcel_class_, "writeString",
                                               "(Ljava/lang/String;)V");
        write_strong_binder_method_ = JNI_GetMethodID(env, parcel_class_, "writeStrongBinder",
                                                      "(Landroid/os/IBinder;)V");
        read_exception_method_ = JNI_GetMethodID(env, parcel_class_, "readException", "()V");
        read_strong_binder_method_ = JNI_GetMethodID(env, parcel_class_, "readStrongBinder",
                                                     "()Landroid/os/IBinder;");
//        createStringArray_ = env->GetMethodID(parcel_class_, "createStringArray",
//                                              "()[Ljava/lang/String;");

        if (auto dead_object_exception_class = JNI_FindClass(env,
                                                             "android/os/DeadObjectException")) {
            deadObjectExceptionClass_ = JNI_NewGlobalRef(env, dead_object_exception_class);
        }
        initialized_ = true;
    }

    void Service::HookBridge(const Context &context, JNIEnv *env) {
        static bool kHooked = false;
        // This should only be ran once, so unlikely
        if (kHooked) [[unlikely]] return;
        if (!initialized_) [[unlikely]] return;
        kHooked = true;
        if (auto bridge_service_class = context.FindClassFromCurrentLoader(env,
                                                                           kBridgeServiceClassName))
            bridge_service_class_ = JNI_NewGlobalRef(env, bridge_service_class);
        else {
            LOGE("server class not found");
            return;
        }

        constexpr const auto *hooker_sig = "(Landroid/os/IBinder;IJJI)Z";

        exec_transact_replace_methodID_ = JNI_GetStaticMethodID(env, bridge_service_class_,
                                                                "execTransact",
                                                                hooker_sig);
        if (!exec_transact_replace_methodID_) {
            LOGE("execTransact class not found");
            return;
        }


        replace_activity_controller_methodID_ = JNI_GetStaticMethodID(env, bridge_service_class_,
                                                                      "replaceActivityController",
                                                                      hooker_sig);
        if (!replace_activity_controller_methodID_) {
            LOGE("replaceActivityShell class not found");
        }

        replace_shell_command_methodID_ = JNI_GetStaticMethodID(env, bridge_service_class_,
                                                                "replaceShellCommand",
                                                                hooker_sig);
        if (!replace_shell_command_methodID_) {
            LOGE("replaceShellCommand class not found");
        }

        auto binder_class = JNI_FindClass(env, "android/os/Binder");
        exec_transact_backup_methodID_ = JNI_GetMethodID(env, binder_class, "execTransact",
                                                         "(IJJI)Z");
        if (!symbol_cache->setTableOverride) {
            LOGE("set table override not found");
        }
        memcpy(&native_interface_replace_, env->functions, sizeof(JNINativeInterface));

        call_boolean_method_va_backup_ = env->functions->CallBooleanMethodV;
        native_interface_replace_.CallBooleanMethodV = &call_boolean_method_va_replace;

        if (symbol_cache->setTableOverride != nullptr) {
            reinterpret_cast<void (*)(JNINativeInterface *)>(symbol_cache->setTableOverride)(
                    &native_interface_replace_);
        }
        if (auto activity_thread_class = JNI_FindClass(env, "android/app/IActivityManager$Stub")) {
            if (auto *set_activity_controller_field = JNI_GetStaticFieldID(env,
                                                                           activity_thread_class,
                                                                           "TRANSACTION_setActivityController",
                                                                           "I")) {
                SET_ACTIVITY_CONTROLLER_CODE = JNI_GetStaticIntField(env, activity_thread_class,
                                                                     set_activity_controller_field);
            }
        }

        LOGD("Done InitService");
    }

    ScopedLocalRef<jobject> Service::RequestBinder(JNIEnv *env, jstring nice_name) {
        if (!initialized_) [[unlikely]] {
            LOGE("Service not initialized");
            return {env, nullptr};
        }

        auto *bridge_service_name = env->NewStringUTF(BRIDGE_SERVICE_NAME.data());
        auto bridge_service = JNI_CallStaticObjectMethod(env, service_manager_class_,
                                                         get_service_method_, bridge_service_name);
        if (!bridge_service) {
            LOGD("can't get %s", BRIDGE_SERVICE_NAME.data());
            return {env, nullptr};
        }

        auto heart_beat_binder = JNI_NewObject(env, binder_class_, binder_ctor_);

        auto data = JNI_CallStaticObjectMethod(env, parcel_class_, obtain_method_);
        auto reply = JNI_CallStaticObjectMethod(env, parcel_class_, obtain_method_);

        auto *descriptor = env->NewStringUTF(BRIDGE_SERVICE_DESCRIPTOR.data());
        JNI_CallVoidMethod(env, data, write_interface_token_method_, descriptor);
        JNI_CallVoidMethod(env, data, write_int_method_, BRIDGE_ACTION_GET_BINDER);
        JNI_CallVoidMethod(env, data, write_string_method_, nice_name);
        JNI_CallVoidMethod(env, data, write_strong_binder_method_, heart_beat_binder);

        auto res = JNI_CallBooleanMethod(env, bridge_service, transact_method_,
                                         BRIDGE_TRANSACTION_CODE,
                                         data,
                                         reply, 0);

        ScopedLocalRef<jobject> service = {env, nullptr};
        if (res) {
            JNI_CallVoidMethod(env, reply, read_exception_method_);
            service = JNI_CallObjectMethod(env, reply, read_strong_binder_method_);
        }
        JNI_CallVoidMethod(env, data, recycleMethod_);
        JNI_CallVoidMethod(env, reply, recycleMethod_);
        if (service) {
            JNI_NewGlobalRef(env, heart_beat_binder);
        }

        return service;
    }

    ScopedLocalRef<jobject> Service::RequestBinderForSystemServer(JNIEnv *env) {
        if (!initialized_ || !bridge_service_class_) [[unlikely]] {
            LOGE("Service not initialized");
            return {env, nullptr};
        }
        auto *bridge_service_name = env->NewStringUTF(SYSTEM_SERVER_BRIDGE_SERVICE_NAME.data());
        ScopedLocalRef<jobject> binder{env, nullptr};
        for (int i = 0; i < 3; ++i) {
            binder = JNI_CallStaticObjectMethod(env, service_manager_class_,
                                                get_service_method_, bridge_service_name);
            if (binder) {
                LOGD("Got binder for system server");
                break;
            }
            LOGI("Fail to get binder for system server, try again in 1s");
            using namespace std::chrono_literals;
            std::this_thread::sleep_for(1s);
        }
        if (!binder) {
            LOGW("Fail to get binder for system server");
            return {env, nullptr};
        }
        auto *method = JNI_GetStaticMethodID(env, bridge_service_class_,
                                             "getApplicationServiceForSystemServer",
                                             "(Landroid/os/IBinder;Landroid/os/IBinder;)Landroid/os/IBinder;");
        auto heart_beat_binder = JNI_NewObject(env, binder_class_, binder_ctor_);
        auto app_binder = JNI_CallStaticObjectMethod(env, bridge_service_class_, method, binder,
                                                     heart_beat_binder);
        if (app_binder) {
            JNI_NewGlobalRef(env, heart_beat_binder);
        }
        return app_binder;
    }
}  // namespace lspd
