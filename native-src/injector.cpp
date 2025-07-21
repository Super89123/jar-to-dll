#include <windows.h>
#include <winuser.h>
#include <tlhelp32.h> // Для работы с ToolHelp Snapshots

#include "injector.h"
#include "jvm/jni.h"
#include "utils.h"

#if __has_include("classes/injector.h")
#include "classes/injector.h"
#else
#include "stub_classes/injector.h"
#endif

#if __has_include("classes/jar.h")
#include "classes/jar.h"
#else
#include "stub_classes/jar.h"
#endif

// Улучшенная функция поиска JNI_GetCreatedJavaVMs
typedef jint(JNICALL* GetCreatedJavaVMs)(JavaVM**, jsize, jsize*);

static GetCreatedJavaVMs GetGetCreatedJavaVMsProc() {
    // Создаем снимок всех модулей процесса
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        Error(L"CreateToolhelp32Snapshot failed");
    }

    MODULEENTRY32W module_entry;
    module_entry.dwSize = sizeof(module_entry);

    if (!Module32FirstW(snapshot, &module_entry)) {
        CloseHandle(snapshot);
        Error(L"Module32First failed");
    }

    GetCreatedJavaVMs result_proc = nullptr;
    bool found_jvm_dll = false;

    do {
        // Ищем все модули с именем jvm.dll (без учета регистра)
        if (_wcsicmp(module_entry.szModule, L"jvm.dll") == 0) {
            found_jvm_dll = true;
            // Пытаемся получить адрес функции
            auto proc = GetProcAddress(module_entry.hModule, "JNI_GetCreatedJavaVMs");
            if (proc) {
                result_proc = reinterpret_cast<GetCreatedJavaVMs>(proc);
                break;
            }
        }
    } while (Module32NextW(snapshot, &module_entry));

    CloseHandle(snapshot);

    // Обработка ошибок
    if (!result_proc) {
        if (found_jvm_dll) {
            Error(L"Can't get JNI_GetCreatedJavaVMs proc from jvm.dll");
        } else {
            Error(L"Can't find jvm.dll in the process");
        }
    }

    return result_proc;
}

static JavaVM* GetJVM() {
    // Получаем функцию через новый метод
    const auto get_created_java_vms = GetGetCreatedJavaVMsProc();

    JavaVM* jvms[1];
    jsize n_vms = 1;
    jsize actual_vms_count = 0;

    // Получаем созданные JVM
    jint result = get_created_java_vms(jvms, n_vms, &actual_vms_count);
    if (result != JNI_OK || actual_vms_count == 0) {
        Error(L"Can't get JVM");
    }

    return jvms[0];
}

static void GetJNIEnv(JavaVM* jvm, JNIEnv*& jni_env) {
  jni_env = nullptr;
  jvm->AttachCurrentThread(reinterpret_cast<void**>(&jni_env), nullptr);
  jvm->GetEnv(reinterpret_cast<void**>(&jni_env), JNI_VERSION_1_8);

  if (!jni_env) {
    Error(L"Can't get JNIEnv");
  }
}

static jclass DefineOrGetInjector(JNIEnv* jni_env) {
  const auto existing_injector_class = jni_env->FindClass(INJECTOR_CLASS_NAME);
  if (existing_injector_class) {
    ShowMessage(L"Injector class is already presented in jvm, using it");
    return existing_injector_class;
  }
  const auto injector_class = jni_env->DefineClass(
      nullptr, nullptr, injector_class_data, sizeof(injector_class_data));
  if (!injector_class) {
    Error(L"Failed to define injector class");
  }
  return injector_class;
}

static jobjectArray GetJarClassesArray(JNIEnv* jni_env) {
  const auto byte_array_class = jni_env->FindClass("[B");
  if (!byte_array_class) {
    Error(L"Failed to get byte array class");
  }
  const auto jar_classes_array = jni_env->NewObjectArray(
      sizeof(jar_classes_sizes) / sizeof(jar_classes_sizes[0]),
      byte_array_class, nullptr);
  if (!jar_classes_array) {
    Error(L"Failed to create jar classes array");
  }
  for (size_t i = 0;
       i < sizeof(jar_classes_sizes) / sizeof(jar_classes_sizes[0]); i++) {
    const auto class_byte_array = jni_env->NewByteArray(jar_classes_sizes[i]);
    if (!class_byte_array) {
      Error(L"Failed to create class byte array");
    }
    jni_env->SetByteArrayRegion(class_byte_array, 0, jar_classes_sizes[i],
                                jar_classes_data[i]);
    jni_env->SetObjectArrayElement(jar_classes_array, static_cast<jint>(i),
                                   class_byte_array);
  }

  return jar_classes_array;
}

static void CallInjector(JNIEnv* jni_env, jclass injector_class,
                  jobjectArray jar_classes_array) {
  const auto inject_method_id =
      jni_env->GetStaticMethodID(injector_class, "inject", "([[B)V");
  if (!inject_method_id) {
    Error(L"Failed to find inject method ID");
  }

  jni_env->CallStaticVoidMethod(
      injector_class, inject_method_id, jar_classes_array);
  ShowMessage(L"Native part ready, now java part is injecting");
}

void RunInjector() {
  ShowMessage(L"Starting");

  const auto jvm = GetJVM();

  JNIEnv* jni_env;
  GetJNIEnv(jvm, jni_env);

  const auto injector_class = DefineOrGetInjector(jni_env);
  const auto jar_classes_array = GetJarClassesArray(jni_env);

  CallInjector(jni_env, injector_class, jar_classes_array);

  FreeLibraryAndExitThread(::global_dll_instance, 0);
}
