#pragma once
#include "rcptr.h"
#include "wings.h"
#include "error.h"
#include "attributetable.h"
#include <string>
#include <vector>
#include <deque>
#include <unordered_set>
#include <unordered_map>
#include <memory>
#include <optional>
#include <cstdlib> // std::abort

namespace std {
    template <> struct hash<WObj> {
        size_t operator()(const WObj& obj) const;
    };
}

bool operator==(const WObj& lhs, const WObj& rhs);
bool operator!=(const WObj& lhs, const WObj& rhs);

struct WContext;

struct WObj {
    enum class Type {
        Null,
        Bool,
        Int,
        Float,
        String,
        Tuple,
        List,
        Map,
        Func,
        Object,
        Class,
        Userdata,
    } type = Type::Null;

    union {
        bool b;
        wint i;
        wfloat f;
        void* u;
        struct {
            WObj* self;
            WFunc fn;
        };
    };
    std::string s;
    std::vector<WObj*> v;
    std::unordered_map<WObj, WObj*> m;
    wings::AttributeTable c;

    std::string className;
    wings::AttributeTable attributes;
    WFinalizer finalizer{};
    std::vector<WObj*> references;
    WContext* context;
};

struct TraceFrame {
    wings::SourcePosition srcPos;
    std::string lineText;
    std::string module;
    std::string func;
};

struct WContext {
    WConfig config{};

    bool lockGc = false;
    size_t lastObjectCountAfterGC = 0;
    std::deque<std::unique_ptr<WObj>> mem;
    std::unordered_multiset<const WObj*> protectedObjects;
    std::unordered_map<std::string, wings::RcPtr<WObj*>> globals;


    struct {
        WError code{};
        std::string message;
        std::vector<TraceFrame> trace;
        std::string traceMessage;
    } err;

    struct {
        WObj* null;
        WObj* _bool;
        WObj* _int;
        WObj* _float;
        WObj* str;
        WObj* tuple;
        WObj* list;
        WObj* map;
        WObj* object;
        WObj* func;
        WObj* userdata;

        WObj* slice;
    } builtinClasses;
    WObj* nullSingleton;
};

namespace wings {
    size_t Guid();
    bool InitLibrary(WContext* context);
    std::string WObjTypeToString(WObj::Type t);
}

#define WASSERT(assertion) do { if (!(assertion)) std::abort(); } while (0)
#define WUNREACHABLE() std::abort()
