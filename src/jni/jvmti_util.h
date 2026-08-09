#pragma once
#include <jni.h>
#include <jvmti.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <initializer_list>

// =================================================================
// JvmtiUtil
// =================================================================
// WHY THIS EXISTS:
//
// env->GetFieldID(cls, name, signature) requires an EXACT signature
// match. Minecraft field signatures are obfuscated:
//
//   thePlayer  is  Lbew;   not  Lnet/minecraft/.../EntityPlayerSP;
//   theWorld   is  Lbdb;   not  Lnet/minecraft/.../WorldClient;
//
// We cannot know those obfuscated type names ahead of time, and they
// change between Lunar builds. Guessing "Ljava/lang/Object;" never
// matches, so every lookup silently fails.
//
// JVMTI GetClassFields hands us the real signature for every field.
// So: enumerate fields, match on NAME only, then build the fieldID
// using the signature JVMTI just told us.
//
// Same problem and same fix for methods.
// =================================================================

class JvmtiUtil {
private:
    inline static jvmtiEnv* s_jvmti = nullptr;

public:
    static jvmtiEnv* Get(JNIEnv* env) {
        if (s_jvmti) return s_jvmti;
        JavaVM* vm = nullptr;
        if (env->GetJavaVM(&vm) != JNI_OK || !vm) return nullptr;
        jvmtiEnv* j = nullptr;
        if (vm->GetEnv((void**)&j, JVMTI_VERSION_1_2) != JNI_OK || !j) {
            printf("[JVMTI] GetEnv failed\n");
            return nullptr;
        }
        s_jvmti = j;
        return s_jvmti;
    }

    // -------------------------------------------------------------
    // Find an instance field by name, ignoring its signature.
    // Tries every candidate name (SRG, MCP, Notch) against every
    // class in the hierarchy.
    // -------------------------------------------------------------
    static jfieldID FindField(JNIEnv* env, jclass cls,
                              std::initializer_list<const char*> names,
                              std::string* outSig = nullptr,
                              bool searchSupers = true)
    {
        jvmtiEnv* jvmti = Get(env);
        if (!jvmti || !cls) return nullptr;

        jclass current = (jclass)env->NewLocalRef(cls);
        jfieldID result = nullptr;

        while (current && !result) {
            jint count = 0;
            jfieldID* fields = nullptr;
            if (jvmti->GetClassFields(current, &count, &fields) == JVMTI_ERROR_NONE && fields) {
                for (jint i = 0; i < count && !result; i++) {
                    char* fName = nullptr;
                    char* fSig = nullptr;
                    if (jvmti->GetFieldName(current, fields[i], &fName, &fSig, nullptr)
                        != JVMTI_ERROR_NONE) continue;

                    if (fName && fSig) {
                        for (const char* want : names) {
                            if (std::strcmp(fName, want) == 0) {
                                // Rebuild through JNI so the ID is a proper
                                // jfieldID usable with Get/SetXxxField.
                                jfieldID id = env->GetFieldID(current, fName, fSig);
                                if (env->ExceptionCheck()) {
                                    env->ExceptionClear();
                                } else if (id) {
                                    result = id;
                                    if (outSig) *outSig = fSig;
                                }
                                break;
                            }
                        }
                    }

                    if (fName) jvmti->Deallocate((unsigned char*)fName);
                    if (fSig)  jvmti->Deallocate((unsigned char*)fSig);
                }
                jvmti->Deallocate((unsigned char*)fields);
            }

            if (result || !searchSupers) break;

            jclass super = env->GetSuperclass(current);
            env->DeleteLocalRef(current);
            current = super;
        }

        if (current) env->DeleteLocalRef(current);
        return result;
    }

    // -------------------------------------------------------------
    // Find a method by name, ignoring its signature.
    // If several overloads share the name, the first is returned;
    // pass argCountHint to disambiguate by parameter count.
    // -------------------------------------------------------------
    static jmethodID FindMethod(JNIEnv* env, jclass cls,
                                std::initializer_list<const char*> names,
                                int argCountHint = -1,
                                std::string* outSig = nullptr,
                                bool searchSupers = true)
    {
        jvmtiEnv* jvmti = Get(env);
        if (!jvmti || !cls) return nullptr;

        jclass current = (jclass)env->NewLocalRef(cls);
        jmethodID result = nullptr;

        while (current && !result) {
            jint count = 0;
            jmethodID* methods = nullptr;
            if (jvmti->GetClassMethods(current, &count, &methods) == JVMTI_ERROR_NONE && methods) {
                for (jint i = 0; i < count && !result; i++) {
                    char* mName = nullptr;
                    char* mSig = nullptr;
                    if (jvmti->GetMethodName(methods[i], &mName, &mSig, nullptr)
                        != JVMTI_ERROR_NONE) continue;

                    if (mName && mSig) {
                        for (const char* want : names) {
                            if (std::strcmp(mName, want) != 0) continue;
                            if (argCountHint >= 0 && CountArgs(mSig) != argCountHint) continue;

                            jmethodID id = env->GetMethodID(current, mName, mSig);
                            if (env->ExceptionCheck()) {
                                env->ExceptionClear();
                            } else if (id) {
                                result = id;
                                if (outSig) *outSig = mSig;
                            }
                            break;
                        }
                    }

                    if (mName) jvmti->Deallocate((unsigned char*)mName);
                    if (mSig)  jvmti->Deallocate((unsigned char*)mSig);
                }
                jvmti->Deallocate((unsigned char*)methods);
            }

            if (result || !searchSupers) break;

            jclass super = env->GetSuperclass(current);
            env->DeleteLocalRef(current);
            current = super;
        }

        if (current) env->DeleteLocalRef(current);
        return result;
    }

    static jmethodID FindStaticMethod(JNIEnv* env, jclass cls,
                                      std::initializer_list<const char*> names,
                                      int argCountHint = -1)
    {
        jvmtiEnv* jvmti = Get(env);
        if (!jvmti || !cls) return nullptr;

        jint count = 0;
        jmethodID* methods = nullptr;
        jmethodID result = nullptr;

        if (jvmti->GetClassMethods(cls, &count, &methods) == JVMTI_ERROR_NONE && methods) {
            for (jint i = 0; i < count && !result; i++) {
                char* mName = nullptr;
                char* mSig = nullptr;
                if (jvmti->GetMethodName(methods[i], &mName, &mSig, nullptr)
                    != JVMTI_ERROR_NONE) continue;

                if (mName && mSig) {
                    for (const char* want : names) {
                        if (std::strcmp(mName, want) != 0) continue;
                        if (argCountHint >= 0 && CountArgs(mSig) != argCountHint) continue;

                        jmethodID id = env->GetStaticMethodID(cls, mName, mSig);
                        if (env->ExceptionCheck()) env->ExceptionClear();
                        else if (id) result = id;
                        break;
                    }
                }

                if (mName) jvmti->Deallocate((unsigned char*)mName);
                if (mSig)  jvmti->Deallocate((unsigned char*)mSig);
            }
            jvmti->Deallocate((unsigned char*)methods);
        }
        return result;
    }

    // True if the class (or a superclass) declares a field with this name
    static bool HasField(JNIEnv* env, jclass cls, const char* name,
                         bool searchSupers = false)
    {
        return FindField(env, cls, { name }, nullptr, searchSupers) != nullptr;
    }

    static std::string GetClassSignature(JNIEnv* env, jclass cls) {
        jvmtiEnv* jvmti = Get(env);
        if (!jvmti || !cls) return {};
        char* sig = nullptr;
        std::string out;
        if (jvmti->GetClassSignature(cls, &sig, nullptr) == JVMTI_ERROR_NONE && sig) {
            out = sig;
            jvmti->Deallocate((unsigned char*)sig);
        }
        return out;
    }

private:
    // "(Lfoo;II)V" -> 3
    static int CountArgs(const char* sig) {
        if (!sig || sig[0] != '(') return -1;
        int n = 0;
        const char* p = sig + 1;
        while (*p && *p != ')') {
            while (*p == '[') p++;
            if (*p == 'L') { while (*p && *p != ';') p++; if (*p) p++; }
            else if (*p) p++;
            n++;
        }
        return n;
    }
};
