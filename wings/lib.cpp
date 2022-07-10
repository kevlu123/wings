#include "impl.h"
#include "gc.h"
#include <iostream>
#include <sstream>
#include <unordered_set>
#include <cmath>

using namespace wings;

static std::string PtrToString(const void* p) {
	std::stringstream ss;
	ss << p;
	return ss.str();
}

static std::string WObjToString(const WObj* val, std::unordered_set<const WObj*>& seen) {
	switch (val->type) {
	case WObj::Type::Null:
		return "None";
	case WObj::Type::Bool:
		return val->b ? "True" : "False";
	case WObj::Type::Int:
		return std::to_string(val->i);
	case WObj::Type::Float: {
		std::string s = std::to_string(val->f);
		s.erase(s.find_last_not_of('0') + 1, std::string::npos);
		if (s.ends_with('.'))
			s.push_back('0');
		return s;
	}
	case WObj::Type::String:
		return val->s;
	case WObj::Type::Func:
		return "<function at " + PtrToString(val) + ">";
	case WObj::Type::Userdata:
		return "<userdata at " + PtrToString(val) + ">";
	case WObj::Type::Object:
		return "<object at " + PtrToString(val) + ">";
	case WObj::Type::Class:
		return "<class at " + PtrToString(val) + ">";
	case WObj::Type::Tuple:
	case WObj::Type::List: {
		bool isTuple = val->type == WObj::Type::Tuple;
		if (seen.contains(val)) {
			return isTuple ? "(...)" : "[...]";
		} else {
			seen.insert(val);
			std::string s(1, isTuple ? '(' : '[');
			for (WObj* child : val->v) {
				s += WObjToString(child, seen) + ", ";
			}
			if (!val->v.empty()) {
				s.pop_back();
				s.pop_back();
			}
			if (isTuple && val->v.size() == 1)
				s.push_back(',');
			return s + (isTuple ? ')' : ']');
		}
	}
	case WObj::Type::Map:
		if (seen.contains(val)) {
			return "{...}";
		} else {
			seen.insert(val);
			std::string s = "{";
			for (const auto& [key, val] : val->m) {
				s += WObjToString(&key, seen) + ": ";
				s += WObjToString(val, seen) + ", ";
			}
			if (!val->m.empty()) {
				s.pop_back();
				s.pop_back();
			}
			return s + "}";
		}
	default:
		WUNREACHABLE();
	}
}

static std::string WObjToString(const WObj* val) {
	std::unordered_set<const WObj*> seen;
	return WObjToString(val, seen);
}

static size_t ConvertNegativeIndex(wint index, size_t size) {
	if (index < 0) {
		return size + index;
	} else {
		return index;
	}
}

static void SetInvalidArgumentCountError(WContext* context, int given, int expected = -1) {
	std::string msg;
	if (expected != -1) {
		msg = "Function takes " +
			std::to_string(expected) +
			" argument(s) but " +
			std::to_string(given) +
			(given == 1 ? " was given" : " were given");
	} else {
		msg = "function does not take " +
			std::to_string(given) +
			" argument(s)";
	}
	WRaiseError(context, msg.c_str());
}

static void SetArgumentError(WContext* context, size_t paramIndex, const std::string& message) {
	std::string msg = "Argument " + std::to_string(paramIndex + 1) + " " + message;
	WRaiseError(context, msg.c_str());
}

static void SetInvalidTypeError(WContext* context, WObj**argv, size_t paramIndex, const std::string& expectedType) {
	SetArgumentError(
		context,
		paramIndex,
		"expected type " + expectedType +
		" but got " + WObjTypeToString(argv[paramIndex]->type)
	);
}

static void SetMissingAttributeError(WContext* context, WObj** argv, size_t paramIndex, const std::string& attribute) {
	SetArgumentError(
		context,
		paramIndex,
		" of type " + WObjTypeToString(argv[paramIndex]->type) +
		" has no attribute " + attribute
	);
}

static void SetIndexOutOfRangeError(WContext* context, WObj** argv, size_t paramIndex) {
	SetArgumentError(
		context,
		paramIndex,
		"index out of range"
	);
}

static void SetDivisionByZeroError(WContext* context) {
	WRaiseError(context, "Division by zero");
}

#define EXPECT_ARG_COUNT(n) do if (argc != n) { SetInvalidArgumentCountError(context, argc, n); return nullptr; } while (0)
#define EXPECT_ARG_COUNT_BETWEEN(min, max) do if (argc < min && argc > max) { SetInvalidArgumentCountError(context, argc); return nullptr; } while (0)
#define EXPECT_ARG_TYPE(index, check, expect) do if (!check(argv[index])) { SetInvalidTypeError(context, argv, index, expect); return nullptr; } while (0)
#define EXPECT_ARG_TYPE_NULL(index) EXPECT_ARG_TYPE(index, WIsNoneType, "NoneType");
#define EXPECT_ARG_TYPE_BOOL(index) EXPECT_ARG_TYPE(index, WIsBool, "bool");
#define EXPECT_ARG_TYPE_INT(index) EXPECT_ARG_TYPE(index, WIsInt, "int");
#define EXPECT_ARG_TYPE_INT_OR_FLOAT(index) EXPECT_ARG_TYPE(index, WIsIntOrFloat, "int or float");
#define EXPECT_ARG_TYPE_STRING(index) EXPECT_ARG_TYPE(index, WIsString, "str");
#define EXPECT_ARG_TYPE_LIST(index) EXPECT_ARG_TYPE(index, WIsList, "list");
#define EXPECT_ARG_TYPE_TUPLE(index) EXPECT_ARG_TYPE(index, WIsTuple, "tuple");
#define GET_LIST_INDEX(list, index)														\
size_t listIndex = ConvertNegativeIndex(WGetInt(argv[index]), argv[list]->v.size());	\
if (listIndex >= argv[list]->v.size()) {												\
	SetIndexOutOfRangeError(context, argv, index);										\
	return nullptr;																		\
}


using WFuncSignature = WObj * (*)(WObj**, int, WContext*);

template <WFuncSignature fn>
static WObj* RegisterStatelessFunction(WContext* context, const char* name) {
	WFunc wfn{};
	wfn.userdata = context;
	wfn.fptr = [](WObj** argv, int argc, void* userdata) { return fn(argv, argc, (WContext*)userdata); };
	wfn.isMethod = false;
	wfn.prettyName = name;

	WObj* obj = WCreateFunction(context, &wfn);
	WSetGlobal(context, name, obj);
	return obj;
}

template <WFuncSignature fn>
static WObj* RegisterStatelessMethod(WContext* context, AttributeTable& attributeTable, const char* attribute, const char* prettyName) {
	WFunc wfn{};
	wfn.userdata = context;
	wfn.fptr = [](WObj** argv, int argc, void* userdata) { return fn(argv, argc, (WContext*)userdata); };
	wfn.isMethod = true;
	wfn.prettyName = prettyName;

	WObj* obj = WCreateFunction(context, &wfn);
	attributeTable.Set(attribute, obj, false);
	return obj;
}

template <WFuncSignature Constructor>
static WObj* CreateClass(WContext* context, const char* name = nullptr) {
	WObj* _class = Alloc(context);
	if (_class == nullptr) {
		return nullptr;
	}

	WFunc constructor{};
	constructor.userdata = _class;
	constructor.isMethod = true;
	constructor.prettyName = name;

	constructor.fptr = [](WObj** argv, int argc, void* userdata) {
		WObj* _class = (WObj*)userdata;
		WObj* instance = Constructor(argv, argc, _class->context);
		if (instance == nullptr)
			return (WObj*)nullptr;

		if (instance->attributes.Empty()) {
			// Just created a fresh instance
			instance->attributes = _class->c.Copy();
		}

		return instance;
	};

	_class->type = WObj::Type::Class;
	_class->fn = constructor;
	_class->c.Set("__class__", _class);

	if (name) {
		WSetGlobal(context, name, _class);
	}

	return _class;
}

namespace wings {

	namespace classlib {

		static WObj* Null(WObj** argv, int argc, WContext* context) {
			// Not callable from user code
			WASSERT(argc == 0);

			WObj* obj = Alloc(context);
			if (obj == nullptr)
				return nullptr;
			obj->type = WObj::Type::Null;
			return obj;
		}

		static WObj* Bool(WObj** argv, int argc, WContext* context) {
			switch (argc) {
			case 0:
				if (WObj* obj = Alloc(context)) {
					obj->type = WObj::Type::Bool;
					obj->b = false;
					return obj;
				} else {
					return nullptr;
				}
			case 1:
				if (WObj* res = WTruthy(argv[0])) {
					if (WIsBool(res)) {
						return res;
					} else {
						SetArgumentError(context, 1, "__nonzero__() method returned a non bool type");
					}
				}
				return nullptr;
			default:
				SetInvalidArgumentCountError(context, argc);
				return nullptr;
			}
		}

		static WObj* Int(WObj** argv, int argc, WContext* context) {
			switch (argc) {
			case 0:
				if (WObj* obj = Alloc(context)) {
					obj->type = WObj::Type::Int;
					obj->i = 0;
					return obj;
				} else {
					return nullptr;
				}
			case 1:
				return WConvertToInt(argv[0]);
			default:
				SetInvalidArgumentCountError(context, argc);
				return nullptr;
			}
		}

		static WObj* Float(WObj** argv, int argc, WContext* context) {
			switch (argc) {
			case 0:
				if (WObj* obj = Alloc(context)) {
					obj->type = WObj::Type::Float;
					obj->f = 0;
					return obj;
				} else {
					return nullptr;
				}
			case 1:
				return WConvertToFloat(argv[0]);
			default:
				SetInvalidArgumentCountError(context, argc);
				return nullptr;
			}
		}

		static WObj* Str(WObj** argv, int argc, WContext* context) {
			switch (argc) {
			case 0:
				if (WObj* obj = Alloc(context)) {
					obj->type = WObj::Type::String;
					return obj;
				} else {
					return nullptr;
				}
			case 1:
				return WConvertToString(argv[0]);
			default:
				SetInvalidArgumentCountError(context, argc);
				return nullptr;
			}
		}

		static WObj* Tuple(WObj** argv, int argc, WContext* context) {
			if (argc > 1) {
				SetInvalidArgumentCountError(context, argc, 1);
				return nullptr;
			}

			WObj* tuple = Alloc(context);
			if (tuple == nullptr)
				return nullptr;
			tuple->type = WObj::Type::Tuple;

			if (argc == 1) {
				auto f = [](WObj* value, void* list) {
					((WObj*)list)->v.push_back(value);
					return true;
				};

				WProtectObject(tuple);
				bool success = WIterate(argv[0], tuple, f);
				WUnprotectObject(tuple);
				if (!success) {
					return nullptr;
				}
			}

			return tuple;
		}

		static WObj* List(WObj** argv, int argc, WContext* context) {
			if (argc > 1) {
				SetInvalidArgumentCountError(context, argc, 1);
				return nullptr;
			}

			WObj* list = Alloc(context);
			if (list == nullptr)
				return nullptr;
			list->type = WObj::Type::List;

			if (argc == 1) {
				auto f = [](WObj* value, void* list) {
					((WObj*)list)->v.push_back(value);
					return true;
				};

				WProtectObject(list);
				bool success = WIterate(argv[0], list, f);
				WUnprotectObject(list);
				if (!success) {
					return nullptr;
				}
			}

			return list;
		}

		static WObj* Map(WObj** argv, int argc, WContext* context) {
			// TODO: validate params

			WObj* obj = Alloc(context);
			if (obj == nullptr)
				return nullptr;
			obj->type = WObj::Type::Map;
			return obj;
		}

		static WObj* Func(WObj** argv, int argc, WContext* context) {
			// Not callable from user code
			WObj* obj = Alloc(context);
			if (obj == nullptr)
				return nullptr;
			obj->type = WObj::Type::Func;
			return obj;
		}

		static WObj* Object(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(0);
			WObj* obj = Alloc(context);
			if (obj == nullptr)
				return nullptr;
			obj->type = WObj::Type::Object;
			return obj;
		}

		static WObj* Userdata(WObj** argv, int argc, WContext* context) {
			// Not callable from user code
			WObj* obj = Alloc(context);
			if (obj == nullptr)
				return nullptr;
			obj->type = WObj::Type::Userdata;
			return obj;
		}

	}

	namespace attrlib {

		static WObj* Object_Pos(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(1);
			return argv[0];
		}

		static WObj* Object_Str(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(1);
			std::string s = WObjToString(argv[0]);
			return WCreateString(context, s.c_str());
		}

		static WObj* Object_Eq(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			return WCreateBool(context, argv[0] == argv[1]);
		}

		static WObj* Object_Ne(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			if (WObj* eq = WEquals(argv[0], argv[1])) {
				return WCreateBool(context, !WGetBool(eq));
			} else {
				return nullptr;
			}
		}

		static WObj* Null_Bool(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(1);
			EXPECT_ARG_TYPE_NULL(0);
			return WCreateNoneType(context);
		}

		static WObj* Null_Eq(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_NULL(0);
			return WCreateBool(context, WIsNoneType(argv[1]));
		}

		static WObj* Bool_Bool(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(1);
			EXPECT_ARG_TYPE_BOOL(0);
			return argv[0];
		}

		static WObj* Bool_Int(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(1);
			EXPECT_ARG_TYPE_BOOL(0);
			return WCreateInt(context, WGetBool(argv[0]) ? 1 : 0);
		}

		static WObj* Bool_Float(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(1);
			EXPECT_ARG_TYPE_BOOL(0);
			return WCreateFloat(context, WGetBool(argv[0]) ? (wfloat)1 : (wfloat)0);
		}

		static WObj* Bool_Eq(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_BOOL(0);
			return WCreateBool(context, WIsBool(argv[1]) && WGetBool(argv[0]) == WGetBool(argv[1]));
		}

		static WObj* Int_Bool(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(1);
			EXPECT_ARG_TYPE_INT(0);
			return WCreateBool(context, WGetInt(argv[0]) != 0);
		}

		static WObj* Int_Int(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(1);
			EXPECT_ARG_TYPE_INT(0);
			return argv[0];
		}

		static WObj* Int_Float(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(1);
			EXPECT_ARG_TYPE_INT(0);
			return WCreateFloat(context, WGetFloat(argv[0]));
		}

		static WObj* Int_Eq(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT(0);
			return WCreateBool(context, WIsInt(argv[1]) && WGetInt(argv[0]) == WGetInt(argv[1]));
		}

		static WObj* Int_Gt(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT(0);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(1);
			return WCreateBool(context, WGetFloat(argv[0]) > WGetFloat(argv[1]));
		}

		static WObj* Int_Ge(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT(0);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(1);
			return WCreateBool(context, WGetFloat(argv[0]) >= WGetFloat(argv[1]));
		}

		static WObj* Int_Lt(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT(0);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(1);
			return WCreateBool(context, WGetFloat(argv[0]) < WGetFloat(argv[1]));
		}

		static WObj* Int_Le(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT(0);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(1);
			return WCreateBool(context, WGetFloat(argv[0]) <= WGetFloat(argv[1]));
		}

		static WObj* Int_Neg(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(1);
			EXPECT_ARG_TYPE_INT(0);
			return WCreateInt(context, -WGetInt(argv[0]));
		}

		static WObj* Int_Add(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT(0);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(1);
			if (WIsInt(argv[1])) {
				return WCreateInt(context, WGetInt(argv[0]) + WGetInt(argv[1]));
			} else {
				return WCreateFloat(context, WGetFloat(argv[0]) + WGetFloat(argv[1]));
			}
		}

		static WObj* Int_Sub(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT(0);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(1);
			if (WIsInt(argv[1])) {
				return WCreateInt(context, WGetInt(argv[0]) - WGetInt(argv[1]));
			} else {
				return WCreateFloat(context, WGetFloat(argv[0]) - WGetFloat(argv[1]));
			}
		}

		static WObj* Int_Mul(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT(0);

			if (WIsString(argv[1])) {
				wint multiplier = WGetInt(argv[0]);
				std::string s;
				for (wint i = 0; i < multiplier; i++)
					s += WGetString(argv[1]);
				return WCreateString(context, s.c_str());
			} else if (WIsInt(argv[1])) {
				return WCreateInt(context, WGetInt(argv[0]) * WGetInt(argv[1]));
			} else if (WIsIntOrFloat(argv[1])) {
				return WCreateFloat(context, WGetFloat(argv[0]) * WGetFloat(argv[1]));
			} else {
				EXPECT_ARG_TYPE_INT_OR_FLOAT(1);
				return nullptr;
			}
		}

		static WObj* Int_Div(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT(0);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(0);

			if (WGetFloat(argv[1]) == 0) {
				SetDivisionByZeroError(context);
				return nullptr;
			}
			return WCreateFloat(context, WGetFloat(argv[0]) / WGetFloat(argv[1]));
		}

		static WObj* Int_FloorDiv(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT(0);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(1);

			if (WGetFloat(argv[1]) == 0) {
				SetDivisionByZeroError(context);
				return nullptr;
			}

			if (WIsInt(argv[1])) {
				return WCreateInt(context, (wint)std::floor(WGetFloat(argv[0]) / WGetFloat(argv[1])));
			} else {
				return WCreateFloat(context, std::floor(WGetFloat(argv[0]) / WGetFloat(argv[1])));
			}
		}

		static WObj* Int_Mod(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT(0);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(1);

			if (WGetFloat(argv[1]) == 0) {
				SetDivisionByZeroError(context);
				return nullptr;
			}

			if (WIsInt(argv[1])) {
				wint mod = WGetInt(argv[1]);
				wint m = WGetInt(argv[0]) % mod;
				if (m < 0)
					m += mod;
				return WCreateInt(context, m);
			} else {
				return WCreateFloat(context, std::fmod(WGetFloat(argv[0]), WGetFloat(argv[1])));
			}
		}

		static WObj* Int_Pow(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT(0);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(1);
			return WCreateFloat(context, std::pow(WGetFloat(argv[0]), WGetFloat(argv[1])));
		}

		static WObj* Int_BitAnd(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT(0);
			EXPECT_ARG_TYPE_INT(1);
			return WCreateInt(context, WGetInt(argv[0]) & WGetInt(argv[1]));
		}

		static WObj* Int_BitOr(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT(0);
			EXPECT_ARG_TYPE_INT(1);
			return WCreateInt(context, WGetInt(argv[0]) | WGetInt(argv[1]));
		}

		static WObj* Int_BitXor(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT(0);
			EXPECT_ARG_TYPE_INT(1);
			return WCreateInt(context, WGetInt(argv[0]) ^ WGetInt(argv[1]));
		}

		static WObj* Int_BitNot(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(1);
			EXPECT_ARG_TYPE_INT(0);
			return WCreateInt(context, ~WGetInt(argv[0]));
		}

		static WObj* Int_ShiftLeft(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT(0);
			EXPECT_ARG_TYPE_INT(1);

			wint shift = WGetInt(argv[1]);
			if (shift < 0) {
				WRaiseError(context, "Shift cannot be negative");
				return nullptr;
			}
			shift = std::min(shift, (wint)sizeof(wint) * 8);
			return WCreateInt(context, WGetInt(argv[0]) << shift);
		}

		static WObj* Int_ShiftRight(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT(0);
			EXPECT_ARG_TYPE_INT(1);

			wint shift = WGetInt(argv[1]);
			if (shift < 0) {
				WRaiseError(context, "Shift cannot be negative");
				return nullptr;
			}
			shift = std::min(shift, (wint)sizeof(wint) * 8);
			return WCreateInt(context, WGetInt(argv[0]) >> shift);
		}

		static WObj* Float_Bool(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(1);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(0);
			return WCreateBool(context, WGetFloat(argv[0]) != 0);
		}

		static WObj* Float_Int(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(1);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(0);
			return WCreateInt(context, (wint)WGetFloat(argv[0]));
		}

		static WObj* Float_Float(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(1);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(0);
			return argv[0];
		}

		static WObj* Float_Eq(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(0);
			return WCreateBool(context, WIsIntOrFloat(argv[1]) && WGetFloat(argv[0]) == WGetFloat(argv[1]));
		}

		static WObj* Float_Neg(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(1);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(0);
			return WCreateFloat(context, -WGetFloat(argv[0]));
		}

		static WObj* Float_Add(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(0);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(1);
			return WCreateFloat(context, WGetFloat(argv[0]) + WGetFloat(argv[1]));
		}

		static WObj* Float_Sub(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(0);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(1);
			return WCreateFloat(context, WGetFloat(argv[0]) - WGetFloat(argv[1]));
		}

		static WObj* Float_Mul(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(0);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(1);
			return WCreateFloat(context, WGetFloat(argv[0]) * WGetFloat(argv[1]));
		}

		static WObj* Float_Div(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(0);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(1);
			return WCreateFloat(context, WGetFloat(argv[0]) / WGetFloat(argv[1]));
		}

		static WObj* Float_FloorDiv(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(0);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(1);
			return WCreateFloat(context, std::floor(WGetFloat(argv[0]) / WGetFloat(argv[1])));
		}

		static WObj* Float_Mod(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(0);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(1);
			return WCreateFloat(context, std::fmod(WGetFloat(argv[0]), WGetFloat(argv[1])));
		}

		static WObj* Float_Pow(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(0);
			EXPECT_ARG_TYPE_INT_OR_FLOAT(1);
			return WCreateFloat(context, std::pow(WGetFloat(argv[0]), WGetFloat(argv[1])));
		}

		static WObj* Str_Bool(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(1);
			EXPECT_ARG_TYPE_STRING(0);
			std::string s = WGetString(argv[0]);
			return WCreateBool(context, !s.empty());
		}

		static WObj* Str_Int(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(1);
			EXPECT_ARG_TYPE_STRING(0);

			auto isDigit = [](char c, int base = 10) {
				switch (base) {
				case 2: return c >= '0' && c <= '1';
				case 8: return c >= '0' && c <= '7';
				case 10: return c >= '0' && c <= '9';
				case 16: return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
				default: WUNREACHABLE();
				}
			};

			auto digitValueOf = [](char c, int base) {
				switch (base) {
				case 2:
				case 8:
				case 10:
					return c - '0';
				case 16:
					if (c >= '0' && c <= '9') {
						return c - '0';
					} else if (c >= 'a' && c <= 'f') {
						return c - 'a' + 10;
					} else {
						return c - 'A' + 10;
					}
				default:
					WUNREACHABLE();
				}
			};

			std::string s = WGetString(argv[0]);
			const char* p = s.c_str();

			int base = 10;
			if (*p == '0') {
				switch (p[1]) {
				case 'b': case 'B': base = 2; break;
				case 'x': case 'X': base = 16; break;
				default: base = 8; break;
				}
			}

			if (base == 2 || base == 16) {
				p += 2;

				if (!isDigit(*p, base)) {
					if (base == 2) {
						WRaiseError(context, "Invalid binary string");
					} else {
						WRaiseError(context, "Invalid hexadecimal string");
					}
					return nullptr;
				}
			}

			uintmax_t value = 0;
			for (; *p && isDigit(*p, base); ++p) {
				value = (base * value) + digitValueOf(*p, base);
			}

			if (value > std::numeric_limits<wuint>::max()) {
				WRaiseError(context, "Integer string is too large");
				return nullptr;
			}

			if (*p) {
				WRaiseError(context, "Invalid integer string");
				return nullptr;
			}

			return WCreateInt(context, (wint)value);
		}

		static WObj* Str_Float(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(1);
			EXPECT_ARG_TYPE_STRING(0);

			auto isDigit = [](char c, int base = 10) {
				switch (base) {
				case 2: return c >= '0' && c <= '1';
				case 8: return c >= '0' && c <= '7';
				case 10: return c >= '0' && c <= '9';
				case 16: return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
				default: WUNREACHABLE();
				}
			};

			auto digitValueOf = [](char c, int base) {
				switch (base) {
				case 2:
				case 8:
				case 10:
					return c - '0';
				case 16:
					if (c >= '0' && c <= '9') {
						return c - '0';
					} else if (c >= 'a' && c <= 'f') {
						return c - 'a' + 10;
					} else {
						return c - 'A' + 10;
					}
				default:
					WUNREACHABLE();
				}
			};

			std::string s = WGetString(argv[0]);
			const char* p = s.c_str();

			int base = 10;
			if (*p == '0') {
				switch (p[1]) {
				case 'b': case 'B': base = 2; break;
				case 'x': case 'X': base = 16; break;
				case '.': break;
				default: base = 8; break;
				}
			}

			if (base == 2 || base == 16) {
				p += 2;

				if (!isDigit(*p, base) && *p != '.') {
					if (base == 2) {
						WRaiseError(context, "Invalid binary string");
					} else {
						WRaiseError(context, "Invalid hexadecimal string");
					}
					return nullptr;
				}
			}

			uintmax_t value = 0;
			for (; *p && isDigit(*p, base); ++p) {
				value = (base * value) + digitValueOf(*p, base);
			}

			wfloat fvalue = (wfloat)value;
			if (*p == '.') {
				++p;
				for (int i = 1; *p && isDigit(*p, base); ++p, ++i) {
					fvalue += digitValueOf(*p, base) * std::pow((wfloat)base, (wfloat)-i);
				}
			}

			if (*p) {
				WRaiseError(context, "Invalid float string");
				return nullptr;
			}

			return WCreateFloat(context, fvalue);
		}

		static WObj* Str_Eq(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_STRING(0);
			return WCreateBool(context, WIsString(argv[1]) && std::strcmp(WGetString(argv[0]), WGetString(argv[1])) == 0);
		}

		static WObj* Str_Mul(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_STRING(0);
			EXPECT_ARG_TYPE_INT(1);
			wint multiplier = WGetInt(argv[1]);
			std::string s;
			for (wint i = 0; i < multiplier; i++)
				s += WGetString(argv[0]);
			return WCreateString(context, s.c_str());
		}

		static WObj* Str_Contains(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_STRING(0);
			EXPECT_ARG_TYPE_STRING(1);
			return WCreateBool(context, std::strstr(WGetString(argv[0]), WGetString(argv[1])));
		}

		template <int IsList>
		static WObj* TupleList_GetItem(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			if constexpr (IsList) {
				EXPECT_ARG_TYPE_LIST(0);
			} else {
				EXPECT_ARG_TYPE_TUPLE(0);
			}

			if (WIsInt(argv[1])) {
				GET_LIST_INDEX(0, 1);
				return argv[0]->v[listIndex];
			}

			WObj* range[3]{};
			for (size_t i = 0; i < std::size(range); i++) {
				const char* attrs[3] = { "start", "stop", "step" };
				auto& entry = range[i];
				entry = WGetAttribute(argv[1], attrs[i]);
				if (entry == nullptr) {
					SetMissingAttributeError(context, argv, i + 1, attrs[i]);
					return nullptr;
				} else if (!WIsInt(entry) && !WIsNoneType(entry)) {
					SetArgumentError(context, 1,
						std::string("attribute ") + attrs[i] +
						" expected type int or NoneType"
						" but got " + WObjTypeToString(range[i]->type)
					);
					return nullptr;
				}
			}

			size_t size = argv[0]->v.size();

			WObj* slice = WCreateList(context);
			WProtectObject(slice);

			wint step = 1;
			if (WIsInt(range[2])) {
				step = WGetInt(range[2]);
				if (step == 0) {
					WRaiseError(context, "Step cannot be zero");
					WUnprotectObject(slice);
					return nullptr;
				}
			}

			wint start = step > 0 ? 0 : ((wint)size - 1);
			if (WIsInt(range[0]))
				start = WGetInt(range[0]);
			
			wint stop = (wint)size;
			bool includeStop = false; // Only applicable when step is negative
			if (WIsInt(range[1])) {
				stop = WGetInt(range[1]);
			} else if (step < 0) {
				stop = 0;
				includeStop = true;
			}

			int64_t startRaw = (int64_t)ConvertNegativeIndex(start, size);
			int64_t stopRaw = (int64_t)ConvertNegativeIndex(stop, size);

			bool failed = false;
			auto appendElem = [&](size_t i) {
				if (i >= size) {
					SetIndexOutOfRangeError(context, argv, 1);
					failed = true;
					return false;
				}
				slice->v.push_back(argv[0]->v[i]);
				return true;
			};

			if (step > 0) {
				for (int64_t i = startRaw; i < stopRaw; i += step)
					if (!appendElem((size_t)i))
						break;
			} else {
				if (includeStop)
					stopRaw--;
				for (int64_t i = startRaw; i > stopRaw; i += step)
					if (!appendElem((size_t)i))
						break;
			}

			WUnprotectObject(slice);
			return failed ? nullptr : slice;
		}

		static WObj* List_SetItem(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(3);
			EXPECT_ARG_TYPE_LIST(0);
			EXPECT_ARG_TYPE_INT(1);

			GET_LIST_INDEX(0, 1);
			argv[0]->v[listIndex] = argv[2];
			return argv[0];
		}

		template <int IsList>
		static WObj* TupleList_Len(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(1);
			if constexpr (IsList) {
				EXPECT_ARG_TYPE_LIST(0);
			} else {
				EXPECT_ARG_TYPE_TUPLE(0);
			}

			return WCreateInt(context, (wint)argv[0]->v.size());
		}

		static WObj* List_Append(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_LIST(0);
			argv[0]->v.push_back(argv[1]);
			return argv[0];
		}

		static WObj* List_Insert(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(3);
			EXPECT_ARG_TYPE_LIST(0);
			EXPECT_ARG_TYPE_INT(1);

			GET_LIST_INDEX(0, 1);
			argv[0]->v.insert(argv[0]->v.begin() + listIndex, argv[2]);
			return argv[0];
		}

		static WObj* List_Pop(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT_BETWEEN(0, 1);
			EXPECT_ARG_TYPE_LIST(0);

			wint negIndex = -1;
			if (argc == 1) {
				EXPECT_ARG_TYPE_INT(1);
				negIndex = WGetInt(argv[1]);
			}

			size_t listIndex = ConvertNegativeIndex(negIndex, argv[0]->v.size());
			if (listIndex >= argv[0]->v.size()) {
				SetIndexOutOfRangeError(context, argv, 1);
				return nullptr;
			}

			WObj* popped = argv[0]->v[listIndex];
			argv[0]->v.erase(argv[0]->v.begin() + listIndex);
			return popped;
		}

		static WObj* List_Remove(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(2);
			EXPECT_ARG_TYPE_LIST(0);

			auto& v = argv[0]->v;
			for (size_t i = 0; i < v.size(); i++) {
				WObj* eq = WEquals(argv[1], v[i]);

				if (eq == nullptr) {
					return nullptr;
				}
				
				if (!WGetBool(eq)) {
					continue;
				}

				if (i >= v.size()) {
					WRaiseError(context, "List modified while iterating");
					return nullptr;
				}
				
				v.erase(v.begin() + i);
				return argv[0];
			}

			WRaiseError(context, "Value not found in list");
			return nullptr;
		}

		static WObj* Tuple_Len(WObj** argv, int argc, WContext* context) {
			EXPECT_ARG_COUNT(1);
			EXPECT_ARG_TYPE_TUPLE(0);

			return WCreateInt(context, (wint)argv[0]->v.size());
		}

	} // namespace attrlib

	namespace lib {

		static WObj* print(WObj** argv, int argc, WContext* context) {
			std::string text;
			for (int i = 0; i < argc; i++) {
				if (WObj* s = WConvertToString(argv[i])) {
					text += WGetString(s);
				} else {
					return nullptr;
				}

				if (i < argc - 1) {
					text += ' ';
				}
			}
			text += '\n';
			WPrint(context, text.c_str(), (int)text.size());
			return WCreateNoneType(context);
		}

		static WObj* set_class_attr(WObj** argv, int argc, WContext* context) {
			// Not callable from user code
			argv[2]->fn.isMethod = true;
			argv[0]->c.Set(argv[1]->s, argv[2]);
			return WCreateNoneType(context);
		}

	} // namespace lib

	bool InitLibrary(WContext* context) {

		// Returns from function if operation resulted in nullptr
#define CheckOperation(op) do { if ((op) == nullptr) return false; } while (0)

		// Create builtin classes
		CheckOperation(context->builtinClasses.null = CreateClass<classlib::Null>(context));
		CheckOperation(context->builtinClasses._bool = CreateClass<classlib::Bool>(context, "bool"));
		CheckOperation(context->builtinClasses._int = CreateClass<classlib::Int>(context, "int"));
		CheckOperation(context->builtinClasses._float = CreateClass<classlib::Float>(context, "float"));
		CheckOperation(context->builtinClasses.str = CreateClass<classlib::Str>(context, "str"));
		CheckOperation(context->builtinClasses.tuple = CreateClass<classlib::Tuple>(context, "tuple"));
		CheckOperation(context->builtinClasses.list = CreateClass<classlib::List>(context, "list"));
		CheckOperation(context->builtinClasses.map = CreateClass<classlib::Map>(context, "dict"));
		CheckOperation(context->builtinClasses.func = CreateClass<classlib::Func>(context));
		CheckOperation(context->builtinClasses.object = CreateClass<classlib::Object>(context, "object"));
		CheckOperation(context->builtinClasses.userdata = CreateClass<classlib::Userdata>(context));

		WObj* emptyBasesTuple{};
		CheckOperation(emptyBasesTuple = WCreateTuple(context, nullptr, 0));
		context->builtinClasses.object->attributes.AddParent(context->builtinClasses.object->c);
		context->builtinClasses.object->attributes.Set("__bases__", emptyBasesTuple);

		// Subclass the object class
		WObj* basesTuple{};
		CheckOperation(basesTuple = WCreateTuple(context, &context->builtinClasses.object, 1));

		AttributeTable& objectAttributes = context->builtinClasses.object->c;
		auto subclassObject = [&](WObj* _class) {
			_class->c.AddParent(objectAttributes, false);
			_class->attributes.AddParent(objectAttributes, false);

			_class->attributes.Set("__bases__", basesTuple, false);
		};

		subclassObject(context->builtinClasses.null);
		subclassObject(context->builtinClasses._bool);
		subclassObject(context->builtinClasses._int);
		subclassObject(context->builtinClasses._float);
		subclassObject(context->builtinClasses.str);
		subclassObject(context->builtinClasses.tuple);
		subclassObject(context->builtinClasses.list);
		subclassObject(context->builtinClasses.map);
		subclassObject(context->builtinClasses.func);
		subclassObject(context->builtinClasses.userdata);

		// Create null (None) singleton
		CheckOperation(context->nullSingleton = WCall(context->builtinClasses.null, nullptr, 0));

		// Register methods of builtin classes
		CheckOperation(RegisterStatelessMethod<attrlib::Object_Pos>(context, context->builtinClasses.object->c, "__pos__", "object.__pos__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Object_Str>(context, context->builtinClasses.object->c, "__str__", "object.__str__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Object_Eq>(context, context->builtinClasses.object->c, "__eq__", "object.__eq__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Object_Ne>(context, context->builtinClasses.object->c, "__ne__", "object.__ne__"));

		CheckOperation(RegisterStatelessMethod<attrlib::Null_Bool>(context, context->builtinClasses.null->c, "__nonzero__", "NoneType.__nonzero__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Null_Eq>(context, context->builtinClasses.null->c, "__eq__", "NoneType.__eq__"));

		CheckOperation(RegisterStatelessMethod<attrlib::Bool_Bool>(context, context->builtinClasses._bool->c, "__nonzero__", "bool.__nonzero__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Bool_Int>(context, context->builtinClasses._bool->c, "__int__", "bool.__int__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Bool_Float>(context, context->builtinClasses._bool->c, "__float__", "int.__float__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Bool_Eq>(context, context->builtinClasses._bool->c, "__eq__", "bool.__eq__"));

		CheckOperation(RegisterStatelessMethod<attrlib::Int_Bool>(context, context->builtinClasses._int->c, "__nonzero__", "int.__nonzero__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Int_Int>(context, context->builtinClasses._int->c, "__int__", "int.__int__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Int_Float>(context, context->builtinClasses._int->c, "__float__", "int.__float__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Int_Eq>(context, context->builtinClasses._int->c, "__eq__", "int.__eq__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Int_Gt>(context, context->builtinClasses._int->c, "__gt__", "int.__gt__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Int_Ge>(context, context->builtinClasses._int->c, "__ge__", "int.__ge__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Int_Lt>(context, context->builtinClasses._int->c, "__lt__", "int.__lt__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Int_Le>(context, context->builtinClasses._int->c, "__le__", "int.__le__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Int_Neg>(context, context->builtinClasses._int->c, "__neg__", "int.__neg__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Int_Add>(context, context->builtinClasses._int->c, "__add__", "int.__add__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Int_Sub>(context, context->builtinClasses._int->c, "__sub__", "int.__sub__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Int_Mul>(context, context->builtinClasses._int->c, "__mul__", "int.__mul__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Int_Div>(context, context->builtinClasses._int->c, "__div__", "int.__div__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Int_FloorDiv>(context, context->builtinClasses._int->c, "__floordiv__", "int.__floordiv__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Int_Mod>(context, context->builtinClasses._int->c, "__mod__", "int.__mod__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Int_Pow>(context, context->builtinClasses._int->c, "__pow__", "int.__pow__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Int_BitAnd>(context, context->builtinClasses._int->c, "__and__", "int.__and__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Int_BitOr>(context, context->builtinClasses._int->c, "__or__", "int.__or__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Int_BitXor>(context, context->builtinClasses._int->c, "__xor__", "int.__xor__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Int_BitNot>(context, context->builtinClasses._int->c, "__invert__", "int.__not__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Int_ShiftLeft>(context, context->builtinClasses._int->c, "__lshift__", "int.__lshift__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Int_ShiftRight>(context, context->builtinClasses._int->c, "__rshift__", "int.__rshift__"));

		CheckOperation(RegisterStatelessMethod<attrlib::Float_Bool>(context, context->builtinClasses._float->c, "__nonzero__", "float.__nonzero__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Float_Int>(context, context->builtinClasses._float->c, "__int__", "float.__int__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Float_Float>(context, context->builtinClasses._float->c, "__float__", "float.__float__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Float_Eq>(context, context->builtinClasses._float->c, "__eq__", "float.__eq__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Float_Neg>(context, context->builtinClasses._float->c, "__neg__", "float.__neg__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Float_Add>(context, context->builtinClasses._float->c, "__add__", "float.__add__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Float_Sub>(context, context->builtinClasses._float->c, "__sub__", "float.__sub__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Float_Mul>(context, context->builtinClasses._float->c, "__mul__", "float.__mul__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Float_Div>(context, context->builtinClasses._float->c, "__div__", "float.__div__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Float_FloorDiv>(context, context->builtinClasses._float->c, "__floordiv__", "float.__floordiv__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Float_Mod>(context, context->builtinClasses._float->c, "__mod__", "float.__mod__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Float_Pow>(context, context->builtinClasses._float->c, "__pow__", "float.__pow__"));

		CheckOperation(RegisterStatelessMethod<attrlib::Str_Bool>(context, context->builtinClasses.str->c, "__nonzero__", "str.__nonzero__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Str_Int>(context, context->builtinClasses.str->c, "__int__", "str.__int__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Str_Float>(context, context->builtinClasses.str->c, "__float__", "str.__float__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Str_Eq>(context, context->builtinClasses.str->c, "__eq__", "str.__eq__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Str_Mul>(context, context->builtinClasses.str->c, "__mul__", "str.__mul__"));
		CheckOperation(RegisterStatelessMethod<attrlib::Str_Contains>(context, context->builtinClasses.str->c, "__contains__", "str.__contains__"));

		CheckOperation(RegisterStatelessMethod<attrlib::TupleList_GetItem<1>>(context, context->builtinClasses.list->c, "__getitem__", "list.__getitem__"));
		CheckOperation(RegisterStatelessMethod<attrlib::List_SetItem>(context, context->builtinClasses.list->c, "__setitem__", "list.__setitem__"));
		CheckOperation(RegisterStatelessMethod<attrlib::TupleList_Len<1>>(context, context->builtinClasses.list->c, "__len__", "list.__len__"));
		//CheckOperation(RegisterStatelessMethod<attrlib::List_Contains>(context, context->builtinClasses.list->c, "__contains__"));
		CheckOperation(RegisterStatelessMethod<attrlib::List_Insert>(context, context->builtinClasses.list->c, "insert", "list.insert"));
		CheckOperation(RegisterStatelessMethod<attrlib::List_Append>(context, context->builtinClasses.list->c, "append", "list.append"));
		//CheckOperation(RegisterStatelessMethod<attrlib::List_Extend>(context, context->builtinClasses.list->c, "extend"));
		CheckOperation(RegisterStatelessMethod<attrlib::List_Pop>(context, context->builtinClasses.list->c, "pop", "list.pop"));
		CheckOperation(RegisterStatelessMethod<attrlib::List_Remove>(context, context->builtinClasses.list->c, "remove", "list.remove"));
		//CheckOperation(RegisterStatelessMethod<attrlib::List_Clear>(context, context->builtinClasses.list->c, "clear"));
		//CheckOperation(RegisterStatelessMethod<attrlib::List_Copy>(context, context->builtinClasses.list->c, "copy"));
		//CheckOperation(RegisterStatelessMethod<attrlib::List_Index>(context, context->builtinClasses.list->c, "index"));
		//CheckOperation(RegisterStatelessMethod<attrlib::List_Reverse>(context, context->builtinClasses.list->c, "reverse"));
		//CheckOperation(RegisterStatelessMethod<attrlib::List_Count>(context, context->builtinClasses.list->c, "count"));

		CheckOperation(RegisterStatelessMethod<attrlib::TupleList_GetItem<0>>(context, context->builtinClasses.tuple->c, "__getitem__", "list.__getitem__"));
		CheckOperation(RegisterStatelessMethod<attrlib::TupleList_Len<0>>(context, context->builtinClasses.tuple->c, "__len__", "tuple.__len__"));

		// Register builtin functions
		CheckOperation(RegisterStatelessFunction<lib::print>(context, "print"));
		CheckOperation(RegisterStatelessFunction<lib::set_class_attr>(context, "set_class_attr"));

		WObj* builtins = WCompile(context,
			R"(
def isinstance(o, t):
	def f(cls):
		if cls == t:
			return True
		for base in cls.__bases__:
			if f(base):
				return True
		return False
	return f(o.__class__)

def len(x):
	return x.__len__()

class Exception:
	def __init__(self, message=""):
		self.message = message
	def __str__(self):
		return self.message

class __Range:
	def __init__(self, start, end, step):
		self.start = start
		self.end = end
		self.step = step
	def __iter__(self):
		return __RangeIter(self.start, self.end, self.step)

class __RangeIter:
	def __init__(self, start, end, step):
		self.cur = start
		self.end = end
		self.step = step
	def __next__(self):
		cur = self.cur
		self.cur = self.cur + self.step
		return cur
	def __end__(self):
		if self.step > 0:
			return self.cur >= self.end
		else:
			return self.cur <= self.end

def range(start, end=None, step=None):
	if end == None:
		return __Range(0, start, 1)
	elif step == None:
		return __Range(start, end, 1)
	else:
		return __Range(start, end, step)

class __Slice:
	def __init__(self, start, stop, step):
		self.start = start
		self.stop = stop
		self.step = step

def slice(start, stop=None, step=None):
	if stop == None:
		return slice(None, start, None)
	elif step == None:
		return slice(start, stop, None)
	else:
		return slice(start, stop, step)

class __ListIter:
	def __init__(self, li):
		self.li = li
		self.i = 0

	def __next__(self):
		v = self.li[self.i]
		self.i = self.i + 1
		return v

	def __end__(self):
		return self.i >= len(self.li)
		
set_class_attr(list, "__iter__", lambda self: __ListIter(self))
set_class_attr(tuple, "__iter__", lambda self: __ListIter(self))
			)",
			"__builtins__"
		);

		CheckOperation(builtins);

		CheckOperation(WCall(builtins, nullptr, 0));

		CheckOperation(context->builtinClasses.slice = WGetGlobal(context, "__Slice"));
		CheckOperation(context->isinstance = WGetGlobal(context, "isinstance"));

		return true;
	}

} // namespace wings
