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

#pragma once

#include "../../src/foundation.h"
#include <string>
#include <mutex>

// docs:
// https://docs.python.org/3/c-api/index.html
// https://docs.python.org/3/extending/embedding.html
// https://docs.python.org/3.8/c-api/init.html#thread-state-and-the-global-interpreter-lock

SCRIPTX_BEGIN_INCLUDE_LIBRARY

#include <Python.h>
#include <frameobject.h>
#include <structmember.h>
#include "../../src/types.h"

SCRIPTX_END_INCLUDE_LIBRARY

#if PY_VERSION_HEX < 0x030a00f0
#error "python version must be greater than 3.10.0"
#endif

namespace script::py_backend {

    struct GeneralObject : PyObject {
        void *instance;
        PyObject *weakrefs;
        PyObject *instanceDict;

        static internal::ClassDefineState *getInstance(PyObject *self) {
            return reinterpret_cast<internal::ClassDefineState *>(reinterpret_cast<GeneralObject *>(self)->instance);
        }
    };

//
// - Locker Helper:
//   1. In CPython3.12, it will be changed to per sub-interpreter per GIL, it is great. But in 3.10 
//      now GIL is global, and we have to use our own lockers instead as GIL cannot be recursive 
//      locking.
//   2. This class is used for PyEngine and EngineScope to protect their process. It works like what
//      GIL does: Keeping at one time only one thread can get access to engines. But unlike GIL, 
//      recursive_mutex supports re-entry.
//   3. The locker named "engineLocker" is used to mutually exclude multi-threaded access to the same 
//      engine, just like what GIL does in the single-interpreter environment.
//   4. "allPyEnginesEnterCount" stores the number of all entered PyEngines to determine whether 
//      the GIL is needed to lock/unlock. If any engine is entered, GIL must be locked; after all 
//      engines are exited, GIL is need to be unlocked.
//   5. Read more docs about locker usage in "PyScope.cc"
//

    class PyEngine;

    class EngineLockerHelper {
    private:
        PyEngine *engine;
        inline static std::recursive_mutex engineLocker;
        inline static int allPyEnginesEnterCount = 0;

    public:
        explicit EngineLockerHelper(PyEngine *currentEngine);

        ~EngineLockerHelper();

        // May wait on lock. After this the GIL must be held.
        void waitToEnterEngine();

        void finishEngineSwitch();

        // May wait on lock.
        void waitToExitEngine();

        // After this the GIL maybe released.
        void finishExitEngine();

        // May wait on lock
        void startDestroyEngine();

        void endDestroyEngine();
    };

// key +1 value +1
    void setAttr(PyObject *obj, PyObject *key, PyObject *value);

// value +1
    void setAttr(PyObject *obj, const char *key, PyObject *value);

    PyObject *getAttr(PyObject *obj, PyObject *key);

    PyObject *getAttr(PyObject *obj, const char *key);

    bool hasAttr(PyObject *obj, PyObject *key);

    bool hasAttr(PyObject *obj, const char *key);

    void delAttr(PyObject *obj, PyObject *key);

    void delAttr(PyObject *obj, const char *key);

// key +1 value +1
    void setDictItem(PyObject *obj, PyObject *key, PyObject *value);

// value +1
    void setDictItem(PyObject *obj, const char *key, PyObject *value);

    PyObject *getDictItem(PyObject *obj, PyObject *key);

    PyObject *getDictItem(PyObject *obj, const char *key);

    PyObject *toStr(const char *s);

    PyObject *toStr(const std::string &s);

    std::string fromStr(PyObject *s);

    class PyEngine;

    PyObject *newCustomInstance(PyTypeObject *pType, PyObject *argsTuple, PyObject *kwds = nullptr);

    PyObject *newExceptionInstance(PyTypeObject *pType, PyObject *pValue, PyObject *pTraceback);

    PyObject *newExceptionInstance(std::string msg);

    void checkAndThrowException();

    bool checkAndClearException();

    PyObject *checkAndGetException();   // return new ref
    PyEngine *currentEngine();

    PyEngine *currentEngineChecked();

// @return borrowed ref
    PyObject *getGlobalMain();

// @return borrowed ref
    PyObject *getGlobalBuiltin();

    void extendLifeTimeToNextLoop(PyEngine *engine, PyObject *obj);
}  // namespace script::py_backend
