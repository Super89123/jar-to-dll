#include <windows.h>
#include <winuser.h>
#include <string>
#include <vector>

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

// Альтернативный способ получения JVM через JNI Invocation API
static JavaVM* GetJVMThroughInvocationAPI() {
    JavaVM* jvm = nullptr;
    JNIEnv* env = nullptr;
    
    JavaVMInitArgs vm_args;
    JavaVMOption options[1];
    
    // Пустая опция, можно добавить нужные параметры JVM
    options[0].optionString = const_cast<char*>("-Djava.class.path=.");
    
    vm_args.version = JNI_VERSION_1_8;
    vm_args.nOptions = 1;
    vm_args.options = options;
    vm_args.ignoreUnrecognized = JNI_TRUE;
    
    jint res = JNI_CreateJavaVM(&jvm, (void**)&env, &vm_args);
    if (res != JNI_OK || !jvm) {
        Error(L"Failed to create JVM through Invocation API");
    }
    
    return jvm;
}

static HMODULE GetJvmDll() {
    // Пытаемся найти jvm.dll в стандартных путях Java
    std::vector<std::wstring> paths = {
        L"jvm.dll",
        L"bin\\server\\jvm.dll",
        L"bin\\client\\jvm.dll",
        L"jre\\bin\\server\\jvm.dll",
        L"jre\\bin\\client\\jvm.dll"
    };

    for (const auto& path : paths) {
        HMODULE jvm_dll = LoadLibraryW(path.c_str());
        if (jvm_dll) {
            return jvm_dll;
        }
    }

    // Если не нашли в стандартных путях, пробуем через GetModuleHandle
    HMODULE jvm_dll = GetModuleHandleW(L"jvm.dll");
    if (!jvm_dll) {
        // Последняя попытка - загрузка из системного пути
        jvm_dll = LoadLibraryW(L"jvm.dll");
        if (!jvm_dll) {
            Error(L"Can't locate jvm.dll in standard paths");
        }
    }
    
    return jvm_dll;
}

typedef jint(JNICALL* GetCreatedJavaVMs_t)(JavaVM**, jsize, jsize*);

static GetCreatedJavaVMs_t GetGetCreatedJavaVMsProc(HMODULE jvm_dll) {
    // Пробуем разные варианты имени функции
    const char* procNames[] = {
        "JNI_GetCreatedJavaVMs",
        "_JNI_GetCreatedJavaVMs@12",
        reinterpret_cast<const char*>(5)  // Попробуем по ordinal
    };

    for (const auto& name : procNames) {
        FARPROC proc = GetProcAddress(jvm_dll, name);
        if (proc) {
            return reinterpret_cast<GetCreatedJavaVMs_t>(proc);
        }
    }

    // Если ничего не сработало, попробуем создать новую JVM
    return nullptr;
}

static JavaVM* GetJVM() {
    // Сначала пробуем стандартный способ
    HMODULE jvm_dll = GetJvmDll();
    auto getCreatedJavaVMs = GetGetCreatedJavaVMsProc(jvm_dll);
    
    if (getCreatedJavaVMs) {
        JavaVM* jvms[1];
        jsize n_vms = 0;
        jint result = getCreatedJavaVMs(jvms, 1, &n_vms);

        if (result == JNI_OK && n_vms > 0) {
            return jvms[0];
        }
    }

    // Если стандартный способ не сработал, пробуем через Invocation API
    return GetJVMThroughInvocationAPI();
}

static void GetJNIEnv(JavaVM* jvm, JNIEnv** jni_env) {
    *jni_env = nullptr;
    jint result = jvm->GetEnv((void**)jni_env, JNI_VERSION_1_8);
    
    if (result == JNI_EDETACHED) {
        result = jvm->AttachCurrentThread((void**)jni_env, nullptr);
    }
    
    if (result != JNI_OK || !*jni_env) {
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
