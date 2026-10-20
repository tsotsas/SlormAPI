// SlormAPI: exposes The Slormancer's runtime (globals, instances, scripts, builtins) as JSON commands - to other
// Aurie modules through the "SlormAPI" interface (SlormAPI_Interface.hpp), and optionally over a loopback-only,
// newline-delimited TCP protocol for development tools (tools/slormapi.py). The TCP server only starts when
// SLORMAPI_SERVER=1 (or SLORMAPI_PORT) is set, or a file "SlormAPI.server" sits next to the DLL.
#include <YYToolkit/YYTK_Shared.hpp>
#include <array>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <cmath>
#include <fstream>
#include <future>
#include <sstream>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <cstring>
#include <set>
#include <utility>
#include "Json.hpp"
#include "Server.hpp"
#include "SlormAPI_Interface.hpp"
#include "GameData.hpp"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#pragma comment(lib, "version.lib")

using namespace Aurie;
using namespace YYTK;
using sj::Json;

static YYTKInterface* g_Yytk = nullptr;
static uint64_t g_Frame = 0;  // only advances if YYTK's Present hook is active
static uint64_t g_CodeEvents = 0;
static uint64_t g_WndProcCalls = 0;
static const auto g_StartTime = std::chrono::steady_clock::now();

// "perf": SlormAPI's own time per frame (off unless asked for; QueryPerformanceCounter around our code only)
static bool g_Perf = false;
static double g_PerfCallbackUs = 0, g_PerfTickUs = 0, g_PerfDetourUs = 0, g_PerfPartUs[3] = {};
static uint64_t g_PerfFrames = 0, g_PerfEvents = 0, g_PerfDetours = 0;
static double PerfNowUs()
{
	static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart); }();
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return static_cast<double>(c.QuadPart) * 1e6 / freq;
}

static double MicrosSinceStart()
{
	return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - g_StartTime).count();
}
static constexpr uint16_t kDefaultPort = 47950;
static constexpr const char* kVersion = "1.0.0";

[[noreturn]] static void Fail(const std::string& Msg) { throw std::runtime_error(Msg); }

// ============================================================================
// RValue -> JSON
// ============================================================================

static RValueType KindOf(const RValue& V) { return static_cast<RValueType>(V.m_Kind & 0x00FFFFFF); }

static YYObjectKind ObjectKindOf(const RValue& V)
{
	if (KindOf(V) != VALUE_OBJECT || !V.m_Object) return OBJECT_KIND_MAX;
	YYObjectKind k = V.m_Object->m_ObjectKind;
	return (k >= 0 && k < OBJECT_KIND_MAX) ? k : OBJECT_KIND_MAX;
}

static RValue GlobalRValue()
{
	CInstance* global = nullptr;
	if (!AurieSuccess(g_Yytk->GetGlobalInstance(&global)) || !global) Fail("global instance unavailable");
	return RValue(global);
}

static bool IsGlobal(const RValue& V)
{
	CInstance* global = nullptr;
	return KindOf(V) == VALUE_OBJECT && AurieSuccess(g_Yytk->GetGlobalInstance(&global)) && V.m_Pointer == global;
}

// string(x) is safe for every kind; RValue::ToString() is only valid for strings.
static std::string GmlString(const RValue& V)
{
	if (KindOf(V) == VALUE_STRING) return V.ToString();
	RValue s = g_Yytk->CallBuiltin("string", { V });
	return KindOf(s) == VALUE_STRING ? s.ToString() : "<" + V.GetKindName() + ">";
}

static int64_t InstanceId(CInstance* Inst)
{
	RValue id;
	if (!AurieSuccess(g_Yytk->GetBuiltin("id", Inst, NULL_INDEX, id))) return -1;
	return id.ToInt64();
}

static std::string ObjectNameOf(CInstance* Inst)
{
	RValue idx;
	if (!AurieSuccess(g_Yytk->GetBuiltin("object_index", Inst, NULL_INDEX, idx))) return "";
	RValue name = g_Yytk->CallBuiltin("object_get_name", { idx });
	return KindOf(name) == VALUE_STRING ? name.ToString() : "";
}

static Json InstanceStub(CInstance* Inst)
{
	Json j = Json::Object();
	j.Add("$inst", InstanceId(Inst));
	j.Add("$object", ObjectNameOf(Inst));
	// position builtins (not instance members): lets hook logs place the instance at the call
	RValue v;
	if (AurieSuccess(g_Yytk->GetBuiltin("x", Inst, NULL_INDEX, v))) j.Add("$x", v.ToDouble());
	if (AurieSuccess(g_Yytk->GetBuiltin("y", Inst, NULL_INDEX, v))) j.Add("$y", v.ToDouble());
	Json bb = Json::Array();
	for (const char* b : { "bbox_left", "bbox_right", "bbox_top", "bbox_bottom" })
		if (AurieSuccess(g_Yytk->GetBuiltin(b, Inst, NULL_INDEX, v))) bb.Push(Json(v.ToDouble()));
	if (bb.a.size() == 4) j.Add("$bbox", bb);
	return j;
}

static Json ToJson(const RValue& V, int Depth, int64_t& Budget);

static Json MembersToJson(const RValue& V, int Depth, int64_t& Budget, Json Into)
{
	RValue copy = V;
	g_Yytk->EnumInstanceMembers(copy, [&](const char* Name, RValue* Value) -> bool
	{
		if (--Budget < 0) { Into.Add("$truncated", true); return true; }
		Into.Add(Name, ToJson(*Value, Depth - 1, Budget));
		return false;
	});
	return Into;
}

static Json ToJson(const RValue& V, int Depth, int64_t& Budget)
{
	switch (KindOf(V))
	{
	case VALUE_REAL: return Json(V.m_Real);
	case VALUE_INT32: return Json(V.m_i32);
	case VALUE_INT64: return Json(V.m_i64);
	case VALUE_BOOL: return Json(V.ToBoolean());
	case VALUE_STRING: return Json(V.ToString());
	case VALUE_UNDEFINED:
	case VALUE_UNSET: return Json();
	case VALUE_PTR:
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "0x%p", V.m_Pointer);
		Json j = Json::Object();
		j.Add("$ptr", buf);
		return j;
	}
	case VALUE_ARRAY:
	{
		RValue copy = V;
		size_t n = 0;
		g_Yytk->GetArraySize(copy, n);
		if (Depth <= 0)
		{
			Json j = Json::Object();
			j.Add("$array", n);
			return j;
		}
		Json j = Json::Array();
		for (size_t k = 0; k < n; ++k)
		{
			if (--Budget < 0) { j.Push(Json("$truncated")); break; }
			RValue* e = nullptr;
			if (AurieSuccess(g_Yytk->GetArrayEntry(copy, k, e)) && e) j.Push(ToJson(*e, Depth - 1, Budget));
			else j.Push(Json());
		}
		return j;
	}
	case VALUE_OBJECT:
		switch (ObjectKindOf(V))
		{
		case OBJECT_KIND_CINSTANCE:
		{
			Json stub = InstanceStub(V.m_Instance);
			return Depth <= 0 ? stub : MembersToJson(V, Depth, Budget, std::move(stub));
		}
		case OBJECT_KIND_YYOBJECTBASE:
		{
			if (Depth <= 0)
			{
				int32_t count = 0;
				g_Yytk->GetInstanceMemberCount(V, count);
				Json j = Json::Object();
				j.Add("$struct", count);
				return j;
			}
			return MembersToJson(V, Depth, Budget, Json::Object());
		}
		case OBJECT_KIND_SCRIPTREF:
		{
			Json j = Json::Object();
			j.Add("$method", GmlString(V));
			return j;
		}
		default:
		{
			Json j = Json::Object();
			j.Add("$object_kind", static_cast<int>(ObjectKindOf(V)));
			j.Add("str", GmlString(V));
			return j;
		}
		}
	case VALUE_REF:
	{
		// Instance ids are refs in this runtime; resolve live instances to instance stubs.
		int64_t id = V.ToInt64();
		CInstance* inst = nullptr;
		if (id >= 100000 && AurieSuccess(g_Yytk->GetInstanceObject(static_cast<int32_t>(id), inst)) && inst)
			return Depth <= 0 ? InstanceStub(inst) : MembersToJson(RValue(inst), Depth, Budget, InstanceStub(inst));
		Json j = Json::Object();
		j.Add("$ref", GmlString(V));
		j.Add("id", id);
		return j;
	}
	default:
	{
		Json j = Json::Object();
		j.Add("$kind", V.GetKindName());
		j.Add("str", GmlString(V));
		return j;
	}
	}
}

static Json ToJson(const RValue& V, int Depth)
{
	int64_t budget = 500000;
	return ToJson(V, Depth, budget);
}

// ============================================================================
// JSON -> RValue
// ============================================================================

static RValue AssetIndex(const std::string& Name)
{
	RValue idx = g_Yytk->CallBuiltin("asset_get_index", { RValue(std::string_view(Name)) });
	if (idx.ToDouble() < 0) Fail("unknown asset '" + Name + "'");
	return idx;
}

static CInstance* InstanceById(int32_t Id)
{
	CInstance* inst = nullptr;
	if (!AurieSuccess(g_Yytk->GetInstanceObject(Id, inst)) || !inst) Fail("no active instance with id " + std::to_string(Id));
	return inst;
}

// Conventions: {"$inst": id} -> instance, {"$asset": "name"} -> asset index, other objects -> structs.
static RValue FromJson(const Json& J)
{
	switch (J.type)
	{
	case Json::Null: return RValue();
	case Json::Bool: return RValue(J.b);
	case Json::Num: return RValue(J.n);
	case Json::Str: return RValue(std::string_view(J.s));
	case Json::Arr:
	{
		std::vector<RValue> v;
		v.reserve(J.a.size());
		for (auto& e : J.a) v.push_back(FromJson(e));
		return RValue(v);
	}
	case Json::Obj:
	{
		if (const Json* id = J.Find("$inst")) return RValue(InstanceById(static_cast<int32_t>(id->n)));
		if (const Json* a = J.Find("$asset")) return AssetIndex(a->s);
		std::map<std::string, RValue> m;
		for (auto& [k, v] : J.o) m[k] = FromJson(v);
		return RValue(m);
	}
	}
	return RValue();
}

static std::vector<RValue> ArgsFromJson(const Json* Args)
{
	std::vector<RValue> out;
	if (!Args) return out;
	if (Args->type != Json::Arr) Fail("'args' must be an array");
	for (auto& a : Args->a) out.push_back(FromJson(a));
	return out;
}

// ============================================================================
// Roots and paths
// ============================================================================

static RValue ObjectRef(const Json& Obj)
{
	if (Obj.type == Json::Num) return RValue(Obj.n);
	if (Obj.type == Json::Str && Obj.s == "all") return RValue(-3.0);
	if (Obj.type == Json::Str) return AssetIndex(Obj.s);
	Fail("'object' must be an object name, index or \"all\"");
}

static std::vector<CInstance*> InstancesOf(const RValue& Obj)
{
	std::vector<CInstance*> out;
	int32_t n = g_Yytk->CallBuiltin("instance_number", { Obj }).ToInt32();
	for (int32_t k = 0; k < n; ++k)
	{
		RValue id = g_Yytk->CallBuiltin("instance_find", { Obj, RValue(static_cast<double>(k)) });
		CInstance* inst = nullptr;
		if (AurieSuccess(g_Yytk->GetInstanceObject(id.ToInt32(), inst)) && inst) out.push_back(inst);
	}
	return out;
}

// root: "global" (default) | instance id | object name (first instance) | {"$inst": id}
static RValue ResolveRoot(const Json* Root)
{
	if (!Root || Root->IsNull() || (Root->type == Json::Str && Root->s == "global")) return GlobalRValue();
	if (Root->type == Json::Num) return RValue(InstanceById(static_cast<int32_t>(Root->n)));
	if (Root->type == Json::Obj) return FromJson(*Root);
	if (Root->type == Json::Str)
	{
		auto insts = InstancesOf(AssetIndex(Root->s));
		if (insts.empty()) Fail("no instance of '" + Root->s + "'");
		return RValue(insts.front());
	}
	Fail("bad 'root'");
}

struct PathTok
{
	bool IsIndex = false;
	std::string Name;
	size_t Index = 0;
};

// "a.b[3].c" or ["a", "b", 3, "c"]
static std::vector<PathTok> ParsePath(const Json* P)
{
	std::vector<PathTok> out;
	if (!P || P->IsNull()) return out;
	if (P->type == Json::Arr)
	{
		for (auto& t : P->a)
		{
			if (t.type == Json::Num) out.push_back({ true, "", static_cast<size_t>(t.n) });
			else if (t.type == Json::Str) out.push_back({ false, t.s, 0 });
			else Fail("path tokens must be strings or numbers");
		}
		return out;
	}
	if (P->type != Json::Str) Fail("'path' must be a string or array");
	const std::string& s = P->s;
	size_t i = 0;
	while (i < s.size())
	{
		if (s[i] == '.') { ++i; continue; }
		if (s[i] == '[')
		{
			size_t close = s.find(']', i);
			if (close == std::string::npos) Fail("unclosed '[' in path");
			out.push_back({ true, "", std::stoull(s.substr(i + 1, close - i - 1)) });
			i = close + 1;
			continue;
		}
		size_t end = s.find_first_of(".[", i);
		if (end == std::string::npos) end = s.size();
		out.push_back({ false, s.substr(i, end - i), 0 });
		i = end;
	}
	return out;
}

static std::string PathString(const std::vector<PathTok>& Path, size_t Count)
{
	std::string out;
	for (size_t k = 0; k < Count; ++k)
		out += Path[k].IsIndex ? "[" + std::to_string(Path[k].Index) + "]" : (out.empty() ? "" : ".") + Path[k].Name;
	return out.empty() ? "<root>" : out;
}

// Finds an existing member without side effects. (YYTK's GetInstanceMember allocates
// a new undefined member when the name is missing, which would mutate game state - so it is only
// used after the engine's hashed *_exists check; the full member walk is the fallback.)
static bool IsUndef(const RValue& V) { auto k = KindOf(V); return k == VALUE_UNDEFINED || k == VALUE_UNSET; }
static bool ReadBuiltin(const RValue& Container, const std::string& Name, RValue& Out);

static RValue* FindMember(const RValue& Container, const std::string& Name)
{
	const char* exists = nullptr;
	std::vector<RValue> args;
	RValue name = RValue(std::string_view(Name));
	if (IsGlobal(Container)) { exists = "variable_global_exists"; args = { name }; }
	else if (ObjectKindOf(Container) == OBJECT_KIND_CINSTANCE) { exists = "variable_instance_exists"; args = { Container, name }; }
	else if (KindOf(Container) == VALUE_OBJECT) { exists = "variable_struct_exists"; args = { Container, name }; }
	if (exists)
	{
		if (!g_Yytk->CallBuiltin(exists, args).ToBoolean()) return nullptr;
		// variable_instance_exists is also true for built-ins (x, depth, ...): asking for their member slot would
		// create an empty variable of that name, so they are read as built-ins instead
		RValue b;
		if (ObjectKindOf(Container) == OBJECT_KIND_CINSTANCE && ReadBuiltin(Container, Name, b)) return nullptr;
		RValue* m = nullptr;
		if (AurieSuccess(g_Yytk->GetInstanceMember(Container, Name.c_str(), m)) && m) return m;
	}
	RValue* found = nullptr;
	RValue copy = Container;
	g_Yytk->EnumInstanceMembers(copy, [&](const char* MemberName, RValue* Value) -> bool
	{
		if (Name != MemberName) return false;
		found = Value;
		return true;
	});
	return found;
}

// Engine built-ins: instance ones (x, object_index, ...) or global ones (room, instance_count, ...).
static bool ReadBuiltin(const RValue& Container, const std::string& Name, RValue& Out)
{
	if (IsGlobal(Container)) return AurieSuccess(g_Yytk->GetBuiltin(Name, nullptr, NULL_INDEX, Out));
	if (ObjectKindOf(Container) == OBJECT_KIND_CINSTANCE) return AurieSuccess(g_Yytk->GetBuiltin(Name, Container.m_Instance, NULL_INDEX, Out));
	return false;
}

// Returns a pointer to engine-owned storage, or to Holder for root/built-in values.
static RValue* Walk(RValue& Root, const std::vector<PathTok>& Path, size_t Count, RValue& Holder)
{
	RValue* cur = &Root;
	for (size_t k = 0; k < Count; ++k)
	{
		const PathTok& t = Path[k];
		if (t.IsIndex)
		{
			if (KindOf(*cur) != VALUE_ARRAY) Fail(PathString(Path, k) + " is not an array");
			RValue* e = nullptr;
			if (!AurieSuccess(g_Yytk->GetArrayEntry(*cur, t.Index, e)) || !e) Fail(PathString(Path, k + 1) + " is out of range");
			cur = e;
			continue;
		}
		if (KindOf(*cur) != VALUE_OBJECT) Fail(PathString(Path, k) + " is not a struct or instance");
		if (RValue* m = FindMember(*cur, t.Name)) { cur = m; continue; }
		RValue b;
		if (ReadBuiltin(*cur, t.Name, b)) { Holder = b; cur = &Holder; continue; }
		Fail(PathString(Path, k + 1) + " does not exist");
	}
	return cur;
}

// ============================================================================
// Draw programs: builtin / script calls with draw-time expressions, run after an object event (overlay) or after a
// hooked script (hook "after", e.g. inside a menu bake). Expressions (JSON): literals; {"$v": name} program
// variable; {"$self": var}; {"$global": var}; {"$arg": i} argument of the hooked call; {"$fn": name, "args": [...]}
// builtin or script call; {"$op": op, "args": [...]} with + - * / == != < <= > >= and or not ? ?? def in_rect mba.
// Pseudo calls: "$mba_set" [name, index, value] edits self.mba entries called name; "$mba_push" [entry] appends
// one (the game's own UI element list: hover and clicks then work natively); "$mba_remove" [index, value] drops the
// entries whose [index] == value, "$mba_remove_in" [x1, y1, x2, y2] the ones placed in that rectangle; "$set" [var,
// value] on self.
// ============================================================================

static std::string ScriptName(const std::string& Name)
{
	return Name.starts_with("gml_Script_") ? Name : "gml_Script_" + Name;
}

static bool IsScript(std::string& Name)   // resolves Name to its gml_Script_ form when it is a game script
{
	if (Name.starts_with("$")) return false;
	std::string script = ScriptName(Name);
	PVOID p = nullptr;
	if (!AurieSuccess(g_Yytk->GetNamedRoutinePointer(script.c_str(), &p)) || !p) return false;
	Name = script;
	return true;
}

struct Expr
{
	enum Kind { Const, Var, SelfVar, GlobalVar, Arg, Fn, Op, Arr } K = Const;
	RValue Value;
	std::string Name;
	int Index = 0;
	bool Script = false;
	std::vector<Expr> Args;
};

struct DrawCall { std::string Fn; bool Script = false; std::vector<Expr> Args; bool HasWhen = false; Expr When; };

struct Program
{
	std::string Owner;   // the hook / overlay owner: widget clicks go to it
	bool Widgets = false;   // uses $button / $radio / $selector
	std::vector<std::pair<std::string, Expr>> Vars;
	bool HasWhen = false;
	Expr When;
	std::vector<DrawCall> Calls;
};

struct EvalCtx
{
	CInstance* Self;
	CInstance* Other;
	int ArgCount;
	RValue** Args;
	std::map<std::string, RValue> Vals;
	double Scale = 0;   // self.ui_scale once read (UiScale)
};

static Expr CompileExpr(const Json& J)
{
	Expr e;
	if (J.type == Json::Arr)
	{
		e.K = Expr::Arr;
		for (auto& a : J.a) e.Args.push_back(CompileExpr(a));
		return e;
	}
	if (J.type == Json::Obj)
	{
		auto args = [&]() { if (const Json* a = J.Find("args")) for (auto& x : a->a) e.Args.push_back(CompileExpr(x)); };
		if (const Json* v = J.Find("$v")) { e.K = Expr::Var; e.Name = v->s; return e; }
		if (const Json* v = J.Find("$self")) { e.K = Expr::SelfVar; e.Name = v->s; return e; }
		if (const Json* v = J.Find("$global")) { e.K = Expr::GlobalVar; e.Name = v->s; return e; }
		if (const Json* v = J.Find("$arg")) { e.K = Expr::Arg; e.Index = static_cast<int>(v->n); return e; }
		if (const Json* v = J.Find("$fn")) { e.K = Expr::Fn; e.Name = v->s; e.Script = IsScript(e.Name); args(); return e; }
		if (const Json* v = J.Find("$op")) { e.K = Expr::Op; e.Name = v->s; args(); return e; }
	}
	e.Value = FromJson(J);
	return e;
}

static DrawCall CompileCall(const Json& J)
{
	DrawCall c;
	c.Fn = J.GetStr("fn");
	if (c.Fn.empty()) Fail("draw call without 'fn'");
	c.Script = IsScript(c.Fn);
	if (const Json* a = J.Find("args")) for (auto& x : a->a) c.Args.push_back(CompileExpr(x));
	if (const Json* w = J.Find("when")) { c.HasWhen = true; c.When = CompileExpr(*w); }
	return c;
}

// {"vars": [[name, expr], ...], "when": expr, "calls": [{"fn", "args", "when"}, ...]}
static Program CompileProgram(const Json& J)
{
	Program p;
	if (const Json* vars = J.Find("vars"))
		for (auto& v : vars->a)
		{
			if (v.type != Json::Arr || v.a.size() != 2 || v.a[0].type != Json::Str) Fail("'vars' entries are [name, expr]");
			p.Vars.emplace_back(v.a[0].s, CompileExpr(v.a[1]));
		}
	if (const Json* w = J.Find("when")) { p.HasWhen = true; p.When = CompileExpr(*w); }
	if (const Json* c = J.Find("calls"))
		for (auto& item : c->a) p.Calls.push_back(CompileCall(item));
	for (auto& c : p.Calls) p.Widgets |= c.Fn == "$button" || c.Fn == "$radio" || c.Fn == "$selector";
	return p;
}

static bool IsNumeric(const RValue& V)
{
	auto k = KindOf(V);
	return k == VALUE_REAL || k == VALUE_INT32 || k == VALUE_INT64 || k == VALUE_BOOL;
}
static double Num(const RValue& V) { return IsNumeric(V) ? V.ToDouble() : 0.0; }
static bool Truthy(const RValue& V)
{
	if (IsUndef(V)) return false;
	if (KindOf(V) == VALUE_STRING) return !V.ToString().empty();
	return IsNumeric(V) ? V.ToDouble() > 0.5 : true;
}
static bool SameValue(const RValue& A, const RValue& B)
{
	bool sa = KindOf(A) == VALUE_STRING, sb = KindOf(B) == VALUE_STRING;
	if (sa || sb) return sa && sb && A.ToString() == B.ToString();
	if (IsUndef(A) || IsUndef(B)) return IsUndef(A) && IsUndef(B);
	return Num(A) == Num(B);
}

static size_t ArrayLen(const RValue& Arr)
{
	if (KindOf(Arr) != VALUE_ARRAY) return 0;
	return static_cast<size_t>(g_Yytk->CallBuiltin("array_length", { Arr }).ToDouble());
}
static RValue* ArrayAt(RValue& Arr, size_t I)
{
	if (KindOf(Arr) != VALUE_ARRAY || I >= ArrayLen(Arr)) return nullptr;
	RValue* e = nullptr;
	return AurieSuccess(g_Yytk->GetArrayEntry(Arr, I, e)) ? e : nullptr;
}
static RValue SelfVar(CInstance* Self, const std::string& Name)
{
	if (!Self) return RValue();
	return g_Yytk->CallBuiltin("variable_instance_get", { RValue(Self), RValue(std::string_view(Name)) });
}

// First entry of self.mba whose [0] == Name (nullptr if none).
static RValue* MbaFind(RValue& Mba, const RValue& Name, size_t From = 0)
{
	for (size_t k = From, n = ArrayLen(Mba); k < n; ++k)
	{
		RValue* e = ArrayAt(Mba, k);
		RValue* n0 = e ? ArrayAt(*e, 0) : nullptr;
		if (n0 && SameValue(*n0, Name)) return e;
	}
	return nullptr;
}

static RValue Eval(const Expr& E, EvalCtx& C);

static RValue CallFn(const std::string& Fn, bool Script, std::vector<RValue>& Args, EvalCtx& C)
{
	RValue r;
	AurieStatus st = Script ? g_Yytk->CallGameScriptEx(r, Fn, C.Self, C.Other, Args)
		: g_Yytk->CallBuiltinEx(r, Fn.c_str(), C.Self, C.Other, Args);
	if (!AurieSuccess(st)) Fail("draw call " + Fn + " failed: " + AurieStatusToString(st));
	return r;
}

static RValue EvalOp(const Expr& E, EvalCtx& C)
{
	const std::string& op = E.Name;
	auto a = [&](size_t i) { return i < E.Args.size() ? Eval(E.Args[i], C) : RValue(); };
	if (op == "and") { for (auto& x : E.Args) if (!Truthy(Eval(x, C))) return RValue(false); return RValue(true); }
	if (op == "or") { for (auto& x : E.Args) if (Truthy(Eval(x, C))) return RValue(true); return RValue(false); }
	if (op == "not") return RValue(!Truthy(a(0)));
	if (op == "?") return Truthy(a(0)) ? a(1) : a(2);
	if (op == "??") { for (auto& x : E.Args) { RValue v = Eval(x, C); if (!IsUndef(v)) return v; } return RValue(); }
	if (op == "def") return RValue(!IsUndef(a(0)));
	if (op == "==") return RValue(SameValue(a(0), a(1)));
	if (op == "!=") return RValue(!SameValue(a(0), a(1)));
	if (op == "in_rect")   // px, py, x, y, w, h
	{
		double px = Num(a(0)), py = Num(a(1)), x = Num(a(2)), y = Num(a(3)), w = Num(a(4)), h = Num(a(5));
		return RValue(px >= x && px < x + w && py >= y && py < y + h);
	}
	if (op == "mba")       // name, index: field of the first self.mba entry with that name (undefined if none)
	{
		RValue mba = SelfVar(C.Self, "mba");
		RValue* e = MbaFind(mba, a(0));
		RValue* f = e ? ArrayAt(*e, static_cast<size_t>(Num(a(1)))) : nullptr;
		return f ? *f : RValue();
	}
	if (op == "+" || op == "*")
	{
		std::vector<RValue> v;
		bool str = false;
		for (auto& x : E.Args) { v.push_back(Eval(x, C)); str |= KindOf(v.back()) == VALUE_STRING; }
		if (op == "+" && str)
		{
			std::string s;
			for (auto& x : v) s += GmlString(x);
			return RValue(std::string_view(s));
		}
		double r = op == "+" ? 0.0 : 1.0;
		for (auto& x : v) r = op == "+" ? r + Num(x) : r * Num(x);
		return RValue(r);
	}
	double x = Num(a(0)), y = Num(a(1));
	if (op == "-") return RValue(E.Args.size() == 1 ? -x : x - y);
	if (op == "/") return RValue(y != 0 ? x / y : 0.0);
	if (op == "<") return RValue(x < y);
	if (op == "<=") return RValue(x <= y);
	if (op == ">") return RValue(x > y);
	if (op == ">=") return RValue(x >= y);
	Fail("unknown op '" + op + "'");
}

static RValue Eval(const Expr& E, EvalCtx& C)
{
	switch (E.K)
	{
	case Expr::Const: return E.Value;
	case Expr::Var: { auto it = C.Vals.find(E.Name); return it == C.Vals.end() ? RValue() : it->second; }
	case Expr::SelfVar: return SelfVar(C.Self, E.Name);
	case Expr::GlobalVar: return g_Yytk->CallBuiltin("variable_global_get", { RValue(std::string_view(E.Name)) });
	case Expr::Arg: return (E.Index >= 0 && E.Index < C.ArgCount && C.Args && C.Args[E.Index]) ? *C.Args[E.Index] : RValue();
	case Expr::Arr: { std::vector<RValue> v; for (auto& x : E.Args) v.push_back(Eval(x, C)); return RValue(v); }
	case Expr::Fn: { std::vector<RValue> v; for (auto& x : E.Args) v.push_back(Eval(x, C)); return CallFn(E.Name, E.Script, v, C); }
	case Expr::Op: return EvalOp(E, C);
	}
	return RValue();
}

// ---- Widgets: the game's own controls in one call each. A click arrives as the event "ui_clicked" {owner, id, action}
// (owner = the program's owner): $button [id, x, y, text, sprite?, tooltip?] (sprite: default the thin grey menu button),
// $radio [id, x, y, label, on, tooltip?] (action "toggle"), $selector [id, x, y, label, value, tooltip?] (actions "prev"
// / "next"; x, y as the game's settings use them), $text [x, y, text, font?, color?, halign?].

static void MbaPushEntry(CInstance* Self, RValue Entry);   // below RunProgram's helpers

static RValue RStr(const std::string& S) { return RValue(std::string_view(S)); }

// The game lays its menus out for 1920x1080 and multiplies every position and size by self.ui_scale (the resolution)
// when it draws; widgets take layout coordinates and do the same.
static double UiScale(EvalCtx& C)
{
	if (C.Scale <= 0)
	{
		double s = Num(SelfVar(C.Self, "ui_scale"));
		C.Scale = s > 0 ? s : 1.0;
	}
	return C.Scale;
}

static void WidgetText(EvalCtx& C, double X, double Y, const std::string& S, const std::string& Font, const std::string& Color,
	double Ha = 0, double Va = 1)
{
	double s = UiScale(C);   // scr_draw_text sizes the font itself
	std::vector<RValue> a = { RValue(X * s), RValue(Y * s), RStr(S), RValue(1.0), RValue(1.0), RStr(Font), RValue(Ha), RValue(Va),
		RValue(0.0), RValue(1.0), RStr(Color), RStr("black"), RValue(1.0), RValue(false) };
	CallFn("gml_Script_scr_draw_text", true, a, C);
}

static void WidgetSprite(EvalCtx& C, const RValue& Spr, double Frame, double X, double Y)
{
	double s = UiScale(C);
	std::vector<RValue> a = { Spr, RValue(Frame), RValue(X * s), RValue(Y * s), RValue(s), RValue(s), RValue(0.0),
		RValue(static_cast<double>(0xFFFFFF)), RValue(1.0) };
	CallFn("draw_sprite_ext", false, a, C);
}

static void WidgetRect(EvalCtx& C, double X1, double Y1, double X2, double Y2, double Color)
{
	std::vector<RValue> c = { RValue(Color) };
	CallFn("draw_set_color", false, c, C);
	double s = UiScale(C);
	std::vector<RValue> r = { RValue(X1 * s), RValue(Y1 * s), RValue(X2 * s), RValue(Y2 * s), RValue(false) };
	CallFn("draw_rectangle", false, r, C);
	std::vector<RValue> w = { RValue(static_cast<double>(0xFFFFFF)) };
	CallFn("draw_set_color", false, w, C);
}

// an entry of self.mba: [name, sprite, x, y, w, h, text, script, argument, tooltip]
static void WidgetEntry(EvalCtx& C, const std::string& Owner, const std::string& Id, const std::string& Action, const RValue& Spr,
	double X, double Y, double W, double H, const RValue& Text, const RValue& Tooltip)
{
	double s = UiScale(C);
	RValue e = g_Yytk->CallBuiltin("array_create", { RValue(10.0), RValue(0.0) });
	RValue fields[10] = { RStr("ui_" + Id), Spr, RValue(X * s), RValue(Y * s), RValue(W * s), RValue(H * s), Text,
		AssetIndex("scr_blacksmith_open_filter_from_inventory"), RStr("slormapi:ui:" + Owner + "|" + Id + "|" + Action), Tooltip };
	for (int k = 0; k < 10; ++k) g_Yytk->CallBuiltin("array_set", { e, RValue(static_cast<double>(k)), fields[k] });
	MbaPushEntry(C.Self, e);
}

static void RunWidget(const std::string& Fn, std::vector<RValue>& A, const std::string& Owner, EvalCtx& C)
{
	auto num = [&](size_t I) { return I < A.size() ? Num(A[I]) : 0.0; };
	auto str = [&](size_t I, const char* Def = "") { return I < A.size() && KindOf(A[I]) == VALUE_STRING ? GmlString(A[I]) : std::string(Def); };
	auto tooltip = [&](size_t I) { return I < A.size() && KindOf(A[I]) == VALUE_STRING ? A[I] : RStr(""); };
	if (Fn == "$text")
	{
		if (A.size() < 3) Fail("$text takes [x, y, text, font?, color?, halign?]");
		WidgetText(C, num(0), num(1), KindOf(A[2]) == VALUE_STRING ? GmlString(A[2]) : GmlString(g_Yytk->CallBuiltin("string", { A[2] })),
			str(3, "ft_standard_12"), str(4, "white"), num(5));
		return;
	}
	if (A.size() < 4) Fail(Fn + " takes [id, x, y, ...]");
	std::string id = KindOf(A[0]) == VALUE_STRING ? GmlString(A[0]) : GmlString(g_Yytk->CallBuiltin("string", { A[0] }));
	double x = num(1), y = num(2);
	if (Fn == "$button")
	{
		RValue spr = A.size() > 4 && IsNumeric(A[4]) ? A[4] : AssetIndex("spr_menu_thin_grey_button");
		double w = g_Yytk->CallBuiltin("sprite_get_width", { spr }).ToDouble(), h = g_Yytk->CallBuiltin("sprite_get_height", { spr }).ToDouble();
		WidgetSprite(C, spr, 0, x, y);
		WidgetText(C, x + w / 2, y + h / 2, str(3), "ft_standard_12", "white", 1, 1);
		WidgetEntry(C, Owner, id, "click", spr, x, y, w, h, A[3], tooltip(5));
	}
	else if (Fn == "$radio")
	{
		bool on = A.size() > 4 && Truthy(A[4]);
		RValue spr = AssetIndex("spr_radio_btn_v3");
		WidgetSprite(C, spr, 0, x, y - 4);
		if (on) WidgetSprite(C, AssetIndex("spr_radio_cross_v2"), 0, x + 18, y + 14);
		WidgetText(C, x + 48, y + 14, str(3), "ft_standard_12", "white");
		WidgetEntry(C, Owner, id, "toggle", spr, x, y - 4, 36, 36, on ? AssetIndex("spr_radio_cross_v2") : RStr(""), tooltip(5));
	}
	else if (Fn == "$selector")
	{
		WidgetText(C, x + 48, y, str(3), "ft_standard_12", "white");
		WidgetRect(C, x + 19, y + 18, x + 278, y + 54, 0x020202);
		WidgetRect(C, x + 23, y + 22, x + 274, y + 50, 0x101010);
		std::string value = A.size() > 4 ? (KindOf(A[4]) == VALUE_STRING ? GmlString(A[4]) : GmlString(g_Yytk->CallBuiltin("string", { A[4] }))) : "";
		WidgetText(C, x + 48, y + 30, value, "ft_standard_12", "grey");
		RValue l = AssetIndex("spr_selector_btn_l_v2"), r = AssetIndex("spr_selector_btn_r_v2");
		WidgetSprite(C, l, 0, x - 8, y + 12);
		WidgetSprite(C, r, 0, x + 288, y + 12);
		WidgetEntry(C, Owner, id, "prev", l, x - 8, y + 12, 24, 36, RStr(""), tooltip(5));
		WidgetEntry(C, Owner, id, "next", r, x + 288, y + 12, 24, 36, RStr(""), tooltip(5));
	}
}

// appends Entry to self.mba, or replaces the entry with the same name and position
static void MbaPushEntry(CInstance* Self, RValue Entry)
{
	RValue mba = SelfVar(Self, "mba");
	if (KindOf(mba) != VALUE_ARRAY) return;
	size_t at = ArrayLen(mba);
	for (size_t k = 0; k < ArrayLen(mba); ++k)
	{
		RValue* e = ArrayAt(mba, k);
		bool same = e && KindOf(Entry) == VALUE_ARRAY;
		for (size_t f : { 0, 2, 3 })
		{
			RValue* a = same ? ArrayAt(*e, f) : nullptr;
			RValue* b = same ? ArrayAt(Entry, f) : nullptr;
			same = a && b && SameValue(*a, *b);
		}
		if (same) { at = k; break; }
	}
	g_Yytk->CallBuiltin("array_set", { mba, RValue(static_cast<double>(at)), Entry });
}

// drops Owner's widget entries from Mba (the game keeps entries it did not make across its own redraws)
static void MbaRemoveOwner(RValue& Mba, const std::string& Owner)
{
	std::string prefix = "slormapi:ui:" + Owner + "|";
	for (size_t k = ArrayLen(Mba); k-- > 0;)
	{
		RValue* e = ArrayAt(Mba, k);
		RValue* arg = e ? ArrayAt(*e, 8) : nullptr;
		if (arg && KindOf(*arg) == VALUE_STRING && GmlString(*arg).starts_with(prefix))
			g_Yytk->CallBuiltin("array_delete", { Mba, RValue(static_cast<double>(k)), RValue(1.0) });
	}
}

static void RunProgram(const Program& P, EvalCtx& C)
{
	if (P.Widgets && C.Self)
	{
		RValue mba = SelfVar(C.Self, "mba");
		if (KindOf(mba) == VALUE_ARRAY) MbaRemoveOwner(mba, P.Owner);   // this run adds the current ones again
	}
	for (auto& [name, e] : P.Vars) C.Vals[name] = Eval(e, C);
	if (P.HasWhen && !Truthy(Eval(P.When, C))) return;
	for (auto& c : P.Calls)
	{
		if (c.HasWhen && !Truthy(Eval(c.When, C))) continue;
		std::vector<RValue> args;
		for (auto& x : c.Args) args.push_back(Eval(x, C));
		if (c.Fn == "$mba_set")          // name, index, value
		{
			if (args.size() != 3) Fail("$mba_set takes [name, index, value]");
			RValue mba = SelfVar(C.Self, "mba");
			size_t idx = static_cast<size_t>(Num(args[1]));
			for (size_t k = 0, n = ArrayLen(mba); k < n; ++k)
			{
				RValue* e = ArrayAt(mba, k);
				RValue* n0 = e ? ArrayAt(*e, 0) : nullptr;
				if (!n0 || !SameValue(*n0, args[0])) continue;
				if (RValue* f = ArrayAt(*e, idx)) *f = args[2];
				else g_Yytk->CallBuiltin("array_set", { *e, RValue(static_cast<double>(idx)), args[2] });
			}
		}
		else if (c.Fn == "$mba_push")    // entry (replaces an entry with the same name and position)
		{
			if (args.size() == 1) MbaPushEntry(C.Self, args[0]);
		}
		else if (c.Fn == "$button" || c.Fn == "$radio" || c.Fn == "$selector" || c.Fn == "$text") RunWidget(c.Fn, args, P.Owner, C);
		else if (c.Fn == "$mba_remove")  // index, value: drops the self.mba entries whose [index] == value
		{
			if (args.size() != 2) Fail("$mba_remove takes [index, value]");
			RValue mba = SelfVar(C.Self, "mba");
			size_t idx = static_cast<size_t>(Num(args[0]));
			for (size_t k = ArrayLen(mba); k-- > 0;)
			{
				RValue* e = ArrayAt(mba, k);
				RValue* f = e ? ArrayAt(*e, idx) : nullptr;
				if (f && SameValue(*f, args[1]))
					g_Yytk->CallBuiltin("array_delete", { mba, RValue(static_cast<double>(k)), RValue(1.0) });
			}
		}
		else if (c.Fn == "$mba_remove_in")   // x1, y1, x2, y2: drops the self.mba entries placed inside
		{
			if (args.size() != 4) Fail("$mba_remove_in takes [x1, y1, x2, y2]");
			RValue mba = SelfVar(C.Self, "mba");
			for (size_t k = ArrayLen(mba); k-- > 0;)
			{
				RValue* e = ArrayAt(mba, k);
				RValue* ex = e ? ArrayAt(*e, 2) : nullptr;
				RValue* ey = e ? ArrayAt(*e, 3) : nullptr;
				if (ex && ey && Num(*ex) >= Num(args[0]) && Num(*ey) >= Num(args[1]) && Num(*ex) <= Num(args[2]) && Num(*ey) <= Num(args[3]))
					g_Yytk->CallBuiltin("array_delete", { mba, RValue(static_cast<double>(k)), RValue(1.0) });
			}
		}
		else if (c.Fn == "$set")         // var, value
		{
			if (C.Self && args.size() == 2) g_Yytk->CallBuiltin("variable_instance_set", { RValue(C.Self), args[0], args[1] });
		}
		else CallFn(c.Fn, c.Script, args, C);
	}
}

// ============================================================================
// Script hooks: a fixed pool of detours, each bound to one slot. A slot holds one layer per owner (mods pass
// "owner": their name; TCP / dev clients use ""), so several mods can hook the same script without replacing each
// other's overrides or programs, and each owner reads only the calls it captured.
// ============================================================================

struct HookEntry
{
	uint64_t Seq;
	std::string Script;
	double StartMicros;
	int CallDepth;
	Json Self;
	Json Args;
	Json Result;
	double Micros;
	Json Stack;
};

struct HookLayer
{
	std::string Owner;
	bool Capture = true;
	std::vector<Json> CaptureArgs;   // log only calls whose arguments match (empty = every call)
	bool Stack = false;              // record debug_get_callstack() per call
	int Depth = 1;
	// After the original returns, set these variables on self (e.g. input flags after scr_get_input).
	std::vector<std::pair<std::string, RValue>> PostSet;
	int64_t PostSetRemaining = -1;   // -1 = unlimited
	// When the call's arguments match (null pattern entries match anything, "abc*" matches a prefix): replace the
	// return value after the original; or, with Skip, do not run the original at all (return Value); or, with
	// SetArgs, run the original with those arguments replaced (on a copy).
	struct Override
	{
		std::vector<Json> Pattern;
		RValue Value;
		bool Skip = false;
		std::vector<std::pair<size_t, RValue>> SetArgs;
		bool Pre() const { return Skip || !SetArgs.empty(); }
	};
	std::vector<Override> Overrides;
	// Variables read from self when a call is captured (e.g. a projectile's frame counter and angles).
	std::vector<std::string> SelfVars;
	// Draw program run after every call (also after a skipped one), e.g. extra drawing inside a menu bake.
	std::shared_ptr<const Program> After;
	double Expiry = 0;   // "ttl_ms" deadline in MicrosSinceStart() (0 = none): after it the layer only passes through
	bool InAfter = false;
	std::string AfterError;
	bool Live() const { return Expiry <= 0 || MicrosSinceStart() <= Expiry; }
};

struct HookSlot
{
	std::string Name;
	PVOID Trampoline = nullptr;
	uint64_t Calls = 0;
	std::vector<HookLayer> Layers;
	HookLayer* Layer(const std::string& Owner)
	{
		for (auto& l : Layers)
			if (l.Owner == Owner) return &l;
		return nullptr;
	}
};

static bool ArgMatches(const Json& Pattern, const RValue* Arg)
{
	if (Pattern.IsNull()) return true;
	if (!Arg) return false;
	if (Pattern.type == Json::Str)
	{
		if (KindOf(*Arg) != VALUE_STRING) return false;
		if (!Pattern.s.empty() && Pattern.s.back() == '*')
			return Arg->ToString().starts_with(std::string_view(Pattern.s).substr(0, Pattern.s.size() - 1));
		return Arg->ToString() == Pattern.s;
	}
	if (Pattern.type == Json::Num) return KindOf(*Arg) != VALUE_STRING && Arg->ToDouble() == Pattern.n;
	if (Pattern.type == Json::Bool) return Arg->ToBoolean() == Pattern.b;
	return false;
}

static bool PatternMatches(const std::vector<Json>& Pattern, int ArgCount, RValue* Args[])
{
	for (size_t k = 0; k < Pattern.size(); ++k)
		if (!ArgMatches(Pattern[k], static_cast<int>(k) < ArgCount ? Args[k] : nullptr)) return false;
	return true;
}

static const HookLayer::Override* MatchPre(const HookLayer& Layer, int ArgCount, RValue* Args[])
{
	for (auto& ov : Layer.Overrides)
		if (ov.Pre() && PatternMatches(ov.Pattern, ArgCount, Args)) return &ov;
	return nullptr;
}

static void RunAfter(std::vector<HookLayer>& Layers, size_t I, CInstance* Self, CInstance* Other, int ArgCount, RValue* Args[])
{
	auto prog = Layers[I].After;
	if (!prog || Layers[I].InAfter) return;
	Layers[I].InAfter = true;
	std::string error;
	try
	{
		EvalCtx c{ Self, Other, ArgCount, Args, {} };
		RunProgram(*prog, c);
	}
	catch (const std::exception& e) { error = e.what(); }
	catch (...) { error = "unknown exception"; }
	if (I >= Layers.size()) return;                     // the program's calls removed layers
	HookLayer& l = Layers[I];
	l.InAfter = false;
	if (!error.empty() && l.After == prog) { l.AfterError = error; l.After.reset(); }
}

static constexpr size_t kMaxHooks = 256;
static std::array<HookSlot, kMaxHooks> g_Hooks;
static std::map<std::string, std::deque<HookEntry>> g_HookLogs;   // per owner
static size_t g_HookLogCap = 100000;
static uint64_t g_HookSeq = 0;
static uint64_t g_HookDropped = 0;
static bool g_InCapture = false;
static int g_CallDepth = 0;
static int g_DetourDepth = 0;   // > 0 while a hooked call runs: TCP requests wait, slots are not freed

template <size_t N>
static RValue& HookDetour(CInstance* Self, CInstance* Other, RValue& Result, int ArgCount, RValue* Args[])
{
	HookSlot& slot = g_Hooks[N];
	auto original = reinterpret_cast<PFUNC_YYGMLScript>(slot.Trampoline);
	slot.Calls++;
	if (slot.Layers.empty() || g_InCapture) return original(Self, Other, Result, ArgCount, Args);
	const bool perfOn = g_Perf;
	const double perf0 = perfOn ? PerfNowUs() : 0;

	// the first matching skip / set_args override of any layer (copied: a call may change the layers meanwhile)
	std::optional<HookLayer::Override> pre;
	for (auto& l : slot.Layers)
		if (l.Live())
			if (const auto* o = MatchPre(l, ArgCount, Args)) { pre = *o; break; }
	std::vector<RValue> argCopy;
	std::vector<RValue*> argPtrs;
	if (pre && !pre->SetArgs.empty())
	{
		for (int k = 0; k < ArgCount; ++k) argCopy.push_back(Args[k] ? *Args[k] : RValue());
		for (auto& [i, v] : pre->SetArgs)
			if (i < argCopy.size()) argCopy[i] = v;
		for (auto& a : argCopy) argPtrs.push_back(&a);
		Args = argPtrs.data();
	}

	// the owners that log this call
	std::vector<std::string> owners, selfVars;
	bool stack = false;
	int depth = 0;
	for (auto& l : slot.Layers)
		if (l.Live() && l.Capture && PatternMatches(l.CaptureArgs, ArgCount, Args))
		{
			owners.push_back(l.Owner);
			stack |= l.Stack;
			depth = (std::max)(depth, l.Depth);
			for (auto& v : l.SelfVars)
				if (std::find(selfVars.begin(), selfVars.end(), v) == selfVars.end()) selfVars.push_back(v);
		}
	std::optional<HookEntry> entry;                      // built only for a call that some owner captures
	if (!owners.empty())
	{
		entry.emplace(HookEntry{ ++g_HookSeq, slot.Name, MicrosSinceStart(), g_CallDepth, Json(), Json::Array(), Json(), 0, Json() });
		HookEntry& e = *entry;
		g_InCapture = true;
		try
		{
			if (stack) e.Stack = ToJson(g_Yytk->CallBuiltin("debug_get_callstack", {}), 1);
			e.Self = Self ? ToJson(RValue(Self), 0) : Json();
			if (Self && !selfVars.empty())
			{
				Json vars = Json::Object();
				for (auto& name : selfVars)
					vars.Add(name, ToJson(g_Yytk->CallBuiltin("variable_instance_get", { RValue(Self), RValue(std::string_view(name)) }), 1));
				e.Self.Add("vars", vars);
			}
			for (int k = 0; k < ArgCount; ++k) e.Args.Push(Args[k] ? ToJson(*Args[k], depth) : Json());
		}
		catch (...) {}
		g_InCapture = false;
	}

	++g_DetourDepth;
	++g_CallDepth;
	const double perfA = perfOn ? PerfNowUs() : 0;
	auto t0 = std::chrono::steady_clock::now();
	RValue& r = (pre && pre->Skip) ? (Result = pre->Value, Result) : original(Self, Other, Result, ArgCount, Args);
	auto t1 = std::chrono::steady_clock::now();
	const double perfB = perfOn ? PerfNowUs() : 0;
	--g_CallDepth;

	// each layer in order: post_set, return-value overrides, its program (by index: a program may change the layers)
	for (size_t i = 0; i < slot.Layers.size(); ++i)
	{
		HookLayer& l = slot.Layers[i];
		if (!l.Live()) continue;
		if (!l.PostSet.empty() && Self && l.PostSetRemaining != 0)
		{
			for (auto& [name, value] : l.PostSet)
				g_Yytk->CallBuiltin("variable_instance_set", { RValue(Self), RValue(std::string_view(name)), value });
			if (l.PostSetRemaining > 0) l.PostSetRemaining--;
		}
		for (auto& ov : l.Overrides)
			if (!ov.Pre() && PatternMatches(ov.Pattern, ArgCount, Args)) { r = ov.Value; break; }
		RunAfter(slot.Layers, i, Self, Other, ArgCount, Args);
	}
	--g_DetourDepth;

	if (entry)
	{
		HookEntry& e = *entry;
		g_InCapture = true;
		try { e.Result = ToJson(r, depth); }
		catch (...) {}
		g_InCapture = false;
		e.Micros = std::chrono::duration<double, std::micro>(t1 - t0).count();
		for (size_t k = 0; k < owners.size(); ++k)
		{
			auto& log = g_HookLogs[owners[k]];
			if (k + 1 < owners.size()) log.push_back(e);
			else log.push_back(std::move(e));
			size_t cap = owners[k].empty() ? g_HookLogCap : (std::min)(g_HookLogCap, static_cast<size_t>(5000));
			while (log.size() > cap) { log.pop_front(); ++g_HookDropped; }
		}
	}
	if (perfOn && g_Perf) { g_PerfDetourUs += (perfA - perf0) + (PerfNowUs() - perfB); ++g_PerfDetours; }
	return r;
}

template <size_t... I>
static std::array<PVOID, sizeof...(I)> MakeDetourTable(std::index_sequence<I...>)
{
	return { reinterpret_cast<PVOID>(&HookDetour<I>)... };
}

static const std::array<PVOID, kMaxHooks>& DetourTable()
{
	static const auto table = MakeDetourTable(std::make_index_sequence<kMaxHooks>{});
	return table;
}

static int FindHook(const std::string& Script)
{
	for (size_t k = 0; k < kMaxHooks; ++k)
		if (g_Hooks[k].Trampoline && g_Hooks[k].Name == Script) return static_cast<int>(k);
	return -1;
}

// The slot of a script, detouring it on first use.
static size_t InstallHook(const std::string& Name)
{
	std::string script = ScriptName(Name);
	if (int existing = FindHook(script); existing >= 0) return static_cast<size_t>(existing);

	CScript* cs = nullptr;
	if (!AurieSuccess(g_Yytk->GetNamedRoutinePointer(script.c_str(), reinterpret_cast<PVOID*>(&cs))) || !cs)
		Fail("unknown script " + script);
	if (!cs->m_Functions || !cs->m_Functions->m_ScriptFunction)
		Fail(script + " has no compiled function");

	size_t slot = kMaxHooks;
	for (size_t k = 0; k < kMaxHooks; ++k)
		if (!g_Hooks[k].Trampoline) { slot = k; break; }
	if (slot == kMaxHooks) Fail("hook pool exhausted (" + std::to_string(kMaxHooks) + ")");

	HookSlot& s = g_Hooks[slot];
	s = HookSlot{};
	s.Name = script;
	AurieStatus st = MmCreateHook(
		g_ArSelfModule,
		"slorm_hook_" + std::to_string(slot),
		reinterpret_cast<PVOID>(cs->m_Functions->m_ScriptFunction),
		DetourTable()[slot],
		&s.Trampoline
	);
	if (!AurieSuccess(st) || !s.Trampoline)
	{
		s = HookSlot{};
		Fail(std::string("MmCreateHook failed: ") + AurieStatusToString(st));
	}
	return slot;
}

static void RemoveHookSlot(size_t Slot)
{
	MmRemoveHook(g_ArSelfModule, "slorm_hook_" + std::to_string(Slot));
	g_Hooks[Slot] = HookSlot{};
}

// ============================================================================
// Commands
// ============================================================================

static Json EventListeners();
static std::string GameVersion();
static Json LastSession();

static Json CmdPing()
{
	Json j = Json::Object();
	j.Add("version", kVersion);
	j.Add("frame", g_Frame);
	j.Add("code_events", g_CodeEvents);
	j.Add("wndproc_calls", g_WndProcCalls);
	j.Add("uptime_s", MicrosSinceStart() / 1e6);
	RValue room;
	if (AurieSuccess(g_Yytk->GetBuiltin("room", nullptr, NULL_INDEX, room)))
		j.Add("room", GmlString(g_Yytk->CallBuiltin("room_get_name", { room })));
	RValue fps;
	if (AurieSuccess(g_Yytk->GetBuiltin("fps_real", nullptr, NULL_INDEX, fps)))
		j.Add("fps_real", fps.ToDouble());
	j.Add("event_listeners", EventListeners());
	j.Add("game_version", GameVersion());
	j.Add("last_session", LastSession());
	return j;
}

static Json CmdGet(const Json& Req)
{
	RValue root = ResolveRoot(Req.Find("root"));
	auto path = ParsePath(Req.Find("path"));
	RValue holder;
	RValue* v = Walk(root, path, path.size(), holder);
	return ToJson(*v, static_cast<int>(Req.GetNum("depth", 1)));
}

static Json CmdKeys(const Json& Req)
{
	RValue root = ResolveRoot(Req.Find("root"));
	auto path = ParsePath(Req.Find("path"));
	RValue holder;
	RValue* v = Walk(root, path, path.size(), holder);
	if (KindOf(*v) != VALUE_OBJECT) Fail(PathString(path, path.size()) + " is not a struct or instance");
	Json out = Json::Array();
	RValue copy = *v;
	g_Yytk->EnumInstanceMembers(copy, [&](const char* Name, RValue*) -> bool { out.Push(Json(Name)); return false; });
	return out;
}

static Json CmdSet(const Json& Req)
{
	RValue root = ResolveRoot(Req.Find("root"));
	auto path = ParsePath(Req.Find("path"));
	if (path.empty()) Fail("'path' is required");
	RValue value = FromJson(Req.At("value"));

	RValue holder;
	RValue* parent = Walk(root, path, path.size() - 1, holder);
	const PathTok& last = path.back();

	if (last.IsIndex)
	{
		if (KindOf(*parent) != VALUE_ARRAY) Fail(PathString(path, path.size() - 1) + " is not an array");
		RValue* e = nullptr;
		if (AurieSuccess(g_Yytk->GetArrayEntry(*parent, last.Index, e)) && e) *e = value;
		else g_Yytk->CallBuiltin("array_set", { *parent, RValue(static_cast<double>(last.Index)), value });
		return Json(true);
	}

	if (KindOf(*parent) != VALUE_OBJECT) Fail(PathString(path, path.size() - 1) + " is not a struct or instance");
	if (RValue* m = FindMember(*parent, last.Name)) { *m = value; return Json(true); }

	RValue name(std::string_view(last.Name));
	if (IsGlobal(*parent))
	{
		if (AurieSuccess(g_Yytk->SetBuiltin(last.Name, nullptr, NULL_INDEX, value))) return Json(true);
		g_Yytk->CallBuiltin("variable_global_set", { name, value });
		return Json(true);
	}
	if (ObjectKindOf(*parent) == OBJECT_KIND_CINSTANCE)
	{
		if (AurieSuccess(g_Yytk->SetBuiltin(last.Name, parent->m_Instance, NULL_INDEX, value))) return Json(true);
		g_Yytk->CallBuiltin("variable_instance_set", { *parent, name, value });
		return Json(true);
	}
	g_Yytk->CallBuiltin("variable_struct_set", { *parent, name, value });
	return Json(true);
}

// ---- overlay: a draw program run right after one object event (default: the game's own GUI draw), so a client
// can draw with the game's sprites and fonts, positioned at draw time. {"cmd":"overlay","event":"gml_Object_...",
// "vars":[[name, expr]], "when": expr, "calls":[{"fn":"draw_set_font","args":[3]}, ...], "ttl_ms": 3000};
// "calls":[] clears; "ttl_ms" alone renews (the overlay clears itself when its client stops renewing it).
static std::string g_OverlayEvent = "gml_Object_obj_ui_next_gen_Draw_64";
static std::shared_ptr<const Program> g_Overlay;
static double g_OverlayExpiry = 0;
static bool g_InOverlay = false;
static uint64_t g_OverlayFrames = 0;
static std::string g_OverlayError;

static Json CmdOverlay(const Json& Req)
{
	if (const Json* ev = Req.Find("event"); ev && ev->type == Json::Str) g_OverlayEvent = ev->s;
	if (const Json* c = Req.Find("calls"); c && c->type == Json::Arr)    // no "calls": just report the state
	{
		if (c->a.empty()) g_Overlay.reset();
		else
		{
			Program prog = CompileProgram(Req);
			prog.Owner = Req.GetStr("owner");
			g_Overlay = std::make_shared<const Program>(std::move(prog));
		}
		g_OverlayError.clear();
		g_OverlayExpiry = 0;
	}
	if (const Json* ttl = Req.Find("ttl_ms")) g_OverlayExpiry = MicrosSinceStart() + ttl->n * 1000.0;
	Json j = Json::Object();
	j.Add("calls", static_cast<int64_t>(g_Overlay ? g_Overlay->Calls.size() : 0));
	j.Add("event", g_OverlayEvent);
	j.Add("frames_drawn", static_cast<int64_t>(g_OverlayFrames));
	if (!g_OverlayError.empty()) j.Add("error", g_OverlayError);
	return j;
}

static void DrawOverlay(CInstance* Self, CInstance* Other)
{
	if (g_OverlayExpiry > 0 && MicrosSinceStart() > g_OverlayExpiry) { g_Overlay.reset(); return; }
	auto prog = g_Overlay;
	g_InOverlay = true;
	try
	{
		EvalCtx c{ Self, Other, 0, nullptr, {} };
		RunProgram(*prog, c);
	}
	catch (const std::exception& e) { g_OverlayError = e.what(); g_Overlay.reset(); }   // a bad call stops it
	catch (...) { g_OverlayError = "unknown exception"; g_Overlay.reset(); }
	++g_OverlayFrames;
	g_InOverlay = false;
}

static Json CmdCall(const Json& Req)
{
	std::string fn = Req.GetStr("fn");
	if (fn.empty()) Fail("'fn' is required");
	std::vector<RValue> args = ArgsFromJson(Req.Find("args"));
	RValue self = ResolveRoot(Req.Find("self"));
	CInstance* selfInst = self.m_Instance;

	RValue result;
	AurieStatus st;
	std::string script = ScriptName(fn);
	PVOID routine = nullptr;
	if (AurieSuccess(g_Yytk->GetNamedRoutinePointer(script.c_str(), &routine)))
		st = g_Yytk->CallGameScriptEx(result, script, selfInst, selfInst, args);
	else
		st = g_Yytk->CallBuiltinEx(result, fn.c_str(), selfInst, selfInst, args);
	if (!AurieSuccess(st)) Fail("call " + fn + " failed: " + AurieStatusToString(st));
	return ToJson(result, static_cast<int>(Req.GetNum("depth", 1)));
}

static Json CmdInstances(const Json& Req)
{
	const Json* vars = Req.Find("vars");
	Json out = Json::Array();
	for (CInstance* inst : InstancesOf(ObjectRef(Req.At("object"))))
	{
		Json j = InstanceStub(inst);
		if (vars && vars->type == Json::Arr)
		{
			RValue iv(inst);
			for (auto& name : vars->a)
			{
				RValue b;
				if (RValue* m = FindMember(iv, name.s)) j.Add(name.s, ToJson(*m, 0));
				else if (ReadBuiltin(iv, name.s, b)) j.Add(name.s, ToJson(b, 0));
				else j.Add(name.s, Json());
			}
		}
		out.Push(std::move(j));
	}
	return out;
}

static Json CmdObjects()
{
	Json out = Json::Array();
	for (int k = 0; k < 100000; ++k)
	{
		if (!g_Yytk->CallBuiltin("object_exists", { RValue(static_cast<double>(k)) }).ToBoolean()) break;
		out.Push(GmlString(g_Yytk->CallBuiltin("object_get_name", { RValue(static_cast<double>(k)) })));
	}
	return out;
}

static Json CmdScripts()
{
	Json out = Json::Array();
	for (int k = 0; k < 1000000; ++k)
	{
		CScript* cs = nullptr;
		if (!AurieSuccess(g_Yytk->GetScriptData(k, cs))) break;
		out.Push(cs && cs->GetName() ? Json(cs->GetName()) : Json());
	}
	return out;
}

// scripts an owner could not hook (e.g. a game update renamed them): owner -> script -> error
static std::map<std::string, std::map<std::string, std::string>> g_HookErrors;

// "owner": the caller's layer (default ""). Hooking again replaces only that owner's layer of the script.
static Json CmdHook(const Json& Req)
{
	std::string owner = Req.GetStr("owner");
	Json out = Json::Object();
	auto install = [&](const std::string& name)
	{
		try
		{
			HookLayer l;
			l.Owner = owner;
			l.Capture = Req.GetBool("capture", true);
			l.Stack = Req.GetBool("stack", false);
			l.Depth = static_cast<int>(Req.GetNum("depth", 1));
			if (const Json* ca = Req.Find("capture_args")) l.CaptureArgs = ca->a;
			if (const Json* ps = Req.Find("post_set"))
				for (auto& [k, v] : ps->o) l.PostSet.emplace_back(k, FromJson(v));
			l.PostSetRemaining = static_cast<int64_t>(Req.GetNum("post_set_count", -1));
			if (const Json* sv = Req.Find("self_vars"))
				for (auto& v : sv->a) l.SelfVars.push_back(v.s);
			if (const Json* ovs = Req.Find("overrides"))
				for (auto& ov : ovs->a)
				{
					HookLayer::Override o;
					if (const Json* a = ov.Find("args")) o.Pattern = a->a;
					if (const Json* v = ov.Find("value")) o.Value = FromJson(*v);
					o.Skip = ov.GetBool("skip", false);
					if (const Json* sa = ov.Find("set_args"))
						for (auto& [k, v] : sa->o) o.SetArgs.emplace_back(static_cast<size_t>(std::stoul(k)), FromJson(v));
					l.Overrides.push_back(std::move(o));
				}
			if (const Json* af = Req.Find("after"); af && af->type == Json::Obj)
			{
				Program prog = CompileProgram(*af);
				prog.Owner = owner;
				l.After = std::make_shared<const Program>(std::move(prog));
			}
			// "ttl_ms": overrides, post_set and "after" stop applying unless the owner renews the hook in time
			if (const Json* ttl = Req.Find("ttl_ms")) l.Expiry = MicrosSinceStart() + ttl->n * 1000.0;

			size_t slot = InstallHook(name);
			HookSlot& hs = g_Hooks[slot];
			if (HookLayer* old = hs.Layer(owner)) *old = std::move(l);
			else hs.Layers.push_back(std::move(l));
			out.Add(ScriptName(name), static_cast<int>(slot));
			if (auto it = g_HookErrors.find(owner); it != g_HookErrors.end()) it->second.erase(ScriptName(name));
		}
		catch (const std::exception& e)
		{
			out.Add(ScriptName(name), Json(std::string("error: ") + e.what()));
			g_HookErrors[owner][ScriptName(name)] = e.what();
		}
	};
	if (const Json* s = Req.Find("script")) install(s->s);
	if (const Json* list = Req.Find("scripts"))
		for (auto& s : list->a) install(s.s);
	return out;
}

// Removes the caller's layers ("owner", default "") of one script ("script") or of all; "all": true removes every
// owner's. A script left without layers is un-detoured.
static Json CmdUnhook(const Json& Req)
{
	Json out = Json::Array();
	const Json* script = Req.Find("script");
	std::string only = script && script->type == Json::Str ? ScriptName(script->s) : "";
	std::string owner = Req.GetStr("owner");
	bool all = Req.GetBool("all", false);
	for (size_t k = 0; k < kMaxHooks; ++k)
	{
		HookSlot& s = g_Hooks[k];
		if (!s.Trampoline || (!only.empty() && s.Name != only)) continue;
		size_t removed = std::erase_if(s.Layers, [&](const HookLayer& l) { return all || l.Owner == owner; });
		if (!removed) continue;
		out.Push(Json(s.Name));
		if (s.Layers.empty() && g_DetourDepth == 0) RemoveHookSlot(k);
	}
	return out;
}

static Json CmdHooks()
{
	Json out = Json::Array();
	for (size_t k = 0; k < kMaxHooks; ++k)
	{
		const HookSlot& s = g_Hooks[k];
		if (!s.Trampoline) continue;
		Json j = Json::Object();
		j.Add("slot", k);
		j.Add("script", s.Name);
		j.Add("calls", s.Calls);
		Json layers = Json::Array();
		for (auto& l : s.Layers)
		{
			Json lj = Json::Object();
			lj.Add("owner", l.Owner);
			lj.Add("capture", l.Capture);
			lj.Add("depth", l.Depth);
			lj.Add("overrides", l.Overrides.size());
			if (l.After) lj.Add("after", true);
			if (!l.AfterError.empty()) lj.Add("after_error", l.AfterError);
			if (!l.Live()) lj.Add("expired", true);
			layers.Push(std::move(lj));
		}
		j.Add("layers", std::move(layers));
		out.Push(std::move(j));
	}
	return out;
}

// The calls captured for "owner" (default ""), oldest first.
static Json CmdHookLog(const Json& Req)
{
	size_t max = static_cast<size_t>(Req.GetNum("max", 10000));
	bool consume = Req.GetBool("consume", true);
	if (const Json* cap = Req.Find("cap")) g_HookLogCap = static_cast<size_t>(cap->n);
	auto& log = g_HookLogs[Req.GetStr("owner")];

	Json entries = Json::Array();
	size_t n = (std::min)(max, log.size());
	for (size_t k = 0; k < n; ++k)
	{
		const HookEntry& e = log[k];
		Json j = Json::Object();
		j.Add("seq", e.Seq);
		j.Add("script", e.Script);
		j.Add("t_us", e.StartMicros);
		j.Add("depth", e.CallDepth);
		j.Add("self", e.Self);
		j.Add("args", e.Args);
		j.Add("result", e.Result);
		j.Add("us", e.Micros);
		if (!e.Stack.IsNull()) j.Add("stack", e.Stack);
		entries.Push(std::move(j));
	}
	if (consume) log.erase(log.begin(), log.begin() + n);

	Json out = Json::Object();
	out.Add("entries", std::move(entries));
	out.Add("remaining", log.size());
	out.Add("dropped", g_HookDropped);
	return out;
}

#define SLORM_RUNNER_FIELDS(F) \
	F(DebugConsoleOutput) \
	F(ReleaseConsoleOutput) \
	F(ShowMessage) \
	F(YYError) \
	F(YYAlloc) \
	F(YYRealloc) \
	F(YYFree) \
	F(YYStrDup) \
	F(YYGetBool) \
	F(YYGetFloat) \
	F(YYGetReal) \
	F(YYGetInt32) \
	F(YYGetUint32) \
	F(YYGetInt64) \
	F(YYGetPtr) \
	F(YYGetPtrOrInt) \
	F(YYGetString) \
	F(BOOL_RValue) \
	F(REAL_RValue) \
	F(PTR_RValue) \
	F(INT64_RValue) \
	F(INT32_RValue) \
	F(HASH_RValue) \
	F(SET_RValue) \
	F(GET_RValue) \
	F(COPY_RValue) \
	F(KIND_RValue) \
	F(FREE_RValue) \
	F(YYCreateString) \
	F(YYCreateArray) \
	F(Script_Find_Id) \
	F(Script_Perform) \
	F(Code_Function_Find) \
	F(HTTP_Get) \
	F(HTTP_Post) \
	F(HTTP_Request) \
	F(ASYNCFunc_SpriteAdd) \
	F(ASYNCFunc_SpriteCleanup) \
	F(CreateSpriteAsync) \
	F(Timing_Time) \
	F(Timing_Sleep) \
	F(YYMutexCreate) \
	F(YYMutexDestroy) \
	F(YYMutexLock) \
	F(YYMutexUnlock) \
	F(CreateAsyncEventWithDSMap) \
	F(CreateAsyncEventWithDSMapAndBuffer) \
	F(CreateDsMap) \
	F(DsMapAddDouble) \
	F(DsMapAddString) \
	F(DsMapAddInt64) \
	F(BufferGetContent) \
	F(BufferWriteContent) \
	F(CreateBuffer) \
	F(DsListCreate) \
	F(DsMapAddList) \
	F(DsListAddMap) \
	F(DsMapClear) \
	F(DsListClear) \
	F(BundleFileExists) \
	F(BundleFileName) \
	F(SaveFileExists) \
	F(SaveFileName) \
	F(Base64Encode) \
	F(DsListAddInt64) \
	F(AddDirectoryToBundleWhitelist) \
	F(AddFileToBundleWhitelist) \
	F(AddDirectoryToSaveWhitelist) \
	F(AddFileToSaveWhitelist) \
	F(KIND_NAME_RValue) \
	F(DsMapAddBool) \
	F(DsMapAddRValue) \
	F(DestroyDsMap) \
	F(StructCreate) \
	F(StructAddBool) \
	F(StructAddDouble) \
	F(StructAddInt) \
	F(StructAddRValue) \
	F(StructAddString) \
	F(WhitelistIsDirectoryIn) \
	F(WhiteListIsFilenameIn) \
	F(WhiteListAddTo) \
	F(DirExists) \
	F(BufferGetFromGML) \
	F(BufferTELL) \
	F(BufferGet) \
	F(FilePrePend) \
	F(StructAddInt32) \
	F(StructAddInt64) \
	F(StructGetMember) \
	F(StructGetKeys) \
	F(YYGetStruct) \
	F(extOptGetRValue) \
	F(extOptGetString) \
	F(extOptGetReal) \
	F(isRunningFromIDE) \
	F(YYArrayGetLength) \
	F(extGetVersion)

static uintptr_t ExeBase() { return reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)); }

// RVAs of the runner's helper functions (labels for static analysis).
static Json CmdRunner()
{
	const YYRunnerInterface& ri = g_Yytk->GetRunnerInterface();
	Json out = Json::Object();
#define F(name) out.Add(#name, ri.name ? static_cast<int64_t>(reinterpret_cast<uintptr_t>(ri.name) - ExeBase()) : -1);
	SLORM_RUNNER_FIELDS(F)
#undef F
	return out;
}

// RVAs of builtin GML functions, for the given candidate names (null if not a builtin).
static Json CmdRoutines(const Json& Req)
{
	Json out = Json::Object();
	for (auto& n : Req.At("names").a)
	{
		if (n.type != Json::Str || n.s.starts_with("gml_")) continue;
		PVOID p = nullptr;
		if (AurieSuccess(g_Yytk->GetNamedRoutinePointer(n.s.c_str(), &p)) && p)
			out.Add(n.s, static_cast<int64_t>(reinterpret_cast<uintptr_t>(p) - ExeBase()));
	}
	return out;
}

// Runner function-table indices of builtin GML functions (YYC calls builtins by index).
static Json CmdRoutineIndex(const Json& Req)
{
	Json out = Json::Object();
	for (auto& n : Req.At("names").a)
	{
		if (n.type != Json::Str) continue;
		int index = -1;
		if (AurieSuccess(g_Yytk->GetNamedRoutineIndex(n.s.c_str(), &index)) && index >= 0)
			out.Add(n.s, index);
	}
	return out;
}

// ============================================================================
// Game knowledge: items, stats, legendaries and skills in readable form, from the game's own data files (GameData.hpp,
// loaded on first use).
// ============================================================================

// read on a background thread from the start (pure file + JSON work, ~60 ms): the first item / skill name in a fight
// must not stall a frame
static gd::GameData g_Data;
static std::shared_future<void> g_DataReady;
static void StartLoadingData()
{
	g_DataReady = std::async(std::launch::async, [] { g_Data.Load(); }).share();
}
static gd::GameData& Data()
{
	if (!g_DataReady.valid()) StartLoadingData();
	g_DataReady.get();                                     // waits only if it is not done yet; rethrows a load error
	return g_Data;
}

static const char* const kSlotKeys[11] = { "helm", "body", "shoulder", "bracer", "glove", "boot", "ring", "ring", "amulet",
	"belt", "cape" };
static const char* const kHeroes[3] = { "Knight", "Huntress", "Mage" };

static int CurrentHero()
{
	RValue* h = FindMember(GlobalRValue(), "current_hero");
	return h && IsNumeric(*h) ? static_cast<int>(h->ToDouble()) : 0;
}

static std::string SlotLabel(const std::string& Slot)
{
	for (auto& s : gd::Slots())
		if (Slot == s.Key) return s.Label;
	return Slot == "weapon" ? "Weapon" : Slot;
}

// an item array (as in save files, obj_loot.ar and the events) -> readable
static Json ItemInfo(const Json& Ar)
{
	if (Ar.type != Json::Arr || Ar.a.empty() || Ar.a[0].type != Json::Arr || Ar.a[0].a.size() < 4) Fail("not an item array");
	auto& d = Data();
	const auto& h = Ar.a[0].a;
	std::string slot = gd::Str(h[1]);
	int level = static_cast<int>(gd::Num(h[2])), hero = CurrentHero();
	Json out = Json::Object();
	out.Add("kind", gd::Str(h[3]));
	out.Add("rarity", gd::Str(h[0]));
	out.Add("slot", slot);
	out.Add("slot_name", SlotLabel(slot));
	out.Add("level", level);
	Json stats = Json::Array(), extras = Json::Array(), leg;
	for (size_t k = 1; k < Ar.a.size(); ++k)
	{
		const Json& l = Ar.a[k];
		if (l.type != Json::Arr || l.a.size() < 2) continue;
		std::string tag = gd::Str(l.a[0]);
		if (tag == "L")
		{
			int id = static_cast<int>(gd::Num(l.a[1]));
			leg = Json::Object();
			leg.Add("id", id);
			leg.Add("name", d.LegName(id));
			leg.Add("roll", l.a.size() > 2 ? gd::Num(l.a[2]) : 0.0);
		}
		else if ((tag == "MA" || tag == "AT" || tag == "RP") && l.a.size() > 2)
		{
			int idx = static_cast<int>(gd::Num(l.a[1]));
			Json e = Json::Object();
			e.Add("type", tag == "MA" ? "skill_mastery" : tag == "AT" ? "attribute" : "reaper_affinity");
			e.Add("index", idx);
			e.Add("value", gd::Num(l.a[2]));
			e.Add("name", tag == "MA" ? d.SkillName(hero, idx) : tag == "AT" ? (idx >= 0 && idx < 8 ? gd::kTraits[idx] : "?")
				: "Reapersmith " + std::to_string(idx));
			extras.Push(std::move(e));
		}
		else if (l.a.size() > 2 && l.a[1].type == Json::Str)
		{
			std::string stat = l.a[1].s;
			double roll = gd::Num(l.a[2]);
			Json s = Json::Object();
			s.Add("tier", tag);   // N normal, M magic, R rare, E epic (gold)
			s.Add("stat", stat);
			s.Add("name", d.StatLabel(stat, slot));
			s.Add("roll", roll);
			s.Add("value", d.AffixValue(stat, roll, level));
			s.Add("pure", l.a.size() > 3 && gd::Num(l.a[3]) > 0);
			stats.Push(std::move(s));
		}
	}
	out.Add("legendary", std::move(leg));
	out.Add("stats", std::move(stats));
	out.Add("extras", std::move(extras));
	return out;
}

// {"item": [...]} | {"inst": loot instance} | {"equipped": 0..10 (the current hero's helm..cape)}
static Json CmdItemInfo(const Json& Req)
{
	if (const Json* it = Req.Find("item")) return ItemInfo(*it);
	if (const Json* inst = Req.Find("inst"))
	{
		CInstance* ci = InstanceById(static_cast<int32_t>(inst->type == Json::Obj ? inst->GetNum("$inst", -1) : inst->n));
		if (!ci) Fail("no such instance");
		return ItemInfo(ToJson(g_Yytk->CallBuiltin("variable_instance_get", { RValue(ci), RValue("ar") }), 4));
	}
	if (const Json* eq = Req.Find("equipped"))
	{
		RValue* invp = FindMember(GlobalRValue(), "inventory");
		if (!invp) Fail("no inventory");
		RValue inv = *invp;
		RValue* cls = ArrayAt(inv, static_cast<size_t>(CurrentHero()));
		RValue* item = cls ? ArrayAt(*cls, static_cast<size_t>(eq->n)) : nullptr;
		if (!item) Fail("no item in that slot");
		return ItemInfo(ToJson(*item, 4));
	}
	Fail("item_info needs item, inst or equipped");
}

static Json CmdStatInfo(const Json& Req)
{
	auto& d = Data();
	std::string stat = Req.GetStr("stat");
	Json out = Json::Object();
	out.Add("stat", stat);
	out.Add("name", d.StatName(stat));
	auto sc = d.Score.find(stat);
	out.Add("score", sc != d.Score.end() ? Json(sc->second) : Json());
	Json slots = Json::Array();
	for (auto& [slot, pool] : d.GoldPool)
		if (std::find(pool.begin(), pool.end(), stat) != pool.end()) slots.Push(Json(slot));
	out.Add("epic_slots", std::move(slots));   // slots where it can roll as a gold (epic) line
	if (const Json* slot = Req.Find("slot"); slot && slot->type == Json::Str) out.Add("label", d.StatLabel(stat, slot->s));
	if (const Json* roll = Req.Find("roll"))                                       // the value of a roll on an item level
		out.Add("value", d.AffixValue(stat, gd::Num(*roll), static_cast<int>(Req.GetNum("level", 100))));
	return out;
}

// {"slot": "helm".."cape", "hero"}: the gold (epic) stats that can roll there and the hero's lootable legendaries, by name
static Json CmdSlotInfo(const Json& Req)
{
	auto& d = Data();
	std::string slot = Req.GetStr("slot");
	bool known = false;
	for (auto& s : gd::Slots()) known |= slot == s.Key;
	if (!known) Fail("unknown slot '" + slot + "' (helm body shoulder bracer glove boot ring amulet belt cape)");
	int hero = static_cast<int>(Req.GetNum("hero", CurrentHero()));
	std::vector<std::string> pool;
	if (auto it = d.GoldPool.find(slot); it != d.GoldPool.end()) pool = it->second;
	std::sort(pool.begin(), pool.end(), [&](const std::string& A, const std::string& B) { return d.StatLabel(A, slot) < d.StatLabel(B, slot); });
	Json stats = Json::Array(), legs = Json::Array();
	for (auto& s : pool)
	{
		Json e = Json::Object();
		e.Add("stat", s);
		e.Add("name", d.StatLabel(s, slot));
		stats.Push(std::move(e));
	}
	for (int id : d.LegendaryPool(slot, hero))
	{
		Json e = Json::Object();
		e.Add("id", id);
		e.Add("name", d.LegName(id));
		legs.Push(std::move(e));
	}
	Json out = Json::Object();
	out.Add("slot", slot);
	out.Add("name", SlotLabel(slot));
	out.Add("hero", hero);
	out.Add("epic_stats", std::move(stats));
	out.Add("legendaries", std::move(legs));
	return out;
}

static Json CmdLegendaryInfo(const Json& Req)
{
	auto& d = Data();
	int id = static_cast<int>(Req.GetNum("id", -1));
	auto it = d.Legendaries.find(id);
	if (it == d.Legendaries.end()) Fail("no legendary " + std::to_string(id));
	const auto& l = it->second;
	Json out = Json::Object();
	out.Add("id", id);
	out.Add("name", l.Name);
	out.Add("slot", l.Item);
	out.Add("hero", l.Hero);   // -1 any, 0-2 a hero, 99 legacy, 999 reaper
	out.Add("lootable", l.Lootable);
	out.Add("description", l.Desc);   // '@' = a value, '#' = a line break, as the game writes it
	out.Add("values", l.Value);
	out.Add("types", l.Type);
	return out;
}

static Json CmdSkillInfo(const Json& Req)
{
	int hero = static_cast<int>(Req.GetNum("hero", CurrentHero())), id = static_cast<int>(Req.GetNum("id", -1));
	Json out = Json::Object();
	out.Add("hero", hero);
	out.Add("id", id);
	out.Add("name", Data().SkillName(hero, id));
	return out;
}

// ============================================================================
// Mods: the DLLs in mods/aurie and mods/native, and what mods tell about themselves ("register_mod")
// ============================================================================

struct ModInfo { std::string Name, Version, Author, Description, File; Json Requires; std::string Game; };
static int CompareVersions(const std::string& A, const std::string& B);
static void LuaModRows(Json& Out);
static bool LuaToggle(const std::string& File, bool On);
static uint64_t LuaVersion();
static Json ModProblems(const ModInfo& M, const Json& Rows);
static std::vector<ModInfo> g_Registered;

static Json CmdRegisterMod(const Json& Req)
{
	ModInfo m{ Req.GetStr("name"), Req.GetStr("version"), Req.GetStr("author"), Req.GetStr("description"), Req.GetStr("file") };
	if (m.Name.empty()) Fail("register_mod needs a name");
	if (m.File.empty()) m.File = m.Name + ".dll";
	if (const Json* r = Req.Find("requires"); r && r->type == Json::Obj) m.Requires = *r;   // {"SlormAPI": "1.0.0", ...}
	m.Game = Req.GetStr("game");                                                       // the game version it was made for
	for (auto& r : g_Registered)
		if (r.Name == m.Name) { r = m; return Json(true); }
	g_Registered.push_back(m);
	return Json(true);
}

static fs::path GameDir()
{
	wchar_t buf[MAX_PATH * 2] = {};
	GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
	return fs::path(buf).parent_path();
}

static std::string Lower(std::string S)
{
	for (auto& c : S) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	return S;
}

// "1.2.3" from the DLL's version resource ("" without one).
static std::string FileVersion(const fs::path& P)
{
	DWORD unused = 0, n = GetFileVersionInfoSizeW(P.c_str(), &unused);
	if (!n) return "";
	std::vector<char> buf(n);
	VS_FIXEDFILEINFO* fi = nullptr;
	UINT len = 0;
	if (!GetFileVersionInfoW(P.c_str(), 0, n, buf.data()) || !VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&fi), &len) || !fi)
		return "";
	char s[48];
	snprintf(s, sizeof(s), "%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS));
	return s;
}

static constexpr const char* kOff = ".disabled";   // "X.dll.disabled": switched off (Aurie loads only *.dll)
static std::set<std::string> g_Toggled;            // files switched on / off this session (lower case)

static fs::path AurieDir() { return GameDir() / "mods" / "aurie"; }
static bool Exists(const fs::path& P) { std::error_code ec; return fs::exists(P, ec); }
// what everything else needs: never switched off from the Mods page
static bool Locked(const std::string& File) { auto f = Lower(File); return f == "slormapi.dll" || f == "yytoolkit.dll"; }
// loader parts that sit in mods/aurie: YYToolkit, and Slorm Loader's module when the game uses that loader
static bool LoaderFile(const std::string& File) { auto f = Lower(File); return f == "yytoolkit.dll" || f == "slormloader.dll"; }

// SlormAPI, the mods of mods/aurie by name (registered ones with what they told about themselves; switched-off ones
// too), then the loader (YYToolkit, mods/native). A switch applies at the next launch: "pending" until then.
static Json CmdMods()
{
	struct Row { ModInfo M; bool Loader, Loaded, Enabled; };
	std::vector<Row> rows;
	std::set<std::string> listed;
	fs::path aurie = AurieDir();
	for (auto& m : g_Registered)
	{
		rows.push_back({ m, false, true, Exists(aurie / m.File) || !Exists(aurie / (m.File + kOff)) });
		listed.insert(Lower(m.File));
	}
	std::error_code ec;
	for (auto& f : fs::directory_iterator(aurie, ec))
	{
		if (!f.is_regular_file()) continue;
		std::string file = f.path().filename().string();
		bool off = Lower(file).ends_with(std::string(".dll") + kOff);
		if (off) file.resize(file.size() - strlen(kOff));
		else if (!Lower(file).ends_with(".dll")) continue;
		if (listed.count(Lower(file)) || (off && Exists(aurie / file))) continue;
		listed.insert(Lower(file));
		ModInfo m{ fs::path(file).stem().string(), FileVersion(f.path()), "", "", file };
		bool loader = LoaderFile(file);
		if (Lower(file) == "yytoolkit.dll") m.Author = "AurieFramework";
		if (Lower(file) == "slormloader.dll") m.Name = "Slorm Loader";
		rows.push_back({ m, loader, GetModuleHandleW(fs::path(file).c_str()) != nullptr, !off });
	}
	// Aurie Core: mods/native/AurieCore.dll (patched game exe) or plugins/AurieCore.asi (Slorm Loader's version.dll)
	auto core = [&](const fs::path& Dir, const char* Ext)
	{
		for (auto& f : fs::directory_iterator(Dir, ec))
		{
			if (!f.is_regular_file() || Lower(f.path().extension().string()) != Ext) continue;
			std::string file = f.path().filename().string(), stem = f.path().stem().string();
			bool aurie = Lower(stem) == "auriecore";
			if (!aurie && std::string(Ext) == ".asi") continue;      // other ASI plugins are not Aurie mods
			ModInfo m{ aurie ? "Aurie Core" : stem, FileVersion(f.path()), aurie ? "AurieFramework" : "", "", file };
			rows.push_back({ m, true, GetModuleHandleW(f.path().filename().c_str()) != nullptr, true });
		}
	};
	core(GameDir() / "mods" / "native", ".dll");
	core(GameDir() / "plugins", ".asi");
	auto rank = [](const Row& R) { return R.Loader ? 2 : Lower(R.M.File) == "slormapi.dll" ? 0 : 1; };
	std::stable_sort(rows.begin(), rows.end(), [&](const Row& A, const Row& B)
	{
		if (rank(A) != rank(B)) return rank(A) < rank(B);
		return rank(A) == 1 && Lower(A.M.Name) < Lower(B.M.Name);
	});

	Json out = Json::Array();
	for (auto& r : rows)
	{
		Json j = Json::Object();
		j.Add("name", r.M.Name);
		j.Add("version", r.M.Version);
		j.Add("author", r.M.Author);
		j.Add("description", r.M.Description);
		j.Add("file", r.M.File);
		j.Add("kind", r.Loader ? "loader" : "mod");
		j.Add("loaded", r.Loaded);
		j.Add("enabled", r.Enabled);
		j.Add("locked", r.Loader || Locked(r.M.File) || !(Exists(aurie / r.M.File) || Exists(aurie / (r.M.File + kOff))));
		j.Add("pending", g_Toggled.count(Lower(r.M.File)) > 0 && r.Enabled != r.Loaded);
		if (!r.M.Game.empty()) j.Add("game", r.M.Game);
		out.Push(std::move(j));
	}
	// second pass: requirements are checked against the whole list
	std::string game = GameVersion();
	for (size_t i = 0; i < rows.size(); ++i)
	{
		out.a[i].Add("problems", ModProblems(rows[i].M, out));
		if (!rows[i].M.Game.empty() && !game.empty() && CompareVersions(rows[i].M.Game, game) != 0)
			out.a[i].Add("note", "Made for game " + rows[i].M.Game);
	}
	LuaModRows(out);
	return out;
}

// A file that exists while the game runs: left over at the next start = the last session did not end normally.
static std::wstring g_SessionFile;
static Json g_LastSession = Json::Object();
static bool g_SessionMenuMarked = false;
static Json LastSession() { return g_LastSession; }

static void SessionEnd()
{
	if (!g_SessionFile.empty()) DeleteFileW(g_SessionFile.c_str());
	g_SessionFile.clear();
}
struct SessionGuard { ~SessionGuard() { SessionEnd(); } };   // a normal exit unloads the DLL
static SessionGuard g_SessionGuard;

static void SessionStart()
{
	fs::path p = AurieDir() / "SlormAPI" / "session.json";
	std::error_code ec;
	fs::create_directories(p.parent_path(), ec);
	bool crashed = Exists(p), startup = false;
	if (crashed)
	{
		std::ifstream f(p, std::ios::binary);
		std::stringstream ss;
		ss << f.rdbuf();
		try { startup = !sj::Parse(ss.str()).GetBool("menu", false); }
		catch (const std::exception&) { startup = true; }
	}
	g_LastSession = Json::Object();
	g_LastSession.Add("crashed", crashed);
	g_LastSession.Add("during_startup", crashed && startup);
	std::ofstream(p, std::ios::binary) << "{\"menu\":false}";
	g_SessionFile = p.wstring();
}

// The runner ends the process without unloading the DLLs (and YYToolkit's window callback does not fire here), so a
// normal exit is seen on the game window itself: subclassed once it exists, closing / destroying it ends the session.
static WNDPROC g_OrigWndProc = nullptr;
static LRESULT CALLBACK SessionWndProc(HWND H, UINT M, WPARAM W, LPARAM L)
{
	if (M == WM_CLOSE || M == WM_DESTROY || M == WM_ENDSESSION) SessionEnd();
	return CallWindowProcW(g_OrigWndProc, H, M, W, L);
}
static BOOL CALLBACK FindGameWindow(HWND H, LPARAM Out)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(H, &pid);
	wchar_t cls[64] = {};
	GetClassNameW(H, cls, 64);
	if (pid != GetCurrentProcessId() || wcscmp(cls, L"YYGameMakerYY") != 0) return TRUE;   // not Aurie's log console
	*reinterpret_cast<HWND*>(Out) = H;
	return FALSE;
}
// Closing Aurie's log console (or Ctrl+C in it) ends the game too: also a normal end, not a crash.
static BOOL WINAPI SessionConsoleHandler(DWORD Type)
{
	if (Type == CTRL_C_EVENT || Type == CTRL_CLOSE_EVENT || Type == CTRL_LOGOFF_EVENT || Type == CTRL_SHUTDOWN_EVENT) SessionEnd();
	return FALSE;                                         // the default handling (ending the process) goes on
}

static void WatchGameWindow()
{
	if (g_OrigWndProc || g_SessionFile.empty()) return;
	HWND wnd = nullptr;
	EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&wnd));
	if (wnd) g_OrigWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SessionWndProc)));
}

// 20 s after start the game is up: a crash after this is not a start-up crash
static void SessionReachedMenu()
{
	if (g_SessionMenuMarked || g_SessionFile.empty()) return;
	g_SessionMenuMarked = true;
	std::ofstream(fs::path(g_SessionFile), std::ios::binary) << "{\"menu\":true}";
}

// -1 / 0 / 1 by the numbers in the versions ("1.10.0" > "1.9.2"; "1.0.2c" == "1.0.2")
static int CompareVersions(const std::string& A, const std::string& B)
{
	auto parts = [](const std::string& V)
	{
		std::vector<int> p;
		int n = 0;
		bool in = false;
		for (char c : V)
		{
			if (isdigit(static_cast<unsigned char>(c))) { n = n * 10 + (c - '0'); in = true; }
			else if (in) { p.push_back(n); n = 0; in = false; }
		}
		if (in) p.push_back(n);
		return p;
	};
	auto a = parts(A), b = parts(B);
	for (size_t i = 0; i < (std::max)(a.size(), b.size()); ++i)
	{
		int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
		if (x != y) return x < y ? -1 : 1;
	}
	return 0;
}

static std::string GameVersion()
{
	RValue* v = FindMember(GlobalRValue(), "version");
	return v ? GmlString(*v) : "";
}

// what keeps a mod from working: unmet "requires", scripts it could not hook
static Json ModProblems(const ModInfo& M, const Json& Rows)
{
	Json out = Json::Array();
	if (M.Requires.type == Json::Obj)
		for (auto& [name, min] : M.Requires.o)
		{
			std::string want = min.type == Json::Str ? min.s : "", have;
			if (Lower(name) == "slormapi") have = kVersion;
			else
				for (auto& r : Rows.a)
					if (Lower(r.GetStr("name")) == Lower(name) && r.GetBool("loaded", false)) have = r.GetStr("version", "0");
			if (have.empty()) out.Push(Json("Needs " + name + (want.empty() ? "" : " " + want + "+")));
			else if (!want.empty() && CompareVersions(have, want) < 0) out.Push(Json("Needs " + name + " " + want + "+ (has " + have + ")"));
		}
	size_t broken = 0;
	for (auto& [owner, scripts] : g_HookErrors)
		if (owner == M.Name || (M.Name == "SlormAPI" && owner.starts_with("SlormAPI"))) broken += scripts.size();
	if (broken) out.Push(Json(std::to_string(broken) + " game function" + (broken > 1 ? "s" : "") + " not found - needs an update"));
	return out;
}

// Switches a mod of mods/aurie on or off for the next launch ("file": "X.dll", "enabled": bool) by renaming it.
static Json CmdModEnable(const Json& Req)
{
	std::string file = Req.GetStr("file");
	bool on = Req.GetBool("enabled", true);
	if (LuaToggle(file, on)) return Json(true);                // a Lua mod: mods/lua, applies right away
	if (file.find_first_of("/\\:") != std::string::npos || !Lower(file).ends_with(".dll")) Fail("bad mod file '" + file + "'");
	if (Locked(file)) Fail(file + " stays on: other mods need it");
	fs::path dll = AurieDir() / file, off = AurieDir() / (file + kOff);
	std::error_code ec;
	if (on && !Exists(dll) && Exists(off)) fs::rename(off, dll, ec);
	else if (!on && Exists(dll)) fs::rename(dll, off, ec);
	if (ec) Fail("could not rename " + file + " (" + ec.message() + ")");
	g_Toggled.insert(Lower(file));
	return Json(true);
}

// ============================================================================
// Mod settings: a mod describes its settings ("register_settings"); the Mods page shows them on that mod's own page,
// SlormAPI keeps the values in mods/aurie/SlormAPI/settings/<mod>.json and reports changes ("setting_changed" event).
// ============================================================================

struct SettingDef
{
	std::string Key, Label, Help, Type;   // type: bool, choice (one of "options"), number (min..max by step)
	Json Default;
	std::vector<Json> Options;
	double Min = 0, Max = 100, Step = 1;
};
struct ModSettings { std::vector<SettingDef> Defs; Json Values = Json::Object(); };
static std::map<std::string, ModSettings> g_Settings;
static uint64_t g_SettingsVer = 0;                                   // bumped on every change (the page redraws)
static std::vector<std::pair<std::string, Json>> g_DirectEvents;     // emitted by commands, delivered with the rest

static fs::path SettingsFile(const std::string& Mod)
{
	std::string safe;
	for (char c : Mod) safe += (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') ? c : '_';
	return AurieDir() / "SlormAPI" / "settings" / (safe + ".json");
}

// ============================================================================
// Mod data: one saved JSON value and a log file per mod, in mods/aurie/SlormAPI/data (Lua mods have no file access).
// Named after the caller's "owner" (forced for Lua mods: they reach only their own files), else "mod".
// ============================================================================

static constexpr size_t kModDataMax = size_t(4) << 20, kModLogMax = size_t(2) << 20;

static fs::path ModDataFile(const Json& Req, const char* Ext)
{
	std::string name = Req.GetStr("owner");
	if (name.empty()) name = Req.GetStr("mod");
	if (name.empty()) Fail("needs \"mod\" (your mod's name)");
	std::string safe;
	for (char c : name) safe += (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') ? c : '_';
	fs::path dir = AurieDir() / "SlormAPI" / "data";
	std::error_code ec;
	fs::create_directories(dir, ec);
	return dir / (safe + Ext);
}

// {"mod", "value"}: saves the value (up to 4 MB); without "value": the saved one (null when there is none)
static Json CmdModData(const Json& Req)
{
	fs::path f = ModDataFile(Req, ".json");
	if (const Json* v = Req.Find("value"))
	{
		std::string text = sj::Dump(*v);
		if (text.size() > kModDataMax) Fail("mod data is limited to 4 MB");
		fs::path tmp = f;
		tmp += ".tmp";
		{
			std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
			o << text;
			if (!o) Fail("cannot write " + f.filename().string());
		}
		std::error_code ec;
		fs::rename(tmp, f, ec);                                      // whole or not at all
		if (ec) Fail("cannot write " + f.filename().string() + ": " + ec.message());
		return Json(true);
	}
	std::ifstream i(f, std::ios::binary);
	if (!i) return Json();
	std::stringstream ss;
	ss << i.rdbuf();
	try { return sj::Parse(ss.str()); }
	catch (const std::exception&) { return Json(); }
}

// {"mod", "line"}: appends "HH:MM:SS line" to data/<mod>.log (past 2 MB the file moves to <mod>.log.old)
static Json CmdModLog(const Json& Req)
{
	fs::path f = ModDataFile(Req, ".log");
	std::string line = Req.GetStr("line");
	if (line.size() > 4096) line.resize(4096);
	for (char& c : line) if (c == '\n' || c == '\r') c = ' ';
	std::error_code ec;
	if (fs::exists(f, ec) && fs::file_size(f, ec) > kModLogMax)
	{
		fs::path old = f;
		old += ".old";
		fs::rename(f, old, ec);
	}
	std::time_t t = std::time(nullptr);
	std::tm tmv{};
	localtime_s(&tmv, &t);
	char ts[16];
	std::strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
	std::ofstream o(f, std::ios::app | std::ios::binary);
	o << ts << " " << line << "\n";
	return Json(true);
}

static Json SettingValue(const ModSettings& S, const SettingDef& D)
{
	const Json* v = S.Values.Find(D.Key);
	return v ? *v : D.Default;
}

// V when it is a valid value of D (numbers are clamped and snapped to the step), else throws
static Json CheckSetting(const SettingDef& D, const Json& V)
{
	if (D.Type == "bool")
	{
		if (V.type != Json::Bool) Fail(D.Key + " takes true or false");
		return V;
	}
	if (D.Type == "choice")
	{
		for (auto& o : D.Options)
			if (sj::Dump(o) == sj::Dump(V)) return V;
		Fail(D.Key + ": " + sj::Dump(V) + " is not one of its options");
	}
	if (D.Type == "number")
	{
		if (V.type != Json::Num) Fail(D.Key + " takes a number");
		double n = (std::min)(D.Max, (std::max)(D.Min, V.n));
		n = D.Min + std::round((n - D.Min) / D.Step) * D.Step;
		return Json((std::min)(D.Max, n));
	}
	Fail(D.Key + ": unknown setting type '" + D.Type + "'");
}

static Json SettingsJson(const std::string& Mod)
{
	auto it = g_Settings.find(Mod);
	if (it == g_Settings.end()) Fail(Mod + " has no settings");
	Json out = Json::Object();
	for (auto& d : it->second.Defs) out.Add(d.Key, SettingValue(it->second, d));
	return out;
}

// {"mod", "settings": [{"key", "label", "help", "type": "bool" | "choice" | "number", "default", "options", "min",
// "max", "step"}]} -> the current values (the saved ones that are still valid, else the defaults)
static Json CmdRegisterSettings(const Json& Req)
{
	std::string mod = Req.GetStr("mod");
	if (mod.empty()) Fail("register_settings needs a mod");
	ModSettings s;
	for (auto& j : Req.At("settings").a)
	{
		SettingDef d;
		d.Key = j.GetStr("key");
		d.Label = j.GetStr("label", d.Key);
		d.Help = j.GetStr("help");
		d.Type = j.GetStr("type", "bool");
		if (const Json* o = j.Find("options")) d.Options = o->a;
		d.Min = j.GetNum("min", 0);
		d.Max = j.GetNum("max", 100);
		d.Step = j.GetNum("step", 1);
		if (d.Key.empty()) Fail("a setting needs a key");
		if (d.Step <= 0 || d.Max < d.Min) Fail(d.Key + ": bad min / max / step");
		if (d.Type == "choice" && d.Options.empty()) Fail(d.Key + ": a choice needs options");
		const Json* def = j.Find("default");
		d.Default = CheckSetting(d, def ? *def : d.Type == "bool" ? Json(false) : d.Type == "choice" ? d.Options[0] : Json(d.Min));
		s.Defs.push_back(std::move(d));
	}
	if (std::ifstream f(SettingsFile(mod), std::ios::binary); f)
	{
		std::stringstream ss;
		ss << f.rdbuf();
		try
		{
			Json saved = sj::Parse(ss.str());
			for (auto& d : s.Defs)
				if (const Json* v = saved.Find(d.Key))
					try { s.Values.Set(d.Key, CheckSetting(d, *v)); }
					catch (const std::exception&) {}           // an old or edited value that no longer fits: the default
		}
		catch (const std::exception&) {}
	}
	g_Settings[mod] = std::move(s);
	++g_SettingsVer;
	return SettingsJson(mod);
}

static Json SetSetting(const std::string& Mod, const std::string& Key, const Json& Value)
{
	auto it = g_Settings.find(Mod);
	if (it == g_Settings.end()) Fail(Mod + " has no settings");
	for (auto& d : it->second.Defs)
	{
		if (d.Key != Key) continue;
		Json v = CheckSetting(d, Value);
		it->second.Values.Set(Key, v);
		fs::path p = SettingsFile(Mod);
		std::error_code ec;
		fs::create_directories(p.parent_path(), ec);
		std::ofstream(p, std::ios::binary) << sj::Dump(it->second.Values);
		++g_SettingsVer;
		Json e = Json::Object();
		e.Add("mod", Mod);
		e.Add("key", Key);
		e.Add("value", v);
		g_DirectEvents.emplace_back("setting_changed", std::move(e));
		return v;
	}
	Fail(Mod + " has no setting '" + Key + "'");
}

static Json CmdSettings(const Json& Req) { return SettingsJson(Req.GetStr("mod")); }
static Json CmdSetSetting(const Json& Req) { return SetSetting(Req.GetStr("mod"), Req.GetStr("key"), Req.At("value")); }

// The Mods page's controls: "toggle" (bool), "prev" / "next" (a choice cycles, a number moves by its step).
static void StepSetting(const std::string& Mod, const std::string& Key, const std::string& Op)
{
	auto it = g_Settings.find(Mod);
	if (it == g_Settings.end()) return;
	for (auto& d : it->second.Defs)
	{
		if (d.Key != Key) continue;
		Json cur = SettingValue(it->second, d);
		int dir = Op == "prev" ? -1 : 1;
		if (d.Type == "bool") SetSetting(Mod, Key, Json(!(cur.type == Json::Bool && cur.b)));
		else if (d.Type == "choice")
		{
			size_t n = d.Options.size(), i = 0;
			for (size_t k = 0; k < n; ++k)
				if (sj::Dump(d.Options[k]) == sj::Dump(cur)) i = k;
			SetSetting(Mod, Key, d.Options[(i + n + dir) % n]);
		}
		else if (d.Type == "number") SetSetting(Mod, Key, Json(cur.n + dir * d.Step));
		return;
	}
}

static std::string SettingText(const Json& V)
{
	if (V.type == Json::Str) return V.s;
	if (V.type == Json::Bool) return V.b ? "On" : "Off";
	if (V.type == Json::Num)
	{
		char b[32];
		snprintf(b, sizeof(b), V.n == std::floor(V.n) ? "%.0f" : "%.2f", V.n);
		return b;
	}
	return sj::Dump(V);
}

// ============================================================================
// Mods page: a MODS button next to DISCORD in the main / pause menu shows the installed mods (CmdMods) in the menu's
// Welcome box. It is drawn into the game's own menu bake by a draw program in SlormAPI's own hook layer (owner
// "SlormAPI"), the same way any mod can do it; clicks go to the sink script with "slormapi:<action>".
// ============================================================================

namespace modspage
{
	constexpr const char* kOwner = "SlormAPI";
	constexpr const char* kSink = "scr_blacksmith_open_filter_from_inventory";
	constexpr double kBtnX = 740, kBtnY = 926;            // left of DISCORD (1100, 926)
	constexpr double kPanelX = 600, kPanelY = 367;        // spr_main_menu_panel as the main menu draws it

	bool Open = false;
	bool Disabled = false;
	double CheckAt = 0, RetryAt = 0;

	Json O(std::initializer_list<std::pair<const char*, Json>> Kv)
	{
		Json j = Json::Object();
		for (auto& p : Kv) j.Add(p.first, p.second);
		return j;
	}
	Json A(std::initializer_list<Json> Xs)
	{
		Json j = Json::Array();
		for (auto& x : Xs) j.Push(x);
		return j;
	}
	Json Asset(const char* Name) { return O({ { "$asset", Name } }); }
	Json Call(const char* Fn, Json Args) { return O({ { "fn", Fn }, { "args", std::move(Args) } }); }
	// the game's text script (fonts by name); drawn right away with these flags (the main menu's own 1, 0 defers it)
	Json Text(double X, double Y, const std::string& S, const char* Font, const char* Color, int Ha = 0)
	{
		return Call("scr_draw_text", A({ X, Y, S, 1, 1, Font, Ha, 1, 0, 1, Color, "black", 1, false }));
	}
	Json Label(double X, double Y, const std::string& S) { return Text(X, Y, S, "ft_standard_12", "white", 1); }
	std::string Clip(std::string S, size_t N) { return S.size() <= N ? S : S.substr(0, N - 3) + "..."; }

	constexpr size_t kPerPage = 5;
	int Page = 0;
	std::string Error;
	uint64_t SettingsSeen = 0;
	bool CrashSeen = false;   // the page was opened after a crash: the button's (!) goes
	uint64_t LuaSeen = 0;

	Json BuildProgram(const Json& Mods)
	{
		auto entry = [](const char* Name, const char* Spr, double X, double Y, double W, double H, const char* Text,
			const std::string& Action)
		{
			return Call("$mba_push", A({ A({ Name, Asset(Spr), X, Y, W, H, Text, Asset(kSink), "slormapi:" + Action }) }));
		};
		auto sprite = [](const char* Spr, double X, double Y) { return Call("draw_sprite", A({ Asset(Spr), 0, X, Y })); };
		// only on the Welcome panel (main_panel 0; New Game / Load Game use the same box and hide DISCORD)
		Json welcome = O({ { "$op", "==" }, { "args", A({ O({ { "$op", "??" }, { "args", A({ O({ { "$self", "main_panel" } }), 0 }) } }), 0 }) } });
		Json calls = Json::Array();
		calls.Push(Call("draw_sprite", A({ Asset("spr_menu_thin_blue_button"), 0, kBtnX, kBtnY })));
		calls.Push(Label(kBtnX + 168, kBtnY + 26, g_LastSession.GetBool("crashed", false) && !CrashSeen ? "MODS  (!)" : "MODS"));
		calls.Push(entry("Mods", "spr_menu_thin_blue_button", kBtnX, kBtnY, 336, 52, "MODS", "mods"));
		if (!Open) return O({ { "when", welcome }, { "calls", std::move(calls) } });

		// the update-notes buttons sit in the box: drop them, then paint the box (border + interior) over the bake
		calls.Push(Call("$mba_remove", A({ 7, Asset("scr_ui_main_menu_patch") })));
		calls.Push(Call("$mba_remove", A({ 7, Asset("scr_ui_main_menu_patch_2") })));
		calls.Push(Call("draw_sprite_part", A({ Asset("spr_main_menu_panel"), 1, 360, 0, 600, 533, kPanelX + 360, kPanelY })));
		auto pager = [&](int& P, size_t Count)
		{
			int pages = (std::max)(1, static_cast<int>((Count + kPerPage - 1) / kPerPage));
			P = ((P % pages) + pages) % pages;
			if (pages > 1)
			{
				calls.Push(sprite("spr_selector_btn_l_v2", 1432, 837));
				calls.Push(entry("ModsPage", "spr_selector_btn_l_v2", 1432, 837, 24, 36, "", "page-"));
				calls.Push(Text(1484, 855, std::to_string(P + 1) + " / " + std::to_string(pages), "ft_standard_14_b", "white", 1));
				calls.Push(sprite("spr_selector_btn_r_v2", 1512, 837));
				calls.Push(entry("ModsPage", "spr_selector_btn_r_v2", 1512, 837, 24, 36, "", "page+"));
			}
		};
		auto rowY = [](size_t I) { return 550 + 58.0 * static_cast<double>(I % kPerPage); };

		// the list of mods
		int loaded = 0;
		bool pending = false;
		for (auto& m : Mods.a)
		{
			loaded += m.GetBool("loaded", false) ? 1 : 0;
			pending |= m.GetBool("pending", false);
		}
		calls.Push(Text(984, 455, "Installed mods", "ft_standard_22_b", "white"));
		calls.Push(Text(984, 483, std::string("SlormAPI ") + kVersion + "   -   " + std::to_string(Mods.a.size()) + " installed, "
			+ std::to_string(loaded) + " loaded", "ft_standard_14_b", "grey"));
		calls.Push(sprite("spr_main_menu_separator", 1260, 500));
		pager(Page, Mods.a.size());
		for (size_t i = Page * kPerPage; i < Mods.a.size() && i < (Page + 1) * kPerPage; ++i)
		{
			const Json& m = Mods.a[i];
			double y = rowY(i);
			bool on = m.GetBool("enabled", true), isLoaded = m.GetBool("loaded", false), locked = m.GetBool("locked", true);
			std::string about = m.GetStr("description");
			if (about.empty()) about = m.GetStr("kind") == "loader" ? "Mod loader" : m.GetStr("file");
			if (!m.GetStr("author").empty()) about += "  -  by " + m.GetStr("author");
			std::string state = m.GetBool("pending", false) ? (on ? "On after a restart" : "Off after a restart")
				: !on ? "Off" : !isLoaded ? "Not loaded" : "";
			if (!m.GetStr("note").empty()) about = m.GetStr("note") + "  -  " + about;
			if (!state.empty()) about = state + "  -  " + about;
			const Json* problems = m.Find("problems");
			bool bad = on && problems && !problems->a.empty();
			if (bad) about = "! " + problems->a[0].s;
			// right to left: the on/off switch, the version
			double right = 1535;
			if (!locked)
			{
				const char* spr = on ? "spr_radio_small_btn_filled" : "spr_radio_small_btn";
				calls.Push(sprite(spr, 1499, y - 6));
				calls.Push(entry("ModsToggle", spr, 1499, y - 6, 36, 36, "", "toggle:" + m.GetStr("file")));
				right = 1482;
			}
			calls.Push(Text(984, y, Clip(m.GetStr("name"), 34), "ft_standard_16", on && isLoaded ? "white" : "grey"));
			if (!m.GetStr("version").empty()) calls.Push(Text(right, y, "v" + m.GetStr("version"), "ft_standard_14_b", "grey", 2));
			calls.Push(Text(984, y + 24, Clip(about, 72), "ft_standard_13_b", bad ? "white" : "grey"));
		}
		if (!Error.empty()) calls.Push(Text(984, 855, Clip(Error, 60), "ft_standard_13_b", "white"));
		else if (pending) calls.Push(Text(984, 855, "Restart the game to apply the changes", "ft_standard_13_b", "white"));
		else if (g_LastSession.GetBool("crashed", false))
			calls.Push(Text(984, 855, g_LastSession.GetBool("during_startup", false)
				? "The game crashed while starting last time - a mod may be the cause"
				: "The game closed unexpectedly last time", "ft_standard_13_b", "white"));
		else calls.Push(Text(984, 855, "Mods are in the game folder: mods\\aurie and mods\\lua", "ft_standard_13_b", "grey"));
		return O({ { "when", welcome }, { "calls", std::move(calls) } });
	}

	// ---- Options menu: a "Mods" tab with every mod's settings, drawn like the game's own settings ----
	bool OptOpen = false;
	std::string OptMod, OptError;
	constexpr double kOptBtnX = 68, kOptBtnY = 601;       // thin button between Contact Support and Main Menu
	constexpr double kOptPanelX = 515, kOptPanelY = 148;  // spr_settings_mono_large as the game draws it

	Json Op(const char* Name, std::initializer_list<Json> Args) { return O({ { "$op", Name }, { "args", A(Args) } }); }

	// the game's selector (label, < box >) at the position scr_ui_settings_create_choice uses
	void Selector(Json& Calls, double X, double Y, const std::string& Label, const std::string& Value, const std::string& Act)
	{
		Calls.Push(Text(X + 48, Y, Clip(Label, 34), "ft_standard_12", "white"));
		Calls.Push(Call("draw_set_color", A({ 0x020202 })));
		Calls.Push(Call("draw_rectangle", A({ X + 19, Y + 18, X + 278, Y + 54, false })));
		Calls.Push(Call("draw_set_color", A({ 0x101010 })));
		Calls.Push(Call("draw_rectangle", A({ X + 23, Y + 22, X + 274, Y + 50, false })));
		Calls.Push(Call("draw_set_color", A({ 0xFFFFFF })));
		Calls.Push(Text(X + 48, Y + 30, Clip(Value, 28), "ft_standard_12", "grey"));
		for (auto [spr, dx, op] : { std::tuple{ "spr_selector_btn_l_v2", -8.0, "prev" }, std::tuple{ "spr_selector_btn_r_v2", 288.0, "next" } })
		{
			Calls.Push(Call("draw_sprite", A({ Asset(spr), 0, X + dx, Y + 12 })));
			Calls.Push(Call("$mba_push", A({ A({ "sel", Asset(spr), X + dx, Y + 12, 24, 36, "", Asset(kSink), "slormapi:" + Act + op }) })));
		}
	}

	// the game's radio (diamond, filled when on)
	void Radio(Json& Calls, double X, double Y, const std::string& Label, bool On, const std::string& Act)
	{
		Calls.Push(Call("draw_sprite", A({ Asset("spr_radio_btn_v3"), 0, X, Y - 4 })));
		if (On) Calls.Push(Call("draw_sprite", A({ Asset("spr_radio_cross_v2"), 0, X + 18, Y + 14 })));
		Calls.Push(Text(X + 48, Y + 14, Clip(Label, 40), "ft_standard_12", "white"));
		Calls.Push(Call("$mba_push", A({ A({ "sel", Asset("spr_radio_btn_v3"), X, Y - 4, 36, 36, On ? Json(788) : Json(""),
			Asset(kSink), "slormapi:" + Act + "toggle" }) })));
	}

	Json BuildOptionsProgram()
	{
		Json calls = Json::Array();
		auto entry = [&](const char* Name, const char* Spr, double X, double Y, double W, double H, const std::string& Label, const std::string& Action)
		{
			calls.Push(Call("$mba_push", A({ A({ Name, Asset(Spr), X, Y, W, H, Label, Asset(kSink), "slormapi:" + Action }) })));
		};
		calls.Push(Call("draw_sprite", A({ Asset("spr_menu_thin_grey_button"), 0, kOptBtnX, kOptBtnY })));
		calls.Push(Call("scr_draw_text", A({ kOptBtnX + 168, kOptBtnY + 26, "Mods", 1, 1, "ft_standard_12", 1, 1, 0, 1, "white", "black", 1, true })));
		if (!OptOpen)
		{
			entry("ModsTab", "spr_menu_thin_grey_button", kOptBtnX, kOptBtnY, 336, 52, "Mods", "opt");
			return O({ { "calls", std::move(calls) } });
		}

		// the game's current tab has no button entry while it is selected: give it one so it can be clicked again
		Json p = O({ { "$self", "settings_panel" } });
		auto pick = [&](Json A0, Json A1, Json A2, Json A3)
		{
			return Op("?", { Op("==", { p, 0 }), A0, Op("?", { Op("==", { p, 1 }), A1, Op("?", { Op("==", { p, 2 }), A2, A3 }) }) });
		};
		calls.Push(Call("$mba_push", A({ A({ "Tab", Asset("spr_menu_large_grey_button"), 68, pick(148, 226, 304, 450), 336, 62,
			pick("Gameplay", "Settings", "Keybinds", "Codex"), Asset("scr_ui_settings_change_panel"), p }) })));
		// the game's controls of that tab sit under our panel: drop them, then paint the panel over the bake
		calls.Push(Call("$mba_remove_in", A({ kOptPanelX + 1, kOptPanelY + 1, kOptPanelX + 1336, kOptPanelY + 789 })));
		calls.Push(Call("draw_sprite", A({ Asset("spr_settings_mono_large"), 0, kOptPanelX, kOptPanelY })));
		calls.Push(Text(1184, 190, "MODS", "ft_standard_16", "white", 1));

		if (g_Settings.empty())
		{
			calls.Push(Text(1184, 520, "No installed mod has settings.", "ft_standard_14", "grey", 1));
			return O({ { "calls", std::move(calls) } });
		}
		if (!g_Settings.count(OptMod)) OptMod = g_Settings.begin()->first;

		// left: the mods that have settings
		double y = 250;
		for (auto& [name, st] : g_Settings)
		{
			if (y > 860) break;
			calls.Push(Call("draw_sprite", A({ Asset("spr_menu_thin_grey_button"), 0, 560, y })));
			calls.Push(Call("scr_draw_text", A({ 728, y + 26, Clip(name, 26), 1, 1, "ft_standard_12", 1, 1, 0, 1, "white", "black", 1, true })));
			if (name != OptMod) entry("ModsTabMod", "spr_menu_thin_grey_button", 560, y, 336, 52, Clip(name, 26), "optmod:" + name);
			y += 66;
		}
		calls.Push(Call("draw_sprite", A({ Asset("spr_settings_separator"), 0, 926, 290 })));

		// right: the chosen mod's settings, in two columns
		std::string title = OptMod, about;
		for (auto& m : g_Registered)
			if (m.Name == OptMod)
			{
				if (!m.Version.empty()) title += "  v" + m.Version;
				about = m.Description;
			}
		calls.Push(Text(990, 250, title, "ft_standard_16", "white"));
		if (!about.empty()) calls.Push(Text(990, 276, Clip(about, 90), "ft_standard_12", "grey"));
		auto& st = g_Settings[OptMod];
		double cx = 990, cy = 320;
		for (auto& d : st.Defs)
		{
			double h = d.Type == "bool" ? (d.Help.empty() ? 44.0 : 64.0) : (d.Help.empty() ? 84.0 : 104.0);
			if (cy + h > 900)
			{
				if (cx > 1000) break;                            // both columns full
				cx = 1420;
				cy = 320;
			}
			std::string act = "set:" + OptMod + "|" + d.Key + "|";
			Json v = SettingValue(st, d);
			if (d.Type == "bool")
			{
				Radio(calls, cx, cy, d.Label, v.type == Json::Bool && v.b, act);
				if (!d.Help.empty()) calls.Push(Text(cx + 48, cy + 38, Clip(d.Help, 60), "ft_standard_11", "grey"));
			}
			else
			{
				Selector(calls, cx, cy, d.Label, SettingText(v), act);
				if (!d.Help.empty()) calls.Push(Text(cx + 48, cy + 74, Clip(d.Help, 60), "ft_standard_11", "grey"));
			}
			cy += h;
		}
		if (!OptError.empty()) calls.Push(Text(990, 905, Clip(OptError, 80), "ft_standard_12", "white"));
		return O({ { "calls", std::move(calls) } });
	}

	RValue UiInstance()
	{
		RValue obj = AssetIndex("obj_ui_next_gen");
		if (g_Yytk->CallBuiltin("instance_number", { obj }).ToDouble() < 1) return RValue();
		return g_Yytk->CallBuiltin("instance_find", { obj, RValue(0.0) });
	}

	bool LayersPresent()
	{
		for (const char* s : { kSink, "scr_ui_menus_main", "scr_ui_menus_settings", "scr_ui_settings_change_panel" })
		{
			int k = FindHook(ScriptName(s));
			if (k < 0 || !g_Hooks[k].Layer(kOwner)) return false;
		}
		return true;
	}

	void Apply(const Json& Mods)
	{
		Json sink = O({ { "cmd", "hook" }, { "owner", kOwner }, { "script", kSink }, { "capture", true }, { "depth", 1 },
			{ "capture_args", A({ "slormapi:*" }) }, { "overrides", A({ O({ { "args", A({ "slormapi:*" }) }, { "skip", true } }) }) } });
		Json bake = O({ { "cmd", "hook" }, { "owner", kOwner }, { "script", "scr_ui_menus_main" }, { "capture", false },
			{ "after", BuildProgram(Mods) } });
		Json options = O({ { "cmd", "hook" }, { "owner", kOwner }, { "script", "scr_ui_menus_settings" }, { "capture", false },
			{ "after", BuildOptionsProgram() } });
		Json tabs = O({ { "cmd", "hook" }, { "owner", kOwner }, { "script", "scr_ui_settings_change_panel" }, { "capture", true },
			{ "depth", 0 } });
		for (auto* req : { &sink, &bake, &options, &tabs })
		{
			Json r = CmdHook(*req);
			for (auto& [k, v] : r.o)
				if (v.type == Json::Str) Fail(v.s);
		}
	}

	void Rebake()
	{
		RValue ui = UiInstance();
		if (KindOf(ui) == VALUE_UNDEFINED) return;
		std::string menu = GmlString(g_Yytk->CallBuiltin("variable_instance_get", { ui, RValue("menu_show") }));
		if (menu == "main" || menu == "settings")
			g_Yytk->CallBuiltin("variable_instance_set", { ui, RValue("rebake_menu"), RValue(true) });
	}

	// Every frame (obj_ui_next_gen's Step): clicks, closing with the menu, our layers (checked every 2 s).
	Json Mods;
	size_t RegisteredSeen = 0;
	double MenuCheckAt = 0;

	void TickInner(double Now)
	{
		bool changed = false;
		if (auto it = g_HookLogs.find(kOwner); it != g_HookLogs.end() && !it->second.empty())
		{
			for (auto& e : it->second)
			{
				if (e.Script == "gml_Script_scr_ui_settings_change_panel")   // a game tab was chosen: ours closes
				{
					if (OptOpen) { OptOpen = false; changed = true; }
					continue;
				}
				if (e.Args.a.empty() || e.Args.a[0].type != Json::Str || !e.Args.a[0].s.starts_with("slormapi:")) continue;
				std::string act = e.Args.a[0].s.substr(9);
				changed = true;
				if (act == "mods")
				{
					Open = !Open;
					if (Open) { Mods = Json(); Page = 0; Error.clear(); CrashSeen = true; }   // a fresh list each time
				}
				else if (act.starts_with("ui:"))                     // a widget: ui:<owner>|<id>|<action>
				{
					std::string rest = act.substr(3);
					size_t a = rest.find('|'), b = rest.rfind('|');
					if (a != std::string::npos && b > a)
					{
						Json d = Json::Object();
						d.Add("owner", rest.substr(0, a));
						d.Add("id", rest.substr(a + 1, b - a - 1));
						d.Add("action", rest.substr(b + 1));
						g_DirectEvents.emplace_back("ui_clicked", std::move(d));
					}
				}
				else if (act == "page+") ++Page;
				else if (act == "page-") --Page;
				else if (act == "opt") { OptOpen = true; OptError.clear(); }
				else if (act.starts_with("optmod:")) { OptMod = act.substr(7); OptError.clear(); }
				else if (act.starts_with("set:"))                      // set:<mod>|<key>|<toggle / prev / next>
				{
					std::string rest = act.substr(4);
					size_t a = rest.find('|'), b = rest.rfind('|');
					OptError.clear();
					if (a != std::string::npos && b > a)
						try { StepSetting(rest.substr(0, a), rest.substr(a + 1, b - a - 1), rest.substr(b + 1)); }
						catch (const std::exception& x) { OptError = x.what(); }
				}
				else if (act.starts_with("toggle:"))
				{
					std::string file = act.substr(7);
					bool on = true;
					for (auto& m : (Mods.IsNull() ? CmdMods() : Mods).a)
						if (m.GetStr("file") == file) on = m.GetBool("enabled", true);
					Error.clear();
					try { CmdModEnable(O({ { "file", file }, { "enabled", !on } })); }
					catch (const std::exception& x) { Error = x.what(); }
					Mods = Json();
				}
			}
			it->second.clear();
		}
		if ((Open || OptOpen) && !changed && Now >= MenuCheckAt)
		{
			MenuCheckAt = Now + 0.25;
			RValue ui = UiInstance();
			auto var = [&](const char* Name) { return g_Yytk->CallBuiltin("variable_instance_get", { ui, RValue(Name) }); };
			std::string menu = KindOf(ui) == VALUE_UNDEFINED ? "" : GmlString(var("menu_show"));
			if (Open && (menu != "main" || Num(var("main_panel")) != 0))
			{
				Open = false;                              // menu closed or another panel: Welcome again next time
				changed = true;
			}
			if (OptOpen && menu != "settings") { OptOpen = false; changed = true; }   // Options closed
		}
		if (g_Registered.size() != RegisteredSeen) { RegisteredSeen = g_Registered.size(); Mods = Json(); changed = true; }
		if (g_SettingsVer != SettingsSeen) { SettingsSeen = g_SettingsVer; changed = true; }
		if (LuaVersion() != LuaSeen) { LuaSeen = LuaVersion(); Mods = Json(); changed = true; }
		if (!changed)
		{
			if (Now < CheckAt) return;
			CheckAt = Now + 2;
			if (LayersPresent()) return;
		}
		if (Mods.IsNull()) Mods = CmdMods();
		Apply(Mods);
		Rebake();
	}

	void Tick()
	{
		double now = MicrosSinceStart() / 1e6;
		WatchGameWindow();
		if (now > 20) SessionReachedMenu();
		if (Disabled) return;
		if (now < RetryAt) return;
		try { TickInner(now); }
		catch (const std::exception& e)
		{
			DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormAPI] Mods page: %s", e.what());
			RetryAt = now + 5;
			CheckAt = 0;
		}
	}
}

static Json CmdModsPage(const Json& Req)
{
	if (const Json* on = Req.Find("enabled"))
	{
		modspage::Disabled = !on->b;
		if (modspage::Disabled)
		{
			CmdUnhook(modspage::O({ { "owner", modspage::kOwner } }));
			modspage::Open = false;
			modspage::Rebake();
		}
		modspage::CheckAt = 0;
	}
	if (const Json* open = Req.Find("open")) { modspage::Open = open->b; modspage::Mods = Json(); modspage::CheckAt = 0; modspage::RegisteredSeen = 0; }
	Json j = Json::Object();
	j.Add("enabled", !modspage::Disabled);
	j.Add("open", modspage::Open);
	return j;
}

// ============================================================================
// Events: what happens in the game, delivered once per frame (obj_ui_next_gen's Step) to C++ callbacks (Subscribe)
// and to JSON subscribers ("subscribe" / "events"). Script events come from SlormAPI's own capture layers (owner
// "SlormAPI.events", installed only while someone listens); the rest are cheap per-frame checks.
// ============================================================================

namespace events
{
	constexpr const char* kOwner = "SlormAPI.events";
	constexpr size_t kQueueCap = 2000;

	struct ScriptEvent { const char* Event; const char* Script; int Depth; std::vector<std::string> SelfVars; };
	const std::vector<ScriptEvent>& ScriptEvents()
	{
		static const std::vector<ScriptEvent> list = {
			{ "enemy_killed", "scr_hero_stats_on_kill", 0, { "enemy_level" } },   // self = the enemy, arg 0 = loot profile
			{ "hero_hit", "scr_hero_proc_when_hit", 0, { "the_health", "final_the_max_health" } },   // damage, attacker
			{ "hero_died", "scr_hero_dead", 0, {} },
			{ "item_picked_up", "scr_pick_up_item", 3, {} },                     // arg 0 = the items, result = picked
			{ "skill_cast", "scr_hero_proc_when_any_cast", 0, {} },              // once per cast, arg 0 = the skill
			{ "run_started", "scr_run_start", 0, {} },
			{ "run_ended", "scr_run_end", 0, {} },
		};
		return list;
	}
	const char* const kWatched[] = { "loot_dropped", "gold_changed", "level_up", "item_equipped", "room_changed", "menu_changed" };
	const char* const kDirect[] = { "setting_changed", "boss_killed", "ui_clicked" };   // from commands (g_DirectEvents) or other events

	struct Sub { uint64_t Id; std::string Event; SlormEventCallback Fn; void* User; };
	std::vector<Sub> Subs;
	std::map<std::string, std::set<std::string>> Owners;   // JSON subscribers: owner -> events
	std::map<std::string, std::deque<Json>> Queues;        // owner -> events not read yet
	uint64_t NextId = 1, Seq = 0, Dropped = 0;
	bool HooksDirty = true;
	double CheckAt = 0;

	bool Known(const std::string& E)
	{
		if (E == "*") return true;
		for (auto& s : ScriptEvents()) if (E == s.Event) return true;
		for (auto* w : kWatched) if (E == w) return true;
		for (auto* w : kDirect) if (E == w) return true;
		return false;
	}
	std::set<std::string> WantedSet;
	bool WantedDirty = true;
	bool Wanted(const std::string& E)
	{
		if (WantedDirty)
		{
			WantedSet.clear();
			for (auto& s : Subs) WantedSet.insert(s.Event);
			for (auto& [o, set] : Owners) WantedSet.insert(set.begin(), set.end());
			WantedDirty = false;
		}
		return WantedSet.count(E) || WantedSet.count("*");
	}

	// a script event's hook also serves the events made from it
	bool WantedScript(const ScriptEvent& Se)
	{
		return Wanted(Se.Event) || (!strcmp(Se.Event, "enemy_killed") && Wanted("boss_killed"));
	}

	// one layer per wanted script event, none for the others
	void SyncHooks()
	{
		for (auto& se : ScriptEvents())
		{
			int k = FindHook(ScriptName(se.Script));
			bool have = k >= 0 && g_Hooks[k].Layer(kOwner);
			if (WantedScript(se) && !have)
			{
				Json req = Json::Object();
				req.Add("owner", kOwner);
				req.Add("script", se.Script);
				req.Add("capture", true);
				req.Add("depth", se.Depth);
				Json sv = Json::Array();
				for (auto& v : se.SelfVars) sv.Push(Json(v));
				req.Add("self_vars", sv);
				Json r = CmdHook(req);
				for (auto& [name, v] : r.o)
					if (v.type == Json::Str) DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormAPI] event %s: %s", se.Event, v.s.c_str());
			}
			else if (!WantedScript(se) && have)
			{
				Json req = Json::Object();
				req.Add("owner", kOwner);
				req.Add("script", se.Script);
				CmdUnhook(req);
			}
		}
	}

	// per-frame watchers
	int64_t MaxLoot = -1;
	double LootCount = -1, LootAt = 0;
	double Gold = -1, Level = -1, LevelClass = -1;
	std::string Room, Menu;
	bool Primed = false;
	std::set<std::string> Seen;   // watchers that have their first reading (the state when listening started)
	double EquipAt = 0;
	int EquipHero = -1;
	std::vector<std::pair<void*, size_t>> EquipSig;   // per slot: the item array (identity) and its length
	std::vector<Json> EquipItems;

	RValue Global(const char* Name)
	{
		RValue v;
		RValue* m = FindMember(GlobalRValue(), Name);
		return m ? *m : v;
	}

	void Emit(std::vector<std::pair<std::string, Json>>& Out, const char* Event, Json Data) { Out.emplace_back(Event, std::move(Data)); }

	void Watch(std::vector<std::pair<std::string, Json>>& Out)
	{
		for (auto [w, key] : { std::pair{ "loot_dropped", "loot" }, std::pair{ "gold_changed", "gold" },
			std::pair{ "level_up", "level" }, std::pair{ "item_equipped", "equip" }, std::pair{ "room_changed", "room" },
			std::pair{ "menu_changed", "menu" } })
			if (!Wanted(w)) Seen.erase(key);
		if (Wanted("loot_dropped"))
		{
			RValue obj = AssetIndex("obj_loot");
			int64_t maxSeen = MaxLoot;
			double count = g_Yytk->CallBuiltin("instance_number", { obj }).ToDouble(), now = MicrosSinceStart() / 1e6;
			bool look = count > 0 && (count != LootCount || now >= LootAt);
			LootCount = count;
			if (look) LootAt = now + 0.5;
			if (look)
				for (CInstance* inst : InstancesOf(obj))
				{
					int64_t id = InstanceId(inst);
					if (id <= MaxLoot) continue;
					maxSeen = (std::max)(maxSeen, id);
					if (!Seen.count("loot")) continue;         // what lies around when listening starts is not new
					Json d = Json::Object();
					d.Add("loot", InstanceStub(inst));
					RValue ar = g_Yytk->CallBuiltin("variable_instance_get", { RValue(inst), RValue("ar") });
					d.Add("item", ToJson(ar, 3));
					try { d.Add("info", ItemInfo(*d.Find("item"))); }
					catch (const std::exception&) {}
					Emit(Out, "loot_dropped", std::move(d));
				}
			MaxLoot = maxSeen;
			Seen.insert("loot");
		}
		if (Wanted("gold_changed"))
		{
			RValue g = Global("gold");
			double gold = IsNumeric(g) ? g.ToDouble() : -1;
			if (Seen.count("gold") && Gold >= 0 && gold >= 0 && gold != Gold)
			{
				Json d = Json::Object();
				d.Add("gold", gold);
				d.Add("delta", gold - Gold);
				Emit(Out, "gold_changed", std::move(d));
			}
			Gold = gold;
			Seen.insert("gold");
		}
		if (Wanted("level_up"))                            // global.level[current_hero] went up (same hero)
		{
			RValue cls = Global("current_hero"), lv = Global("level");
			RValue* l = IsNumeric(cls) ? ArrayAt(lv, static_cast<size_t>(cls.ToDouble())) : nullptr;
			double level = l && IsNumeric(*l) ? l->ToDouble() : -1, c = IsNumeric(cls) ? cls.ToDouble() : -1;
			if (Seen.count("level") && c == LevelClass && level > Level && Level >= 0)
			{
				Json d = Json::Object();
				d.Add("level", level);
				d.Add("previous", Level);
				d.Add("hero", c);                          // 0 Knight, 1 Huntress, 2 Mage
				Emit(Out, "level_up", std::move(d));
			}
			Level = level;
			LevelClass = c;
			Seen.insert("level");
		}
		if (Wanted("item_equipped") && MicrosSinceStart() / 1e6 >= EquipAt)   // the hero's 11 slots, 4 times a second
		{
			EquipAt = MicrosSinceStart() / 1e6 + 0.25;
			int hero = CurrentHero();
			RValue* inv = FindMember(GlobalRValue(), "inventory");
			RValue* cls = inv ? ArrayAt(*inv, static_cast<size_t>(hero)) : nullptr;
			if (cls)
			{
				bool fresh = !Seen.count("equip") || hero != EquipHero;
				if (fresh) { EquipSig.assign(11, { nullptr, 0 }); EquipItems.assign(11, Json()); }
				for (size_t i = 0; i < 11; ++i)
				{
					RValue* it = ArrayAt(*cls, i);
					std::pair<void*, size_t> sig{ it && KindOf(*it) == VALUE_ARRAY ? it->m_Pointer : nullptr, it ? ArrayLen(*it) : 0 };
					if (!fresh && sig == EquipSig[i]) continue;
					Json item = it ? ToJson(*it, 4) : Json();
					if (!fresh)
					{
						Json d = Json::Object();
						d.Add("index", static_cast<int>(i));
						d.Add("slot", kSlotKeys[i]);
						d.Add("hero", hero);
						d.Add("item", item);
						d.Add("previous", EquipItems[i]);
						try { d.Add("info", ItemInfo(item)); }
						catch (const std::exception&) {}
						Emit(Out, "item_equipped", std::move(d));
					}
					EquipSig[i] = sig;
					EquipItems[i] = std::move(item);
				}
				EquipHero = hero;
				Seen.insert("equip");
			}
		}
		if (Wanted("room_changed"))
		{
			RValue room;
			std::string name;
			if (AurieSuccess(g_Yytk->GetBuiltin("room", nullptr, NULL_INDEX, room)))
				name = GmlString(g_Yytk->CallBuiltin("room_get_name", { room }));
			if (Seen.count("room") && name != Room)
			{
				Json d = Json::Object();
				d.Add("room", name);
				d.Add("previous", Room);
				Emit(Out, "room_changed", std::move(d));
			}
			Room = name;
			Seen.insert("room");
		}
		if (Wanted("menu_changed"))
		{
			RValue ui = modspage::UiInstance();
			std::string menu = KindOf(ui) == VALUE_UNDEFINED ? "" : GmlString(g_Yytk->CallBuiltin("variable_instance_get", { ui, RValue("menu_show") }));
			if (Seen.count("menu") && menu != Menu)
			{
				Json d = Json::Object();
				d.Add("menu", menu);
				d.Add("previous", Menu);
				Emit(Out, "menu_changed", std::move(d));
			}
			Menu = menu;
			Seen.insert("menu");
		}
		Primed = true;
	}

	bool DeadThisFrame = false, DeadSent = false;

	bool HeroAlive()
	{
		RValue obj = AssetIndex("obj_parent_hero");
		if (g_Yytk->CallBuiltin("instance_number", { obj }).ToDouble() < 1) return false;
		RValue hero = g_Yytk->CallBuiltin("instance_find", { obj, RValue(0.0) });
		return GmlString(g_Yytk->CallBuiltin("variable_instance_get", { hero, RValue("actual_state") })) != "dead";
	}

	// the calls our layers captured since the last frame, as events
	void Convert(std::vector<std::pair<std::string, Json>>& Out)
	{
		auto it = g_HookLogs.find(kOwner);
		if (it == g_HookLogs.end()) return;
		for (auto& e : it->second)
		{
			const Json* vars = e.Self.Find("vars");
			auto var = [&](const char* N) { const Json* v = vars ? vars->Find(N) : nullptr; return v ? *v : Json(); };
			auto arg = [&](size_t I) { return I < e.Args.a.size() ? e.Args.a[I] : Json(); };
			auto stub = [](const Json& J)
			{
				Json s = Json::Object();
				if (const Json* i = J.Find("$inst")) s.Add("$inst", *i);
				if (const Json* o = J.Find("$object")) s.Add("$object", *o);
				return s;
			};
			std::string script = e.Script.substr(11);   // without gml_Script_
			Json d = Json::Object();
			if (script == "scr_hero_stats_on_kill")
			{
				d.Add("enemy", stub(e.Self));
				if (const Json* x = e.Self.Find("$x")) d.Add("x", *x);
				if (const Json* y = e.Self.Find("$y")) d.Add("y", *y);
				d.Add("level", var("enemy_level"));
				d.Add("profile", arg(0));                 // trash / good / elite / boss
				if (gd::Str(arg(0)) == "boss") Emit(Out, "boss_killed", d);
				Emit(Out, "enemy_killed", std::move(d));
			}
			else if (script == "scr_hero_proc_when_hit")
			{
				d.Add("damage", arg(0));
				d.Add("attacker", stub(arg(1)));
				d.Add("life", var("the_health"));
				d.Add("max_life", var("final_the_max_health"));
				Emit(Out, "hero_hit", std::move(d));
			}
			else if (script == "scr_hero_dead") DeadThisFrame = true;   // runs every step while dead: one event per death
			else if (script == "scr_hero_proc_when_any_cast")
			{
				int hero = CurrentHero(), id = static_cast<int>(gd::Num(arg(0), -1));
				d.Add("skill", id);
				d.Add("hero", hero);
				try { d.Add("name", Data().SkillName(hero, id)); }
				catch (const std::exception&) {}
				Emit(Out, "skill_cast", std::move(d));
			}
			else if (script == "scr_run_start") Emit(Out, "run_started", std::move(d));
			else if (script == "scr_run_end") Emit(Out, "run_ended", std::move(d));
			else if (script == "scr_pick_up_item")
			{
				d.Add("items", arg(0));
				d.Add("picked", e.Result);
				Emit(Out, "item_picked_up", std::move(d));
			}
		}
		it->second.clear();
	}

	void CallGuarded(const Sub& S, const char* Event, const char* Data);

	void Tick()
	{
		if (Subs.empty() && Owners.empty())
		{
			if (HooksDirty) { SyncHooks(); HooksDirty = false; g_HookLogs.erase(kOwner); }   // nobody listens: no layers
			if (Primed) { Primed = false; MaxLoot = -1; Gold = -1; Seen.clear(); }
			g_DirectEvents.clear();
			return;
		}
		double now = MicrosSinceStart() / 1e6;
		if (HooksDirty || now >= CheckAt) { SyncHooks(); HooksDirty = false; CheckAt = now + 2; }
		std::vector<std::pair<std::string, Json>> out;
		out.swap(g_DirectEvents);
		Convert(out);
		if (DeadThisFrame && !DeadSent) { Emit(out, "hero_died", Json::Object()); DeadSent = true; }
		else if (!DeadThisFrame && DeadSent && HeroAlive()) DeadSent = false;
		DeadThisFrame = false;
		Watch(out);
		for (auto& [name, data] : out)
		{
			++Seq;
			std::string dump;
			auto subs = Subs;                              // a callback may (un)subscribe
			for (auto& s : subs)
				if (s.Event == name || s.Event == "*")
				{
					if (dump.empty()) dump = sj::Dump(data);
					CallGuarded(s, name.c_str(), dump.c_str());
				}
			for (auto& [owner, set] : Owners)
				if (set.count(name) || set.count("*"))
				{
					Json ev = Json::Object();
					ev.Add("seq", Seq);
					ev.Add("event", name);
					ev.Add("t_us", MicrosSinceStart());
					ev.Add("data", data);
					auto& q = Queues[owner];
					q.push_back(std::move(ev));
					while (q.size() > kQueueCap) { q.pop_front(); ++Dropped; }
				}
		}
	}

	uint64_t Subscribe(const std::string& Event, SlormEventCallback Fn, void* User)
	{
		if (!Fn || !Known(Event)) return 0;
		Subs.push_back({ NextId, Event, Fn, User });
		HooksDirty = true; WantedDirty = true;
		return NextId++;
	}
	void Unsubscribe(uint64_t Id)
	{
		std::erase_if(Subs, [&](const Sub& S) { return S.Id == Id; });
		HooksDirty = true; WantedDirty = true;
	}
}

// A crash in another mod's callback must not take the game down: drop that subscription instead.
struct EventCall { SlormEventCallback Fn; const char* Event; const char* Data; void* User; };
static void CallEvent(void* P) { auto* c = static_cast<EventCall*>(P); c->Fn(c->Event, c->Data, c->User); }
static DWORD RunGuarded(void (*Fn)(void*), void* P);
void events::CallGuarded(const Sub& S, const char* Event, const char* Data)
{
	EventCall c{ S.Fn, Event, Data, S.User };
	if (DWORD code = RunGuarded(CallEvent, &c))
	{
		DbgPrintEx(LOG_SEVERITY_ERROR, "[SlormAPI] event callback crashed (0x%08lX) on %s: unsubscribed", code, Event);
		Unsubscribe(S.Id);
	}
}

// {"owner", "events": [...]}: queue these events for owner (read them with "events"); "*" = all.
static Json CmdSubscribe(const Json& Req)
{
	std::string owner = Req.GetStr("owner");
	auto& set = events::Owners[owner];
	for (auto& e : Req.At("events").a)
	{
		if (!events::Known(e.s)) Fail("unknown event '" + e.s + "'");
		set.insert(e.s);
	}
	events::HooksDirty = true; events::WantedDirty = true;
	Json out = Json::Array();
	for (auto& e : set) out.Push(Json(e));
	return out;
}

// {"owner", "events"?}: stop (some of) owner's events; without "events" all of them, and drop its queue.
static Json CmdUnsubscribe(const Json& Req)
{
	std::string owner = Req.GetStr("owner");
	if (const Json* list = Req.Find("events"))
	{
		for (auto& e : list->a) events::Owners[owner].erase(e.s);
		if (events::Owners[owner].empty()) events::Owners.erase(owner);
	}
	else { events::Owners.erase(owner); events::Queues.erase(owner); }
	events::HooksDirty = true; events::WantedDirty = true;
	return Json(true);
}

// {"owner", "max"}: owner's queued events, oldest first (consumed).
static Json CmdEvents(const Json& Req)
{
	auto& q = events::Queues[Req.GetStr("owner")];
	size_t n = (std::min)(static_cast<size_t>(Req.GetNum("max", 1000)), q.size());
	Json list = Json::Array();
	for (size_t k = 0; k < n; ++k) list.Push(std::move(q[k]));
	q.erase(q.begin(), q.begin() + n);
	Json out = Json::Object();
	out.Add("events", std::move(list));
	out.Add("remaining", q.size());
	out.Add("dropped", events::Dropped);
	return out;
}

// C++ callbacks per event, and the JSON owners listening
static Json EventListeners()
{
	std::map<std::string, int> counts;
	for (auto& sub : events::Subs) ++counts[sub.Event];
	Json cb = Json::Object();
	for (auto& [e, n] : counts) cb.Add(e, n);
	Json owners = Json::Array();
	for (auto& [o, set] : events::Owners) owners.Push(Json(o));
	Json j = Json::Object();
	j.Add("callbacks", std::move(cb));
	j.Add("owners", std::move(owners));
	return j;
}

static Json CmdEventTypes()
{
	Json out = Json::Array();
	for (auto& s : events::ScriptEvents()) out.Push(Json(s.Event));
	for (auto* w : events::kWatched) out.Push(Json(w));
	for (auto* w : events::kDirect) out.Push(Json(w));
	return out;
}

// ============================================================================
// UI: a mod's drawing in any menu by name ("main", "inventory", "settings", "skills", ...): its program runs after the
// game bakes that menu (scr_ui_menus_<menu>), in the mod's own hook layer
// ============================================================================

static void UiRefresh()
{
	RValue ui = modspage::UiInstance();
	if (KindOf(ui) == VALUE_UNDEFINED) return;
	if (GmlString(g_Yytk->CallBuiltin("variable_instance_get", { ui, RValue("menu_show") })) != "none")
		g_Yytk->CallBuiltin("variable_instance_set", { ui, RValue("rebake_menu"), RValue(true) });
}

static std::string MenuScript(const Json& Req)
{
	std::string menu = Req.GetStr("menu");
	if (menu.empty() || menu.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") != std::string::npos) Fail("bad menu name '" + menu + "'");
	return "scr_ui_menus_" + menu;
}

// {"owner", "menu", "program": {vars, when, calls}} (replaces the owner's earlier program for that menu)
static Json CmdUiAdd(const Json& Req)
{
	Json req = Json::Object();
	req.Add("owner", Req.GetStr("owner"));
	req.Add("script", MenuScript(Req));
	req.Add("capture", false);
	req.Add("after", Req.At("program"));
	Json r = CmdHook(req);
	for (auto& [k, v] : r.o)
		if (v.type == Json::Str) Fail(v.s);
	UiRefresh();
	return Json(true);
}

static Json CmdUiRemove(const Json& Req)
{
	RValue ui = modspage::UiInstance();
	if (KindOf(ui) != VALUE_UNDEFINED)
	{
		RValue mba = g_Yytk->CallBuiltin("variable_instance_get", { ui, RValue("mba") });
		if (KindOf(mba) == VALUE_ARRAY) MbaRemoveOwner(mba, Req.GetStr("owner"));
	}
	Json req = Json::Object();
	req.Add("owner", Req.GetStr("owner"));
	req.Add("script", MenuScript(Req));
	Json r = CmdUnhook(req);
	UiRefresh();
	return r;
}

// {"enable": bool, "reset": bool} -> SlormAPI's own time per frame since the last reset (the overlay's draw is included
// in "callbacks"; "detours" excludes the hooked scripts' own run)
static Json CmdPerf(const Json& Req)
{
	if (const Json* e = Req.Find("enable")) g_Perf = e->b;
	if (Req.GetBool("reset", false) || Req.Find("enable"))
	{
		g_PerfCallbackUs = g_PerfTickUs = g_PerfDetourUs = 0;
		g_PerfPartUs[0] = g_PerfPartUs[1] = g_PerfPartUs[2] = 0;
		g_PerfFrames = g_PerfEvents = g_PerfDetours = 0;
	}
	double f = g_PerfFrames ? static_cast<double>(g_PerfFrames) : 1;
	Json j = Json::Object();
	j.Add("enabled", g_Perf);
	j.Add("frames", g_PerfFrames);
	j.Add("callbacks_us_per_frame", (g_PerfCallbackUs - g_PerfTickUs) / f);   // every object event: request pump + checks
	j.Add("ticks_us_per_frame", g_PerfTickUs / f);                           // Mods page, events, Lua
	j.Add("modspage_us_per_frame", g_PerfPartUs[0] / f);
	j.Add("events_us_per_frame", g_PerfPartUs[1] / f);
	j.Add("lua_us_per_frame", g_PerfPartUs[2] / f);
	j.Add("detours_us_per_frame", g_PerfDetourUs / f);                       // hook layers around the game's scripts
	j.Add("object_events_per_frame", static_cast<double>(g_PerfEvents) / f);
	j.Add("hooked_calls_per_frame", static_cast<double>(g_PerfDetours) / f);
	return j;
}

static Json Dispatch(const Json& Req);

static Json SafeDispatch(const Json& Req)
{
	Json resp = Json::Object();
	try
	{
		resp.Add("result", Dispatch(Req));
		resp.Add("ok", true);
	}
	catch (const std::exception& e) { resp.Add("ok", false); resp.Add("error", e.what()); }
	catch (...) { resp.Add("ok", false); resp.Add("error", "unknown exception (likely a GML runtime error)"); }
	return resp;
}

static Json Dispatch(const Json& Req)
{
	std::string cmd = Req.GetStr("cmd");
	if (cmd == "ping") return CmdPing();
	if (cmd == "get") return CmdGet(Req);
	if (cmd == "keys") return CmdKeys(Req);
	if (cmd == "set") return CmdSet(Req);
	if (cmd == "call") return CmdCall(Req);
	if (cmd == "instances") return CmdInstances(Req);
	if (cmd == "objects") return CmdObjects();
	if (cmd == "scripts") return CmdScripts();
	if (cmd == "hook") return CmdHook(Req);
	if (cmd == "unhook") return CmdUnhook(Req);
	if (cmd == "hooks") return CmdHooks();
	if (cmd == "hooklog") return CmdHookLog(Req);
	if (cmd == "runner") return CmdRunner();
	if (cmd == "routines") return CmdRoutines(Req);
	if (cmd == "routine_index") return CmdRoutineIndex(Req);
	if (cmd == "overlay") return CmdOverlay(Req);
	if (cmd == "register_mod") return CmdRegisterMod(Req);
	if (cmd == "mods") return CmdMods();
	if (cmd == "mods_page") return CmdModsPage(Req);
	if (cmd == "mod_enable") return CmdModEnable(Req);
	if (cmd == "subscribe") return CmdSubscribe(Req);
	if (cmd == "unsubscribe") return CmdUnsubscribe(Req);
	if (cmd == "events") return CmdEvents(Req);
	if (cmd == "event_types") return CmdEventTypes();
	if (cmd == "register_settings") return CmdRegisterSettings(Req);
	if (cmd == "settings") return CmdSettings(Req);
	if (cmd == "set_setting") return CmdSetSetting(Req);
	if (cmd == "item_info") return CmdItemInfo(Req);
	if (cmd == "ui_add") return CmdUiAdd(Req);
	if (cmd == "perf") return CmdPerf(Req);
	if (cmd == "ui_remove") return CmdUiRemove(Req);
	if (cmd == "ui_refresh") { UiRefresh(); return Json(true); }
	if (cmd == "stat_info") return CmdStatInfo(Req);
	if (cmd == "legendary_info") return CmdLegendaryInfo(Req);
	if (cmd == "skill_info") return CmdSkillInfo(Req);
	if (cmd == "slot_info") return CmdSlotInfo(Req);
	if (cmd == "mod_data") return CmdModData(Req);
	if (cmd == "mod_log") return CmdModLog(Req);
	if (cmd == "batch")
	{
		// All sub-commands run in the same frame, so their reads are mutually consistent.
		Json out = Json::Array();
		for (auto& sub : Req.At("cmds").a) out.Push(SafeDispatch(sub));
		return out;
	}
	Fail("unknown cmd '" + cmd + "'");
}

static std::string HandleLine(const std::string& Line)
{
	Json resp;
	try
	{
		Json req = sj::Parse(Line);
		resp = SafeDispatch(req);
		if (const Json* id = req.Find("id")) resp.Add("id", *id);
	}
	catch (const std::exception& e)
	{
		resp = Json::Object();
		resp.Add("ok", false);
		resp.Add("error", e.what());
	}
	return sj::Dump(resp);
}

// Last line of defense: an access violation in a bad call must not take the game down.
struct GuardedCall { const std::string* In; std::string* Out; };

static void HandleInto(void* P)
{
	auto* c = static_cast<GuardedCall*>(P);
	*c->Out = HandleLine(*c->In);
}

static DWORD RunGuarded(void (*Fn)(void*), void* P)
{
	__try { Fn(P); return 0; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}

static std::string HandleLineGuarded(const std::string& Line)
{
	std::string out;
	GuardedCall c{ &Line, &out };
	DWORD code = RunGuarded(HandleInto, &c);
	g_InCapture = false;
	if (code)
	{
		char buf[96];
		snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"native exception 0x%08lX\"}", code);
		return buf;
	}
	return out;
}

// ============================================================================
// Lua mods: mods/lua/<name>.lua, one Lua 5.5 state each. Only the safe libraries (base, coroutine, math, string, table,
// utf8 - no io, os, package or debug): a script reaches the game through the "slorm" table only. Scripts load once the
// game's UI runs, reload when their file changes, switch on / off right away from the Mods page (X.lua.disabled), and a
// call that runs longer than 200 ms is stopped instead of freezing the game. Errors show on the Mods page.
// ============================================================================

static void LuaPush(lua_State* L, const Json& J, int Depth = 0)
{
	if (Depth > 64) { lua_pushnil(L); return; }
	switch (J.type)
	{
	case Json::Bool: lua_pushboolean(L, J.b); break;
	case Json::Num:
		if (J.n == std::floor(J.n) && std::fabs(J.n) < 9e15) lua_pushinteger(L, static_cast<lua_Integer>(J.n));
		else lua_pushnumber(L, J.n);
		break;
	case Json::Str: lua_pushlstring(L, J.s.data(), J.s.size()); break;
	case Json::Arr:
		lua_createtable(L, static_cast<int>(J.a.size()), 0);
		for (size_t i = 0; i < J.a.size(); ++i) { LuaPush(L, J.a[i], Depth + 1); lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1)); }
		break;
	case Json::Obj:
		lua_createtable(L, 0, static_cast<int>(J.o.size()));
		for (auto& [k, v] : J.o) { LuaPush(L, v, Depth + 1); lua_setfield(L, -2, k.c_str()); }
		break;
	default: lua_pushnil(L); break;
	}
}

static char g_LuaNull;   // slorm.null: JSON null where Lua has no nil (inside arrays, e.g. "any" in hook patterns)

// a Lua value -> JSON: a table with keys 1..n only (or an empty one) is an array, other tables are objects
static Json LuaToJson(lua_State* L, int Idx, int Depth = 0)
{
	Idx = lua_absindex(L, Idx);
	switch (lua_type(L, Idx))
	{
	case LUA_TNIL: return Json();
	case LUA_TLIGHTUSERDATA:
		if (lua_touserdata(L, Idx) == &g_LuaNull) return Json();
		Fail("a userdata cannot be passed to the game");
	case LUA_TBOOLEAN: return Json(lua_toboolean(L, Idx) != 0);
	case LUA_TNUMBER: return Json(static_cast<double>(lua_tonumber(L, Idx)));
	case LUA_TSTRING: { size_t n = 0; const char* s = lua_tolstring(L, Idx, &n); return Json(std::string(s, n)); }
	case LUA_TTABLE:
	{
		if (Depth > 64) Fail("table nested too deep");
		lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, Idx));
		size_t count = 0;
		bool seq = true;
		lua_pushnil(L);
		while (lua_next(L, Idx))
		{
			++count;
			if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) < 1 || lua_tointeger(L, -2) > n) seq = false;
			lua_pop(L, 1);
		}
		if (seq && count == static_cast<size_t>(n))
		{
			Json a = Json::Array();
			for (lua_Integer i = 1; i <= n; ++i) { lua_rawgeti(L, Idx, i); a.Push(LuaToJson(L, -1, Depth + 1)); lua_pop(L, 1); }
			return a;
		}
		Json o = Json::Object();
		lua_pushnil(L);
		while (lua_next(L, Idx))
		{
			std::string key;
			if (lua_type(L, -2) == LUA_TSTRING) key = lua_tostring(L, -2);
			else if (lua_type(L, -2) == LUA_TNUMBER)
			{
				lua_pushvalue(L, -2);                         // a copy: lua_tostring would change the key lua_next needs
				key = lua_tostring(L, -1);
				lua_pop(L, 1);
			}
			if (!key.empty()) o.Add(key, LuaToJson(L, -1, Depth + 1));
			lua_pop(L, 1);
		}
		return o;
	}
	default: Fail(std::string("a ") + luaL_typename(L, Idx) + " cannot be passed to the game");
	}
}

namespace lua
{
	struct Mod;
	struct Handler { Mod* M; int Ref; bool OwnClicks; uint64_t Id; bool Dead = false; };
	struct Timer { int Id, Ref; double Every, Next; bool Repeat; };
	struct Mod
	{
		std::string Stem, Name, Version, Author, Description;
		fs::path Path;
		fs::file_time_type Time{};
		bool Enabled = true, Tried = false;
		lua_State* L = nullptr;
		std::string Error;
		std::vector<std::unique_ptr<Handler>> Handlers;
		std::vector<Timer> Timers;
		int NextTimer = 1;
		std::set<std::string> Menus;
		std::string SettingsName;
		double CallStart = 0;
		size_t MemUsed = 0;
		std::string Owner() const { return "lua:" + Stem; }
		std::string Display() const { return Name.empty() ? Stem : Name; }
	};
	std::vector<std::unique_ptr<Mod>> Mods;
	double ScanAt = 0;
	uint64_t Version = 0;   // bumped when a script loads / unloads / fails (the Mods page redraws)

	Mod* Of(lua_State* L) { return *static_cast<Mod**>(lua_getextraspace(L)); }

	// each script may use at most 64 MB: past that an allocation fails and Lua raises "not enough memory"
	constexpr size_t kMemLimit = size_t(64) << 20;
	void* Alloc(void* Ud, void* Ptr, size_t OSize, size_t NSize)
	{
		Mod* m = static_cast<Mod*>(Ud);
		size_t old = Ptr ? OSize : 0;
		if (NSize == 0)
		{
			free(Ptr);
			m->MemUsed -= old;
			return nullptr;
		}
		if (NSize > old && m->MemUsed - old + NSize > kMemLimit) return nullptr;
		void* p = realloc(Ptr, NSize);
		if (p) m->MemUsed = m->MemUsed - old + NSize;
		return p;
	}
	int Panic(lua_State* L)
	{
		DbgPrintEx(LOG_SEVERITY_ERROR, "[Lua] unprotected error: %s", lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
		return 0;
	}

	void LimitHook(lua_State* L, lua_Debug*)
	{
		Mod* m = Of(L);
		if (m && MicrosSinceStart() - m->CallStart > 200000) luaL_error(L, "took longer than 200 ms (an endless loop?)");
	}

	int Traceback(lua_State* L)
	{
		const char* msg = lua_tostring(L, 1);
		luaL_traceback(L, L, msg ? msg : "error", 1);
		return 1;
	}

	// calls the function below the NArgs arguments on top; an error is logged and kept for the Mods page
	bool PCall(Mod& M, int NArgs)
	{
		lua_State* L = M.L;
		int base = lua_gettop(L) - NArgs;
		lua_pushcfunction(L, Traceback);
		lua_insert(L, base);
		M.CallStart = MicrosSinceStart();
		int st = lua_pcall(L, NArgs, 0, base);
		lua_remove(L, base);
		if (st == LUA_OK) return true;
		std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
		lua_pop(L, 1);
		DbgPrintEx(LOG_SEVERITY_ERROR, "[Lua %s] %s", M.Stem.c_str(), err.c_str());
		M.Error = err.substr(0, err.find('\n'));
		++Version;
		return false;
	}

	// Game engine functions that reach outside the game (files, network, URLs, the OS, dialogs, the clipboard): not for
	// Lua mods, in "call" or in draw programs. Game scripts (scr_...) and everything else stay available.
	bool HostFunction(std::string Fn)
	{
		Fn = Lower(Fn);
		if (Fn.starts_with("gml_script_")) return false;
		static const char* const prefixes[] = { "file_", "directory_", "http_", "network_", "url_", "execute_", "shell_",
			"environment_", "clipboard_", "buffer_save", "buffer_load", "ini_", "ds_map_secure", "external_", "steam_",
			"get_open", "get_save", "get_string", "get_integer", "show_", "screen_save", "sprite_save", "surface_save",
			"sprite_add", "sprite_replace", "font_add", "audio_create_stream", "video_", "load_csv", "zip_", "game_save",
			"game_load", "os_", "extension_", "window_handle", "window_device", "physics_world_", "parameter_" };
		for (auto* p : prefixes)
			if (Fn.starts_with(p)) return true;
		return false;
	}

	void CheckFunctions(const Json& J)
	{
		if (J.type == Json::Obj)
		{
			for (auto& [k, v] : J.o)
			{
				if ((k == "fn" || k == "$fn") && v.type == Json::Str && HostFunction(v.s))
					throw std::runtime_error("'" + v.s + "' reaches outside the game and is not available to Lua mods");
				CheckFunctions(v);
			}
		}
		else if (J.type == Json::Arr)
			for (auto& v : J.a) CheckFunctions(v);
	}

	// a script acts as itself only: its own owner, its own settings, nothing that switches or unhooks other mods
	void Sanitize(Json& Req, const Mod& M, int Depth)
	{
		if (Req.type != Json::Obj) throw std::runtime_error("a command is a table");
		std::string cmd = Req.GetStr("cmd");
		static const std::set<std::string> blocked = { "mod_enable", "mods_page", "perf", "overlay", "runner", "routines",
			"routine_index" };
		if (blocked.count(cmd)) throw std::runtime_error("'" + cmd + "' is not available to Lua mods");
		if (cmd == "set_setting" && Req.GetStr("mod") != M.Display())
			throw std::runtime_error("a Lua mod changes its own settings only");
		if (cmd == "batch")
		{
			if (Depth > 0) throw std::runtime_error("nested batches are not available to Lua mods");
			for (auto& kv : Req.o)
				if (kv.first == "cmds" && kv.second.type == Json::Arr)
					for (auto& sub : kv.second.a) Sanitize(sub, M, Depth + 1);
		}
		CheckFunctions(Req);
		Req.Set("owner", M.Owner());
		Req.Set("all", false);
	}

	// ---- the C side of the slorm table
	int Request(lua_State* L)
	{
		Mod* m = Of(L);
		std::string err;
		{
			Json resp;
			try
			{
				Json req = LuaToJson(L, 1);
				if (req.type != Json::Obj) throw std::runtime_error("slorm.request takes a table");
				Sanitize(req, *m, 0);
				if (req.GetStr("cmd") == "register_settings") m->SettingsName = req.GetStr("mod");
				resp = SafeDispatch(req);
			}
			catch (const std::exception& e) { err = e.what(); }
			if (err.empty())
			{
				if (resp.GetBool("ok", false))
				{
					const Json* r = resp.Find("result");
					LuaPush(L, r ? *r : Json());
					return 1;
				}
				err = resp.GetStr("error", "error");
			}
		}
		return luaL_error(L, "%s", err.c_str());
	}

	void EventCallback(const char* Event, const char* DataJson, void* User)
	{
		auto* h = static_cast<Handler*>(User);
		if (h->Dead || !h->M->L) return;
		Json d;
		try { d = sj::Parse(DataJson ? DataJson : "null"); }
		catch (const std::exception&) { return; }
		if (h->OwnClicks && d.GetStr("owner") != h->M->Owner()) return;
		lua_rawgeti(h->M->L, LUA_REGISTRYINDEX, h->Ref);
		LuaPush(h->M->L, d);
		lua_pushstring(h->M->L, Event);
		PCall(*h->M, 2);
	}

	int On(lua_State* L)          // on(event, fn [, own_clicks]) -> id
	{
		Mod* m = Of(L);
		const char* ev = luaL_checkstring(L, 1);
		luaL_checktype(L, 2, LUA_TFUNCTION);
		if (!events::Known(ev)) return luaL_error(L, "unknown event '%s'", ev);
		auto h = std::make_unique<Handler>();
		h->M = m;
		h->OwnClicks = lua_toboolean(L, 3) != 0;
		lua_pushvalue(L, 2);
		h->Ref = luaL_ref(L, LUA_REGISTRYINDEX);
		h->Id = events::Subscribe(ev, EventCallback, h.get());
		lua_pushinteger(L, static_cast<lua_Integer>(h->Id));
		m->Handlers.push_back(std::move(h));
		return 1;
	}

	int Off(lua_State* L)         // off(id)
	{
		Mod* m = Of(L);
		uint64_t id = static_cast<uint64_t>(luaL_checkinteger(L, 1));
		for (auto& h : m->Handlers)
			if (h->Id == id && !h->Dead)
			{
				events::Unsubscribe(id);
				h->Dead = true;                                   // freed with the script (a dispatch may still hold it)
				luaL_unref(L, LUA_REGISTRYINDEX, h->Ref);
			}
		return 0;
	}

	int AddTimer(lua_State* L, bool Repeat)
	{
		Mod* m = Of(L);
		double sec = luaL_checknumber(L, 1);
		luaL_checktype(L, 2, LUA_TFUNCTION);
		lua_pushvalue(L, 2);
		Timer t{ m->NextTimer++, luaL_ref(L, LUA_REGISTRYINDEX), (std::max)(sec, 0.0), MicrosSinceStart() / 1e6 + sec, Repeat };
		m->Timers.push_back(t);
		lua_pushinteger(L, t.Id);
		return 1;
	}
	int Every(lua_State* L) { return AddTimer(L, true); }    // every(seconds, fn) -> id
	int After(lua_State* L) { return AddTimer(L, false); }   // after(seconds, fn) -> id

	int Cancel(lua_State* L)      // cancel(timer id)
	{
		Mod* m = Of(L);
		int id = static_cast<int>(luaL_checkinteger(L, 1));
		for (size_t i = 0; i < m->Timers.size(); ++i)
			if (m->Timers[i].Id == id)
			{
				luaL_unref(L, LUA_REGISTRYINDEX, m->Timers[i].Ref);
				m->Timers.erase(m->Timers.begin() + static_cast<std::ptrdiff_t>(i));
				break;
			}
		return 0;
	}

	int Log(lua_State* L)         // log(...) (print too)
	{
		std::string line;
		for (int i = 1, n = lua_gettop(L); i <= n; ++i)
		{
			size_t len = 0;
			const char* s = luaL_tolstring(L, i, &len);
			if (i > 1) line += "\t";
			line.append(s, len);
			lua_pop(L, 1);
		}
		DbgPrintEx(LOG_SEVERITY_INFO, "[Lua %s] %s", Of(L)->Stem.c_str(), line.c_str());
		return 0;
	}

	int ModInfo(lua_State* L)     // _mod{name, version, author, description}
	{
		Mod* m = Of(L);
		luaL_checktype(L, 1, LUA_TTABLE);
		auto field = [&](const char* K, std::string& Out)
		{
			lua_getfield(L, 1, K);
			if (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER) Out = lua_tostring(L, -1);
			lua_pop(L, 1);
		};
		field("name", m->Name);
		field("version", m->Version);
		field("author", m->Author);
		field("description", m->Description);
		++Version;
		return 0;
	}

	int DisplayName(lua_State* L) { lua_pushstring(L, Of(L)->Display().c_str()); return 1; }
	int Clock(lua_State* L) { lua_pushnumber(L, MicrosSinceStart() / 1e6); return 1; }   // seconds, for time budgets
	int Menu(lua_State* L) { Of(L)->Menus.insert(luaL_checkstring(L, 1)); return 0; }

	// helpers in Lua on top of request / on
	const char* kPrelude = R"LUA(
local S = slorm
do
  local raw_load = load
  load = function(chunk, name, mode, env) return raw_load(chunk, name, "t", env) end   -- text only
end
function S.asset(name) return { ["$asset"] = name } end
function S.inst(id) return { ["$inst"] = id } end
function S.get(path, root, depth) return S.request{ cmd = "get", path = path, root = root, depth = depth or 2 } end
function S.set(path, value, root) return S.request{ cmd = "set", path = path, value = value, root = root } end
function S.call(fn, ...) return S.request{ cmd = "call", fn = fn, args = { ... }, depth = 2 } end
function S.call_on(self, fn, ...) return S.request{ cmd = "call", fn = fn, args = { ... }, self = self, depth = 2 } end
function S.instances(object, vars) return S.request{ cmd = "instances", object = object, vars = vars } end
function S.item_info(item) return S.request{ cmd = "item_info", item = item } end
function S.slot_info(slot, hero) return S.request{ cmd = "slot_info", slot = slot, hero = hero } end
function S.stat_info(stat, slot, roll, level)
  return S.request{ cmd = "stat_info", stat = stat, slot = slot, roll = roll, level = level }
end
function S.load_data() return S.request{ cmd = "mod_data" } end
function S.save_data(value) return S.request{ cmd = "mod_data", value = value } end
function S.log_file(...)
  local t = {}
  for i = 1, select("#", ...) do t[i] = tostring((select(i, ...))) end
  return S.request{ cmd = "mod_log", line = table.concat(t, "	") }
end
function S.mod(info) S._mod(info) end
local values = {}
function S.settings(list)
  values = S.request{ cmd = "register_settings", mod = S.display_name(), settings = list }
  return values
end
function S.setting(key) return values[key] end
S.on("setting_changed", function(e) if e.mod == S.display_name() then values[e.key] = e.value end end)
function S.ui(menu, calls, when)
  S.request{ cmd = "ui_add", menu = menu, program = { calls = calls, when = when } }
  S._menu(menu)
end
function S.ui_remove(menu) S.request{ cmd = "ui_remove", menu = menu } end
function S.text(x, y, text, font, color, halign)
  return { fn = "$text", args = { x, y, tostring(text), font or "ft_standard_12", color or "white", halign or 0 } }
end
function S.button(id, x, y, text, tooltip, sprite)
  return { fn = "$button", args = { id, x, y, tostring(text), S.asset(sprite or "spr_menu_thin_grey_button"), tooltip or "" } }
end
function S.radio(id, x, y, label, on, tooltip)
  return { fn = "$radio", args = { id, x, y, tostring(label), on and true or false, tooltip or "" } }
end
function S.selector(id, x, y, label, value, tooltip)
  return { fn = "$selector", args = { id, x, y, tostring(label), tostring(value), tooltip or "" } }
end
function S.on_click(fn) return S.on("ui_clicked", function(e) fn(e.id, e.action) end, true) end
print = S.log
)LUA";

	void Unload(Mod& M)
	{
		for (auto& h : M.Handlers)
			if (!h->Dead) events::Unsubscribe(h->Id);
		M.Handlers.clear();
		Json owner = Json::Object();
		owner.Add("owner", M.Owner());
		for (auto& menu : M.Menus)
		{
			Json r = owner;
			r.Add("menu", menu);
			try { CmdUiRemove(r); }
			catch (const std::exception&) {}
		}
		M.Menus.clear();
		try { CmdUnhook(owner); CmdUnsubscribe(owner); }
		catch (const std::exception&) {}
		if (!M.SettingsName.empty()) { g_Settings.erase(M.SettingsName); ++g_SettingsVer; M.SettingsName.clear(); }
		M.Timers.clear();
		if (M.L) { lua_close(M.L); M.L = nullptr; }
		++Version;
	}

	void Load(Mod& M)
	{
		M.Error.clear();
		M.Name.clear(); M.Version.clear(); M.Author.clear(); M.Description.clear();
		M.Tried = true;
		std::ifstream f(M.Path, std::ios::binary);
		std::stringstream ss;
		ss << f.rdbuf();
		std::string src = ss.str();
		M.MemUsed = 0;
		lua_State* L = lua_newstate(Alloc, &M, static_cast<unsigned>(GetTickCount64() ^ reinterpret_cast<uintptr_t>(&M)));
		if (!L) { M.Error = "out of memory"; return; }
		lua_atpanic(L, Panic);
		M.L = L;
		*static_cast<Mod**>(lua_getextraspace(L)) = &M;
		luaL_openselectedlibs(L, LUA_GLIBK | LUA_COLIBK | LUA_MATHLIBK | LUA_STRLIBK | LUA_TABLIBK | LUA_UTF8LIBK, 0);
		for (const char* g : { "dofile", "loadfile" }) { lua_pushnil(L); lua_setglobal(L, g); }
		// no precompiled code: malformed bytecode is a way out of any Lua sandbox
		lua_getglobal(L, "string");
		lua_pushnil(L);
		lua_setfield(L, -2, "dump");
		lua_pop(L, 1);
		static const luaL_Reg fns[] = { { "request", Request }, { "on", On }, { "off", Off }, { "every", Every },
			{ "after", After }, { "cancel", Cancel }, { "log", Log }, { "_mod", ModInfo }, { "display_name", DisplayName },
			{ "_menu", Menu }, { "clock", Clock }, { nullptr, nullptr } };
		lua_newtable(L);
		luaL_setfuncs(L, fns, 0);
		lua_pushlightuserdata(L, &g_LuaNull);
		lua_setfield(L, -2, "null");
		lua_pushstring(L, M.Stem.c_str());
		lua_setfield(L, -2, "name");
		lua_pushstring(L, M.Owner().c_str());
		lua_setfield(L, -2, "owner");
		lua_pushstring(L, kVersion);
		lua_setfield(L, -2, "version");
		lua_setglobal(L, "slorm");
		lua_sethook(L, LimitHook, LUA_MASKCOUNT, 10000);
		bool ok = luaL_loadbufferx(L, kPrelude, strlen(kPrelude), "=slorm", "t") == LUA_OK && PCall(M, 0);
		if (ok)
		{
			std::string chunk = "@" + M.Stem + ".lua";
			if (luaL_loadbufferx(L, src.data(), src.size(), chunk.c_str(), "t") != LUA_OK)   // text only: no bytecode
			{
				M.Error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "cannot load";
				DbgPrintEx(LOG_SEVERITY_ERROR, "[Lua %s] %s", M.Stem.c_str(), M.Error.c_str());
				ok = false;
			}
			else ok = PCall(M, 0);
		}
		std::string error = M.Error;
		if (!ok) Unload(M);                                     // a script that failed to start does not stay half-loaded
		M.Error = error;
		if (ok) DbgPrintEx(LOG_SEVERITY_INFO, "[Lua %s] loaded", M.Stem.c_str());
		++Version;
	}

	// mods/lua: new, changed, removed, switched on / off
	void Scan()
	{
		fs::path dir = GameDir() / "mods" / "lua";
		std::error_code ec;
		fs::create_directories(dir, ec);
		std::map<std::string, std::pair<fs::path, bool>> found;
		for (auto& f : fs::directory_iterator(dir, ec))
		{
			if (!f.is_regular_file()) continue;
			std::string name = f.path().filename().string(), low = Lower(name);
			if (low.ends_with(".lua")) found[name.substr(0, name.size() - 4)] = { f.path(), true };
			else if (low.ends_with(".lua.disabled") && !found.count(name.substr(0, name.size() - 13)))
				found[name.substr(0, name.size() - 13)] = { f.path(), false };
		}
		for (size_t i = Mods.size(); i-- > 0;)
			if (!found.count(Mods[i]->Stem)) { Unload(*Mods[i]); Mods.erase(Mods.begin() + static_cast<std::ptrdiff_t>(i)); }
		for (auto& [stem, pe] : found)
		{
			Mod* m = nullptr;
			for (auto& x : Mods)
				if (x->Stem == stem) m = x.get();
			if (!m) { Mods.push_back(std::make_unique<Mod>()); m = Mods.back().get(); m->Stem = stem; }
			bool wasEnabled = m->Enabled;
			m->Path = pe.first;
			m->Enabled = pe.second;
			auto t = fs::last_write_time(m->Path, ec);
			if (!m->Enabled)
			{
				if (m->L) Unload(*m);
				if (wasEnabled) { m->Error.clear(); ++Version; }
				m->Time = t;
				continue;
			}
			if (m->Tried && t == m->Time && wasEnabled) continue;   // unchanged (loaded, or failed: wait for an edit)
			if (m->L) Unload(*m);
			m->Time = t;
			Load(*m);
		}
	}

	void Tick()
	{
		double now = MicrosSinceStart() / 1e6;
		if (now >= ScanAt) { ScanAt = now + 2; Scan(); }
		for (auto& mp : Mods)
		{
			Mod& m = *mp;
			if (!m.L) continue;
			std::vector<int> due;
			for (auto& t : m.Timers)
				if (now >= t.Next) due.push_back(t.Id);
			for (int id : due)
			{
				auto it = std::find_if(m.Timers.begin(), m.Timers.end(), [&](const Timer& T) { return T.Id == id; });
				if (it == m.Timers.end() || !m.L) continue;   // cancelled by an earlier callback
				int ref = it->Ref;
				bool repeat = it->Repeat;
				if (repeat) it->Next = now + (std::max)(it->Every, 0.01);
				else m.Timers.erase(it);
				lua_rawgeti(m.L, LUA_REGISTRYINDEX, ref);
				PCall(m, 0);
				if (!repeat && m.L) luaL_unref(m.L, LUA_REGISTRYINDEX, ref);
			}
		}
	}
}

// the Mods page's rows for the Lua mods, and their on / off switch
static void LuaModRows(Json& Out)
{
	size_t at = Out.a.size();
	for (size_t i = 0; i < Out.a.size(); ++i)
		if (Out.a[i].GetStr("kind") == "loader") { at = i; break; }
	std::vector<Json> rows;
	for (auto& mp : lua::Mods)
	{
		const lua::Mod& m = *mp;
		Json j = Json::Object();
		j.Add("name", m.Display());
		j.Add("version", m.Version);
		j.Add("author", m.Author);
		j.Add("description", m.Description.empty() ? "Lua script" : m.Description);
		j.Add("file", m.Stem + ".lua");
		j.Add("kind", "lua");
		j.Add("loaded", m.L != nullptr);
		j.Add("enabled", m.Enabled);
		j.Add("locked", false);
		j.Add("pending", false);
		Json problems = Json::Array();
		if (!m.Error.empty()) problems.Push(Json("Lua error: " + m.Error));
		j.Add("problems", std::move(problems));
		rows.push_back(std::move(j));
	}
	Out.a.insert(Out.a.begin() + static_cast<std::ptrdiff_t>(at), rows.begin(), rows.end());
}

static bool LuaToggle(const std::string& File, bool On)
{
	if (!Lower(File).ends_with(".lua") || File.find_first_of("/\\:") != std::string::npos) return false;
	fs::path dir = GameDir() / "mods" / "lua", on = dir / File, off = dir / (File + kOff);
	std::error_code ec;
	if (On && !Exists(on) && Exists(off)) fs::rename(off, on, ec);
	else if (!On && Exists(on)) fs::rename(on, off, ec);
	if (ec) Fail("could not rename " + File + " (" + ec.message() + ")");
	lua::ScanAt = 0;                                          // applies on the next frame
	return true;
}

static uint64_t LuaVersion() { return lua::Version; }

// Requests are answered from whichever game-thread callback fires first. The guard stops
// re-entry when a called script itself triggers object events.
static bool g_Pumping = false;

static void Pump()
{
	if (g_Pumping || g_DetourDepth > 0 || !slorm::HasPendingRequests()) return;
	g_Pumping = true;
	slorm::DrainRequests(HandleLineGuarded);
	g_Pumping = false;
}

static void FrameCallback(FWFrame& Context)
{
	UNREFERENCED_PARAMETER(Context);
	++g_Frame;
	Pump();
}

// Each callback must have a distinct body, or /OPT:ICF folds them into one address
// and YYTK rejects the duplicate registration.
struct PerfScope
{
	bool On;
	double T0;
	double* Into;
	PerfScope(double* Into_) : On(g_Perf), T0(g_Perf ? PerfNowUs() : 0), Into(Into_) {}
	~PerfScope() { if (On && g_Perf) *Into += PerfNowUs() - T0; }
};

// obj_ui_next_gen's Step code, once seen (compared by pointer from then on; forgotten if it stops showing up)
static CCode* g_UiStepCode = nullptr;
static uint64_t g_EventsSinceUiStep = 0;

static void CodeEventCallback(FWCodeEvent& Context)
{
	++g_CodeEvents;
	if (g_Perf) ++g_PerfEvents;
	PerfScope perf(&g_PerfCallbackUs);
	Pump();
	CCode* code0 = std::get<2>(Context.Arguments());
	bool uiStep = false;
	if (code0)
	{
		if (code0 == g_UiStepCode) uiStep = true;
		else if (!g_UiStepCode && code0->GetName() && !strcmp(code0->GetName(), "gml_Object_obj_ui_next_gen_Step_0"))
		{
			g_UiStepCode = code0;
			uiStep = true;
		}
		else if (g_UiStepCode && ++g_EventsSinceUiStep > 200000) { g_UiStepCode = nullptr; g_EventsSinceUiStep = 0; }
	}
	if (uiStep)
	{
		g_EventsSinceUiStep = 0;
		PerfScope tick(&g_PerfTickUs);
		if (g_Perf) ++g_PerfFrames;
		{ PerfScope p(&g_PerfPartUs[0]); modspage::Tick(); }
		try { PerfScope p(&g_PerfPartUs[1]); events::Tick(); }
		catch (const std::exception& e) { DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormAPI] events: %s", e.what()); }
		try { PerfScope p(&g_PerfPartUs[2]); lua::Tick(); }
		catch (const std::exception& e) { DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormAPI] lua: %s", e.what()); }
	}
	if (!g_Overlay || g_InOverlay) return;
	auto& args = Context.Arguments();
	CCode* code = std::get<2>(args);
	if (!code || !code->GetName() || g_OverlayEvent != code->GetName()) return;
	Context.Call();                                   // the game's own drawing first, the overlay on top
	DrawOverlay(std::get<0>(args), std::get<1>(args));
}

// ---- the interface other modules call (same commands as the TCP protocol, on the calling - game - thread)
struct SlormApiImpl : public SlormAPIInterface
{
	std::string Last;
	AurieStatus Create() override { return AURIE_SUCCESS; }
	void Destroy() override {}
	void QueryVersion(short& Major, short& Minor, short& Patch) override { Major = 1; Minor = 0; Patch = 0; }
	const char* Request(const char* RequestJson) override
	{
		Last = HandleLineGuarded(RequestJson ? RequestJson : "");
		return Last.c_str();
	}
	uint64_t Subscribe(const char* Event, SlormEventCallback Callback, void* User) override
	{
		return events::Subscribe(Event ? Event : "", Callback, User);
	}
	void Unsubscribe(uint64_t SubscriptionId) override { events::Unsubscribe(SubscriptionId); }
};
static SlormApiImpl g_Interface;
static bool g_ServerStarted = false;

static bool ServerWanted(const fs::path& ModulePath, uint16_t& Port)
{
	char env[16] = {};
	bool wanted = false;
	if (GetEnvironmentVariableA("SLORMAPI_PORT", env, sizeof(env)) > 0) { Port = static_cast<uint16_t>(atoi(env)); wanted = true; }
	if (GetEnvironmentVariableA("SLORMAPI_SERVER", env, sizeof(env)) > 0 && env[0] == '1') wanted = true;
	std::error_code ec;
	if (fs::exists(ModulePath.parent_path() / "SlormAPI.server", ec)) wanted = true;
	return wanted;
}

static void WndProcCallback(FWWndProc& Context)
{
	++g_WndProcCalls;
	UNREFERENCED_PARAMETER(Context);
	Pump();
}

EXPORTED AurieStatus ModuleInitialize(IN AurieModule* Module, IN const fs::path& ModulePath)
{
	g_Yytk = YYTK::GetInterface();
	if (!g_Yytk) return AURIE_MODULE_DEPENDENCY_NOT_RESOLVED;

	uint16_t port = kDefaultPort;
	if (ServerWanted(ModulePath, port))
	{
		std::string error;
		if (!slorm::StartServer(port, error))
		{
			DbgPrintEx(LOG_SEVERITY_ERROR, "[SlormAPI] %s", error.c_str());
			return AURIE_EXTERNAL_ERROR;
		}
		g_ServerStarted = true;
	}

	AurieStatus st = AURIE_SUCCESS;
	const std::pair<EventTriggers, PVOID> callbacks[] = {
		{ EVENT_FRAME, reinterpret_cast<PVOID>(FrameCallback) },
		{ EVENT_OBJECT_CALL, reinterpret_cast<PVOID>(CodeEventCallback) },
		{ EVENT_WNDPROC, reinterpret_cast<PVOID>(WndProcCallback) },
	};
	size_t registered = 0;
	for (auto& [trigger, routine] : callbacks)
	{
		st = g_Yytk->CreateCallback(Module, trigger, routine, 0);
		if (!AurieSuccess(st)) break;
		++registered;
	}
	if (!AurieSuccess(st))
	{
		for (size_t k = 0; k < registered; ++k) g_Yytk->RemoveCallback(Module, callbacks[k].second);
		if (g_ServerStarted) slorm::StopServer();
		DbgPrintEx(LOG_SEVERITY_ERROR, "[SlormAPI] callback registration failed: %s", AurieStatusToString(st));
		return st;
	}
	g_Registered.push_back({ "SlormAPI", kVersion, "thothas", "Modding API, the Mods page and mod settings", "SlormAPI.dll" });
	SetConsoleCtrlHandler(SessionConsoleHandler, TRUE);
	StartLoadingData();
	try { SessionStart(); }
	catch (const std::exception& e) { DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormAPI] session file: %s", e.what()); }
	st = ObCreateInterface(Module, &g_Interface, SLORMAPI_INTERFACE_NAME);
	if (!AurieSuccess(st))
		DbgPrintEx(LOG_SEVERITY_ERROR, "[SlormAPI] interface not created: %s", AurieStatusToString(st));

	if (g_ServerStarted)
		DbgPrintEx(LOG_SEVERITY_INFO, "[SlormAPI] %s listening on 127.0.0.1:%u", kVersion, port);
	else
		DbgPrintEx(LOG_SEVERITY_INFO, "[SlormAPI] %s ready (interface only, TCP server off)", kVersion);
	return AURIE_SUCCESS;
}

EXPORTED AurieStatus ModuleUnload(IN AurieModule* Module, IN const fs::path& ModulePath)
{
	UNREFERENCED_PARAMETER(ModulePath);
	if (g_OrigWndProc)                                   // the window must not call into an unloaded DLL
	{
		HWND wnd = nullptr;
		EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&wnd));
		if (wnd && GetWindowLongPtrW(wnd, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(SessionWndProc))
			SetWindowLongPtrW(wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_OrigWndProc));
		g_OrigWndProc = nullptr;
	}
	for (auto& mp : lua::Mods) lua::Unload(*mp);
	ObDestroyInterface(Module, SLORMAPI_INTERFACE_NAME);
	if (g_ServerStarted) slorm::StopServer();
	for (size_t k = 0; k < kMaxHooks; ++k)
		if (g_Hooks[k].Trampoline) RemoveHookSlot(k);
	return AURIE_SUCCESS;
}
