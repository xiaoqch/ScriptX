/*
 * Tencent is pleased to support the open source community by making ScriptX available.
 * Copyright (C) 2021 THL A29 Limited, a Tencent company.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "PyEngine.h"
#include "PyInternalHelper.h"
#include "PyRuntimeSettings.h"
#include "PyReference.hpp"
#include <cstring>
#include "../../src/Utils.h"
#include "../../src/utils/Helper.hpp"
#include "../../src/foundation.h"

SCRIPTX_BEGIN_IGNORE_DEPRECARED

namespace script::py_backend {

    PyEngine::PyEngine(std::shared_ptr<utils::MessageQueue> queue)
            : queue_(queue ? std::move(queue) : std::make_shared<utils::MessageQueue>()),
              engineLockHelper(this) {
        if (Py_IsInitialized() == 0) {
            // Not initialized. So no thread state at this time
            PyStatus status;
            // Set interpreter configs
            PyConfig config;
            PyConfig_InitPythonConfig(&config);
            status = PyConfig_SetBytesString(&config, &config.stdio_encoding, "utf-8");
            if (PyStatus_Exception(status)) {
                PyConfig_Clear(&config);
                Py_ExitStatusException(status);
            }
            py_runtime_settings::initDefaultPythonRuntimeSettings();

            // Init main interpreter
            status = Py_InitializeFromConfig(&config);
            if (PyStatus_Exception(status)) {
                PyConfig_Clear(&config);
                Py_ExitStatusException(status);
            }
            PyConfig_Clear(&config);
            // Initialize types
            namespaceType_ = makeNamespaceType();
            staticPropertyType_ = makeStaticPropertyType();
            defaultMetaType_ = makeDefaultMetaclass();
            emptyPyFunction = makeEmptyPyFunction();

            // PyThreadState_GET will cause FATAL error so here use PyThreadState_Swap instead
            // Store mainInterpreterState and mainThreadState
            PyThreadState *mainThreadState = PyEval_SaveThread();
            mainInterpreterState_ = mainThreadState->interp;
            mainThreadStateInTLS_.set(mainThreadState);
            // After this, thread state of main interpreter is loaded, and GIL is released.
            // Any code will run in sub-interpreters. The main interpreter just keeps the runtime environment.
        }

        // Use here to protect thread state switch
        engineLockHelper.waitToEnterEngine();

        // Create new interpreter
        PyThreadState *newSubState = Py_NewInterpreter();
        if (!newSubState) {
            throw Exception("Fail to create sub interpreter");
        }
        subInterpreterState_ = newSubState->interp;

        // Create exception class
        scriptxExceptionTypeObj = (PyTypeObject *) PyErr_NewExceptionWithDoc("Scriptx.ScriptxException",
                                                                             "Exception from ScriptX", PyExc_Exception,
                                                                             NULL);

        // Store created new sub thread state
        subThreadStateInTLS_.set(newSubState);

        // Exit engine locker
        engineLockHelper.finishExitEngine();
    }

    PyEngine::PyEngine() : PyEngine(nullptr) {}

    PyEngine::~PyEngine() = default;

    void PyEngine::destroy() noexcept {
        destroying = true;
        engineLockHelper.startDestroyEngine();
        ScriptEngine::destroyUserData();

        {
            // EngineScope enter(this);
            messageQueue()->removeMessageByTag(this);
            messageQueue()->shutdown();
            PyEngine::refsKeeper.dtor(this);          // destroy all Global and Weak refs of current engine
        }

        // =========================================
        // Attention! The logic below is partially referenced from Py_FinalizeEx and Py_EndInterpreter
        // in Python source code, so it may need to be re-adapted as the CPython backend's version
        // is updated.

        // Swap to correct target thread state
        PyThreadState *tstate = subThreadStateInTLS_.get();
        PyInterpreterState *interp = tstate->interp;
        PyThreadState *oldThreadState = PyThreadState_Swap(tstate);

        // Set finalizing sign
        SetPyInterpreterStateFinalizing(interp);

        /* Destroy the state of all threads of the interpreter, except of the
          current thread. In practice, only daemon threads should still be alive,
          except if wait_for_thread_shutdown() has been cancelled by CTRL+C.
          Clear frames of other threads to call objects destructors. Destructors
          will be called in the current Python thread. */
        _PyThreadState_DeleteExcept(tstate);

        PyGC_Collect();

        // End sub-interpreter
        Py_EndInterpreter(tstate);

        // Recover old thread state
        PyThreadState_Swap(oldThreadState);

        // =========================================

        engineLockHelper.endDestroyEngine();
    }

    Local<Value> PyEngine::get(const Local<String> &key) {
        // First find in __builtins__
        PyObject *item = getDictItem(getGlobalBuiltin(), key.toStringHolder().c_str());
        if (item)
            return py_interop::toLocal<Value>(item);
        else {
            // No found. Find in __main__
            item = getDictItem(getGlobalMain(), key.toStringHolder().c_str());
            if (item)
                return py_interop::toLocal<Value>(item);
            else
                return py_interop::toLocal<Value>(Py_None);
        }
    }

    void PyEngine::set(const Local<String> &key, const Local<Value> &value) {
        setDictItem(getGlobalBuiltin(), key.toStringHolder().c_str(), value.val_);
        //Py_DECREF(value.val_);
    }

    Local<Value> PyEngine::eval(const Local<String> &script) { return eval(script, Local<Value>()); }

    Local<Value> PyEngine::eval(const Local<String> &script, const Local<String> &sourceFile) {
        return eval(script, sourceFile.asValue());
    }

//
// Attention! CPython's eval is much different from other languages. We have not found a perfect way
// to solve the problem of eval yet. There is still room for improvement here.
// - Reason: Python has three different types of "eval" c-api, but none of them is perfect:
//   1. "Py_eval_input" can only execute simple expression code like "2+3" or "funcname(a,b)". Something
//      like assignments (a=3) or definitions (def func():xxx) are not supported.
//   2. "Py_file_input" can execute any type and any length of Python code, but it returns nothing!
//      It means that the return value will always be None, no matter what you actually eval.
//   3. "Py_single_input" cannot be used here. It is used for CPython interactive console and will print 
//      anything returned directly to console.
// - Because of the deliberate design of CPython, we can only use some rule to "guess" which mode is the
//   most suitable, and try our best to get the return value while ensuring that the code can be executed 
//   properly.
// - Logic we use below in eval:
//   1. Firstly, we check that if the code contains something like "\n" (multi-line) or " = " (assignments).
//     If found, we can only execute this code in "Py_file_input" mode, and returns None.
//   2. Secondly, we try to eval the code in "Py_eval_input" mode. It may fail. If eval succeeds, we can 
//     get return value and return directly.
//   3. If eval in "Py_eval_input" mode fails (get exception), and get a SyntaxError, we can reasonably  
//     guess that the cause of this exception is that the code is not a conforming expression. So we clear
//     this exception and try to eval it in "Py_file_input" mode again. (Goto 5)
//     (When we get a SyntexError, the code have not been actually executed, and will not have any 
//     side-effect. So re-eval is ok)
//   4. If we got an exception but it is not a SyntaxError, we must throw it out because the problems is 
//     not related to "Py_eval_input" mode.
//   5. If eval in "Py_file_input" mode succeeds, just return None directly. If the eval still fails, we
//     throw out the exception got here.
// - See more docs at docs/en/Python.md. There is still room for improvement in this logic. 
//
    Local<Value> PyEngine::eval(const Local<String> &script, const Local<Value> &sourceFile) {
        Tracer tracer(this, "PyEngine::eval");
        const char *source = script.toStringHolder().c_str();

        bool mayCodeBeExpression = true;
        // Use simple rules to find out the input that cannot be an expression
        if (strchr(source, '\n') != nullptr)
            mayCodeBeExpression = false;
        else if (strstr(source, " = ") != nullptr)
            mayCodeBeExpression = false;

        if (!mayCodeBeExpression) {
            // No way to get return value. result value is always Py_None
            PyObject *result = PyRun_StringFlags(source, Py_file_input, getGlobalMain(), nullptr, nullptr);
            return py_interop::asLocal<Value>(result);
        }
        // Try to eval in "Py_eval_input" mode
        PyObject *result = PyRun_StringFlags(source, Py_eval_input, getGlobalMain(), nullptr, nullptr);
        if (PyErr_Occurred()) {
            // Get exception
            PyTypeObject *pType;
            PyObject *pValue, *pTraceback;
            PyErr_Fetch((PyObject **) (&pType), &pValue, &pTraceback);
            PyErr_NormalizeException((PyObject **) (&pType), &pValue, &pTraceback);

            // is SyntaxError?
            std::string typeName{pType->tp_name};
            if (typeName.find("SyntaxError") != std::string::npos) {
                Py_XDECREF(pType);
                Py_XDECREF(pValue);
                Py_XDECREF(pTraceback);
                // Code is not actually executed now. Try Py_file_input again.
                PyObject *result = PyRun_StringFlags(source, Py_file_input, getGlobalMain(), nullptr, nullptr);
                checkAndThrowException();     // If get exception again, just throw it
                return py_interop::asLocal<Value>(result);    // Succeed in Py_file_input. Return None.
            } else {
                // Not SyntaxError. Must throw out here
                Exception e(py_interop::asLocal<Value>(newExceptionInstance(pType, pValue, pTraceback)));
                Py_XDECREF(pType);
                Py_XDECREF(pValue);
                Py_XDECREF(pTraceback);
                throw e;
            }
        } else
            return py_interop::asLocal<Value>(result);    // No exception. Return the value got.
    }

    Local<Value> PyEngine::loadFile(const Local<String> &scriptFile) {
        Tracer tracer(this, "PyEngine::loadFile");
        std::string sourceFilePath = scriptFile.toString();
        if (sourceFilePath.empty()) {
            throw Exception("script file no found");
        }
        Local<Value> content = internal::readAllFileContent(scriptFile);
        if (content.isNull()) {
            throw Exception("can't load script file");
        }

        std::size_t pathSymbol = sourceFilePath.rfind("/");
        if (pathSymbol != std::string::npos) {
            sourceFilePath = sourceFilePath.substr(pathSymbol + 1);
        } else {
            pathSymbol = sourceFilePath.rfind("\\");
            if (pathSymbol != std::string::npos) sourceFilePath = sourceFilePath.substr(pathSymbol + 1);
        }
        Local<String> sourceFileName = String::newString(sourceFilePath);

        const char *source = content.asString().toStringHolder().c_str();
        PyObject *result = PyRun_StringFlags(source, Py_file_input, getGlobalMain(), nullptr, nullptr);
        checkAndThrowException();
        return py_interop::asLocal<Value>(result);
    }

    std::shared_ptr<utils::MessageQueue> PyEngine::messageQueue() { return queue_; }

    void PyEngine::gc() {
        if (isDestroying())
            return;
        PyGC_Collect();
    }

    void PyEngine::adjustAssociatedMemory(int64_t count) {}

    ScriptLanguage PyEngine::getLanguageType() { return ScriptLanguage::kPython; }

    std::string PyEngine::getEngineVersion() { return Py_GetVersion(); }

    bool PyEngine::isDestroying() const { return destroying; }

    void PyEngine::performRegisterNativeClass(
            internal::TypeIndex typeIndex, const internal::ClassDefineState* classDefine,
            script::ScriptClass* (*instanceTypeToScriptClass)(void*)) {
        auto name_obj = toStr(classDefine->className.c_str());

        auto *heap_type = (PyHeapTypeObject *) PyType_GenericAlloc(PyEngine::defaultMetaType_, 0);
        if (!heap_type) {
            Py_FatalError("error allocating type!");
        }

        heap_type->ht_name = Py_NewRef(name_obj);
        heap_type->ht_qualname = Py_NewRef(name_obj);
        Py_DECREF(name_obj);

        auto *type = &heap_type->ht_type;
        type->tp_name = classDefine->className.c_str();
        Py_INCREF(&PyBaseObject_Type);
        type->tp_base = &PyBaseObject_Type;
        type->tp_basicsize = static_cast<Py_ssize_t>(sizeof(GeneralObject));
        type->tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HEAPTYPE;

        // enable object dict
        type->tp_dictoffset = SCRIPTX_OFFSET_OF(GeneralObject, instanceDict);

        /* Support weak references (needed for the keep_alive feature) */
        type->tp_weaklistoffset = SCRIPTX_OFFSET_OF(GeneralObject, weakrefs);

        type->tp_new = [](PyTypeObject *type, PyObject *args, PyObject *kwds) -> PyObject * {
            PyObject *self = type->tp_alloc(type, 0);
            return self;
        };
        type->tp_init = [](PyObject *self, PyObject *args, PyObject *kwds) -> int {
            auto engine = currentEngine();
            auto classDefine =
                    reinterpret_cast<const internal::ClassDefineState *>(engine->registeredTypesReverse_[self->ob_type]);
            if (classDefine->instanceDefine.constructor) {
                Tracer tracer(engine, classDefine->className);
                GeneralObject *cppSelf = reinterpret_cast<GeneralObject *>(self);

                if (!PyTuple_Check(args)) {
                    throw Exception(std::string("Can't create class ") + Py_TYPE(self)->tp_name);
                    return -1;
                }

                if (PyTuple_Size(args) == 1) {
                    PyObject *maybeCapsule = PyTuple_GetItem(args, 0);
                    if (PyCapsule_CheckExact(maybeCapsule)) {
                        // Passed a cpp this in capsule
                        // Logic for ScriptClass(const ScriptClass::ConstructFromCpp<T>)
                        cppSelf->instance = (void *) PyCapsule_GetPointer(maybeCapsule, nullptr);
                    }
                }

                if (cppSelf->instance == nullptr) {
                    // Python-side constructor
                    // Logic for ScriptClass::ScriptClass(const Local<Object>& thiz)
                    cppSelf->instance =
                            classDefine->instanceDefine.constructor(py_interop::makeArguments(engine, self, args));
                    if (cppSelf->instance == nullptr) {
                        throw Exception(std::string("Can't create class ") + Py_TYPE(self)->tp_name);
                        return -1;
                    }
                }
            } else {
                // Will never reach here. If pass nullptr to constructor(), ScriptX will make
                // constructor to be a function that always returns nullptr.
                return -1;
            }
            return 0;
        };
        type->tp_dealloc = [](PyObject *self) {
            auto type = Py_TYPE(self);
            delete (internal::ClassDefineState* *) (reinterpret_cast<GeneralObject *>(self)->instance);
            type->tp_free(self);
            Py_DECREF(type);
        };

        if (PyType_Ready(type) < 0) {
            throw Exception("PyType_Ready failed in make_object_base_type()");
        }

        setAttr((PyObject *) type, "__module__", toStr("scriptx_builtins"));

        this->registerStaticProperty(classDefine, (PyObject *) type);
        this->registerStaticFunction(classDefine, (PyObject *) type);
        this->registerInstanceProperty(classDefine, (PyObject *) type);
        this->registerInstanceFunction(classDefine, (PyObject *) type);
        this->registeredTypes_.emplace(classDefine, type);
        this->registeredTypesReverse_.emplace(type, classDefine);
        this->nameSpaceSet(classDefine, classDefine->className.c_str(), (PyObject *) type);
    };

    void* PyEngine::performGetNativeInstance(const Local<script::Value>& value,
                                   const internal::ClassDefineState* classDefine) {
        if (!performIsInstanceOf(value, classDefine)) {
            throw Exception("Unmatched type of the value!");
        }
        return GeneralObject::getInstance(value.val_);
    }

    bool PyEngine::performIsInstanceOf(const Local<script::Value>& value,
                             const internal::ClassDefineState* classDefine) {
        return registeredTypes_[classDefine] == value.val_->ob_type;
    }

    Local<Object> PyEngine::performNewNativeClass(internal::TypeIndex typeIndex,
                                        const internal::ClassDefineState* classDefine, size_t size,
                                        const Local<script::Value>* args) {
        PyObject *tuple = PyTuple_New(size);
        for (size_t i = 0; i < size; ++i) {
            Py_INCREF(args[i].val_);      // PyTuple_SetItem will steal the ref
            PyTuple_SetItem(tuple, i, args[i].val_);
        }

        PyTypeObject *type = registeredTypes_[classDefine];
        PyObject *obj = py_backend::newCustomInstance(type, tuple);
        Py_DECREF(tuple);
        return py_interop::asLocal<Object>(obj);
    }

}  // namespace script::py_backend

SCRIPTX_END_IGNORE_DEPRECARED
