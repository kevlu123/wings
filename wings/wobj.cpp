#include "impl.h"
#include "gc.h"
#include "executor.h"
#include <algorithm>

using namespace wings;

extern "C" {

    WObj* WCreateNoneType(WContext* context) {
        WASSERT(context);
        return context->builtins.none;
    }

    WObj* WCreateBool(WContext* context, bool value) {
        WASSERT(context);
        if (WObj* v = WCall(context->builtins._bool, nullptr, 0)) {
            v->Get<bool>() = value;
            return v;
        } else {
            return nullptr;
        }
    }

    WObj* WCreateInt(WContext* context, wint value) {
        WASSERT(context);
        if (WObj* v = WCall(context->builtins._int, nullptr, 0)) {
            v->Get<wint>() = value;
            return v;
        } else {
            return nullptr;
        }
    }

    WObj* WCreateFloat(WContext* context, wfloat value) {
        WASSERT(context);
        if (WObj* v = WCall(context->builtins._float, nullptr, 0)) {
            v->Get<wfloat>() = value;
            return v;
        } else {
            return nullptr;
        }
    }

    WObj* WCreateString(WContext* context, const char* value) {
        WASSERT(context);
        if (WObj* v = WCall(context->builtins.str, nullptr, 0)) {
            v->Get<std::string>() = value;
            return v;
        } else {
            return nullptr;
        }
    }

    WObj* WCreateTuple(WContext* context, WObj** argv, int argc) {
        std::vector<WObjRef> refs;
        WASSERT(context && argc >= 0);
        if (argc > 0) {
            WASSERT(argv);
            for (int i = 0; i < argc; i++) {
                refs.emplace_back(argv[i]);
                WASSERT(argv[i]);
            }
        }

        if (WObj* v = WCall(context->builtins.tuple, nullptr, 0)) {
            v->Get<std::vector<WObj*>>() = std::vector<WObj*>(argv, argv + argc);
            return v;
        } else {
            return nullptr;
        }
    }

    WObj* WCreateList(WContext* context, WObj** argv, int argc) {
        std::vector<WObjRef> refs;
        WASSERT(context && argc >= 0);
        if (argc > 0) {
            WASSERT(argv);
            for (int i = 0; i < argc; i++) {
                refs.emplace_back(argv[i]);
                WASSERT(argv[i]);
            }
        }

        if (WObj* v = WCall(context->builtins.list, nullptr, 0)) {
            v->Get<std::vector<WObj*>>() = std::vector<WObj*>(argv, argv + argc);
            return v;
        } else {
            return nullptr;
        }
    }

    WObj* WCreateDictionary(WContext* context, WObj** keys, WObj** values, int argc) {
        std::vector<WObjRef> refs;
        WASSERT(context && argc >= 0);
        if (argc > 0) {
            WASSERT(keys && values);
            for (int i = 0; i < argc; i++) {
                refs.emplace_back(keys[i]);
                refs.emplace_back(values[i]);
                WASSERT(keys[i] && values[i] && WIsImmutableType(keys[i]));
            }
        }

        // Pass a dummy kwargs to prevent stack overflow from recursion
        WObj* dummyKwargs = Alloc(context);
        if (dummyKwargs == nullptr)
            return nullptr;
        dummyKwargs->type = "__map";
        wings::WDict wd{};
        dummyKwargs->data = &wd;

        if (WObj* v = WCall(context->builtins.dict, nullptr, 0, dummyKwargs)) {
            for (int i = 0; i < argc; i++)
                v->Get<wings::WDict>().insert({ keys[i], values[i] });
            return v;
        } else {
            return nullptr;
        }
    }

    WObj* WCreateFunction(WContext* context, const WFuncDesc* value) {
        WASSERT(context);
        if (WObj* v = WCall(context->builtins.func, nullptr, 0)) {
            v->Get<WObj::Func>() = {
                nullptr,
                value->fptr,
                value->userdata,
                value->isMethod,
                std::string(value->prettyName),
            };
            return v;
        } else {
            return nullptr;
        }
    }

    WObj* WCreateClass(WContext* context, const char* name, WObj** bases, int baseCount) {
        std::vector<WObjRef> refs;
        WASSERT(context && name && baseCount >= 0);
        if (baseCount > 0) {
            WASSERT(bases);
            for (int i = 0; i < baseCount; i++) {
                WASSERT(bases[i] && WIsClass(bases[i]));
                refs.emplace_back(bases[i]);
            }
        }

        // Allocate class
        WObj* _class = Alloc(context);
        if (_class == nullptr) {
            return nullptr;
        }
        refs.emplace_back(_class);
        _class->type = "__class";
        _class->data = new WObj::Class{ std::string(name) };
        _class->finalizer.fptr = [](WObj* obj, void*) { delete (WObj::Class*)obj->data; };
        _class->Get<WObj::Class>().instanceAttributes.Set("__class__", _class);
        _class->attributes.AddParent(context->builtins.object->Get<WObj::Class>().instanceAttributes);

        // Set bases
        int actualBaseCount = baseCount ? baseCount : 1;
        WObj** actualBases = baseCount ? bases : &context->builtins.object;
        for (int i = 0; i < actualBaseCount; i++) {
            _class->Get<WObj::Class>().instanceAttributes.AddParent(actualBases[i]->Get<WObj::Class>().instanceAttributes);
            _class->Get<WObj::Class>().bases.push_back(actualBases[i]);
        }
        if (WObj* basesTuple = WCreateTuple(context, actualBases, actualBaseCount)) {
            _class->attributes.Set("__bases__", basesTuple);
        } else {
            return nullptr;
        }

        // Set __str__()
        WFuncDesc tostr{};
        tostr.isMethod = true;
        tostr.prettyName = "__str__";
        tostr.userdata = context;
        tostr.fptr = [](WObj** argv, int argc, WObj* kwargs, void* ud) {
            //... Expect 1 arg
            std::string s = "<class '" + argv[0]->Get<WObj::Class>().name + "'>";
            return WCreateString(argv[0]->context, s.c_str());
        };
        if (WObj* tostrFn = WCreateFunction(context, &tostr)) {
            WSetAttribute(_class, "__str__", tostrFn);
        } else {
            return nullptr;
        }

        // Set construction function. This function forwards to __init__().
        _class->Get<WObj::Class>().userdata = _class;
        _class->Get<WObj::Class>().ctor = [](WObj** argv, int argc, WObj* kwargs, void* userdata) -> WObj* {
            WObj* _classObj = (WObj*)userdata;
            WContext* context = _classObj->context;

            WObj* instance = Alloc(context);
            if (instance == nullptr)
                return nullptr;
            WObjRef ref(instance);

            instance->attributes = _classObj->Get<WObj::Class>().instanceAttributes.Copy();
            instance->type = _classObj->Get<WObj::Class>().name;

            if (WObj* init = WGetAttribute(instance, "__init__")) {
                if (WIsFunction(init)) {
                    WObj* ret = WCall(init, argv, argc, kwargs);
                    if (ret == nullptr) {
                        return nullptr;
                    } else if (!WIsNoneType(ret)) {
                        WRaiseException(context, "__init__() returned a non NoneType type");
                        return nullptr;
                    }
                }
            }

            return instance;
        };

        // Set init method
        std::string initName = std::string(name) + ".__init__";
        WFuncDesc init{};
        init.prettyName = initName.c_str();
        init.isMethod = true;
        init.userdata = _class;
        init.fptr = [](WObj** argv, int argc, WObj* kwargs, void* userdata) -> WObj* {
            //... Expect >= 1 args
            WObj* _class = (WObj*)userdata;
            const auto& bases = _class->Get<WObj::Class>().bases;
            if (bases.empty())
                return nullptr;

            if (WObj* baseInit = WGetAttributeFromBase(argv[0], "__init__", bases[0])) {
                WObj* ret = WCall(baseInit, argv + 1, argc - 1, kwargs);
                if (ret == nullptr) {
                    return nullptr;
                } else if (!WIsNoneType(ret)) {
                    WRaiseException(argv[0]->context, "__init__() returned a non NoneType type");
                    return nullptr;
                }
            }

            return WCreateNoneType(argv[0]->context);
        };
        WObj* initFn = WCreateFunction(context, &init);
        if (initFn == nullptr)
            return nullptr;
        WLinkReference(initFn, _class);
        WAddAttributeToClass(_class, "__init__", initFn);

        return _class;
    }
    
    void WAddAttributeToClass(WObj* _class, const char* name, WObj* attribute) {
        WASSERT(_class && name && attribute && WIsClass(_class));
        _class->Get<WObj::Class>().instanceAttributes.Set(name, attribute);
    }

    bool WIsNoneType(const WObj* obj) {
        WASSERT(obj);
        return obj->type == "__null";
    }

    bool WIsBool(const WObj* obj) {
        WASSERT(obj);
        return obj->type == "__bool";
    }

    bool WIsInt(const WObj* obj) {
        WASSERT(obj);
        return obj->type == "__int";
    }

    bool WIsIntOrFloat(const WObj* obj) {
        WASSERT(obj);
        return obj->type == "__int" || obj->type == "__float";
    }

    bool WIsString(const WObj* obj) {
        WASSERT(obj);
        return obj->type == "__str";
    }

    bool WIsTuple(const WObj* obj) {
        WASSERT(obj);
        return obj->type == "__tuple";
    }

    bool WIsList(const WObj* obj) {
        WASSERT(obj);
        return obj->type == "__list";
    }

    bool WIsDictionary(const WObj* obj) {
        WASSERT(obj);
        return obj->type == "__map";
    }

    bool WIsClass(const WObj* obj) {
        WASSERT(obj);
        return obj->type == "__class";
    }

    bool WIsFunction(const WObj* obj) {
        WASSERT(obj);
        return obj->type == "__func";
    }

    bool WIsImmutableType(const WObj* obj) {
        WASSERT(obj);
        if (WIsTuple(obj)) {
            for (WObj* elem : obj->Get<std::vector<WObj*>>())
                if (!WIsImmutableType(elem))
                    return false;
            return true;
        } else {
            return WIsNoneType(obj)
                || WIsBool(obj)
                || WIsIntOrFloat(obj)
                || WIsString(obj);
        }
    }

    bool WGetBool(const WObj* obj) {
        WASSERT(obj && WIsBool(obj));
        return obj->Get<bool>();
    }

    wint WGetInt(const WObj* obj) {
        WASSERT(obj && WIsInt(obj));
        return obj->Get<wint>();
    }

    wfloat WGetFloat(const WObj* obj) {
        WASSERT(obj && WIsIntOrFloat(obj));
        if (WIsInt(obj)) return (wfloat)obj->Get<wint>();
        else return obj->Get<wfloat>();
    }

    const char* WGetString(const WObj* obj) {
        WASSERT(obj && WIsString(obj));
        return obj->Get<std::string>().c_str();
    }

    void WGetFunction(const WObj* obj, WFuncDesc* fn) {
        WASSERT(obj && fn && WIsFunction(obj));
        const auto& f = obj->Get<WObj::Func>();
        fn->fptr = f.fptr;
        fn->userdata = f.userdata;
        fn->isMethod = f.isMethod;
        fn->prettyName = f.prettyName.c_str();
    }

    void* WTryGetUserdata(const WObj* obj, const char* type) {
        if (obj->type == std::string(type)) {
            return obj->data;
        } else {
            return nullptr;
        }
    }

	void WGetFinalizer(const WObj* obj, WFinalizer* finalizer) {
		WASSERT(obj && finalizer);
		*finalizer = obj->finalizer;
	}

	void WSetFinalizer(WObj* obj, const WFinalizer* finalizer) {
		WASSERT(obj && finalizer);
		obj->finalizer = *finalizer;
	}

    WObj* WGetAttribute(WObj* obj, const char* member) {
        WASSERT(obj && member);
        WObj* mem = obj->attributes.Get(member);
        if (mem && WIsFunction(mem) && mem->Get<WObj::Func>().isMethod) {
            mem->Get<WObj::Func>().self = obj;
        }
        return mem;
    }

    void WSetAttribute(WObj* obj, const char* member, WObj* value) {
        WASSERT(obj && member && value);
        obj->attributes.Set(member, value);
    }

    WObj* WGetAttributeFromBase(WObj* obj, const char* member, WObj* baseClass) {
        WASSERT(obj && member);

        WObj* mem{};
        if (baseClass == nullptr) {
            mem = obj->attributes.GetFromBase(member);
        } else {
            mem = baseClass->Get<WObj::Class>().instanceAttributes.Get(member);
        }

        if (mem && WIsFunction(mem) && mem->Get<WObj::Func>().isMethod) {
            mem->Get<WObj::Func>().self = obj;
        }
        return mem;
    }

    bool WIsInstance(WObj* instance, WObj* type) {
        WASSERT(instance && type && WIsClass(type));
        WObj* argv[2] = { instance, type };
        WObj* res = WCall(instance->context->builtins.isinstance, argv, 2, nullptr);
        if (res == nullptr) {
            // If an exception occurred, report false anyway
            WClearCurrentException(instance->context);
            return false;
        } else {
            return WGetBool(res);
        }
    }

    bool WIterate(WObj* obj, void* userdata, bool(*callback)(WObj* value, void* userdata)) {
        WASSERT(obj && callback);
        WObj* iter = WCallMethod(obj, "__iter__", nullptr, 0);
        if (iter == nullptr) {
            WRaiseException(obj->context, "Object is not iterable (does not implement the __iter__ method)");
            return false;
        }
        WProtectObject(iter);

        while (true) {
            WObj* endReached = WCallMethod(iter, "__end__", nullptr, 0);
            if (endReached == nullptr) {
                WRaiseException(obj->context, "Iterator does not implement the __end__ method");
                WUnprotectObject(iter);
                return false;
            }

            WObj* truthy = WConvertToBool(endReached);
            if (truthy == nullptr) {
                WUnprotectObject(iter);
                return false;
            } else if (WGetBool(truthy)) {
                WUnprotectObject(iter);
                return true;
            }

            WObj* value = WCallMethod(iter, "__next__", nullptr, 0);
            if (endReached == nullptr) {
                WRaiseException(obj->context, "Iterator does not implement the __next__ method");
                WUnprotectObject(iter);
                return false;
            }

            WProtectObject(value);
            if (!callback(value, userdata)) {
                WUnprotectObject(value);
                WUnprotectObject(iter);
                return false;
            }
            WUnprotectObject(value);
        }
    }

    WObj* WConvertToBool(WObj* arg) {
        if (WObj* res = WCallMethod(arg, "__nonzero__", nullptr, 0)) {
            if (WIsBool(res)) {
                return res;
            } else {
                WRaiseException(arg->context, "__nonzero__() returned a non bool type");
            }
        }
        return nullptr;
    }

    WObj* WConvertToInt(WObj* arg) {
        if (WObj* res = WCallMethod(arg, "__int__", nullptr, 0)) {
            if (WIsInt(res)) {
                return res;
            } else {
                WRaiseException(arg->context, "__int__() returned a non int type");
            }
        }
        return nullptr;
    }

    WObj* WConvertToFloat(WObj* arg) {
        if (WObj* res = WCallMethod(arg, "__float__", nullptr, 0)) {
            if (WIsIntOrFloat(res)) {
                return res;
            } else {
                WRaiseException(arg->context, "__float__() returned a non float type");
            }
        }
        return nullptr;
    }

    WObj* WConvertToString(WObj* arg) {
        if (WObj* res = WCallMethod(arg, "__str__", nullptr, 0)) {
            if (WIsString(res)) {
                return res;
            } else {
                WRaiseException(arg->context, "__str__() returned a non str type");
            }
        }
        return nullptr;
    }

    WObj* WCall(WObj* callable, WObj** argv, int argc, WObj* kwargsDict) {
        WASSERT(callable && argc >= 0 && (argc == 0 || argv));
        if (WIsFunction(callable) || WIsClass(callable)) {
            if (argc)
                WASSERT(argv);
            for (int i = 0; i < argc; i++)
                WASSERT(argv[i]);

            if (kwargsDict) {
                if (!WIsDictionary(kwargsDict)) {
                    WRaiseException(kwargsDict->context, "Keyword arguments must be a dictionary");
                    return nullptr;
                }
                for (const auto& [key, value] : kwargsDict->Get<wings::WDict>()) {
                    if (!WIsString(key)) {
                        WRaiseException(kwargsDict->context, "Keyword arguments dictionary must only contain string keys");
                        return nullptr;
                    }
                }
            }

            std::vector<WObjRef> refs;
            refs.emplace_back(callable);
            for (int i = 0; i < argc; i++)
                refs.emplace_back(argv[i]);

            WObj* (*fptr)(WObj**, int, WObj*, void*);
            void* userdata = nullptr;
            WObj* self = nullptr;
            std::string prettyName;
            if (WIsFunction(callable)) {
                const auto& func = callable->Get<WObj::Func>();
                if (func.self)
                    self = func.self;
                fptr = func.fptr;
                userdata = func.userdata;
                prettyName = func.prettyName;
            } else {
                fptr = callable->Get<WObj::Class>().ctor;
                userdata = callable->Get<WObj::Class>().userdata;
            }

            std::vector<WObj*> argsWithSelf;
            if (self) {
                argsWithSelf.push_back(self);
                refs.emplace_back(self);
            }
            argsWithSelf.insert(argsWithSelf.end(), argv, argv + argc);

            // If the dictionary (map) class doesn't exist yet then skip this
            if (kwargsDict == nullptr && callable != callable->context->builtins.dict && callable->context->builtins.dict) {
                kwargsDict = WCreateDictionary(callable->context);
                if (kwargsDict == nullptr)
                    return nullptr;
            }
            refs.emplace_back(kwargsDict);

            WObj* ret = fptr(argsWithSelf.data(), (int)argsWithSelf.size(), kwargsDict, userdata);

            if (ret == nullptr && fptr != &DefObject::Run) {
                // Native function failed
                callable->context->trace.push_back({
                    {},
                    "",
                    "<Native>",
                    prettyName
                    });
            }

            return ret;
        } else {
            return WCallMethod(callable, "__call__", argv, argc);
        }
    }

    WObj* WCallMethod(WObj* obj, const char* member, WObj** argv, int argc, WObj* kwargsDict) {
        WASSERT(obj && member);
        if (argc)
            WASSERT(argv);
        for (int i = 0; i < argc; i++)
            WASSERT(argv[i]);

        WObj* method = WGetAttribute(obj, member);
        if (method == nullptr) {
            std::string msg = "Object of type " +
                WObjTypeToString(obj) +
                " has no attribute " +
                member;
            WRaiseException(obj->context, msg.c_str());
            return nullptr;
        } else {
            return WCall(method, argv, argc, kwargsDict);
        }
    }

    WObj* WCallMethodFromBase(WObj* obj, const char* member, WObj** argv, int argc, WObj* kwargsDict, WObj* baseClass) {
        WASSERT(obj && member);
        if (argc)
            WASSERT(argv);
        for (int i = 0; i < argc; i++)
            WASSERT(argv[i]);

        WObj* method = WGetAttributeFromBase(obj, member, baseClass);
        if (method == nullptr) {
            std::string msg = "Object of type " +
                WObjTypeToString(obj) +
                " has no attribute " +
                member;
            WRaiseException(obj->context, msg.c_str());
            return nullptr;
        } else {
            return WCall(method, argv, argc, kwargsDict);
        }
    }

    WObj* WGetIndex(WObj* obj, WObj* index) {
        return WCallMethod(obj, "__getitem__", &index, 1);
    }

    WObj* WSetIndex(WObj* obj, WObj* index, WObj* value) {
        WObj* argv[2] = { index, value };
        return WCallMethod(obj, "__setitem__", argv, 2);
    }

    WObj* WPositive(WObj* arg) {
        return WCallMethod(arg, "__pos__", nullptr, 0);
    }

    WObj* WNegative(WObj* arg) {
        return WCallMethod(arg, "__neg__", nullptr, 0);
    }

    WObj* WAdd(WObj* lhs, WObj* rhs) {
        return WCallMethod(lhs, "__add__", &rhs, 1);
    }

    WObj* WSubtract(WObj* lhs, WObj* rhs) {
        return WCallMethod(lhs, "__sub__", &rhs, 1);
    }

    WObj* WMultiply(WObj* lhs, WObj* rhs) {
        return WCallMethod(lhs, "__mul__", &rhs, 1);
    }

    WObj* WDivide(WObj* lhs, WObj* rhs) {
        return WCallMethod(lhs, "__truediv__", &rhs, 1);
    }

    WObj* WFloorDivide(WObj* lhs, WObj* rhs) {
        return WCallMethod(lhs, "__floordiv__", &rhs, 1);
    }

    WObj* WModulo(WObj* lhs, WObj* rhs) {
        return WCallMethod(lhs, "__mod__", &rhs, 1);
    }

    WObj* WPower(WObj* lhs, WObj* rhs) {
        return WCallMethod(lhs, "__pow__", &rhs, 1);
    }

    WObj* WEquals(WObj* lhs, WObj* rhs) {
        if (WObj* res = WCallMethod(lhs, "__eq__", &rhs, 1)) {
            if (WIsBool(res)) {
                return res;
            } else {
                WRaiseException(lhs->context, "__eq__() returned a non bool type");
            }
        }
        return nullptr;
    }

    WObj* WNotEquals(WObj* lhs, WObj* rhs) {
        if (WObj* res = WCallMethod(lhs, "__ne__", &rhs, 1)) {
            if (WIsBool(res)) {
                return res;
            } else {
                WRaiseException(lhs->context, "__ne__() returned a non bool type");
            }
        }
        return nullptr;
    }

    WObj* WLessThan(WObj* lhs, WObj* rhs) {
        if (WObj* res = WCallMethod(lhs, "__lt__", &rhs, 1)) {
            if (WIsBool(res)) {
                return res;
            } else {
                WRaiseException(lhs->context, "__lt__() returned a non bool type");
            }
        }
        return nullptr;
    }

    WObj* WLessThanOrEqual(WObj* lhs, WObj* rhs) {
        if (WObj* res = WCallMethod(lhs, "__le__", &rhs, 1)) {
            if (WIsBool(res)) {
                return res;
            } else {
                WRaiseException(lhs->context, "__le__() returned a non bool type");
            }
        }
        return nullptr;
    }

    WObj* WGreaterThan(WObj* lhs, WObj* rhs) {
        if (WObj* res = WCallMethod(lhs, "__gt__", &rhs, 1)) {
            if (WIsBool(res)) {
                return res;
            } else {
                WRaiseException(lhs->context, "__gt__() returned a non bool type");
            }
        }
        return nullptr;
    }

    WObj* WGreaterThanOrEqual(WObj* lhs, WObj* rhs) {
        if (WObj* res = WCallMethod(lhs, "__ge__", &rhs, 1)) {
            if (WIsBool(res)) {
                return res;
            } else {
                WRaiseException(lhs->context, "__ge__() returned a non bool type");
            }
        }
        return nullptr;
    }

    WObj* WIn(WObj* container, WObj* obj) {
        if (WObj* res = WCallMethod(container, "__contains__", &obj, 1)) {
            if (WIsBool(res)) {
                return res;
            } else {
                WRaiseException(container->context, "__contains__() returned a non bool type");
            }
        }
        return nullptr;
    }

    WObj* WNotIn(WObj* container, WObj* obj) {
        if (WObj* inOp = WIn(container, obj)) {
            return WCreateBool(container->context, !WGetBool(inOp));
        } else {
            return nullptr;
        }
    }

    WObj* WBitAnd(WObj* lhs, WObj* rhs) {
        return WCallMethod(lhs, "__and__", &rhs, 1);
    }

    WObj* WBitOr(WObj* lhs, WObj* rhs) {
        return WCallMethod(lhs, "__or__", &rhs, 1);
    }

    WObj* WBitNot(WObj* arg) {
        return WCallMethod(arg, "__invert__", nullptr, 0);
    }

    WObj* WBitXor(WObj* lhs, WObj* rhs) {
        return WCallMethod(lhs, "__xor__", &rhs, 1);
    }

    WObj* WShiftLeft(WObj* lhs, WObj* rhs) {
        return WCallMethod(lhs, "__lshift__", &rhs, 1);
    }

    WObj* WShiftRight(WObj* lhs, WObj* rhs) {
        return WCallMethod(lhs, "__rshift__", &rhs, 1);
    }

} // extern "C"
