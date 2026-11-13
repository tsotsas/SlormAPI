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
#include <functional>
#include <future>
#include <sstream>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <cstring>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "Json.hpp"
#include "Server.hpp"
#include "SlormAPI_Interface.hpp"
#include "GameData.hpp"
#include "StatCatalog.hpp"
#include "FreshHero.hpp"
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

// "catch_errors" (development): a GML error in an object event leaves that event instead of stopping the game with the
// runner's error dialog. YYToolkit has already logged the message and the engine stack; an event that failed
// kCatchMute times is skipped from then on (one log entry per failing spot, not one per frame).
static bool g_CatchErrors = false;
static std::map<std::string, uint64_t> g_CaughtErrors;
static constexpr uint64_t kCatchMute = 3;

// "perf": SlormAPI's own time per frame (off unless asked for; QueryPerformanceCounter around our code only)
static bool g_Perf = false;
static double g_PerfCallbackUs = 0, g_PerfTickUs = 0, g_PerfDetourUs = 0, g_PerfPartUs[3] = {};
static uint64_t g_PerfFrames = 0, g_PerfEvents = 0, g_PerfDetours = 0;
static double g_PerfExchangeUs = 0;                     // classes::Exchange (a class's data in its base hero's entries)
static uint64_t g_PerfExchanges = 0;
static double PerfNowUs()
{
	static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart); }();
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return static_cast<double>(c.QuadPart) * 1e6 / freq;
}
// time spent in a scope, into a perf counter (while perf is on)
struct PerfScope
{
	bool On;
	double T0;
	double* Into;
	PerfScope(double* Into_) : On(g_Perf), T0(g_Perf ? PerfNowUs() : 0), Into(Into_) {}
	~PerfScope() { if (On && g_Perf) *Into += PerfNowUs() - T0; }
};

// "profile" (development): each object event's time (its own, and with the events it ran inside it) and the whole
// frame's, since the last reset - what the game's frame time goes to. While on, each event runs inside SlormAPI's
// callback (Context.Call), so another module's callback after ours sees it already run: for measuring only.
namespace prof
{
	bool On = false;
	struct Row { double Self = 0, Total = 0; uint64_t Calls = 0; };
	std::unordered_map<CCode*, Row> Rows;
	std::vector<double> Inner;                               // per running event: its nested events' time so far
	double FrameUs = 0, TopUs = 0, LastFrame = 0;
	uint64_t Frames = 0;
	void Reset() { Rows.clear(); FrameUs = TopUs = LastFrame = 0; Frames = 0; }
	void Frame()
	{
		double now = PerfNowUs();
		if (LastFrame > 0) { FrameUs += now - LastFrame; ++Frames; }
		LastFrame = now;
	}
	template <class F> void Run(CCode* Code, F&& Call)
	{
		struct Scope
		{
			CCode* Code; double T0 = PerfNowUs();
			Scope(CCode* C) : Code(C) { Inner.push_back(0); }
			~Scope()                                         // (also while a GML error unwinds: nothing may throw)
			{
				double dt = PerfNowUs() - T0, inner = Inner.back();
				Inner.pop_back();
				if (!Inner.empty()) Inner.back() += dt; else TopUs += dt;
				try { Row& r = Rows[Code]; r.Self += dt - inner; r.Total += dt; ++r.Calls; }
				catch (...) {}
			}
		} scope(Code);
		Call();
	}
}

static double MicrosSinceStart()
{
	return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - g_StartTime).count();
}
static constexpr uint16_t kDefaultPort = 47950;
static constexpr const char* kVersion = "2.0.0";

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

// a request that names a hero instance that is gone (its run left, the hero switched) does nothing: for a Lua mod it gives
// nil (lua::Request; said once in the log), for a TCP client its error, marked "hero_gone". A cast's late pieces (its
// timers) then do nothing on its hero; what does not name it (an animation at a place, a sound, a buff on the hero now)
// is the mod's to check: slorm.exists(e.hero)
struct HeroGone : std::runtime_error { using std::runtime_error::runtime_error; };
static std::set<int32_t> g_HeroIds;                          // the hero instances seen (each frame: lua::Tick)

static CInstance* InstanceById(int32_t Id)
{
	CInstance* inst = nullptr;
	if (!AurieSuccess(g_Yytk->GetInstanceObject(Id, inst)) || !inst)
	{
		std::string msg = "no active instance with id " + std::to_string(Id);
		if (g_HeroIds.count(Id)) throw HeroGone(msg + " (a hero gone)");
		Fail(msg);
	}
	return inst;
}

// an object's first instance, or nullptr (none: instance_find gives noone)
static CInstance* FirstInstance(const RValue& Obj)
{
	RValue inst = g_Yytk->CallBuiltin("instance_find", { Obj, RValue(0.0) });
	if (KindOf(inst) == VALUE_UNDEFINED || inst.ToDouble() < 0) return nullptr;
	CInstance* ci = nullptr;
	return AurieSuccess(g_Yytk->GetInstanceObject(static_cast<int32_t>(inst.ToInt64()), ci)) ? ci : nullptr;
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
		if (const Json* p = J.Find("$ptr"); p && p->type == Json::Str && J.o.size() == 1)   // a pointer, as read
		{
			RValue r;
			r.m_Kind = VALUE_PTR;
			r.m_Pointer = reinterpret_cast<PVOID>(static_cast<uintptr_t>(std::stoull(p->s, nullptr, 16)));
			return r;
		}
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
	bool Overlay = false;        // drawn over the GUI, after the menu: positions get the menu's place (OffX / OffY)
	double OffX = 0, OffY = 0;   // then self.menu_x_pos / menu_y_pos (read with Scale)
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

// (the runner's own length, as YYTK's GetArrayEntry checks it: not array_length through CallBuiltin, ~0.7 us a call)
static size_t ArrayLen(const RValue& Arr)
{
	size_t n = 0;
	if (KindOf(Arr) != VALUE_ARRAY || !AurieSuccess(g_Yytk->GetArraySize(const_cast<RValue&>(Arr), n))) return 0;
	return n;
}
static RValue* ArrayAt(RValue& Arr, size_t I)
{
	if (KindOf(Arr) != VALUE_ARRAY) return nullptr;
	RValue* e = nullptr;                                    // (GetArrayEntry checks I against the length)
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

// The game lays its menus out for 1920x1080 and multiplies every position and size by self.ui_scale (the resolution's
// width / 1920). It puts a menu on screen at self.menu_x_pos / menu_y_pos (centred on a 4:3 or 16:10 screen, moving
// while it slides in) itself, for what its draw script draws and for the mouse over self.mba: widgets and entries take
// layout coordinates times the scale. Only an overlay drawn after the menu (C.Overlay) adds that place (GuiX / GuiY).
static double UiScale(EvalCtx& C)
{
	if (C.Scale <= 0)
	{
		double s = Num(SelfVar(C.Self, "ui_scale"));
		C.Scale = s > 0 ? s : 1.0;
		if (C.Overlay)
		{
			C.OffX = Num(SelfVar(C.Self, "menu_x_pos"));
			C.OffY = Num(SelfVar(C.Self, "menu_y_pos"));
		}
	}
	return C.Scale;
}
static double GuiX(EvalCtx& C, double X) { return X * UiScale(C) + C.OffX; }
static double GuiY(EvalCtx& C, double Y) { return Y * UiScale(C) + C.OffY; }

static void WidgetText(EvalCtx& C, double X, double Y, const std::string& S, const std::string& Font, const std::string& Color,
	double Ha = 0, double Va = 1)
{
	// (scr_draw_text sizes the font itself)
	std::vector<RValue> a = { RValue(GuiX(C, X)), RValue(GuiY(C, Y)), RStr(S), RValue(1.0), RValue(1.0), RStr(Font), RValue(Ha), RValue(Va),
		RValue(0.0), RValue(1.0), RStr(Color), RStr("black"), RValue(1.0), RValue(false) };
	CallFn("gml_Script_scr_draw_text", true, a, C);
}

static void WidgetSprite(EvalCtx& C, const RValue& Spr, double Frame, double X, double Y)
{
	double s = UiScale(C);
	std::vector<RValue> a = { Spr, RValue(Frame), RValue(GuiX(C, X)), RValue(GuiY(C, Y)), RValue(s), RValue(s), RValue(0.0),
		RValue(static_cast<double>(0xFFFFFF)), RValue(1.0) };
	CallFn("draw_sprite_ext", false, a, C);
}

static void WidgetRect(EvalCtx& C, double X1, double Y1, double X2, double Y2, double Color)
{
	std::vector<RValue> c = { RValue(Color) };
	CallFn("draw_set_color", false, c, C);
	std::vector<RValue> r = { RValue(GuiX(C, X1)), RValue(GuiY(C, Y1)), RValue(GuiX(C, X2)), RValue(GuiY(C, Y2)), RValue(false) };
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

// appends Entry to self.mba, or replaces the entry with the same name and position. A menu without entries of its own
// leaves mba 0 (the Profile with a controller: its Close buttons are mouse-only): ours get a list, which the
// controller's navigation reads like the game's
static void MbaPushEntry(CInstance* Self, RValue Entry)
{
	RValue mba = SelfVar(Self, "mba");
	if (KindOf(mba) != VALUE_ARRAY && Self && IsNumeric(mba))
	{
		mba = g_Yytk->CallBuiltin("array_create", { RValue(0.0) });
		g_Yytk->CallBuiltin("variable_instance_set", { RValue(Self), RValue("mba"), mba });
		mba = SelfVar(Self, "mba");
	}
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

// drops the entries whose argument starts with Prefix from Mba (the game keeps entries it did not make across its own
// redraws)
static void MbaRemovePrefix(RValue& Mba, const std::string& prefix)
{
	for (size_t k = ArrayLen(Mba); k-- > 0;)
	{
		RValue* e = ArrayAt(Mba, k);
		RValue* arg = e ? ArrayAt(*e, 8) : nullptr;
		if (arg && KindOf(*arg) == VALUE_STRING && GmlString(*arg).starts_with(prefix))
			g_Yytk->CallBuiltin("array_delete", { Mba, RValue(static_cast<double>(k)), RValue(1.0) });
	}
}
static void MbaRemoveOwner(RValue& Mba, const std::string& Owner) { MbaRemovePrefix(Mba, "slormapi:ui:" + Owner + "|"); }

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

// SlormAPI's own features hook game scripts in C++ too (e.g. the save around custom legendaries): Pre runs before the
// original and may answer the call itself (return true: the original does not run), Post runs after it.
struct NativeHook
{
	std::string Owner;
	bool (*Pre)(CInstance* Self, int ArgCount, RValue** Args, RValue& Result) = nullptr;
	void (*Post)(CInstance* Self, int ArgCount, RValue** Args, RValue& Result) = nullptr;
};

struct HookSlot
{
	std::string Name;
	PVOID Trampoline = nullptr;
	uint64_t Calls = 0;
	std::vector<HookLayer> Layers;
	std::vector<NativeHook> Natives;
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
static RValue& HookDetourLayers(HookSlot& slot, PFUNC_YYGMLScript original, CInstance* Self, CInstance* Other, RValue& Result,
	int ArgCount, RValue* Args[]);

template <size_t N>
static RValue& HookDetour(CInstance* Self, CInstance* Other, RValue& Result, int ArgCount, RValue* Args[])
{
	HookSlot& slot = g_Hooks[N];
	auto original = reinterpret_cast<PFUNC_YYGMLScript>(slot.Trampoline);
	slot.Calls++;
	if (!slot.Natives.empty() && !g_InCapture)
	{
		for (size_t i = 0; i < slot.Natives.size(); ++i)   // (natives change only when SlormAPI installs one)
			if (slot.Natives[i].Pre && slot.Natives[i].Pre(Self, ArgCount, Args, Result))
				return Result;                           // answered by SlormAPI: the original does not run
		RValue& r = slot.Layers.empty() ? original(Self, Other, Result, ArgCount, Args)
			: HookDetourLayers<N>(slot, original, Self, Other, Result, ArgCount, Args);
		for (size_t i = slot.Natives.size(); i-- > 0;)     // the last one in undoes its change first
			if (slot.Natives[i].Post) slot.Natives[i].Post(Self, ArgCount, Args, r);
		return r;
	}
	if (slot.Layers.empty() || g_InCapture) return original(Self, Other, Result, ArgCount, Args);
	return HookDetourLayers<N>(slot, original, Self, Other, Result, ArgCount, Args);
}

template <size_t N>
static RValue& HookDetourLayers(HookSlot& slot, PFUNC_YYGMLScript original, CInstance* Self, CInstance* Other, RValue& Result,
	int ArgCount, RValue* Args[])
{
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

// installs (or replaces) SlormAPI's own C++ hook of Owner on a script; false when the script cannot be hooked
static bool NativeHookInstall(const std::string& Script, const NativeHook& Hook)
{
	try
	{
		HookSlot& s = g_Hooks[InstallHook(Script)];
		for (auto& n : s.Natives)
			if (n.Owner == Hook.Owner) { n = Hook; return true; }
		s.Natives.push_back(Hook);
		return true;
	}
	catch (const std::exception& e)
	{
		DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormAPI] %s: %s", Script.c_str(), e.what());
		return false;
	}
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
		if (s.Layers.empty() && s.Natives.empty() && g_DetourDepth == 0) RemoveHookSlot(k);
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
		if (!s.Natives.empty())
		{
			Json natives = Json::Array();
			for (auto& n : s.Natives) natives.Push(Json(n.Owner));
			j.Add("native", std::move(natives));
		}
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

static bool CustomLegendary(int Ref, std::string& Name, std::string& Description);   // the custom legendaries (below)

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
			std::string cname, cdesc;
			leg.Add("name", CustomLegendary(id, cname, cdesc) ? cname : d.LegName(id));
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
	for (auto& c : gd::kStatCatalog)
		if (stat == c.Key) { out.Add("unit", c.Unit); out.Add("item_affix", c.Affix); }
	if (const Json* slot = Req.Find("slot"); slot && slot->type == Json::Str) out.Add("label", d.StatLabel(stat, slot->s));
	if (const Json* roll = Req.Find("roll"))                                       // the value of a roll on an item level
		out.Add("value", d.AffixValue(stat, gd::Num(*roll), static_cast<int>(Req.GetNum("level", 100))));
	return out;
}

// every stat input of the game: key, name, unit (flat, percent, multiplier_percent, seconds), item affix or not
static Json CmdStatList()
{
	auto& d = Data();
	Json out = Json::Array();
	for (auto& c : gd::kStatCatalog)
	{
		Json j = Json::Object();
		j.Add("stat", c.Key);
		j.Add("name", d.StatName(c.Key));
		j.Add("unit", c.Unit);
		j.Add("item_affix", c.Affix);
		out.Push(std::move(j));
	}
	return out;
}

// the hero's final stats (what the game computed from items, skills, buffs...): the hero's final_* variables
static Json CmdHeroStats()
{
	RValue obj = AssetIndex("obj_parent_hero");
	if (g_Yytk->CallBuiltin("instance_number", { obj }).ToDouble() < 1) Fail("no hero");
	RValue hero = g_Yytk->CallBuiltin("instance_find", { obj, RValue(0.0) });
	auto num = [&](const char* Var) -> Json
	{
		RValue v = g_Yytk->CallBuiltin("variable_instance_get", { hero, RValue(Var) });
		return IsNumeric(v) ? Json(v.ToDouble()) : Json();
	};
	Json stats = Json::Object();
	RValue names = g_Yytk->CallBuiltin("variable_instance_get_names", { hero });
	for (size_t k = 0, n = ArrayLen(names); k < n; ++k)
	{
		RValue* name = ArrayAt(names, k);
		if (!name || KindOf(*name) != VALUE_STRING) continue;
		std::string s = name->ToString();
		if (!s.starts_with("final_")) continue;
		RValue v = g_Yytk->CallBuiltin("variable_instance_get", { hero, *name });
		if (IsNumeric(v)) stats.Add(s.substr(6), v.ToDouble());
	}
	Json out = Json::Object();
	out.Add("hero", CurrentHero());
	out.Add("instance", ToJson(hero, 0));
	out.Add("life", num("the_health"));
	out.Add("max_life", num("final_the_max_health"));
	out.Add("mana", num("the_mana"));
	out.Add("max_mana", num("final_the_max_mana"));
	out.Add("shield", num("the_shield"));
	out.Add("max_shield", num("the_max_shield"));
	out.Add("stats", std::move(stats));
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
	if (std::string cname, cdesc; CustomLegendary(id, cname, cdesc))
	{
		Json out = Json::Object();
		out.Add("id", id);
		out.Add("name", cname);
		out.Add("description", cdesc);
		out.Add("custom", true);
		return out;
	}
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

static std::string LegendaryWaitingNotice();       // "! 2 custom legendaries wait for their mod: ..." (below)
// the prompt before a save with custom legendaries whose mod is missing is played (legend, below): in the Welcome box
struct LegendaryPromptView { std::string Title, Sub; std::vector<std::pair<std::string, std::string>> Rows; size_t More = 0; };
static bool LegendaryPrompt(LegendaryPromptView& View);
static void LegendaryPromptClick(const std::string& Action);   // "load" (load anyway) or "back"
static bool g_MenuChanged = false;                 // the main menu bake must be drawn again
static constexpr const char* kOff = ".disabled";   // "X.dll.disabled": switched off (Aurie loads only *.dll)
static std::set<std::string> g_Toggled;            // files switched on / off this session (lower case)

static fs::path AurieDir() { return GameDir() / "mods" / "aurie"; }
static bool Exists(const fs::path& P) { std::error_code ec; return fs::exists(P, ec); }
// what everything else needs: never switched off from the Mods page
// File: a path relative to mods/aurie ("X.dll", or "Folder/X.dll": Aurie loads the DLLs of every subfolder too)
static std::string BaseName(const std::string& File) { return Lower(fs::path(File).filename().string()); }
static bool Locked(const std::string& File) { auto f = BaseName(File); return f == "slormapi.dll" || f == "yytoolkit.dll"; }
// loader parts that sit in mods/aurie: YYToolkit, and Slorm Loader's module when the game uses that loader
static bool LoaderFile(const std::string& File) { auto f = BaseName(File); return f == "yytoolkit.dll" || f == "slormloader.dll"; }
// a path inside mods/aurie that the Mods page may switch: relative, no "..", no drive
static bool SafeModPath(const std::string& File)
{
	if (File.empty() || File.find(':') != std::string::npos || File[0] == '/' || File[0] == '\\') return false;
	for (auto& part : fs::path(File)) if (part == ".." || part == ".") return false;
	return true;
}

// A file a mod manager deployed as a hardlink (Vortex: more than one name for the same data). Renaming it to switch it
// off would break the manager's deployment: it is switched in the manager instead.
static bool HardLinked(const fs::path& P)
{
	HANDLE h = CreateFileW(P.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	BY_HANDLE_FILE_INFORMATION fi{};
	bool linked = GetFileInformationByHandle(h, &fi) && fi.nNumberOfLinks > 1;
	CloseHandle(h);
	return linked;
}

// SlormAPI, the mods of mods/aurie by name (registered ones with what they told about themselves; switched-off ones
// too), then the loader (YYToolkit, mods/native). A switch applies at the next launch: "pending" until then.
static Json CmdMods()
{
	struct Row { ModInfo M; bool Loader, Loaded, Enabled; };
	std::vector<Row> rows;
	std::set<std::string> listed;
	fs::path aurie = AurieDir();
	// every DLL Aurie loads: mods/aurie and its subfolders (a mod may keep its files in mods/aurie/<folder>/)
	struct Dll { std::string Rel; fs::path Path; bool Off; };
	std::vector<Dll> dlls;
	std::map<std::string, std::string> byName;                      // "x.dll" -> its path relative to mods/aurie
	std::error_code ec;
	for (auto it = fs::recursive_directory_iterator(aurie, fs::directory_options::skip_permission_denied, ec);
		it != fs::recursive_directory_iterator(); it.increment(ec))
	{
		if (ec) break;
		std::error_code fe;
		if (!it->is_regular_file(fe)) continue;
		std::string rel = it->path().lexically_relative(aurie).generic_string(), low = Lower(rel);
		bool off = low.ends_with(std::string(".dll") + kOff);
		if (off) rel.resize(rel.size() - strlen(kOff));
		else if (!low.ends_with(".dll")) continue;
		dlls.push_back({ rel, it->path(), off });
		std::string name = BaseName(rel);
		if (!byName.count(name) || rel.find('/') == std::string::npos) byName[name] = rel;   // the top-level one first
	}
	for (auto m : g_Registered)
	{
		if (!Exists(aurie / m.File) && !Exists(aurie / (m.File + kOff)))
			if (auto it = byName.find(BaseName(m.File)); it != byName.end()) m.File = it->second;   // in a subfolder
		rows.push_back({ m, false, true, Exists(aurie / m.File) || !Exists(aurie / (m.File + kOff)) });
		listed.insert(Lower(m.File));
	}
	for (auto& d : dlls)
	{
		const std::string& file = d.Rel;
		bool off = d.Off;
		if (listed.count(Lower(file)) || (off && Exists(aurie / file))) continue;
		listed.insert(Lower(file));
		ModInfo m{ fs::path(file).stem().string(), FileVersion(d.Path), "", "", file };
		bool loader = LoaderFile(file);
		if (BaseName(file) == "yytoolkit.dll") m.Author = "AurieFramework";
		if (BaseName(file) == "slormloader.dll")
		{
			m.Name = "Slorm Loader";
			if (m.Version.empty())                                   // no version resource: its runtime metadata has one
			{
				std::ifstream rt(GameDir() / "slorm-runtime.json", std::ios::binary);
				std::stringstream ss;
				ss << rt.rdbuf();
				try { m.Version = sj::Parse(ss.str()).GetStr("version"); } catch (const std::exception&) {}
			}
		}
		rows.push_back({ m, loader, GetModuleHandleW(fs::path(file).filename().c_str()) != nullptr, !off });
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
		bool managed = !r.Loader && HardLinked(aurie / r.M.File);
		j.Add("locked", managed || r.Loader || Locked(r.M.File) || !(Exists(aurie / r.M.File) || Exists(aurie / (r.M.File + kOff))));
		if (managed) j.Add("managed", true);
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
		else if (out.a[i].GetBool("managed", false))
			out.a[i].Add("note", "Switched on / off in Vortex");
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
	if (!SafeModPath(file) || !Lower(file).ends_with(".dll")) Fail("bad mod file '" + file + "'");
	if (Locked(file)) Fail(file + " stays on: other mods need it");
	fs::path dll = AurieDir() / file, off = AurieDir() / (file + kOff);
	if (HardLinked(dll)) Fail(file + " is managed by Vortex: switch it on / off there");
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

namespace classes { void ProfileClick(const std::string& Action); }

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
	// the layout is the game's 1920x1080 one; the game multiplies positions and sizes by self.ui_scale when it draws
	Json Scale() { return O({ { "$self", "ui_scale" } }); }
	Json U(Json X) { return O({ { "$op", "*" }, { "args", A({ std::move(X), Scale() }) } }); }
	Json Sprite(const char* Spr, Json X, Json Y)
	{
		return Call("draw_sprite_ext", A({ Asset(Spr), 0, U(std::move(X)), U(std::move(Y)), Scale(), Scale(), 0, 16777215, 1 }));
	}
	Json Entry(const char* Name, const char* Spr, Json X, Json Y, Json W, Json H, Json Text, const std::string& Action)
	{
		return Call("$mba_push", A({ A({ Name, Asset(Spr), U(std::move(X)), U(std::move(Y)), U(std::move(W)), U(std::move(H)),
			std::move(Text), Asset(kSink), "slormapi:" + Action }) }));
	}
	// the game's text script (fonts by name); drawn right away with these flags (the main menu's own 1, 0 defers it)
	Json Text(double X, double Y, const std::string& S, const char* Font, const char* Color, int Ha = 0)
	{
		return Call("scr_draw_text", A({ U(X), U(Y), S, 1, 1, Font, Ha, 1, 0, 1, Color, "black", 1, false }));
	}
	Json Label(double X, double Y, const std::string& S) { return Text(X, Y, S, "ft_standard_12", "white", 1); }
	std::string Clip(std::string S, size_t N) { return S.size() <= N ? S : S.substr(0, N - 3) + "..."; }

	constexpr size_t kPerPage = 5;
	int Page = 0;
	std::string Error;
	uint64_t SettingsSeen = 0;
	bool CrashSeen = false;   // the page was opened after a crash: the button's (!) goes
	uint64_t LuaSeen = 0;

	// the game keeps entries it did not make across its own redraws: ours are dropped first on every run (on every
	// panel), then the Welcome panel adds the current ones again
	Json Finish(Json Calls, const Json& When)
	{
		Json all = Json::Array();
		for (const char* name : { "Mods", "ModsPage", "ModsToggle", "LegendaryPrompt" }) all.Push(Call("$mba_remove", A({ 0, name })));
		for (auto& c : Calls.a)
		{
			Json x = c;
			x.Add("when", When);
			all.Push(std::move(x));
		}
		return O({ { "calls", std::move(all) } });
	}

	Json BuildProgram(const Json& Mods)
	{
		auto entry = [](const char* Name, const char* Spr, double X, double Y, double W, double H, const char* Text,
			const std::string& Action)
		{
			return Entry(Name, Spr, X, Y, W, H, Text, Action);
		};
		// only on the Welcome panel (main_panel 0; New Game / Load Game use the same box and hide DISCORD)
		Json welcome = O({ { "$op", "==" }, { "args", A({ O({ { "$op", "??" }, { "args", A({ O({ { "$self", "main_panel" } }), 0 }) } }), 0 }) } });
		Json calls = Json::Array();
		calls.Push(Sprite("spr_menu_thin_blue_button", kBtnX, kBtnY));
		calls.Push(Label(kBtnX + 168, kBtnY + 26, g_LastSession.GetBool("crashed", false) && !CrashSeen ? "MODS  (!)" : "MODS"));
		calls.Push(entry("Mods", "spr_menu_thin_blue_button", kBtnX, kBtnY, 336, 52, "MODS", "mods"));

		// a save with custom legendaries whose mod is missing is about to be played: LOAD ANYWAY or BACK
		if (LegendaryPromptView lp; LegendaryPrompt(lp))
		{
			calls.Push(Call("$mba_remove", A({ 7, Asset("scr_ui_main_menu_patch") })));
			calls.Push(Call("$mba_remove", A({ 7, Asset("scr_ui_main_menu_patch_2") })));
			calls.Push(Call("draw_sprite_part_ext", A({ Asset("spr_main_menu_panel"), 1, 360, 0, 600, 533, U(kPanelX + 360), U(kPanelY),
				Scale(), Scale(), 16777215, 1 })));
			calls.Push(Text(984, 455, lp.Title, "ft_standard_22_b", "white"));
			calls.Push(Text(984, 483, Clip(lp.Sub, 70), "ft_standard_14_b", "grey"));
			calls.Push(Sprite("spr_main_menu_separator", 1260, 500));
			double y = 540;
			for (auto& [name, mod] : lp.Rows)
			{
				calls.Push(Text(984, y, Clip(name, 30), "ft_standard_16", "item_highlight"));
				calls.Push(Text(1535, y + 2, Clip("mod: " + mod, 30), "ft_standard_13_b", "grey", 2));
				y += 38;
			}
			if (lp.More) calls.Push(Text(984, y, "and " + std::to_string(lp.More) + " more", "ft_standard_13_b", "grey"));
			bool one = lp.Rows.size() + lp.More == 1;
			calls.Push(Text(984, 712, std::string(one ? "Without its mod it loads as an Epic, and becomes legendary again"
				: "Without their mods they load as Epics, and become legendary again"), "ft_standard_13_b", "white"));
			calls.Push(Text(984, 734, std::string(one ? "when the mod is back. Until then, don't reforge, upgrade or sell it."
				: "when the mods are back. Until then, don't reforge, upgrade or sell them."), "ft_standard_13_b", "white"));
			for (auto [label, x, act] : { std::tuple{ "Load anyway", 1084.0, "legendary:load" }, std::tuple{ "Back", 1268.0, "legendary:back" } })
			{
				calls.Push(Sprite("spr_menu_grey_button", x, 790));
				calls.Push(Label(x + 84, 816, label));
				calls.Push(entry("LegendaryPrompt", "spr_menu_grey_button", x, 790, 168, 52, label, act));
			}
			return Finish(std::move(calls), welcome);
		}
		if (!Open) return Finish(std::move(calls), welcome);

		// the update-notes buttons sit in the box: drop them, then paint the box (border + interior) over the bake
		calls.Push(Call("$mba_remove", A({ 7, Asset("scr_ui_main_menu_patch") })));
		calls.Push(Call("$mba_remove", A({ 7, Asset("scr_ui_main_menu_patch_2") })));
		calls.Push(Call("draw_sprite_part_ext", A({ Asset("spr_main_menu_panel"), 1, 360, 0, 600, 533, U(kPanelX + 360), U(kPanelY),
			Scale(), Scale(), 16777215, 1 })));
		auto pager = [&](int& P, size_t Count)
		{
			int pages = (std::max)(1, static_cast<int>((Count + kPerPage - 1) / kPerPage));
			P = ((P % pages) + pages) % pages;
			if (pages > 1)
			{
				calls.Push(Sprite("spr_selector_btn_l_v2", 1432, 837));
				calls.Push(entry("ModsPage", "spr_selector_btn_l_v2", 1432, 837, 24, 36, "", "page-"));
				calls.Push(Text(1484, 855, std::to_string(P + 1) + " / " + std::to_string(pages), "ft_standard_14_b", "white", 1));
				calls.Push(Sprite("spr_selector_btn_r_v2", 1512, 837));
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
		calls.Push(Sprite("spr_main_menu_separator", 1260, 500));
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
				calls.Push(Sprite(spr, 1499, y - 6));
				calls.Push(entry("ModsToggle", spr, 1499, y - 6, 36, 36, "", "toggle:" + m.GetStr("file")));
				right = 1482;
			}
			calls.Push(Text(984, y, Clip(m.GetStr("name"), 34), "ft_standard_16", on && isLoaded ? "white" : "grey"));
			if (!m.GetStr("version").empty()) calls.Push(Text(right, y, "v" + m.GetStr("version"), "ft_standard_14_b", "grey", 2));
			calls.Push(Text(984, y + 24, Clip(about, 72), "ft_standard_13_b", bad ? "white" : "grey"));
		}
		if (!Error.empty()) calls.Push(Text(984, 855, Clip(Error, 60), "ft_standard_13_b", "white"));
		else if (pending) calls.Push(Text(984, 855, "Restart the game to apply the changes", "ft_standard_13_b", "white"));
		else if (std::string waiting = LegendaryWaitingNotice(); !waiting.empty())
			calls.Push(Text(984, 855, Clip(waiting, 70), "ft_standard_13_b", "white"));
		else if (g_LastSession.GetBool("crashed", false))
			calls.Push(Text(984, 855, g_LastSession.GetBool("during_startup", false)
				? "The game crashed while starting last time - a mod may be the cause"
				: "The game closed unexpectedly last time", "ft_standard_13_b", "white"));
		else calls.Push(Text(984, 855, "Mods are in the game folder: mods\\aurie and mods\\lua", "ft_standard_13_b", "grey"));
		return Finish(std::move(calls), welcome);
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
		Calls.Push(Call("draw_rectangle", A({ U(X + 19), U(Y + 18), U(X + 278), U(Y + 54), false })));
		Calls.Push(Call("draw_set_color", A({ 0x101010 })));
		Calls.Push(Call("draw_rectangle", A({ U(X + 23), U(Y + 22), U(X + 274), U(Y + 50), false })));
		Calls.Push(Call("draw_set_color", A({ 0xFFFFFF })));
		Calls.Push(Text(X + 48, Y + 30, Clip(Value, 28), "ft_standard_12", "grey"));
		for (auto [spr, dx, op] : { std::tuple{ "spr_selector_btn_l_v2", -8.0, "prev" }, std::tuple{ "spr_selector_btn_r_v2", 288.0, "next" } })
		{
			Calls.Push(Sprite(spr, X + dx, Y + 12));
			Calls.Push(Entry("sel", spr, X + dx, Y + 12, 24, 36, "", Act + op));
		}
	}

	// the game's radio (diamond, filled when on)
	void Radio(Json& Calls, double X, double Y, const std::string& Label, bool On, const std::string& Act)
	{
		Calls.Push(Sprite("spr_radio_btn_v3", X, Y - 4));
		if (On) Calls.Push(Sprite("spr_radio_cross_v2", X + 18, Y + 14));
		Calls.Push(Text(X + 48, Y + 14, Clip(Label, 40), "ft_standard_12", "white"));
		Calls.Push(Entry("sel", "spr_radio_btn_v3", X, Y - 4, 36, 36, On ? Json(788) : Json(""), Act + "toggle"));
	}

	Json BuildOptionsProgram()
	{
		Json calls = Json::Array();
		auto entry = [&](const char* Name, const char* Spr, double X, double Y, double W, double H, const std::string& Label, const std::string& Action)
		{
			calls.Push(Entry(Name, Spr, X, Y, W, H, Label, Action));
		};
		calls.Push(Sprite("spr_menu_thin_grey_button", kOptBtnX, kOptBtnY));
		calls.Push(Call("scr_draw_text", A({ U(kOptBtnX + 168), U(kOptBtnY + 26), "Mods", 1, 1, "ft_standard_12", 1, 1, 0, 1, "white", "black", 1, true })));
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
		calls.Push(Call("$mba_push", A({ A({ "Tab", Asset("spr_menu_large_grey_button"), U(68), U(pick(148, 226, 304, 450)), U(336), U(62),
			pick("Gameplay", "Settings", "Keybinds", "Codex"), Asset("scr_ui_settings_change_panel"), p }) })));
		// the game's controls of that tab sit under our panel: drop them, then paint the panel over the bake
		calls.Push(Call("$mba_remove_in", A({ U(kOptPanelX + 1), U(kOptPanelY + 1), U(kOptPanelX + 1336), U(kOptPanelY + 789) })));
		calls.Push(Sprite("spr_settings_mono_large", kOptPanelX, kOptPanelY));
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
			calls.Push(Sprite("spr_menu_thin_grey_button", 560, y));
			calls.Push(Call("scr_draw_text", A({ U(728), U(y + 26), Clip(name, 26), 1, 1, "ft_standard_12", 1, 1, 0, 1, "white", "black", 1, true })));
			if (name != OptMod) entry("ModsTabMod", "spr_menu_thin_grey_button", 560, y, 336, 52, Clip(name, 26), "optmod:" + name);
			y += 66;
		}
		calls.Push(Sprite("spr_settings_separator", 926, 290));

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
				if (act.starts_with("legendary:")) LegendaryPromptClick(act.substr(10));
				else if (act.starts_with("class:")) classes::ProfileClick(act.substr(6));
				else if (act == "mods")
				{
					LegendaryPromptClick("close");
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
		if (LegendaryPromptView lp; LegendaryPrompt(lp) && !changed && Now >= MenuCheckAt)
		{
			MenuCheckAt = Now + 0.25;
			RValue ui = UiInstance();
			auto var = [&](const char* Name) { return g_Yytk->CallBuiltin("variable_instance_get", { ui, RValue(Name) }); };
			if (KindOf(ui) == VALUE_UNDEFINED || GmlString(var("menu_show")) != "main" || Num(var("main_panel")) != 0)
				LegendaryPromptClick("close");                     // the menu closed or another panel was chosen
		}
		if (g_MenuChanged) { g_MenuChanged = false; changed = true; }
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
			{ "hero_hit", "scr_hero_proc_when_hit", 0, { "the_health", "final_the_max_health", "the_shield", "the_max_shield" } },   // damage, attacker
			{ "hero_died", "scr_hero_dead", 0, {} },
			{ "item_picked_up", "scr_pick_up_item", 3, {} },                     // arg 0 = the items, result = picked
			{ "skill_cast", "scr_hero_proc_when_any_cast", 0, {} },              // once per cast, arg 0 = the skill
			{ "run_started", "scr_run_start", 0, {} },
			{ "run_ended", "scr_run_end", 0, {} },
			{ "hit_dealt", "scr_hero_proc_when_crit_brutal", 0, {} },          // once per hero hit: skill, strike type, target
			{ "damage_dealt", "scr_damage_shield", 0, {} },                     // arg 0 = target, result = health damage
			{ "status_applied", "scr_buff_list_add", 0, {} },                   // key, duration, target
			{ "cooldown_started", "scr_new_attack_state_start_cooldown", 0, {} },   // self = hero, arg 0 = skill
			{ "stats_refreshed", "scr_stats_refresh", 0, {} },                  // self = the instance refreshed
		};
		return list;
	}
	const char* const kWatched[] = { "loot_dropped", "gold_changed", "level_up", "item_equipped", "room_changed", "menu_changed",
		"status_removed" };
	const char* const kDirect[] = { "setting_changed", "boss_killed", "ui_clicked",   // from commands (g_DirectEvents) or other events
		"class_cast", "class_hit",                                                    // a custom class's skills (kit "own")
		"legacy_used",                                                                // ... and its activable legacies
		"class_achievement",                                                          // ... and its achievements' steps
		"mission_started", "mission_done",                                            // ... and its story missions
		"dialogue_choice" };                                                          // a "say" question answered Yes

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
		return Wanted(Se.Event) || (!strcmp(Se.Event, "enemy_killed") && Wanted("boss_killed"))
			|| (!strcmp(Se.Event, "status_applied") && Wanted("status_removed"));   // removals are watched from applications
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

	// statuses on enemies seen applied, checked a few times a second until their stacks are gone ("status_removed")
	struct Tracked { int64_t Inst; std::string Key; Json Target; };
	std::map<std::pair<int64_t, std::string>, Tracked> Statuses;
	double StatusAt = 0;

	int64_t HeroId()
	{
		RValue obj = AssetIndex("obj_parent_hero");
		if (g_Yytk->CallBuiltin("instance_number", { obj }).ToDouble() < 1) return -1;
		return g_Yytk->CallBuiltin("instance_find", { obj, RValue(0.0) }).ToInt64();
	}

	// the stacks of a status on an instance (-1: the instance is gone)
	double StatusStacks(int64_t Inst, const std::string& Key)
	{
		CInstance* inst = nullptr;
		if (!AurieSuccess(g_Yytk->GetInstanceObject(static_cast<int32_t>(Inst), inst)) || !inst) return -1;
		RValue r;
		std::vector<RValue> args = { RValue(std::string_view(Key)), RValue(inst) };
		if (!AurieSuccess(g_Yytk->CallGameScriptEx(r, "gml_Script_scr_buff_list_get_stack", inst, inst, args))) return 0;
		return IsNumeric(r) ? r.ToDouble() : 0;
	}

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
		if (Wanted("status_removed") && MicrosSinceStart() / 1e6 >= StatusAt)
		{
			StatusAt = MicrosSinceStart() / 1e6 + 0.25;
			for (auto it = Statuses.begin(); it != Statuses.end();)
			{
				double stacks = StatusStacks(it->second.Inst, it->second.Key);
				if (stacks > 0) { ++it; continue; }
				Json d = Json::Object();
				d.Add("status", it->second.Key);
				try { d.Add("name", Data().BuffName(it->second.Key)); }
				catch (const std::exception&) {}
				d.Add("target", it->second.Target);
				d.Add("reason", stacks < 0 ? "target_gone" : "ended");
				Emit(Out, "status_removed", std::move(d));
				it = Statuses.erase(it);
			}
		}
		else if (!Wanted("status_removed")) Statuses.clear();
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
		if (it == g_HookLogs.end() || it->second.empty()) return;
		const int64_t heroId = HeroId();
		bool statsSent = false;
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
				d.Add("shield", var("the_shield"));                 // Light Shield after the hit
				d.Add("max_shield", var("the_max_shield"));
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
			else if (script == "scr_hero_proc_when_crit_brutal")   // skill, ?, ?, "normal" | "critical" | "brutal", ?, target
			{
				std::string strike = gd::Str(arg(3));
				d.Add("target", stub(arg(5)));
				d.Add("skill", arg(0));
				d.Add("strike", strike == "brutal" ? "ancestral" : strike);
				Emit(Out, "hit_dealt", std::move(d));
			}
			else if (script == "scr_damage_shield")                // target, ... -> the damage left after shields
			{
				const Json* t = arg(0).Find("$inst");
				double dmg = gd::Num(e.Result);
				if (!t || static_cast<int64_t>(t->n) == heroId || dmg <= 0) continue;   // the hero's own damage is hero_hit
				d.Add("target", stub(arg(0)));
				d.Add("damage", dmg);
				Emit(Out, "damage_dealt", std::move(d));
			}
			else if (script == "scr_buff_list_add")                // key, duration, target
			{
				const Json* t = arg(2).Find("$inst");
				const Json* s = e.Self.Find("$inst");
				if (!t || static_cast<int64_t>(t->n) == heroId || (s && s->n == t->n)) continue;   // statuses on others only
				std::string key = gd::Str(arg(0));
				int64_t inst = static_cast<int64_t>(t->n);
				double stacks = StatusStacks(inst, key);
				d.Add("status", key);
				try { d.Add("name", Data().BuffName(key)); }
				catch (const std::exception&) {}
				d.Add("target", stub(arg(2)));
				d.Add("duration", arg(1));
				d.Add("stacks", stacks > 0 ? stacks : 0.0);
				if (Wanted("status_removed") && stacks > 0 && Statuses.size() < 4096)
					Statuses[{ inst, key }] = Tracked{ inst, key, stub(arg(2)) };
				Emit(Out, "status_applied", std::move(d));
			}
			else if (script == "scr_new_attack_state_start_cooldown")
			{
				int h = CurrentHero(), id = static_cast<int>(gd::Num(arg(0), -1));
				d.Add("skill", id);
				d.Add("hero", h);
				try { d.Add("name", Data().SkillName(h, id)); }
				catch (const std::exception&) {}
				d.Add("cooldown", e.Result);
				Emit(Out, "cooldown_started", std::move(d));
			}
			else if (script == "scr_stats_refresh")
			{
				const Json* s = e.Self.Find("$inst");
				if (!s || static_cast<int64_t>(s->n) != heroId || statsSent) continue;   // the hero's, once per frame
				statsSent = true;
				d.Add("hero", CurrentHero());
				Emit(Out, "stats_refreshed", std::move(d));
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

// The runner's "errors disabled" flag, found like YYToolkit's Zeus::FindErrorSuppressionVariable: the first
// "mov [rip+x], 1" in is_nan (which sets it around its own checks). nullptr if not found.
static bool* RunnerErrorsFlag()
{
	static bool* flag = []() -> bool*
	{
		PVOID fn = nullptr;
		if (!AurieSuccess(g_Yytk->GetNamedRoutinePointer("is_nan", &fn)) || !fn) return nullptr;
		auto* p = static_cast<uint8_t*>(fn);
		if (p[0] == 0xE9) { int32_t rel; memcpy(&rel, p + 1, 4); p = p + 5 + rel; }      // a jump thunk
		for (int i = 0; i < 0x100 - 10; ++i)
		{
			int32_t disp;
			if (p[i] == 0xC6 && p[i + 1] == 0x05 && p[i + 6] == 0x01)                    // mov byte [rip+disp], 1
			{ memcpy(&disp, p + i + 2, 4); return reinterpret_cast<bool*>(p + i + 7 + disp); }
			if (p[i] == 0xC7 && p[i + 1] == 0x05 && p[i + 6] == 0x01 && !p[i + 7] && !p[i + 8] && !p[i + 9])   // dword
			{ memcpy(&disp, p + i + 2, 4); return reinterpret_cast<bool*>(p + i + 10 + disp); }
		}
		return nullptr;
	}();
	return flag;
}

// {"address": "0x1420f8130"} -> the game's static RValue at that address of the decompiled exe (image base
// 0x140000000; strings among them are only set at runtime). Development, for reading decompiled code.
static Json CmdStaticValue(const Json& Req)
{
	uint64_t a = std::stoull(Req.GetStr("address"), nullptr, 16);
	if (a < 0x140001000 || a >= 0x150000000) Fail("address outside the exe image");
	auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
	auto* v = reinterpret_cast<const RValue*>(base + (a - 0x140000000));
	MEMORY_BASIC_INFORMATION mbi{};
	if (!VirtualQuery(v, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) Fail("address not mapped");
	return ToJson(*v, 2);
}

// {"enable": bool, "errors_off": bool, "reset": bool} -> {"enabled", "errors_off", "errors": {event: count}}
// (development, see g_CatchErrors; errors_off sets the runner's own "errors disabled" flag)
static Json CmdCatchErrors(const Json& Req)
{
	if (const Json* e = Req.Find("errors_off"))
	{
		bool* f = RunnerErrorsFlag();
		if (!f) Fail("the runner's errors flag was not found");
		*f = e->b;
	}
	if (const Json* e = Req.Find("enable")) g_CatchErrors = e->b;
	if (Req.GetBool("reset", false)) g_CaughtErrors.clear();
	Json j = Json::Object(), errors = Json::Object();
	for (auto& [name, n] : g_CaughtErrors) errors.Add(name, n);
	j.Add("enabled", g_CatchErrors);
	j.Add("errors_off", RunnerErrorsFlag() ? Json(*RunnerErrorsFlag()) : Json());
	j.Add("errors", std::move(errors));
	return j;
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
		g_PerfExchangeUs = 0;
		g_PerfExchanges = 0;
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
	j.Add("exchange_us_per_frame", g_PerfExchangeUs / f);
	j.Add("exchanges_per_frame", static_cast<double>(g_PerfExchanges) / f);
	return j;
}

// {"enable": bool, "reset": bool, "top": n} -> per frame since the last reset: the frame's time, the object events'
// (outermost ones only, so no double count), and the n events with the most time of their own
static Json CmdProfile(const Json& Req)
{
	if (const Json* e = Req.Find("enable"))                 // (turned on: from zero; turned off: what it has, still read)
	{
		if (e->b && !prof::On) prof::Reset();
		prof::On = e->b;
	}
	Json j = Json::Object();
	double f = prof::Frames ? static_cast<double>(prof::Frames) : 1;
	j.Add("enabled", prof::On);
	j.Add("frames", prof::Frames);
	j.Add("frame_us", prof::FrameUs / f);
	j.Add("events_us", prof::TopUs / f);
	std::vector<std::pair<CCode*, prof::Row>> rows(prof::Rows.begin(), prof::Rows.end());
	std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.second.Self > b.second.Self; });
	double want = Req.GetNum("top", 60);
	size_t top = want > 0 ? static_cast<size_t>((std::min)(want, 1e6)) : 0;
	Json list = Json::Array();
	for (size_t i = 0; i < rows.size() && i < top; ++i)
	{
		Json r = Json::Array();
		r.Push(Json(std::string(rows[i].first->GetName() ? rows[i].first->GetName() : "?")));
		r.Push(Json(rows[i].second.Self / f));
		r.Push(Json(rows[i].second.Total / f));
		r.Push(Json(static_cast<double>(rows[i].second.Calls) / f));
		list.Push(std::move(r));
	}
	j.Add("rows", std::move(list));
	if (Req.GetBool("reset", false)) prof::Reset();
	return j;
}

// ============================================================================
// Buffs: the game's own buff list (scr_buff_list_add), which takes stats with it - the game's stat refresh, timers,
// stacks and its buff bar do the rest. Used by custom legendaries and by the buff_add / buff_remove commands.
// ============================================================================

static RValue HeroInstance()
{
	RValue obj = AssetIndex("obj_parent_hero");
	if (g_Yytk->CallBuiltin("instance_number", { obj }).ToDouble() < 1) return RValue();
	return g_Yytk->CallBuiltin("instance_find", { obj, RValue(0.0) });
}

static std::string Fmt(double V)
{
	char b[32];
	snprintf(b, sizeof(b), "%.10g", V);
	return b;
}

// the definition row the game made for a buff key (created on its first add): names, its icon (another buff's), kind
static void BuffDescribe(double Ref, const std::string& Name, const std::string& Description, const std::string& IconOf, bool Debuff)
{
	RValue* table = FindMember(GlobalRValue(), "buffs");
	if (!table) return;
	RValue* row = ArrayAt(*table, static_cast<size_t>(Ref));
	if (!row || KindOf(*row) != VALUE_ARRAY) return;
	auto put = [&](size_t I, const RValue& V) { if (RValue* c = ArrayAt(*row, I)) *c = V; };
	if (!IconOf.empty())                                       // the icon of one of the game's buffs, by its key
		for (size_t k = 0, n = ArrayLen(*table); k < n; ++k)
		{
			RValue* other = ArrayAt(*table, k);
			RValue* key = other ? ArrayAt(*other, 1) : nullptr;
			if (!key || KindOf(*key) != VALUE_STRING || key->ToString() != IconOf) continue;
			if (RValue* spr = ArrayAt(*other, 2)) put(2, *spr);
			if (RValue* frame = ArrayAt(*other, 3)) put(3, *frame);
			break;
		}
	put(5, RStr(Debuff ? "debuff" : "buff"));
	for (size_t c = 13; c + 1 < ArrayLen(*row); c += 2)
	{
		if (!Name.empty()) put(c, RStr(Name));
		if (!Description.empty()) put(c + 1, RStr(Description));
	}
}

// adds Stacks of a buff to Target: stats {key: value}, Seconds < 0 = until removed; returns the stacks it has now
static double BuffAdd(const RValue& Target, const std::string& Key, const std::vector<std::pair<std::string, double>>& Stats,
	double Seconds, int Stacks, int MaxStacks, const std::string& Name, const std::string& Description, const std::string& IconOf,
	bool Debuff)
{
	if (Key.empty() || Key.find_first_of(",|") != std::string::npos) Fail("a buff key without , or |");
	if (!IconOf.empty())                                       // the icon must be one of the game's buffs
	{
		bool found = false;
		if (RValue* table = FindMember(GlobalRValue(), "buffs"))
			for (size_t k = 0, n = ArrayLen(*table); k < n && !found; ++k)
				if (RValue* row = ArrayAt(*table, k))
					if (RValue* key = ArrayAt(*row, 1); key && KindOf(*key) == VALUE_STRING && key->ToString() == IconOf)
						if (RValue* frame = ArrayAt(*row, 3); frame && IsNumeric(*frame)) found = true;
		if (!found) Fail("icon '" + IconOf + "' is not one of the game's buffs (e.g. slow, fire_burn, ice_chill)");
	}
	std::string values, keys;
	for (auto& [k, v] : Stats)
	{
		values += (values.empty() ? "" : ",") + Fmt(v);
		keys += (keys.empty() ? "" : ",") + k;
	}
	CInstance* inst = Target.m_Instance;
	double ticks = Seconds < 0 ? -1.0 : std::round(Seconds * 60.0);    // the game counts buff time 60 per second
	// shown in the game's buff bar only with an icon (one of the game's buffs'): the bar reads the icon of every shown
	// buff, and a buff the game made for a new key has none - hidden, like the game's own passives
	bool shown = !IconOf.empty();
	RValue r;
	std::vector<RValue> args = { RStr(Key), RValue(ticks), Target, RValue(static_cast<double>((std::max)(1, Stacks))), RValue(shown),
		RValue(shown), RStr(values), RStr(keys), RValue(static_cast<double>((std::max)(1, MaxStacks))) };
	if (!AurieSuccess(g_Yytk->CallGameScriptEx(r, "gml_Script_scr_buff_list_add", inst, inst, args))) Fail("scr_buff_list_add failed");
	if (IsNumeric(r) && shown) BuffDescribe(r.ToDouble(), Name, Description, IconOf, Debuff);
	RValue n;
	std::vector<RValue> q = { RStr(Key), Target };
	g_Yytk->CallGameScriptEx(n, "gml_Script_scr_buff_list_get_stack", inst, inst, q);
	return IsNumeric(n) ? n.ToDouble() : 0;
}

static double BuffStacks(const RValue& Target, const std::string& Key)
{
	RValue n;
	std::vector<RValue> q = { RStr(Key), Target };
	g_Yytk->CallGameScriptEx(n, "gml_Script_scr_buff_list_get_stack", Target.m_Instance, Target.m_Instance, q);
	return IsNumeric(n) ? n.ToDouble() : 0;
}

static void BuffRemove(const RValue& Target, const std::string& Key)
{
	RValue r;
	std::vector<RValue> q = { RStr(Key), Target };
	g_Yytk->CallGameScriptEx(r, "gml_Script_scr_buff_list_remove", Target.m_Instance, Target.m_Instance, q);
}

static RValue BuffTarget(const Json& Req)
{
	if (const Json* t = Req.Find("target"); t && !t->IsNull()) return ResolveRoot(t);
	RValue hero = HeroInstance();
	if (KindOf(hero) == VALUE_UNDEFINED) Fail("no hero");
	return RValue(InstanceById(static_cast<int32_t>(hero.ToInt64())));
}

static std::vector<std::pair<std::string, double>> StatsOf(const Json* J, double Scale = 1.0)
{
	std::vector<std::pair<std::string, double>> out;
	if (!J) return out;
	if (J->type != Json::Obj) Fail("'stats' is an object {stat: value}");
	for (auto& [k, v] : J->o)
	{
		bool known = false;
		for (auto& c : gd::kStatCatalog) known |= k == c.Key;
		if (!known) Fail("unknown stat '" + k + "' (stat_list has every stat)");
		// the game keeps these two old flat-knockback inputs, but its stat code crashes on them: not through buffs
		if (k == "knockback_melee_add" || k == "knockback_projectile_add")
			Fail("'" + k + "' cannot be given by a buff (the game's stat code does not take it); use the _percent one");
		if (v.type != Json::Num) Fail("stat '" + k + "' takes a number");
		out.emplace_back(k, v.n * Scale);
	}
	return out;
}

// {target?, key, stats?, duration? (seconds), stacks?, max_stacks?, name?, description?, icon? (a game buff key), debuff?}
static Json CmdBuffAdd(const Json& Req)
{
	RValue target = BuffTarget(Req);
	double stacks = BuffAdd(target, Req.GetStr("key"), StatsOf(Req.Find("stats")), Req.GetNum("duration", -1),
		static_cast<int>(Req.GetNum("stacks", 1)), static_cast<int>(Req.GetNum("max_stacks", 1)), Req.GetStr("name"),
		Req.GetStr("description"), Req.GetStr("icon"), Req.GetBool("debuff", false));
	return Json(stacks);
}
static Json CmdBuffRemove(const Json& Req) { BuffRemove(BuffTarget(Req), Req.GetStr("key")); return Json(true); }
static Json CmdBuffStacks(const Json& Req) { return Json(BuffStacks(BuffTarget(Req), Req.GetStr("key"))); }

// ============================================================================
// Custom legendaries: rows appended to the game's own legendary table (global.legendary), so they drop, show up in
// reforging, show their name and description and count as equipped like the game's own. Their stats are a passive
// game buff on the hero while one is worn (scaled by the item's legendary roll, like the game's); the rows are never
// "stat-only", so the game's own equipment calculator - which caches those rows - never works on them. Effects run on
// SlormAPI's events with the game's buffs. Saves never hold them: around the game's save of the inventory each custom legendary is
// written as an Epic with the same stats, turned back after loading (data/legendary_items.json) - a game without
// SlormAPI just finds ordinary Epics.
// ============================================================================

namespace legend
{
	struct Effect
	{
		std::string Trigger;                 // hit, crit, ancestral, kill, boss_kill, hero_hit, cast, status, every
		std::string Status;                  // trigger "status": the status key that must be applied ("" = any)
		double Chance = 100, Cooldown = 0, Every = 0, Last = -1e18;
		bool Scale = true;                   // amounts scale with the item's legendary roll (75-100%), like the game's
		Json Buff, Heal;
	};
	struct Def
	{
		std::string Id, Owner, Local, Name, Description, Slot, Icon;
		int Hero = -1, Ref = -1, TemplateRef = 8;
		bool Lootable = true, Active = true;
		std::vector<std::pair<std::string, double>> Stats;
		std::vector<double> Values;          // more numbers for the description's @ (not stats)
		std::vector<Effect> Effects;
	};
	std::vector<Def> Defs;
	int Base = -1;                           // the table's size before the first custom legendary
	uint64_t Version = 0;
	std::vector<uint64_t> Subs;              // event subscriptions for the effects

	Def* ByRef(double Ref)
	{
		for (auto& d : Defs) if (d.Ref == static_cast<int>(Ref) && d.Ref >= 0) return &d;
		return nullptr;
	}
	Def* ById(const std::string& Id)
	{
		for (auto& d : Defs) if (d.Id == Id) return &d;
		return nullptr;
	}
	bool IsCustom(const RValue& V)
	{
		if (Defs.empty()) return false;
		double ref = -1;
		if (IsNumeric(V)) ref = V.ToDouble();
		else if (KindOf(V) == VALUE_STRING) { try { ref = std::stod(V.ToString()); } catch (...) { return false; } }
		return ref >= Base && ByRef(ref) != nullptr;
	}

	void RefreshHero()
	{
		RValue hero = HeroInstance();
		if (KindOf(hero) == VALUE_UNDEFINED) return;
		CInstance* h = InstanceById(static_cast<int32_t>(hero.ToInt64()));
		RValue r;
		g_Yytk->CallGameScriptEx(r, "gml_Script_scr_hero_fast_check_reset", h, h, {});   // the per-legendary caches
		for (const char* f : { "self_refresh_equipment", "self_refresh_legendaries", "self_refresh_stats" })
			g_Yytk->CallBuiltin("variable_instance_set", { RValue(h), RValue(f), RValue(true) });
	}

	// ---- the table row
	RValue BuildRow(const Def& D, RValue& Table)
	{
		size_t n = ArrayLen(Table), tmpl = SIZE_MAX;
		for (size_t i = 0; i < static_cast<size_t>(Base) && i < n && tmpl == SIZE_MAX; ++i)   // one of the game's of that slot
		{
			RValue* row = ArrayAt(Table, i);
			RValue* slot = row ? ArrayAt(*row, 13) : nullptr;
			if (slot && KindOf(*slot) == VALUE_STRING && slot->ToString() == D.Slot) tmpl = i;
		}
		if (tmpl == SIZE_MAX) tmpl = 8;
		RValue* src = ArrayAt(Table, tmpl);
		if (!src || ArrayLen(*src) < 21) Fail("unexpected legendary table");
		std::vector<RValue> row;
		for (size_t c = 0, m = ArrayLen(*src); c < m; ++c) row.push_back(*ArrayAt(*src, c));
		// the description's @ numbers (stats first, then the extra values); no STAT: SlormAPI applies the stats itself
		std::string value, stat, type, upgrade, range;
		bool first = true;
		auto add = [&](double V, bool Pct)
		{
			std::string sep = first ? "" : "|";
			first = false;
			value += sep + Fmt(V); stat += sep; type += sep + (Pct ? "%" : ""); upgrade += sep + "0"; range += sep + "1";
		};
		if (D.Active)
		{
			for (auto& [k, v] : D.Stats) add(v, k.ends_with("_percent") || k.ends_with("_mult"));
			for (double v : D.Values) add(v, false);
		}
		row[0] = RValue(static_cast<double>(D.Ref));
		row[1] = RStr("");
		row[2] = RStr(value);
		row[3] = RStr(stat);
		row[9] = RValue(static_cast<double>(D.Hero));
		row[10] = RStr("");
		row[11] = RStr(type);
		row[12] = RStr(upgrade);
		row[13] = RStr(D.Slot);
		if (!D.Icon.empty())                                    // the icon of one of the game's legendaries, by its number
			if (RValue* other = ArrayAt(Table, static_cast<size_t>(std::stoi(D.Icon))))
				if (RValue* ic = ArrayAt(*other, 14)) row[14] = *ic;
		row[15] = RStr(range);
		row[16] = RValue(D.Lootable && D.Active ? 1.0 : 0.0);
		row[17] = RValue(0.0);                                  // not stat-only: the game's equipment calculator skips it
		std::string name = D.Active ? D.Name : D.Name + " (mod off)";
		for (size_t c = 19; c + 1 < row.size(); c += 2) { row[c] = RStr(name); row[c + 1] = RStr(D.Description); }
		return RValue(row);
	}

	void WriteRow(Def& D)
	{
		RValue* table = FindMember(GlobalRValue(), "legendary");
		if (!table || KindOf(*table) != VALUE_ARRAY) Fail("the game's legendary table is not loaded yet");
		if (Base < 0) Base = static_cast<int>(ArrayLen(*table));
		if (D.Ref < 0) D.Ref = static_cast<int>(ArrayLen(*table));
		RValue row = BuildRow(D, *table);
		g_Yytk->CallBuiltin("array_set", { *table, RValue(static_cast<double>(D.Ref)), row });
	}

	// ---- saves: custom legendaries are saved as Epics; the Epics are known again by their exact contents
	std::map<std::string, Json> Saved;          // fingerprint of the Epic -> {"id", "line": the L line}
	std::map<std::string, Json> Slots;          // save slot ("1") -> [{"id", "name"}]: what that save holds
	bool SavedLoaded = false, SavedDirty = false;
	fs::path SavedFile() { return AurieDir() / "SlormAPI" / "data" / "legendary_items.json"; }

	void LoadSaved()
	{
		if (SavedLoaded) return;
		SavedLoaded = true;
		std::ifstream f(SavedFile(), std::ios::binary);
		if (!f) return;
		std::stringstream ss;
		ss << f.rdbuf();
		try
		{
			Json j = sj::Parse(ss.str());
			if (const Json* items = j.Find("items"); items && items->type == Json::Obj)
				for (auto& [k, v] : items->o) Saved[k] = v;
			if (const Json* slots = j.Find("slots"); slots && slots->type == Json::Obj)
				for (auto& [k, v] : slots->o) Slots[k] = v;
		}
		catch (const std::exception&) {}
	}

	void WriteSaved()
	{
		Json items = Json::Object();
		for (auto& [k, v] : Saved) items.Add(k, v);
		Json j = Json::Object();
		j.Add("version", 1);
		j.Add("items", std::move(items));
		Json slots = Json::Object();
		for (auto& [k, v] : Slots) slots.Add(k, v);
		j.Add("slots", std::move(slots));
		std::error_code ec;
		fs::create_directories(SavedFile().parent_path(), ec);
		fs::path tmp = SavedFile();
		tmp += ".tmp";
		{ std::ofstream o(tmp, std::ios::binary | std::ios::trunc); o << sj::Dump(j); }
		fs::rename(tmp, SavedFile(), ec);
		SavedDirty = false;
	}

	// every value as text, so the same item matches before a save and after a load ("98" == 98, false == "0")
	void Print(const RValue& V, std::string& Out, int Depth = 0)
	{
		if (Depth > 4) return;
		if (KindOf(V) == VALUE_ARRAY)
		{
			Out += '[';
			RValue copy = V;
			for (size_t k = 0, n = ArrayLen(copy); k < n; ++k)
			{
				if (k) Out += ',';
				if (RValue* e = ArrayAt(copy, k)) Print(*e, Out, Depth + 1);
			}
			Out += ']';
		}
		else if (KindOf(V) == VALUE_STRING) Out += V.ToString();
		else if (KindOf(V) == VALUE_BOOL) Out += V.ToBoolean() ? "1" : "0";
		else if (IsNumeric(V)) Out += Fmt(V.ToDouble());
	}
	std::string Fingerprint(const RValue& Item) { std::string s; Print(Item, s); return s; }

	bool IsItem(const RValue& V)
	{
		if (KindOf(V) != VALUE_ARRAY || ArrayLen(V) < 1) return false;
		RValue copy = V;
		RValue* h = ArrayAt(copy, 0);
		RValue* kind = h && KindOf(*h) == VALUE_ARRAY ? ArrayAt(*h, 3) : nullptr;
		return kind && KindOf(*kind) == VALUE_STRING && kind->ToString() == "equipment";
	}

	// the custom legendary an item carries (and the index of its L line), or nullptr
	Def* CustomOf(RValue& Item, size_t& Line)
	{
		for (size_t k = 1, n = ArrayLen(Item); k < n; ++k)
		{
			RValue* l = ArrayAt(Item, k);
			RValue* tag = l && KindOf(*l) == VALUE_ARRAY ? ArrayAt(*l, 0) : nullptr;
			if (!tag || KindOf(*tag) != VALUE_STRING || tag->ToString() != "L") continue;
			RValue* ref = ArrayAt(*l, 1);
			if (ref && IsCustom(*ref)) { Line = k; return ByRef(IsNumeric(*ref) ? ref->ToDouble() : std::stod(ref->ToString())); }
			return nullptr;
		}
		return nullptr;
	}

	RValue Header(RValue& Item, const char* Rarity)
	{
		RValue* h = ArrayAt(Item, 0);
		std::vector<RValue> out;
		for (size_t c = 0, n = h ? ArrayLen(*h) : 0; c < n; ++c) out.push_back(*ArrayAt(*h, c));
		if (!out.empty()) out[0] = RStr(Rarity);
		return RValue(out);
	}

	// the Epic a custom legendary is saved as: the same item without its L line
	RValue Fallback(RValue& Item, size_t Line)
	{
		std::vector<RValue> out = { Header(Item, "epic") };
		for (size_t k = 1, n = ArrayLen(Item); k < n; ++k)
			if (k != Line) out.push_back(*ArrayAt(Item, k));
		return RValue(out);
	}

	// every item slot of the heroes' inventories (equipped, bag, stash)
	template <class F> void EachSlot(F Fn)
	{
		RValue* inv = FindMember(GlobalRValue(), "inventory");
		if (!inv || KindOf(*inv) != VALUE_ARRAY) return;
		for (size_t h = 0, nh = ArrayLen(*inv); h < nh; ++h)
		{
			RValue* hero = ArrayAt(*inv, h);
			if (!hero || KindOf(*hero) != VALUE_ARRAY) continue;
			for (size_t i = 0, ni = ArrayLen(*hero); i < ni; ++i)
				if (RValue* slot = ArrayAt(*hero, i); slot && IsItem(*slot)) Fn(h, i, *slot);
		}
	}

	struct Swap { size_t Hero, Index; RValue Original; };
	std::vector<Swap> Swaps;

	// ---- what each save slot holds, so a save can be checked before it is loaded
	struct Waiting { std::string Name, Id; };

	bool IsEpic(RValue& Item)
	{
		RValue* h = ArrayAt(Item, 0);
		RValue* rarity = h ? ArrayAt(*h, 0) : nullptr;
		return rarity && KindOf(*rarity) == VALUE_STRING && rarity->ToString() == "epic";
	}

	// the custom legendaries in the inventory now: as legendaries, or as the Epics they were saved as (one per id)
	std::vector<Waiting> Contents()
	{
		std::vector<Waiting> out;
		auto add = [&](const std::string& Id, const std::string& Name)
		{
			for (auto& w : out) if (w.Id == Id) return;
			out.push_back({ Name, Id });
		};
		EachSlot([&](size_t, size_t, RValue& Item)
		{
			size_t line = 0;
			if (Def* d = Defs.empty() ? nullptr : CustomOf(Item, line)) add(d->Id, d->Name);
			else if (!Saved.empty() && IsEpic(Item))
				if (auto it = Saved.find(Fingerprint(Item)); it != Saved.end())
					add(it->second.GetStr("id"), it->second.GetStr("name", it->second.GetStr("id")));
		});
		return out;
	}

	int SaveSlot()
	{
		RValue* v = FindMember(GlobalRValue(), "save_file");
		return v && IsNumeric(*v) ? static_cast<int>(v->ToDouble()) : -1;
	}

	// remembers what the current save slot holds (written with the next WriteSaved)
	void RecordSlot()
	{
		int slot = SaveSlot();
		if (slot < 0) return;
		Json list = Json::Array();
		for (auto& w : Contents())
		{
			Json e = Json::Object();
			e.Add("id", w.Id);
			e.Add("name", w.Name);
			list.Push(std::move(e));
		}
		std::string key = std::to_string(slot);
		auto it = Slots.find(key);
		if (list.a.empty())
		{
			if (it != Slots.end()) { Slots.erase(it); SavedDirty = true; }
			return;
		}
		if (it == Slots.end() || sj::Dump(it->second) != sj::Dump(list)) { Slots[key] = std::move(list); SavedDirty = true; }
	}

	RValue* SlotAt(size_t Hero, size_t Index)
	{
		RValue* inv = FindMember(GlobalRValue(), "inventory");
		RValue* hero = inv ? ArrayAt(*inv, Hero) : nullptr;
		return hero ? ArrayAt(*hero, Index) : nullptr;
	}

	// heroes FromHero+: their custom legendaries become the Epics they are saved as (recorded to come back), the
	// originals into Out
	void SaveAsEpics(size_t FromHero, std::vector<Swap>& Out)
	{
		EachSlot([&](size_t H, size_t I, RValue& Item)
		{
			size_t line = 0;
			Def* d = H >= FromHero ? CustomOf(Item, line) : nullptr;
			if (!d) return;
			RValue fallback = Fallback(Item, line);
			Json rec = Json::Object();
			rec.Add("id", d->Id);
			rec.Add("name", d->Name);                            // for the warning when its mod is missing
			rec.Add("line", ToJson(*ArrayAt(Item, line), 2));
			std::string fp = Fingerprint(fallback);
			if (!Saved.count(fp) || sj::Dump(Saved[fp]) != sj::Dump(rec)) { Saved[fp] = rec; SavedDirty = true; }
			Out.push_back({ H, I, Item });
			Item = fallback;
		});
	}

	// a custom class's heroes (3+) are saved by SlormAPI (classes.json), not by the game: their items too go there as
	// Epics (a legendary's number differs between sessions), and come back with Restore like the game's
	std::vector<Swap> ClassSwaps;
	void BeforeClassSave()
	{
		LoadSaved();
		ClassSwaps.clear();
		if (!Defs.empty()) SaveAsEpics(3, ClassSwaps);
		if (SavedDirty) WriteSaved();
	}
	void AfterClassSave()
	{
		for (auto& s : ClassSwaps)
			if (RValue* slot = SlotAt(s.Hero, s.Index)) *slot = s.Original;
		ClassSwaps.clear();
	}

	bool PreSave(CInstance*, int ArgCount, RValue** Args, RValue&)
	{
		if (ArgCount < 1 || !Args[0] || KindOf(*Args[0]) != VALUE_STRING || Args[0]->ToString() != "inventory") return false;
		LoadSaved();
		Swaps.clear();
		RecordSlot();                                            // what this save holds, for the check before a load
		if (Defs.empty()) return false;
		SaveAsEpics(0, Swaps);
		return false;                                            // the game saves the Epics
	}

	void PostSave(CInstance*, int ArgCount, RValue** Args, RValue&)
	{
		for (auto& s : Swaps)
			if (RValue* slot = SlotAt(s.Hero, s.Index)) *slot = s.Original;
		bool any = !Swaps.empty();
		Swaps.clear();
		if (SavedDirty) WriteSaved();
		(void)ArgCount; (void)Args; (void)any;
	}

	// Epics that were custom legendaries when they were saved become them again (after a load, or when one registers)
	int Restore()
	{
		LoadSaved();
		if (Saved.empty() || Defs.empty()) return 0;
		int restored = 0;
		EachSlot([&](size_t, size_t, RValue& Item)
		{
			RValue* h = ArrayAt(Item, 0);
			RValue* rarity = h ? ArrayAt(*h, 0) : nullptr;
			if (!rarity || KindOf(*rarity) != VALUE_STRING || rarity->ToString() != "epic") return;
			auto it = Saved.find(Fingerprint(Item));
			if (it == Saved.end()) return;
			Def* d = ById(it->second.GetStr("id"));
			if (!d || d->Ref < 0) return;
			Json line = it->second.At("line");
			if (line.type != Json::Arr || line.a.size() < 3) return;
			line.a[1] = Json(static_cast<double>(d->Ref));        // this session's number for it
			std::vector<RValue> out = { Header(Item, "legendary"), FromJson(line) };
			for (size_t k = 1, n = ArrayLen(Item); k < n; ++k) out.push_back(*ArrayAt(Item, k));
			Item = RValue(out);
			++restored;
		});
		if (restored) RefreshHero();
		return restored;
	}

	// Epics in the loaded inventory that were custom legendaries whose mod is not loaded (or switched off) now:
	// asked about before the save is played (CONTINUE, Load Game), else shown as a warning over the game for a
	// while; and on the Mods page until they are back
	std::vector<Waiting> WaitingList;
	double CheckAt = 0, WarnLeft = 0, WarnTick = 0;

	void CheckWaiting()
	{
		LoadSaved();
		WaitingList.clear();
		if (Saved.empty()) return;
		if (KindOf(HeroInstance()) != VALUE_UNDEFINED)           // a save is loaded: remember what it holds
		{
			RecordSlot();
			if (SavedDirty) WriteSaved();
		}
		EachSlot([&](size_t, size_t, RValue& Item)
		{
			if (!IsEpic(Item)) return;
			auto it = Saved.find(Fingerprint(Item));
			if (it == Saved.end()) return;
			Def* d = ById(it->second.GetStr("id"));
			if (d && d->Active && d->Ref >= 0) return;            // (restored on its next check)
			WaitingList.push_back({ it->second.GetStr("name", it->second.GetStr("id")), it->second.GetStr("id") });
		});
	}

	std::string WaitingNames(size_t Max = 3)
	{
		std::string names;
		for (size_t i = 0; i < WaitingList.size() && i < Max; ++i) names += (i ? ", " : "") + WaitingList[i].Name;
		if (WaitingList.size() > Max) names += ", ...";
		return names;
	}

	// the mod a custom legendary belongs to: "lua:my_mod:item" -> "my_mod (Lua)", "MyMod:item" -> "MyMod"
	std::string ModOf(const std::string& Id)
	{
		std::string owner = Id.substr(0, Id.rfind(':'));
		return owner.starts_with("lua:") ? owner.substr(4) + " (Lua)" : owner;
	}

	std::string WaitingMods()
	{
		std::set<std::string> mods;
		for (auto& w : WaitingList) mods.insert(ModOf(w.Id));
		std::string out;
		for (auto& m : mods) out += (out.empty() ? "" : ", ") + m;
		return out;
	}

	// ---- the prompt before such a save is played: LOAD ANYWAY or BACK (drawn in the main menu by the Mods page)
	struct Prompt { bool Open = false; int Slot = -1; std::vector<Waiting> List; };   // Slot -1: CONTINUE
	Prompt Ask;
	bool Confirmed = false;        // the player knows about the loaded save's waiting legendaries: no prompt, no banner
	bool PassLoad = false;         // LOAD ANYWAY on a save slot: that load goes through...
	bool ConfirmNextLoad = false;  // ...and counts as confirmed

	void PostLoad(CInstance*, int, RValue**, RValue&)
	{
		Restore();
		Confirmed = ConfirmNextLoad;
		ConfirmNextLoad = false;
		WarnLeft = 0;                                              // a banner of the save before is not for this one
		CheckAt = MicrosSinceStart() / 1e6 + 4;                    // Lua mods that start late get time to register first
	}

	// the custom legendaries of a save slot whose mod is not loaded (or switched off) now
	std::vector<Waiting> WaitingInSlot(int Slot)
	{
		LoadSaved();
		std::vector<Waiting> out;
		auto it = Slots.find(std::to_string(Slot));
		if (it == Slots.end() || it->second.type != Json::Arr) return out;
		RValue* prefix = FindMember(GlobalRValue(), "name_save_file");          // "save_": the slot's file, save_<slot>
		std::string file = (prefix && KindOf(*prefix) == VALUE_STRING ? prefix->ToString() : std::string("save_")) + std::to_string(Slot);
		if (!g_Yytk->CallBuiltin("file_exists", { RStr(file) }).ToBoolean()) return out;   // deleted: a new game starts there
		for (auto& e : it->second.a)
		{
			Def* d = ById(e.GetStr("id"));
			if (d && d->Active && d->Ref >= 0) continue;
			out.push_back({ e.GetStr("name", e.GetStr("id")), e.GetStr("id") });
		}
		return out;
	}

	CInstance* UiSelf()
	{
		RValue ui = modspage::UiInstance();
		return KindOf(ui) == VALUE_UNDEFINED ? nullptr : InstanceById(static_cast<int32_t>(ui.ToInt64()));
	}

	void ChangePanel(CInstance* Ui, const char* Panel)
	{
		if (!Ui) return;
		RValue r;
		g_Yytk->CallGameScriptEx(r, "gml_Script_scr_main_change_panel", Ui, Ui, { RStr(Panel) });
	}

	void OpenPrompt(CInstance* Ui, int Slot, std::vector<Waiting> List)
	{
		Ask = { true, Slot, std::move(List) };
		modspage::Open = false;
		if (Ui && g_Yytk->CallBuiltin("variable_instance_get", { RValue(Ui), RValue("main_panel") }).ToDouble() != 0)
			ChangePanel(Ui, "main_0");                             // the Welcome box shows it
		g_MenuChanged = true;
		DbgPrintEx(LOG_SEVERITY_INFO, "[SlormAPI] asking before %s: %zu custom legendar%s without its mod",
			Slot < 0 ? "continuing" : ("loading save " + std::to_string(Slot)).c_str(), Ask.List.size(), Ask.List.size() == 1 ? "y" : "ies");
	}

	// CONTINUE plays the save that is already loaded
	bool PreMenuButton(CInstance* Self, int ArgCount, RValue** Args, RValue&)
	{
		if (Confirmed || ArgCount < 1 || !Args[0] || KindOf(*Args[0]) != VALUE_STRING || Args[0]->ToString() != "continue") return false;
		CheckWaiting();
		if (WaitingList.empty()) return false;
		OpenPrompt(Self, -1, WaitingList);
		return true;
	}

	// Load Game: a save slot is loaded
	bool PreLoadSlot(CInstance* Self, int ArgCount, RValue** Args, RValue&)
	{
		if (PassLoad) { PassLoad = false; return false; }
		if (ArgCount < 1 || !Args[0] || !IsNumeric(*Args[0])) return false;
		int slot = static_cast<int>(Args[0]->ToDouble());
		std::vector<Waiting> list = WaitingInSlot(slot);
		if (list.empty()) return false;
		OpenPrompt(Self, slot, std::move(list));
		return true;
	}

	void Click(const std::string& Action)
	{
		if (!Ask.Open) return;
		Prompt was = Ask;
		Ask = Prompt{};
		g_MenuChanged = true;
		CInstance* ui = UiSelf();
		if (Action == "back")
		{
			if (was.Slot >= 0) ChangePanel(ui, "main_2");          // back to the saves
		}
		else if (Action == "load" && ui)
		{
			RValue r;
			Confirmed = true;
			WarnLeft = 0;
			if (was.Slot < 0)
			{
				g_Yytk->CallGameScriptEx(r, "gml_Script_scr_ui_main_menu_buttons", ui, ui, { RStr("continue") });
			}
			else
			{
				PassLoad = ConfirmNextLoad = true;
				g_Yytk->CallGameScriptEx(r, "gml_Script_scr_save_load_save", ui, ui, { RValue(static_cast<double>(was.Slot)) });
				PassLoad = false;
			}
		}
	}

	// over the game's GUI while no menu is open, for WarnLeft seconds of being shown
	void DrawWarning(CInstance* Self)
	{
		if (WarnLeft <= 0 || WaitingList.empty() || !Self) return;
		RValue menu = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Self), RValue("menu_show") });
		if (KindOf(menu) == VALUE_STRING && menu.ToString() != "none") return;
		double now = MicrosSinceStart() / 1e6;
		if (WarnTick > 0) WarnLeft -= (std::min)(0.25, now - WarnTick);
		WarnTick = now;
		Confirmed = true;                                          // seen: CONTINUE in the pause menu does not ask again
		double sc = Num(g_Yytk->CallBuiltin("variable_instance_get", { RValue(Self), RValue("ui_scale") }));
		if (sc <= 0) sc = 1;                                       // the 1920x1080 layout times the game's ui_scale
		auto text = [&](double X, double Y, const std::string& S, const char* Font, const char* Color)
		{
			std::vector<RValue> a = { RValue(X * sc), RValue(Y * sc), RStr(S), RValue(1.0), RValue(1.0), RStr(Font), RValue(1.0), RValue(1.0),
				RValue(0.0), RValue(1.0), RStr(Color), RStr("black"), RValue(1.0), RValue(false) };
			RValue r;
			g_Yytk->CallGameScriptEx(r, "gml_Script_scr_draw_text", Self, Self, a);
		};
		double alpha = (std::min)(1.0, WarnLeft);                  // fades out in its last second
		RValue oldAlpha = g_Yytk->CallBuiltin("draw_get_alpha", {}), oldColor = g_Yytk->CallBuiltin("draw_get_color", {});
		g_Yytk->CallBuiltin("draw_set_alpha", { RValue(0.88 * alpha) });
		g_Yytk->CallBuiltin("draw_set_color", { RValue(static_cast<double>(0x0a0a0a)) });
		g_Yytk->CallBuiltin("draw_rectangle", { RValue(380 * sc), RValue(130 * sc), RValue(1540 * sc), RValue(262 * sc), RValue(false) });
		g_Yytk->CallBuiltin("draw_set_color", { RValue(static_cast<double>(0x2a5ad0)) });       // the game's orange (BGR)
		g_Yytk->CallBuiltin("draw_rectangle", { RValue(380 * sc), RValue(130 * sc), RValue(1540 * sc), RValue(262 * sc), RValue(true) });
		g_Yytk->CallBuiltin("draw_set_alpha", { RValue(alpha) });
		size_t n = WaitingList.size();
		text(960, 160, std::to_string(n) + (n == 1 ? " custom legendary is" : " custom legendaries are") + " waiting for a mod that is not loaded",
			"ft_standard_16", "white");
		text(960, 196, WaitingNames() + "   -   mod: " + WaitingMods(), "ft_standard_13_b", "item_highlight");
		text(960, 230, std::string(n == 1 ? "It stays an Epic" : "They stay Epics") + " until the mod is back. Don't reforge, upgrade or sell "
			+ (n == 1 ? "it" : "them") + ", or it can't become legendary again.", "ft_standard_13_b", "grey");
		g_Yytk->CallBuiltin("draw_set_alpha", { oldAlpha });
		g_Yytk->CallBuiltin("draw_set_color", { oldColor });
	}

	// last line of defence: a custom legendary number never reaches a save file (another container than the
	// inventory, e.g. the shop): the save gets the game's own legendary its row was copied from
	thread_local RValue g_SafeRef;
	bool PreDeconstruct(CInstance*, int ArgCount, RValue** Args, RValue&)
	{
		if (Defs.empty() || ArgCount < 3 || !Args[0] || !Args[2] || !IsNumeric(*Args[2]) || Args[2]->ToDouble() != 1) return false;
		if (!IsCustom(*Args[0])) return false;
		Def* d = ByRef(IsNumeric(*Args[0]) ? Args[0]->ToDouble() : std::stod(Args[0]->ToString()));
		g_SafeRef = RValue(static_cast<double>(d ? d->TemplateRef : 8));
		Args[0] = &g_SafeRef;
		return false;
	}

	// the legendary storage keeps one slot per game legendary: custom ones are never "owned" there
	bool PreStorageOwned(CInstance*, int ArgCount, RValue** Args, RValue& Result)
	{
		if (ArgCount < 1 || !Args[0] || !IsCustom(*Args[0])) return false;
		Result = RValue(false);
		return true;
	}

	// and if one ever lands in the storage area, it goes back to the bag
	double WatchAt = 0;
	void Watch()
	{
		auto num = [](const char* N) { RValue* v = FindMember(GlobalRValue(), N); return v && IsNumeric(*v) ? v->ToDouble() : -1; };
		double ch = num("character_slots"), inv = num("inventory_slots"), mat = num("material_slots"), st = num("storage_slots"),
			leg = num("storage_legendary_slots");
		if (ch < 0 || inv < 0 || mat < 0 || st < 0 || leg < 0) return;
		size_t start = static_cast<size_t>(ch + inv + mat + st - leg), bag0 = static_cast<size_t>(ch), bag1 = static_cast<size_t>(ch + inv);
		RValue* all = FindMember(GlobalRValue(), "inventory");
		for (size_t h = 0, nh = all ? ArrayLen(*all) : 0; h < nh; ++h)
		{
			RValue* hero = ArrayAt(*all, h);
			if (!hero || KindOf(*hero) != VALUE_ARRAY) continue;
			for (size_t i = start, n = ArrayLen(*hero); i < n; ++i)
			{
				RValue* slot = ArrayAt(*hero, i);
				size_t line = 0;
				if (!slot || !IsItem(*slot) || !CustomOf(*slot, line)) continue;
				for (size_t b = bag0; b < bag1; ++b)
					if (RValue* free = ArrayAt(*hero, b); free && !IsItem(*free) && KindOf(*free) != VALUE_ARRAY)
					{
						*free = *slot;
						*slot = RValue(0.0);
						DbgPrintEx(LOG_SEVERITY_INFO, "[SlormAPI] a custom legendary was moved out of the legendary storage to the bag");
						break;
					}
			}
		}
	}

	bool Hooked = false;
	void EnsureHooks()
	{
		if (Hooked) return;
		Hooked = true;
		NativeHookInstall("scr_save_save", { "SlormAPI.legendary", PreSave, PostSave });
		NativeHookInstall("scr_save_load", { "SlormAPI.legendary", nullptr, PostLoad });
		NativeHookInstall("scr_loot_save_deconstruct", { "SlormAPI.legendary", PreDeconstruct, nullptr });
		NativeHookInstall("scr_storage_check_if_legendary_owned", { "SlormAPI.legendary", PreStorageOwned, nullptr });
		NativeHookInstall("scr_ui_main_menu_buttons", { "SlormAPI.legendary", PreMenuButton, nullptr });
		NativeHookInstall("scr_save_load_save", { "SlormAPI.legendary", PreLoadSlot, nullptr });
	}

	// ---- effects
	std::map<int, double> Equipped;             // ref -> legendary roll, read once per frame
	uint64_t EquippedAt = UINT64_MAX;

	void ReadEquipped()
	{
		if (EquippedAt == g_CodeEvents) return;
		EquippedAt = g_CodeEvents;
		Equipped.clear();
		RValue hero = HeroInstance();
		if (KindOf(hero) == VALUE_UNDEFINED) return;
		RValue list = g_Yytk->CallBuiltin("variable_instance_get", { hero, RValue("legendaries_equipped") });
		for (size_t k = 0, n = ArrayLen(list); k < n; ++k)
		{
			RValue* row = ArrayAt(list, k);
			RValue* ref = row ? ArrayAt(*row, 2) : nullptr;
			if (!ref || !IsNumeric(*ref) || !ByRef(ref->ToDouble())) continue;
			double roll = 100;
			if (RValue* item = ArrayAt(*row, 0))
			{
				size_t line = 0;
				if (CustomOf(*item, line))
					if (RValue* r = ArrayAt(*ArrayAt(*item, line), 2))
						roll = IsNumeric(*r) ? r->ToDouble() : std::atof(r->ToString().c_str());
			}
			Equipped[static_cast<int>(ref->ToDouble())] = (std::max)(Equipped[static_cast<int>(ref->ToDouble())], roll);
		}
	}

	std::mt19937_64 Rng{ std::random_device{}() };

	void Run(Def& D, Effect& E, double Roll)
	{
		double now = MicrosSinceStart() / 1e6;
		if (now - E.Last < E.Cooldown) return;
		if (E.Chance < 100 && std::uniform_real_distribution<double>(0, 100)(Rng) >= E.Chance) return;
		E.Last = now;
		double scale = E.Scale ? Roll / 100.0 : 1.0;
		RValue hero = HeroInstance();
		if (KindOf(hero) == VALUE_UNDEFINED) return;
		RValue target(InstanceById(static_cast<int32_t>(hero.ToInt64())));
		if (E.Buff.type == Json::Obj)
		{
			size_t idx = &E - D.Effects.data();
			std::string key = "slormapi_" + D.Owner + "_" + D.Local + "_" + std::to_string(idx);
			for (auto& c : key) if (!isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
			BuffAdd(target, key, StatsOf(E.Buff.Find("stats"), scale), E.Buff.GetNum("duration", 5), 1,
				static_cast<int>(E.Buff.GetNum("max_stacks", 1)), E.Buff.GetStr("name", D.Name), E.Buff.GetStr("description"),
				E.Buff.GetStr("icon"), false);
		}
		if (E.Heal.type == Json::Obj)
		{
			auto get = [&](const char* V) { RValue v = g_Yytk->CallBuiltin("variable_instance_get", { target, RValue(V) }); return IsNumeric(v) ? v.ToDouble() : 0; };
			auto set = [&](const char* V, double X) { g_Yytk->CallBuiltin("variable_instance_set", { target, RValue(V), RValue(X) }); };
			double life = get("the_health"), maxLife = get("final_the_max_health"), mana = get("the_mana"), maxMana = get("final_the_max_mana");
			double addLife = (E.Heal.GetNum("life", 0) + maxLife * E.Heal.GetNum("life_percent", 0) / 100) * scale;
			double addMana = (E.Heal.GetNum("mana", 0) + maxMana * E.Heal.GetNum("mana_percent", 0) / 100) * scale;
			double addShield = (E.Heal.GetNum("shield", 0) + maxLife * E.Heal.GetNum("shield_percent", 0) / 100) * scale;
			if (addLife > 0 && life > 0) set("the_health", (std::min)(maxLife, life + addLife));
			if (addMana > 0) set("the_mana", (std::min)(maxMana, mana + addMana));
			if (addShield > 0)                                      // the Light Shield: its balance and its reference total
			{
				double shield = get("the_shield") + addShield;
				set("the_shield", shield);
				set("the_max_shield", (std::max)(get("the_max_shield"), shield));
			}
		}
	}

	void OnEvent(const char* Event, const char* DataJson, void*)
	{
		ReadEquipped();
		if (Equipped.empty()) return;
		std::string ev = Event;
		Json data;
		bool parsed = false;
		for (auto& d : Defs)
		{
			if (!d.Active || d.Ref < 0) continue;
			auto eq = Equipped.find(d.Ref);
			if (eq == Equipped.end()) continue;
			for (auto& e : d.Effects)
			{
				bool hit = false;
				if (e.Trigger == "hit" || e.Trigger == "crit" || e.Trigger == "ancestral")
				{
					if (ev != "hit_dealt") continue;
					if (!parsed) { data = sj::Parse(DataJson ? DataJson : "{}"); parsed = true; }
					std::string strike = data.GetStr("strike");
					hit = e.Trigger == "hit" || (e.Trigger == "crit" && strike == "critical") || (e.Trigger == "ancestral" && strike == "ancestral");
				}
				else if (e.Trigger == "kill") hit = ev == "enemy_killed";
				else if (e.Trigger == "boss_kill") hit = ev == "boss_killed";
				else if (e.Trigger == "hero_hit") hit = ev == "hero_hit";
				else if (e.Trigger == "cast") hit = ev == "skill_cast";
				else if (e.Trigger == "status" && ev == "status_applied")
				{
					if (!parsed) { data = sj::Parse(DataJson ? DataJson : "{}"); parsed = true; }
					hit = e.Status.empty() || data.GetStr("status") == e.Status;
				}
				if (hit) Run(d, e, eq->second);
			}
		}
	}

	// the stats of the worn custom legendaries: one passive buff each, kept while it is worn
	std::map<std::string, double> Applied;      // def id -> the roll its buff was made for
	double PassiveAt = 0;

	std::string PassiveKey(const Def& D)
	{
		std::string key = "slormapi_leg_" + D.Owner + "_" + D.Local;
		for (auto& c : key) if (!isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
		return key;
	}

	void Passives()
	{
		RValue hero = HeroInstance();
		if (KindOf(hero) == VALUE_UNDEFINED) { Applied.clear(); return; }
		RValue target(InstanceById(static_cast<int32_t>(hero.ToInt64())));
		ReadEquipped();
		bool changed = false;
		for (auto& d : Defs)
		{
			if (d.Stats.empty() && !Applied.count(d.Id)) continue;
			std::string key = PassiveKey(d);
			auto eq = Equipped.find(d.Ref);
			bool want = d.Active && !d.Stats.empty() && eq != Equipped.end();
			auto ap = Applied.find(d.Id);
			if (!want)
			{
				if (ap != Applied.end()) { BuffRemove(target, key); Applied.erase(ap); changed = true; }
				continue;
			}
			if (ap != Applied.end() && ap->second == eq->second && BuffStacks(target, key) > 0) continue;
			BuffRemove(target, key);                               // (a room change, a new roll or new stats: made again)
			std::vector<std::pair<std::string, double>> stats;
			for (auto& [k, v] : d.Stats) stats.emplace_back(k, v * eq->second / 100.0);
			BuffAdd(target, key, stats, -1, 1, 1, "", "", "", false);
			Applied[d.Id] = eq->second;
			changed = true;
		}
		if (changed)
			g_Yytk->CallBuiltin("variable_instance_set", { target, RValue("self_refresh_stats"), RValue(true) });
	}

	void EnsureHooks();
	bool Started = false;

	void Tick()
	{
		double now = MicrosSinceStart() / 1e6;
		if (!Started)                                               // custom legendaries were saved before: their saves
		{                                                           // are watched even when no mod registers one now
			Started = true;
			LoadSaved();
			if (!Saved.empty())
			{
				EnsureHooks();
				if (CheckAt <= 0) CheckAt = now + 4;                // the save the game loaded while starting
			}
		}
		if (CheckAt > 0 && now >= CheckAt)
		{
			CheckAt = 0;
			CheckWaiting();
			if (!WaitingList.empty() && !Confirmed)
			{
				WarnLeft = 15;
				DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormAPI] %zu custom legendar%s wait for their mod (%s): %s",
					WaitingList.size(), WaitingList.size() == 1 ? "y" : "ies", WaitingMods().c_str(), WaitingNames(10).c_str());
			}
		}
		if (Defs.empty()) return;
		if (now >= PassiveAt) { PassiveAt = now + 0.25; Passives(); }
		bool every = false;
		for (auto& d : Defs) for (auto& e : d.Effects) every |= e.Trigger == "every";
		if (every)
		{
			ReadEquipped();
			for (auto& d : Defs)
			{
				auto eq = Equipped.find(d.Ref);
				if (!d.Active || eq == Equipped.end()) continue;
				for (auto& e : d.Effects)
					if (e.Trigger == "every" && now - e.Last >= (std::max)(0.1, e.Every)) Run(d, e, eq->second);
			}
		}
		if (now >= WatchAt) { WatchAt = now + 0.5; Watch(); }
	}

	// subscribe to exactly the events the active effects need
	void SyncSubs()
	{
		for (auto id : Subs) events::Unsubscribe(id);
		Subs.clear();
		std::set<std::string> want;
		static const std::map<std::string, std::string> evOf = { { "hit", "hit_dealt" }, { "crit", "hit_dealt" },
			{ "ancestral", "hit_dealt" }, { "kill", "enemy_killed" }, { "boss_kill", "boss_killed" }, { "hero_hit", "hero_hit" },
			{ "cast", "skill_cast" }, { "status", "status_applied" } };
		for (auto& d : Defs)
			if (d.Active)
				for (auto& e : d.Effects)
					if (auto it = evOf.find(e.Trigger); it != evOf.end()) want.insert(it->second);
		for (auto& ev : want) Subs.push_back(events::Subscribe(ev, OnEvent, nullptr));
	}

	Effect ParseEffect(const Json& J)
	{
		static const std::set<std::string> triggers = { "hit", "crit", "ancestral", "kill", "boss_kill", "hero_hit", "cast", "status",
			"every" };
		Effect e;
		e.Trigger = J.GetStr("trigger");
		if (!triggers.count(e.Trigger)) Fail("effect trigger '" + e.Trigger + "' (hit, crit, ancestral, kill, boss_kill, hero_hit, cast, status, every)");
		e.Status = J.GetStr("status_key");
		e.Chance = J.GetNum("chance", 100);
		e.Cooldown = J.GetNum("cooldown", 0);
		e.Every = J.GetNum("seconds", 1);
		e.Scale = J.GetBool("scale_with_roll", true);
		if (const Json* b = J.Find("buff")) { e.Buff = *b; StatsOf(e.Buff.Find("stats")); }
		if (const Json* h = J.Find("heal")) e.Heal = *h;
		if (e.Buff.type != Json::Obj && e.Heal.type != Json::Obj) Fail("an effect needs a buff or a heal");
		return e;
	}

	// {owner, id, name, description, slot, hero?, lootable?, stats?, values?, icon?, effects?}
	Json Register(const Json& Req)
	{
		Def d;
		d.Owner = Req.GetStr("owner");
		d.Local = Req.GetStr("id");
		if (d.Owner.empty()) Fail("register_legendary needs your mod's name as owner");
		if (d.Local.empty()) Fail("register_legendary needs an id");
		d.Id = d.Owner + ":" + d.Local;
		d.Name = Req.GetStr("name", d.Local);
		d.Description = Req.GetStr("description");
		d.Slot = Req.GetStr("slot");
		bool known = false;
		for (auto& s : gd::Slots()) known |= d.Slot == s.Key;
		if (!known) Fail("slot '" + d.Slot + "' (helm body shoulder bracer glove boot ring amulet belt cape)");
		d.Hero = static_cast<int>(Req.GetNum("hero", -1));
		d.Lootable = Req.GetBool("lootable", true);
		d.Stats = StatsOf(Req.Find("stats"));
		if (const Json* v = Req.Find("values"); v && v->type == Json::Arr)
			for (auto& x : v->a) d.Values.push_back(x.type == Json::Num ? x.n : 0);
		if (const Json* ic = Req.Find("icon"); ic && ic->type == Json::Num) d.Icon = std::to_string(static_cast<int>(ic->n));
		if (const Json* fx = Req.Find("effects"); fx && fx->type == Json::Arr)
			for (auto& e : fx->a) d.Effects.push_back(ParseEffect(e));
		EnsureHooks();
		Def* old = ById(d.Id);
		if (old) { d.Ref = old->Ref; d.TemplateRef = old->TemplateRef; *old = d; }
		else Defs.push_back(d);
		Def& cur = old ? *old : Defs.back();
		WriteRow(cur);
		cur.TemplateRef = 8;                                     // the game's legendary a save falls back to (same slot)
		if (RValue* table = FindMember(GlobalRValue(), "legendary"))
			for (size_t i = 0; i < static_cast<size_t>(Base); ++i)
				if (RValue* row = ArrayAt(*table, i); row && ArrayAt(*row, 13) && KindOf(*ArrayAt(*row, 13)) == VALUE_STRING
					&& ArrayAt(*row, 13)->ToString() == cur.Slot) { cur.TemplateRef = static_cast<int>(i); break; }
		++Version;
		SyncSubs();
		RefreshHero();
		Restore();
		if (!WaitingList.empty()) CheckWaiting();                // the warning goes when its legendaries are back
		Applied.erase(cur.Id);                                   // its stats may have changed: the buff is made again
		Json out = Json::Object();
		out.Add("id", cur.Id);
		out.Add("ref", cur.Ref);
		return out;
	}

	// a mod's legendaries stay in the table (items keep their numbers) but stop dropping and doing anything
	void Unregister(const std::string& Owner, const std::string& Local = "")
	{
		bool changed = false;
		for (auto& d : Defs)
			if (d.Owner == Owner && (Local.empty() || d.Local == Local) && d.Active)
			{
				d.Active = false;
				try { WriteRow(d); } catch (const std::exception&) {}
				changed = true;
			}
		if (changed) { ++Version; SyncSubs(); RefreshHero(); }
	}

	Json List()
	{
		ReadEquipped();
		Json out = Json::Array();
		for (auto& d : Defs)
		{
			Json j = Json::Object();
			j.Add("id", d.Id);
			j.Add("ref", d.Ref);
			j.Add("name", d.Name);
			j.Add("slot", d.Slot);
			j.Add("owner", d.Owner);
			j.Add("active", d.Active);
			auto eq = Equipped.find(d.Ref);
			j.Add("equipped", eq != Equipped.end() ? Json(eq->second) : Json());
			out.Push(std::move(j));
		}
		return out;
	}

	Def& Find(const Json& Req)
	{
		std::string id = Req.GetStr("id");
		Def* d = ById(id);
		if (!d && !Req.GetStr("owner").empty()) d = ById(Req.GetStr("owner") + ":" + id);
		if (!d) Fail("no custom legendary '" + id + "'");
		return *d;
	}

	// a drop of that legendary at the hero (for testing and rewards): an equipment drop of its slot, made legendary
	Json Drop(const Json& Req)
	{
		Def& d = Find(Req);
		RValue hero = HeroInstance();
		if (KindOf(hero) == VALUE_UNDEFINED) Fail("no hero");
		CInstance* h = InstanceById(static_cast<int32_t>(hero.ToInt64()));
		RValue lootObj = AssetIndex("obj_loot");
		double roll = Req.GetNum("roll", 100);
		for (int tries = 0; tries < 80; ++tries)
		{
			std::set<int64_t> before;
			for (CInstance* l : InstancesOf(lootObj)) before.insert(InstanceId(l));
			RValue r;
			std::vector<RValue> args = { RValue(100.0), RValue(100.0), RValue(false), RValue(100000.0), RValue(100000.0), RValue(false),
				RValue(-1.0), RValue(false) };
			g_Yytk->CallGameScriptEx(r, "gml_Script_scr_create_loot_equipment", h, h, args);
			for (CInstance* l : InstancesOf(lootObj))
			{
				int64_t id = InstanceId(l);
				if (before.count(id)) continue;
				RValue ar = g_Yytk->CallBuiltin("variable_instance_get", { RValue(l), RValue("ar") });
				RValue* head = ArrayAt(ar, 0);
				RValue* slot = head ? ArrayAt(*head, 1) : nullptr;
				bool match = slot && KindOf(*slot) == VALUE_STRING && slot->ToString() == d.Slot;
				size_t line = SIZE_MAX;
				for (size_t k = 1, n = ArrayLen(ar); k < n && match; ++k)
					if (RValue* ln = ArrayAt(ar, k); ln && KindOf(*ln) == VALUE_ARRAY && ArrayAt(*ln, 0) && KindOf(*ArrayAt(*ln, 0)) == VALUE_STRING
						&& ArrayAt(*ln, 0)->ToString() == "L") line = k;
				if (!match || line == SIZE_MAX)
				{
					g_Yytk->CallBuiltin("instance_destroy", { RValue(l) });
					continue;
				}
				RValue* ln = ArrayAt(ar, line);
				*ArrayAt(*ln, 1) = RValue(static_cast<double>(d.Ref));
				*ArrayAt(*ln, 2) = RValue(roll);
				RValue name = g_Yytk->CallBuiltin("variable_instance_get", { RValue(l), RValue("loot_name") });
				g_Yytk->CallBuiltin("variable_instance_set", { RValue(l), RValue("loot_name"), RStr(" " + d.Name + " ") });
				g_Yytk->CallBuiltin("variable_instance_set", { RValue(l), RValue("baked"), RValue(false) });
				(void)name;
				return InstanceStub(l);
			}
		}
		Fail("could not make a drop of that slot");
	}
}

// ============================================================================
// Custom classes (the guide: docs/CLASSES.md): playable heroes from mods, numbered after the game's three -
// hero 3, 4, ... Every per-hero structure of the game gets their entries. The game's save never sees them: before
// each save they are taken out (and the saved current hero is a game hero), after it they are put back; their data
// lives in mods/aurie/SlormAPI/data/classes.json per save slot. Where the game switches on the hero number (0 / 1 / 2),
// a custom hero acts as its base hero.
// ============================================================================

namespace lua { void StatsRefreshed(CInstance* Hero); }    // (S.on_stats: after SlormAPI's own, below)
namespace classes
{
	struct Def
	{
		std::string Owner, Id, Name;
		std::string Title;                                   // in the menus, like "The Mighty Knight" (default: Name)
		std::string Weapon;                                  // its reapers' noun, like "Sword" (default: its base hero's)
		int Attribute = -1;                                  // the attribute a new hero starts with a point in (default:
		                                                     // its base hero's: Knight 0, Huntress 6, Mage 5)
		int Base = 0;
		bool Game = false;                                   // a game hero's content from mods (register_hero): Base = that hero
		bool Blank = false;                                  // base "none": the Knight only as the game's frame (BlankArt)
		double HubX = -1, HubY = -1;                         // its citizen's spot in the hub (-1: next to its base hero's)
		bool AutoSpot = false;                               // that spot was chosen by SlormAPI
		int Cid = -1;                                        // its citizen (a row added to global.citizen)
		int CidBase = -1;                                    // the base hero its citizen's row was made from
		std::map<int, int> Sprites;                          // base hero's sprite -> the class's, drawn in its place
		std::map<int, int> SpritesBack;                      // a sprite of a look it had -> the game's (a skin picked: back)
		int Reapers = -1;                                    // its reapers' strip (its base hero's, in its look)
		int ReapersAncient = -1;                             // its primordial reapers' (the base's spr_reapers_*_corrupted)
		std::map<int, int> Art;                              // "art": its own sprites, game sprite -> its sprite
		std::vector<int> Made;                               // sprites made for its look as built (deleted when built again)
		std::string Talk;                                    // its citizen's line in the hub (default: "Shall we let ...")
		Json NameText, TitleText, TalkText;                  // name, title, talk as given: a text, or one per language
		// "reapers": its own version of game reapers - their cells (texts, stats, damage, levels, evolution, drops) while it is
		// played, their art in its strips, and with code "own" the game's scripted effect of that reaper off (its Lua's instead)
		struct ReaperCell { size_t Ref, Col; Json Mine; };
		std::vector<ReaperCell> ReaperCells;
		std::map<int, int> ReaperArt, ReaperArtPrimordial;   // frame of the reaper strips -> its sprite
		std::set<int> ReaperOwnCode;
		struct ReaperStat { std::string Stat; double Value; bool PerLevel; };
		std::map<int, std::vector<ReaperStat>> ReaperStats;  // an own-code reaper's stat lines (applied by SlormAPI)
		std::map<std::string, double> Sounds;                // "sounds": its hero's sound for a game one (-1: none, -2: the game's)
		std::set<std::string> Silenced;                      // (a blank base: the game's sounds its hero played none of, logged)
		// its skins ("skins": [{id, name, art, look}]): other looks, picked in the Character menu; its own art and look
		// as registered (a skin's go over them), and the skin its look was built with ("\x01": not yet)
		Json Skins = Json::Array();
		Json SkinDefault;                                    // "skin_name": the name of its own look in the list
		std::map<int, int> BaseArt;
		std::string SkinShown = "\x01";
		// its achievements, a category of its own in the Profile: its base hero's (mirrored: the game's count on its data),
		// then its own ("achievements": [{id, name, description ("$": the step), icon, steps, stat}])
		struct Achievement
		{
			std::string Id;
			int Source = -1;                                 // mirrored: the base hero's row it is counted like
			std::string Stat;                                // its own: a player stat of its hero, else its value (set by Lua)
			std::vector<double> Steps;
			RValue Row;                                      // its row (global.achievements' columns)
			double Progress[4] = { 0, 1, 0, 1 };             // [steps done, steps, value, next step], counted by Tick (numbers:
			                                                 // an array kept here only would be freed by the game's GC)
		};
		std::vector<Achievement> Achievements;
		bool AchievementsMirror = true;                      // achievements_base "none": its own only
		int CategoryIcon = -1;                               // its category's icon (its art, or its base hero's in its look)
		bool AchDirty = true;                                // its achievements to count (played: always)
		Json AchievementDefs = Json::Array();
		bool TalkSelf = false;                               // said by itself (its name, its portrait), not by Bryan
		struct Swap { int Sprite, Orig, Mine; };
		std::vector<Swap> Swaps;                             // art the game draws by name (spec art, banners): the class's
		                                                     // assigned into the game's sprite while it is played
		Json Skills = Json::Object();                        // {"<row>": {name, description, <dat_cla field>: value}}
		// kit "own": its own skill table (skills 0-10, then Entries, then the class / mechanic / spec rows with Texts) and
		// its own skill code (events class_cast / class_hit); else its base hero's skills and casts
		bool Own = false;
		Json Entries = Json::Array();                        // [{skill, tier, kind, name, description, max, ...}]
		Json Texts = Json::Object();                         // {"class" | "mechanics" | "mechanic" | "spec": [{name, description}]}
		RValue Table;                                        // its table as built
		std::map<std::pair<size_t, size_t>, RValue> Pristine;   // the cells entry effects change, as built
		std::string Passives;                                // entries that give hero stats: "12,15,..." (scr_skill_refresh_passives)
		struct Effect { int Entry; size_t Row, Col, Index; double Percent, Add; };
		std::vector<Effect> Effects;                         // per rank of an entry: a skill row's cell changed
		int Icons = -1;                                      // its skill icons (one frame per row of its table)
		// its Ancestral Legacies: the game's table (global.elements, shared by all heroes) with its own legacies after it,
		// swapped in while it is played; the game's legacies in the realms it fills (all of them with LegacyBaseHidden)
		// are hidden
		Json Legacies = Json::Array();                       // [{id, realm, kind, name, description, stat, value, ...}]
		std::vector<std::pair<std::string, int>> LegacyFrom; // each one's mod (owner) and its place in that mod's list
		bool LegacyBaseHidden = false;
		bool LegaciesRestored = false;                       // a game hero's: its saved mod legacies put back (KeepGameLegacies)
		std::set<int> HiddenLegacies;                        // the game's legacies its tree hides
		std::set<int> AurasOn;                               // its aura legacies toggled on (rows)
		std::set<std::string> AurasPending;                  // a game hero's: toggled on before its mods' parts changed (by
															 // LegacyKey), on again once its table is built (ApplyPendingAuras)
		bool LegsLogged = false;
		RValue Elements;                                     // its legacy table, built when first played
		std::string ElementPassives;                         // its stat legacies (",150,151,...")
		int ElementIcons = -1;                               // spr_elements with its icons after the game's
		// its story missions, after the game's in the story table while it is played ("its story missions", below)
		Json Missions = Json::Array();                       // [{id, name, description, map, act, objective, level, after, ...}]
		std::vector<std::string> MissionFrom;                // each one's mod (owner)
		std::vector<std::pair<double, double>> MissionPos;   // their nodes on their act's map, as built
		std::vector<double> MissionRuns;                     // its hero's runs of each
		std::vector<double> MissionSeen;                     // ... as announced ("mission_done"; kept around saves)
		std::map<int, int> ActArt;                           // an act's map sprites with a frame for each: game -> its
		// its own story area ("area": {name, map, rooms, entrance, hub_x, hub_y, act}): an entrance of its own in the hub,
		// opening an act page that shows its art and its missions with "area"
		Json Area;
		int AreaMap = -1, AreaRooms = -1, AreaDoor = -1;     // its sprites: the page's map, its missions' rooms, the entrance
		int AreaStrip = -1;                                  // its rooms as the page's strip (one frame per area mission)
		int AreaEntrance = -1;                               // its entrance in the hub (an instance)
		// its mechanic on the HUD (class_meter): value of max, "lit" while it is unleashed; -1: none
		double MeterValue = -1, MeterMax = 10;
		bool MeterLit = false;
		std::map<int, int> MeterArt;                         // its base hero's widget sprites as drawn (its art, else the game's)
		std::string Key() const { return Owner + ":" + Id; }
	};
	std::vector<Def> Defs;                                   // hero 3 + i
	int ContentVer = 0;                                      // +1 at each content change (Lua's cached rows asked again)
	// the game's heroes' content from mods (register_hero: art, skins, reapers, sounds, achievements, missions, legacies),
	// one Def per hero, made of every mod's part; Used: something given
	Def GameDefs[3];
	bool GameUsed[3] = {};
	// a content's hero number: a class's 3 + its index, a game hero's own
	int HeroOf(const Def& D) { return D.Game ? D.Base : 3 + static_cast<int>(&D - Defs.data()); }
	// the content of a hero number: a class's Def, a game hero's (when a mod gave it anything), else none
	Def* ContentOf(int Hero)
	{
		if (Hero >= 0 && Hero < 3) return GameUsed[Hero] ? &GameDefs[Hero] : nullptr;
		return Hero >= 3 && static_cast<size_t>(Hero) - 3 < Defs.size() ? &Defs[static_cast<size_t>(Hero) - 3] : nullptr;
	}
	bool AnyContent() { return !Defs.empty() || GameUsed[0] || GameUsed[1] || GameUsed[2]; }
	bool Saving = false;                                         // inside the game's save (PreSave .. PostSave)
	// every content: the classes', then the game heroes' that have any
	template <class F> void ForEachContent(F&& Fn)
	{
		for (auto& d : Defs) Fn(d);
		for (int h = 0; h < 3; ++h) if (GameUsed[h]) Fn(GameDefs[h]);
	}
	// a content by its key: a class's "owner:id", a game hero's "hero:<n>"
	Def* ByKey(const std::string& Key)
	{
		Def* out = nullptr;
		ForEachContent([&](Def& d) { if (!out && d.Key() == Key) out = &d; });
		return out;
	}
	std::string LangText(const Json* V, size_t Lang);
	size_t CurrentLang();
	// its name / title (in the menus; default its name) in a language
	std::string ClassName(const Def& D, size_t Lang) { std::string t = LangText(&D.NameText, Lang); return t.empty() ? D.Name : t; }
	std::string ClassTitle(const Def& D, size_t Lang) { std::string t = LangText(&D.TitleText, Lang); return t.empty() ? ClassName(D, Lang) : t; }
	// its talk in a language: its pages joined ("a/b"; given as a text, a list of pages, or one of those per language)
	const Json* LangPick(const Json* V, size_t Lang);
	std::string TalkPages(const Def& D, size_t Lang)
	{
		const Json* t = LangPick(&D.TalkText, Lang);
		if (!t) return "";
		if (t->type == Json::Str) return t->s;
		std::string out;
		if (t->type == Json::Arr)
			for (auto& page : t->a)
				if (page.type == Json::Str) out += (out.empty() ? "" : "/\n") + page.s;
		return out;
	}
	extern size_t AchGameRows;                               // (achievements, below)
	Json& AchData(const Def& D, const char* Group);           // (its save data's groups, below)
	void SkinClick(const std::string& What);                  // (its skins, below)
	extern int AreaNaming;                                    // (its story area, below)
	void AreaEntranceEvent(CInstance* Self, bool Enter);
	bool AreaString(const std::string& Key, RValue& Result);
	void AreaViewOff();
	void AreaFrame();

	// the game's per-hero structures (globals, and variables of obj_ui_next_gen), indexed by hero. Not
	// slorm_temple_blessing: 3 long too, but [blessing][upgrade] (the account's, bought at the Mage)
	const char* const kGlobals[] = { "attack_clicks", "aura_array", "battlefield_environment", "battlefield_journey",
		"current_weapon", "debug_skills", "elements_equipped", "elements_rank", "elements_realms", "essence",
		"fetched_stats", "inventory", "level", "loadouts", "loot_filter", "match_level", "mission_data",
		"necromancy_summons", "player_stats", "reaper_pity", "reapers_data", "selected_enemy_level", "selected_game_mode",
		"selected_torment", "selected_torment_plus", "sheet_name", "skill_equipped", "skill_rank", "skills", "skin_data",
		"sprite_class", "sprite_skills", "sprite_skills_los", "sprite_skills_support", "store_equipment_list", "traits",
		"warlords_account", "warlords_data", "xp" };
	const char* const kUi[] = { "temp_elems_realms_equipped", "temp_equipped", "temp_elems", "temp_elems_equipped",
		"temp_skills" };
	const char* const kColumns[] = { "runes", "ultimatums" };   // rows [owned, level, equipped by hero 0, 1, 2, ...]
	constexpr size_t kColumn0 = 2;
	constexpr size_t kReaperBlock = 240;                     // obj_ui_next_gen.weapons_discovered: 240 per hero
	// copied from the base hero (what the class is, not its progress)
	const std::set<std::string> kFromBase = { "sheet_name", "sprite_class", "sprite_skills", "sprite_skills_los",
		"sprite_skills_support", "skills", "debug_skills" };

	Json Fresh;                                               // FreshHero.hpp, parsed
	int WarlordsChecked = -1;                                 // the class whose Warlords' rewards were checked (in place)
	std::map<std::string, Json> SlotData;                     // this save slot's data per class key (from classes.json)
	std::map<std::string, Json> AllSlots;                     // classes.json "slots"
	bool FileLoaded = false, Hooked = false;
	int Depth = 0, SwappedAt = 0, Saved = -1;
	std::string ActiveKey;                                    // the custom class being played ("" = a game hero)
	bool Loading = false;                                     // inside scr_save_load (it saves too, half loaded)
	int PendingHero = -1;                                     // after a load: the custom hero the save was played with
	int ResumeHero = -1;                                      // registered after the load: switched to (Resume)
	void SyncElements();
	void SyncMissions(bool Out = false);
	RValue LookSlot(const Def& D, const RValue& Base);

	size_t Wanted() { return 3 + Defs.size(); }
	fs::path DataFile() { return AurieDir() / "SlormAPI" / "data" / "classes.json"; }
	int SaveSlot() { RValue* v = FindMember(GlobalRValue(), "save_file"); return v && IsNumeric(*v) ? static_cast<int>(v->ToDouble()) : -1; }
	RValue* CurrentHero() { return FindMember(GlobalRValue(), "current_hero"); }
	RValue UiValue() { RValue ui = modspage::UiInstance(); return KindOf(ui) == VALUE_UNDEFINED ? RValue() : RValue(InstanceById(static_cast<int32_t>(ui.ToInt64()))); }

	// {"$fill": v, "$n": n} -> n copies of v
	Json Expand(const Json& J)
	{
		if (J.type == Json::Obj)
			if (const Json* f = J.Find("$fill"))
			{
				Json a = Json::Array();
				Json one = Expand(*f);
				for (int k = 0, n = static_cast<int>(J.GetNum("$n", 0)); k < n; ++k) a.Push(one);
				return a;
			}
		if (J.type == Json::Arr)
		{
			Json a = Json::Array();
			for (auto& e : J.a) a.Push(Expand(e));
			return a;
		}
		return J;
	}

	const Json& FreshData()
	{
		if (Fresh.type != Json::Obj)
		{
			std::string text;
			for (const char* part : kFreshHeroParts) text += part;
			Fresh = sj::Parse(text);
		}
		return Fresh;
	}

	void LoadFile()
	{
		if (FileLoaded) return;
		FileLoaded = true;
		std::ifstream f(DataFile(), std::ios::binary);
		if (!f) return;
		std::stringstream ss;
		ss << f.rdbuf();
		try
		{
			Json j = sj::Parse(ss.str());
			if (const Json* s = j.Find("slots"); s && s->type == Json::Obj)
				for (auto& [k, v] : s->o) AllSlots[k] = v;
		}
		catch (const std::exception& e)
		{
			// unreadable: kept beside it (the next write makes a new file; the custom heroes' data stays in the copy)
			fs::path bad = DataFile();
			bad += ".bad-" + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()).count());
			std::error_code ec;
			fs::copy_file(DataFile(), bad, fs::copy_options::skip_existing, ec);
			DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormAPI] classes.json unreadable (%s): kept as %s", e.what(), bad.filename().string().c_str());
		}
	}

	void WriteFile()
	{
		Json slots = Json::Object();
		for (auto& [k, v] : AllSlots) slots.Add(k, v);
		Json j = Json::Object();
		j.Add("version", 1);
		j.Add("slots", std::move(slots));
		std::error_code ec;
		fs::create_directories(DataFile().parent_path(), ec);
		fs::path tmp = DataFile();
		tmp += ".tmp";
		{
			std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
			o << sj::Dump(j);
			if (!o) return;
		}
		fs::rename(tmp, DataFile(), ec);
	}

	// the slot's saved data of class i (null when it has none: a new hero)
	const Json* Saved_(size_t I, const char* Group, const std::string& Name)
	{
		auto it = SlotData.find(Defs[I].Key());
		if (it == SlotData.end()) return nullptr;
		const Json* g = it->second.Find(Group);
		return g ? g->Find(Name) : nullptr;
	}

	// ---- the class's skill table: its base hero's (rows like dat_cla_<n>.json), with its own names, numbers, damage
	// columns of a row in memory, by dat_cla field (found by matching the JSON rows against the game's table)
	const std::map<std::string, size_t> kSkillColumns = { { "DESC_VALUE", 3 }, { "DESC_VALUE_BASE", 4 },
		{ "DESC_VALUE_PER_LVL", 5 }, { "DESC_VALUE_TYPE", 6 }, { "DESC_VALUE_REAL", 7 }, { "GENRE", 8 }, { "UNLOCK_LEVEL", 9 },
		{ "UPGRADE_NUMBER", 10 }, { "COST", 11 }, { "COST_LEVEL", 12 }, { "COST_TYPE", 13 }, { "TYPE", 14 }, { "ACTIVE_BOX", 15 },
		{ "DMG_TYPE", 16 }, { "SLORM_TIER", 19 }, { "COOLDOWN", 20 }, { "POSE_TYPE", 21 }, { "PRECAST_TIME", 22 },
		{ "PRECAST_SPEED", 23 }, { "CAST_TIME", 24 }, { "CAST_SPEED", 25 }, { "KNOCKBACK", 26 }, { "ORDER", 27 },
		{ "EXTRA_NBR", 29 }, { "AUTO_CD", 30 } };
	constexpr size_t kSkillNameColumn = 32;                  // name, description, then the same per language

	// the game's languages, in the order of their columns in its tables, and as global.lang names them
	const char* const kLangs[] = { "en", "fr", "ch", "tw", "jp", "de", "es", "ru", "kr", "it", "pt", "po", "tur" };
	// an option for one language: the option itself, or - an object of languages {en = ..., fr = ...} - its language's
	// (missing: English, else the first given); also "zh", "ja", "ko", "pl", "tr"
	const Json* LangPick(const Json* V, size_t Lang)
	{
		if (!V || V->type != Json::Obj) return V;
		static const std::map<std::string, std::string> alias = { { "zh", "ch" }, { "ja", "jp" }, { "ko", "kr" }, { "pl", "po" }, { "tr", "tur" } };
		auto code = [](std::string K) { K = Lower(K); auto a = alias.find(K); return a != alias.end() ? a->second : K; };
		bool any = false;
		for (auto& [k, v] : V->o)
		{
			std::string c = code(k);
			for (const char* l : kLangs) any |= c == l;
		}
		if (!any) return V;                                        // (not an object of languages)
		const Json* en = nullptr, *first = nullptr;
		for (auto& [k, v] : V->o)
		{
			std::string c = code(k);
			if (Lang < std::size(kLangs) && c == kLangs[Lang]) return &v;
			if (c == "en") en = &v;
			if (!first) first = &v;
		}
		return en ? en : first;
	}
	std::string LangText(const Json* V, size_t Lang)
	{
		const Json* t = LangPick(V, Lang);
		return t && t->type == Json::Str ? t->s : "";
	}
	size_t CurrentLang()
	{
		RValue* l = FindMember(GlobalRValue(), "lang");
		std::string code = l && KindOf(*l) == VALUE_STRING ? Lower(l->ToString()) : "en";
		for (size_t i = 0; i < std::size(kLangs); ++i) if (code == kLangs[i]) return i;
		return 0;
	}

	// a value for a cell, of the cell's type (numbers stay numbers, texts texts)
	RValue CellValue(const RValue& Old, const Json& V)
	{
		if (KindOf(Old) == VALUE_STRING) return V.type == Json::Str ? RStr(V.s) : RStr(sj::Dump(V));
		if (IsNumeric(Old) && V.type == Json::Str) { try { return RValue(std::stod(V.s)); } catch (...) {} }
		return FromJson(V);
	}

	RValue OwnTable(Def& D, const RValue& BaseTable);
	void SetTexts(RValue& Row, const Json& O, size_t First);

	RValue ClassTable(Def& D, const RValue& BaseTable)
	{
		if (D.Own)
		{
			RValue table = OwnTable(D, BaseTable);
			D.Table = FromJson(ToJson(table, 4));                // as built, apart from the live one (effects change that)
			return table;
		}
		RValue table = FromJson(ToJson(BaseTable, 4));      // its own copy
		for (size_t r = 0, n = ArrayLen(table); r < n; ++r)
		{
			RValue* row = ArrayAt(table, r);
			if (!row || KindOf(*row) != VALUE_ARRAY) continue;
			const Json* o = D.Skills.Find(std::to_string(r));
			if (!o || o->type != Json::Obj) continue;
			SetTexts(*row, *o, kSkillNameColumn);                // (a text, or one per language)
			for (auto& [k, v] : o->o)
			{
				auto col = kSkillColumns.find(k);
				if (col == kSkillColumns.end()) continue;
				if (RValue* cell = ArrayAt(*row, col->second)) *cell = CellValue(*cell, v);
			}
		}
		return table;
	}

	// ---- kit "own": the class's table from the mod's rows
	// field names a mod may use for a row, besides the dat_cla ones (kSkillColumns)
	const std::map<std::string, std::string> kRowFields = { { "skill", "ACTIVE_BOX" }, { "tier", "UNLOCK_LEVEL" },
		{ "max", "UPGRADE_NUMBER" }, { "order", "ORDER" }, { "kind", "TYPE" }, { "genre", "GENRE" }, { "stat", "DESC_VALUE" },
		{ "value", "DESC_VALUE_BASE" }, { "per_rank", "DESC_VALUE_PER_LVL" }, { "per_level", "DESC_VALUE_PER_LVL" },
		{ "value_type", "DESC_VALUE_TYPE" }, { "real", "DESC_VALUE_REAL" }, { "cost", "COST" }, { "cost_per_rank", "COST_LEVEL" },
		{ "cost_type", "COST_TYPE" }, { "dmg_type", "DMG_TYPE" }, { "cooldown", "COOLDOWN" }, { "extra", "EXTRA_NBR" },
		{ "precast", "PRECAST_TIME" }, { "cast_time", "CAST_TIME" }, { "pose", "POSE_TYPE" }, { "knockback", "KNOCKBACK" },
		{ "precast_speed", "PRECAST_SPEED" }, { "cast_speed", "CAST_SPEED" }, { "auto_cd", "AUTO_CD" }, { "unlock", "UNLOCK_LEVEL" } };
	constexpr size_t kRefColumn = 0, kRefColumn2 = 28, kMasteryColumn = 31, kTypeColumn = 14, kBoxColumn = 15, kTierColumn = 9,
		kOrderColumn = 27;
	const char* const kSpecialTypes[] = { "class", "mechanics", "mechanic", "spec" };

	std::string CellText(const RValue& V) { return KindOf(V) == VALUE_STRING ? V.ToString() : (IsNumeric(V) ? std::to_string(V.ToDouble()) : ""); }

	// a row's name and description in every language
	void SetTexts(RValue& Row, const Json& O, size_t First = kSkillNameColumn)
	{
		size_t len = ArrayLen(Row);
		for (auto [key, c0] : { std::pair{ "name", First }, std::pair{ "description", First + 1 } })
			if (const Json* v = O.Find(key); v && (v->type == Json::Str || v->type == Json::Obj))
				for (size_t c = c0; c < len; c += 2)
					if (RValue* cell = ArrayAt(Row, c); cell && KindOf(*cell) == VALUE_STRING) *cell = RStr(LangText(v, (c - c0) / 2));
	}

	// a row's fields from a mod's object ({name, description, skill, tier, ..., or dat_cla names})
	void SetFields(RValue& Row, const Json& O)
	{
		SetTexts(Row, O);
		for (auto& [k, v] : O.o)
		{
			auto f = kRowFields.find(k);
			auto col = kSkillColumns.find(f != kRowFields.end() ? f->second : k);
			size_t c = col != kSkillColumns.end() ? col->second : (k == "mastery" ? kMasteryColumn : SIZE_MAX);
			if (c == SIZE_MAX) continue;
			Json val = v;
			if (v.type == Json::Arr)                             // {a, b} -> "a|b" (the table's multi-value cells)
			{
				std::string s;
				for (size_t i = 0; i < v.a.size(); ++i) s += (i ? "|" : "") + (v.a[i].type == Json::Str ? v.a[i].s : sj::Dump(v.a[i]));
				val = Json(s);
			}
			if (RValue* cell = ArrayAt(Row, c)) *cell = CellValue(*cell, val);
		}
	}

	// an empty cell of the same kind (number 0 or "")
	void Blank(RValue& Row, size_t C)
	{
		if (RValue* cell = ArrayAt(Row, C)) *cell = KindOf(*cell) == VALUE_STRING ? RStr("") : RValue(0.0);
	}

	// a row with values has one stat key, value type and per-rank value per value ("a|b"), as every game row does (an
	// entry with values and no stat key crashed the game): missing ones repeat the last, or a default
	void AlignValues(RValue& Row, size_t Stat = 3)          // stat keys at Stat, then values, per-rank values, value types
	{
		auto split = [&](size_t C)
		{
			std::vector<std::string> p;
			RValue* cell = ArrayAt(Row, C);
			std::string s = cell ? CellText(*cell) : "";
			if (cell && IsNumeric(*cell) && KindOf(*cell) != VALUE_STRING)
			{
				char buf[64];
				snprintf(buf, sizeof buf, "%g", cell->ToDouble());
				s = buf;
			}
			if (s.empty()) return p;
			for (size_t a = 0;;) { size_t b = s.find('|', a); p.push_back(s.substr(a, b - a)); if (b == std::string::npos) break; a = b + 1; }
			return p;
		};
		size_t n = split(Stat + 1).size();
		if (!n) return;
		for (auto [c, def] : { std::pair{ Stat, "damage" }, std::pair{ Stat + 2, "0" }, std::pair{ Stat + 3, "" } })
		{
			std::vector<std::string> p = split(c);
			if (p.size() == n) continue;
			while (p.size() < n) p.push_back(p.empty() || c == Stat + 3 ? def : p.back());
			p.resize(n);
			std::string out;
			for (size_t i = 0; i < n; ++i) out += (i ? "|" : "") + p[i];
			if (RValue* cell = ArrayAt(Row, c)) *cell = RStr(out);
		}
	}

	// the class's table, as long as its base hero's and more: game code checks the base hero's entry numbers on any
	// hero's table (e.g. scr_health_lose -> row 190), so rows 11 .. n-1 stay - its base hero's class / mechanic / spec
	// rows there get the mod's Texts ("{Name}" in a spec's mechanic list = the row number of the special row named
	// Name), every other one is an inert filler (type "none": in no tree, never equipped; its skill stays, the game's
	// entry check reads that skill's mastery first) - and its own entries come after them (rows n+, from a template
	// upgrade / passive row). REF = row number; required mastery per tier.
	constexpr const char* kFillerType = "none";
	constexpr double kFillerLevel = 9999;
	RValue OwnTable(Def& D, const RValue& BaseTable)
	{
		RValue base = FromJson(ToJson(BaseTable, 4));
		size_t n = ArrayLen(base);
		std::map<std::string, const RValue*> tpl;                // first base row of each type
		for (size_t r = 11; r < n; ++r)
			if (RValue* row = ArrayAt(base, r); row && KindOf(*row) == VALUE_ARRAY)
				if (RValue* t = ArrayAt(*row, kTypeColumn); t && KindOf(*t) == VALUE_STRING && !tpl.count(t->ToString())) tpl[t->ToString()] = row;
		auto copyOf = [](const RValue& R) { return FromJson(ToJson(R, 2)); };
		auto typeOf = [](const RValue& R) { const RValue* t = ArrayAt(const_cast<RValue&>(R), kTypeColumn); return t && KindOf(*t) == VALUE_STRING ? t->ToString() : std::string(); };
		std::vector<RValue> rows;
		Json none = Json::Object();                              // (a blank base: its template's texts out)
		none.Set("name", std::string());
		none.Set("description", std::string());
		for (size_t k = 0; k < 11 && k < n; ++k)
		{
			RValue row = copyOf(*ArrayAt(base, k));
			if (D.Blank) SetTexts(row, none);
			if (const Json* o = D.Skills.Find(std::to_string(k)); o && o->type == Json::Obj) { SetFields(row, *o); AlignValues(row); }
			rows.push_back(row);
		}
		std::map<std::string, size_t> named;                     // special row name -> row number (for "{Name}")
		std::vector<size_t> specs;                                // spec rows (their mechanic lists are filled in after)
		std::map<std::string, size_t> nth;                       // special type -> rows of it so far
		for (size_t r = 11; r < n; ++r)
		{
			RValue row = copyOf(*ArrayAt(base, r));
			std::string type = typeOf(row);
			bool special = std::find_if(std::begin(kSpecialTypes), std::end(kSpecialTypes), [&](const char* t) { return type == t; }) != std::end(kSpecialTypes);
			if (special)
			{
				const Json* list = D.Texts.Find(type);
				size_t k = nth[type]++;
				if (D.Blank) SetTexts(row, none);
				if (list && list->type == Json::Arr && k < list->a.size() && list->a[k].type == Json::Obj) SetTexts(row, list->a[k]);
				if (RValue* nm = ArrayAt(row, kSkillNameColumn)) named[CellText(*nm)] = r;
				if (type == "spec") specs.push_back(r);
			}
			else if (RValue* t = ArrayAt(row, kTypeColumn))
			{
				*t = RStr(kFillerType);
				// at a level no hero reaches: a level-up names every row unlocked at the new level but upgrades and
				// passives as "New Active Skill Unlocked" (scr_get_level) - its base hero's names, for a filler
				if (RValue* tier = ArrayAt(row, kTierColumn)) *tier = RValue(kFillerLevel);
			}
			rows.push_back(row);
		}
		D.Passives.clear();
		D.Effects.clear();
		std::map<std::pair<int, int>, int> inTier;               // (skill, tier) -> entries so far
		for (auto& e : D.Entries.a)
		{
			if (e.type != Json::Obj) continue;
			std::string kind = e.GetStr("kind", "upgrade");
			auto t = tpl.find(kind);
			if (t == tpl.end()) t = tpl.find("upgrade");
			if (t == tpl.end()) break;
			RValue row = copyOf(*t->second);
			for (size_t c : { 3, 4, 5, 6, 7, 8, 11, 12, 29 }) Blank(row, c);
			int id = static_cast<int>(rows.size());
			int skill = static_cast<int>(e.GetNum("skill", 3)), tier = static_cast<int>(e.GetNum("tier", 1));
			int k = inTier[{ skill, tier }]++;
			if (RValue* order = ArrayAt(row, kOrderColumn)) *order = RValue(static_cast<double>(k + 1));   // its place in the tier (0 crashes the menu)
			if (RValue* cell = ArrayAt(row, kMasteryColumn))     // supports: the tier; actives: 2 x tier - 1, then 2 x tier
				*cell = RValue(static_cast<double>(skill <= 2 ? tier : 2 * tier - (k == 0 ? 1 : 0)));
			SetFields(row, e);
			AlignValues(row);
			if (e.GetBool("hero_stat", false)) D.Passives += (D.Passives.empty() ? "" : ",") + std::to_string(id);
			if (const Json* fx = e.Find("effects"); fx && fx->type == Json::Arr)   // [{row, field, index, percent, add}] per rank
				for (auto& f : fx->a)
				{
					auto fl = kRowFields.find(f.GetStr("field", "value"));
					auto col = kSkillColumns.find(fl != kRowFields.end() ? fl->second : f.GetStr("field"));
					if (col == kSkillColumns.end()) continue;
					D.Effects.push_back({ id, static_cast<size_t>(f.GetNum("row", skill)), col->second,
						static_cast<size_t>(f.GetNum("index", 0)), f.GetNum("percent", 0), f.GetNum("add", 0) });
				}
			rows.push_back(row);
		}
		for (size_t at : specs)                                  // "{Name}" -> its row number, in every language
			for (size_t c = kSkillNameColumn + 1, n = ArrayLen(rows[at]); c < n; c += 2)
			{
				RValue* d = ArrayAt(rows[at], c);
				if (!d || KindOf(*d) != VALUE_STRING) continue;
				std::string text = d->ToString(), out;
				for (size_t p = 0; p < text.size();)
				{
					size_t a = text.find('{', p), b = a == std::string::npos ? a : text.find('}', a);
					if (b == std::string::npos) { out += text.substr(p); break; }
					auto it = named.find(text.substr(a + 1, b - a - 1));
					out += text.substr(p, a - p) + (it != named.end() ? std::to_string(it->second) : text.substr(a, b - a + 1));
					p = b + 1;
				}
				if (out != text) *d = RStr(out);
			}
		for (size_t r = 0; r < rows.size(); ++r)
			for (size_t c : { kRefColumn, kRefColumn2 })
				if (RValue* cell = ArrayAt(rows[r], c); cell && IsNumeric(*cell)) *cell = RValue(static_cast<double>(r));
		D.Pristine.clear();
		for (auto& e : D.Effects)
			if (e.Row < rows.size())
				if (RValue* cell = ArrayAt(rows[e.Row], e.Col)) D.Pristine[{ e.Row, e.Col }] = *cell;
		return RValue(rows);
	}

	// hero 3 + I's value of a per-hero structure: saved, else a never-played hero's, else its base hero's
	RValue NewEntry(size_t I, const char* Group, const std::string& Name, RValue& Arr)
	{
		int base = Defs[I].Base;
		if (Name == "store_equipment_list")                   // a ds_list per hero: its own, new
			return g_Yytk->CallBuiltin("ds_list_create", {});
		if (!kFromBase.count(Name))
		{
			if (const Json* s = Saved_(I, Group, Name)) return FromJson(*s);
			const Json* heroes = FreshData().Find("heroes");
			const Json* g = heroes && base < static_cast<int>(heroes->a.size()) ? heroes->a[base].Find(Group) : nullptr;
			if (const Json* f = g ? g->Find(Name) : nullptr)
			{
				Json fresh = Expand(*f);
				if (Name == "traits" && Defs[I].Attribute >= 0 && fresh.type == Json::Arr && static_cast<size_t>(Defs[I].Attribute) < fresh.a.size())
					for (size_t k = 0; k < fresh.a.size(); ++k) fresh.a[k] = Json(k == static_cast<size_t>(Defs[I].Attribute) ? 1.0 : 0.0);
				return FromJson(fresh);
			}
		}
		RValue* b = ArrayAt(Arr, static_cast<size_t>(base));
		if (b && Name == "skills") return ClassTable(Defs[I], *b);
		if (Name == "sprite_skills" && Defs[I].Icons >= 0) return RValue(static_cast<double>(Defs[I].Icons));
		if (b && Name.starts_with("sprite_")) return LookSlot(Defs[I], *b);
		return b ? FromJson(ToJson(*b, 32)) : RValue();          // a copy, not shared with the base hero
	}

	// class I's per-row arrays (masteries and ranks, equipped) as long as its table (an own table is longer than its
	// base hero's)
	void PadRows(size_t I)
	{
		RValue* skills = FindMember(GlobalRValue(), "skills");
		RValue* table = skills ? ArrayAt(*skills, 3 + I) : nullptr;
		size_t rows = table && KindOf(*table) == VALUE_ARRAY ? ArrayLen(*table) : 0;
		auto pad = [&](RValue* All)
		{
			RValue* mine = All && KindOf(*All) == VALUE_ARRAY ? ArrayAt(*All, 3 + I) : nullptr;
			if (!mine || KindOf(*mine) != VALUE_ARRAY || ArrayLen(*mine) >= rows) return;
			std::vector<RValue> items;
			for (size_t k = 0, n = ArrayLen(*mine); k < n; ++k) items.push_back(*ArrayAt(*mine, k));
			while (items.size() < rows) items.push_back(RValue(0.0));
			*mine = RValue(items);
		};
		pad(FindMember(GlobalRValue(), "skill_rank"));
		pad(FindMember(GlobalRValue(), "skill_equipped"));
		RValue ui = UiValue();
		if (KindOf(ui) != VALUE_UNDEFINED) { pad(FindMember(ui, "temp_skills")); pad(FindMember(ui, "temp_equipped")); }
		// a filler is never taken (picks from an earlier kit of the class would bring back its base hero's entries)
		RValue* equipped = FindMember(GlobalRValue(), "skill_equipped");
		RValue* mine = equipped && KindOf(*equipped) == VALUE_ARRAY ? ArrayAt(*equipped, 3 + I) : nullptr;
		for (size_t r = 11; mine && KindOf(*mine) == VALUE_ARRAY && r < rows; ++r)
			if (RValue* row = ArrayAt(*table, r); row && KindOf(*row) == VALUE_ARRAY)
				if (RValue* t = ArrayAt(*row, kTypeColumn); t && KindOf(*t) == VALUE_STRING && t->ToString() == kFillerType)
					if (RValue* e = ArrayAt(*mine, r); e && IsNumeric(*e) && e->ToDouble() != 0) *e = RValue(0.0);
	}

	// gives every per-hero structure its entries for the custom heroes (no-op when they are there; Force: made anew
	// from the slot's data, after a load - the game may have written its defaults there)
	void Extend(bool Force = false)
	{
		if (Defs.empty()) return;
		size_t want = Wanted();
		auto grow = [&](RValue* Arr, const char* Group, const std::string& Name)
		{
			if (!Arr || KindOf(*Arr) != VALUE_ARRAY) return;
			size_t n = ArrayLen(*Arr);
			if ((n >= want && !Force) || n < 3) return;
			size_t keep = Force ? 3 : n;
			std::vector<RValue> items;
			for (size_t k = 0; k < keep; ++k) items.push_back(*ArrayAt(*Arr, k));
			for (size_t h = keep; h < want; ++h)
			{
				if (Force && Name == "store_equipment_list" && h < n) items.push_back(*ArrayAt(*Arr, h));   // keep its ds_list
				else items.push_back(NewEntry(h - 3, Group, Name, *Arr));
			}
			*Arr = RValue(items);
		};
		RValue global = GlobalRValue();
		for (const char* name : kGlobals) grow(FindMember(global, name), "globals", name);
		RValue ui = UiValue();
		if (KindOf(ui) != VALUE_UNDEFINED)
		{
			for (const char* name : kUi) grow(FindMember(ui, name), "ui", name);
			if (RValue* wd = FindMember(ui, "weapons_discovered"); wd && KindOf(*wd) == VALUE_ARRAY)
			{
				size_t n = ArrayLen(*wd);
				if ((n < want * kReaperBlock || Force) && n >= 3 * kReaperBlock)
				{
					if (Force) n = 3 * kReaperBlock;
					std::vector<RValue> items;
					for (size_t k = 0; k < n; ++k) items.push_back(*ArrayAt(*wd, k));
					Json fresh = Expand(*FreshData().Find("weapons_discovered"));
					for (size_t h = n / kReaperBlock; h < want; ++h)
					{
						const Json* s = Saved_(h - 3, "blocks", "weapons_discovered");
						const Json& src = s && s->type == Json::Arr && s->a.size() == kReaperBlock ? *s : fresh;
						for (auto& e : src.a) items.push_back(FromJson(e));
					}
					*wd = RValue(items);
				}
			}
		}
		for (const char* name : kColumns)
		{
			RValue* rows = FindMember(global, name);
			if (!rows || KindOf(*rows) != VALUE_ARRAY) continue;
			const Json* freshCol = FreshData().At("columns").Find(name);
			for (size_t r = 0, nr = ArrayLen(*rows); r < nr; ++r)
			{
				RValue* row = ArrayAt(*rows, r);
				if (!row || KindOf(*row) != VALUE_ARRAY) continue;
				size_t n = ArrayLen(*row);
				if ((n >= kColumn0 + want && !Force) || n < kColumn0 + 3) continue;
				if (Force) n = kColumn0 + 3;
				std::vector<RValue> items;
				for (size_t k = 0; k < n; ++k) items.push_back(*ArrayAt(*row, k));
				for (size_t h = n - kColumn0; h < want; ++h)
				{
					const Json* s = Saved_(h - 3, "columns", name);
					if (s && s->type == Json::Arr && r < s->a.size()) items.push_back(FromJson(s->a[r]));
					else if (freshCol && r < freshCol->a.size()) items.push_back(FromJson(freshCol->a[r]));
					else items.push_back(RValue(0.0));
				}
				*row = RValue(items);
			}
		}
		if (RValue* hi = FindMember(global, "heroes_ingame")) *hi = RValue(static_cast<double>(want));
		for (size_t i = 0; i < Defs.size(); ++i)
			if (Defs[i].Own) PadRows(i);
	}

	// GameMaker's garbage collector frees arrays it cannot reach from GML, whatever C++ holds: an array kept across frames
	// (the game's legacy table while a class's is in place, a class's table, what the save hook holds during a save)
	// also lives in a global of ours
	void Anchor(const std::string& Name, const RValue& V)
	{
		g_Yytk->CallBuiltin("variable_global_set", { RStr("__slormapi_" + Name), V });
	}

	// ---- around the game's save: out before it, back after it
	struct Taken { RValue* Where; RValue Whole; };
	std::vector<Taken> Took;
	int TookHero = -1;

	Json EntryJson(const RValue& V) { return ToJson(V, 32); }
	Json& Child(Json& Parent, const std::string& Key)
	{
		if (const Json* f = Parent.Find(Key); f && f->type == Json::Obj) return const_cast<Json&>(*f);
		return Parent.Set(Key, Json::Object());
	}

	void KeepGameLegacies(Def& D, bool AroundSave);              // (a game hero's mod legacies around a save, below)
	void TrimGameRanks(int Hero);
	void SaveGameHeroData()                                      // the game heroes' content's data into the slot's record
	{
		int slot = SaveSlot();
		if (slot < 0 || Loading) return;
		LoadFile();
		Json& rec = AllSlots[std::to_string(slot)];
		if (rec.type != Json::Obj) rec = Json::Object();
		for (int h = 0; h < 3; ++h)
			if (SlotData.count(GameDefs[h].Key()) && SlotData[GameDefs[h].Key()].type == Json::Obj)
				rec.Set(GameDefs[h].Key(), SlotData[GameDefs[h].Key()]);
	}
	bool PreSave(CInstance*, int, RValue**, RValue&)
	{
		if (!AnyContent() || !Took.empty() || Saving) return false;
		Saving = true;
		SyncMissions(true);                                      // the game's story (its missions' runs into its save data)
		for (int h = 0; h < 3; ++h)                              // a game hero's mod legacies: out of the game's arrays
			if (GameUsed[h] && !GameDefs[h].Legacies.a.empty()) KeepGameLegacies(GameDefs[h], true);
			else TrimGameRanks(h);                               // (the others' rank rows: as the game's, 150)
		if (Defs.empty())                                        // (game heroes' content only: the game's tables, its data)
		{
			std::vector<RValue> held;
			for (auto& tk : Took) held.push_back(tk.Whole);
			Anchor("took", RValue(held));
			SaveGameHeroData();
			SyncElements();
			return false;
		}
		Extend();
		legend::BeforeClassSave();                               // its custom legendaries as Epics (back in PostSave)
		int slot = SaveSlot();
		std::map<std::string, Json> record;                  // what was saved before, updated with what is here now
		for (auto& d : Defs)
		{
			Json j = SlotData.count(d.Key()) && SlotData[d.Key()].type == Json::Obj ? SlotData[d.Key()] : Json::Object();
			j.Set("name", d.Name);
			j.Set("base", static_cast<double>(d.Base));
			record[d.Key()] = std::move(j);
		}
		auto cut = [&](RValue* Arr, const char* Group, const std::string& Name, size_t Keep)
		{
			if (!Arr || KindOf(*Arr) != VALUE_ARRAY) return;
			size_t n = ArrayLen(*Arr);
			if (n <= Keep) return;
			std::vector<RValue> keep;
			for (size_t k = 0; k < Keep; ++k) keep.push_back(*ArrayAt(*Arr, k));
			if (Group)
				for (size_t h = 3; h < n && h - 3 < Defs.size(); ++h)
				{
					if (Name != "store_equipment_list" && !kFromBase.count(Name))
						Child(record[Defs[h - 3].Key()], Group).Set(Name, EntryJson(*ArrayAt(*Arr, h)));
				}
			Took.push_back({ Arr, *Arr });
			*Arr = RValue(keep);
		};
		RValue global = GlobalRValue();
		for (const char* name : kGlobals) cut(FindMember(global, name), "globals", name, 3);
		RValue ui = UiValue();
		if (KindOf(ui) != VALUE_UNDEFINED)
		{
			for (const char* name : kUi) cut(FindMember(ui, name), "ui", name, 3);
			if (AchGameRows)                                         // the UI's arrays by achievement row: the game's rows
				for (const char* name : { "achievements_discovered", "single_achievement_step" }) cut(FindMember(ui, name), nullptr, "", AchGameRows);
			if (RValue* wd = FindMember(ui, "weapons_discovered"); wd && KindOf(*wd) == VALUE_ARRAY)
			{
				for (size_t h = 3, n = ArrayLen(*wd) / kReaperBlock; h < n && h - 3 < Defs.size(); ++h)
				{
					Json block = Json::Array();
					for (size_t k = 0; k < kReaperBlock; ++k) block.Push(ToJson(*ArrayAt(*wd, h * kReaperBlock + k), 4));
					Child(record[Defs[h - 3].Key()], "blocks").Set("weapons_discovered", std::move(block));
				}
				cut(wd, nullptr, "", 3 * kReaperBlock);
			}
		}
		for (const char* name : kColumns)
		{
			RValue* rows = FindMember(global, name);
			if (!rows || KindOf(*rows) != VALUE_ARRAY) continue;
			std::vector<Json> cols(Defs.size(), Json::Array());
			for (size_t r = 0, nr = ArrayLen(*rows); r < nr; ++r)
			{
				RValue* row = ArrayAt(*rows, r);
				if (!row || KindOf(*row) != VALUE_ARRAY) continue;
				for (size_t h = 3; h < Wanted(); ++h)
				{
					RValue* e = ArrayAt(*row, kColumn0 + h);
					cols[h - 3].Push(e ? ToJson(*e, 2) : Json(0.0));
				}
				cut(row, nullptr, "", kColumn0 + 3);
			}
			for (size_t i = 0; i < Defs.size(); ++i)
				if (!cols[i].a.empty()) Child(record[Defs[i].Key()], "columns").Set(name, std::move(cols[i]));
		}
		if (RValue* hi = FindMember(global, "heroes_ingame")) { Took.push_back({ hi, *hi }); *hi = RValue(3.0); }
		{
			std::vector<RValue> held;
			for (auto& tk : Took) held.push_back(tk.Whole);
			Anchor("took", RValue(held));
		}
		if (RValue* h = CurrentHero(); h && IsNumeric(*h) && h->ToDouble() >= 3)
		{
			size_t i = static_cast<size_t>(h->ToDouble()) - 3;
			TookHero = static_cast<int>(h->ToDouble());
			*h = RValue(static_cast<double>(i < Defs.size() ? Defs[i].Base : 0));   // a vanilla game can load it
		}
		if (slot >= 0 && !Loading)                            // a save during a load leaves the custom heroes' data alone
		{
			LoadFile();
			Json s = Json::Object();
			// classes saved in this slot that are not registered now (their mod switched off) keep their data
			if (auto old = AllSlots.find(std::to_string(slot)); old != AllSlots.end() && old->second.type == Json::Obj)
				for (auto& [k, v] : old->second.o)
					if (k != "$active" && !record.count(k)) s.Add(k, v);
			for (auto& [k, v] : record) s.Add(k, v);
			for (int h = 0; h < 3; ++h)                           // the game heroes' content's data (its mods gone: as left)
				if (SlotData.count(GameDefs[h].Key()) && SlotData[GameDefs[h].Key()].type == Json::Obj)
					s.Set(GameDefs[h].Key(), SlotData[GameDefs[h].Key()]);
			// the hero being played now (during the game's own hero switch: the one it leaves; PostSwitch then updates it)
			ActiveKey = TookHero >= 3 && static_cast<size_t>(TookHero) - 3 < Defs.size() ? Defs[static_cast<size_t>(TookHero) - 3].Key() : "";
			if (!ActiveKey.empty()) s.Add("$active", ActiveKey);
			AllSlots[std::to_string(slot)] = std::move(s);
			for (auto& [k, v] : record) SlotData[k] = v;
		}
		SyncElements();                                          // the game's legacy table while it saves
		return false;
	}

	void PostSave(CInstance*, int, RValue**, RValue&)
	{
		bool any = !Took.empty() || Saving;
		Saving = false;
		for (size_t k = Took.size(); k-- > 0;) *Took[k].Where = Took[k].Whole;
		Took.clear();
		legend::AfterClassSave();
		Anchor("took", RValue());
		if (TookHero >= 0) { if (RValue* h = CurrentHero()) *h = RValue(static_cast<double>(TookHero)); TookHero = -1; }
		if (any) WriteFile();
		SyncElements();
	}

	// after the game loads a save slot: the custom heroes' data of that slot, and the one that was played (the game then
	// switches to the saved hero - its base hero - which PreSwitch turns into the custom one)
	bool PreLoad(CInstance*, int, RValue**, RValue&)
	{
		Loading = true;
		SyncElements();                                          // the game's legacy table while it loads
		if (!Defs.empty())                                       // the game loads its three heroes only
			if (RValue* hi = FindMember(GlobalRValue(), "heroes_ingame")) *hi = RValue(3.0);
		return false;
	}
	void PostLoad(CInstance*, int, RValue**, RValue&)
	{
		Loading = false;
		SlotData.clear();                                        // (as saved: content registered later takes its own from the file)
		if (!AnyContent()) return;
		LoadFile();
		ActiveKey.clear();
		PendingHero = ResumeHero = -1;
		auto it = AllSlots.find(std::to_string(SaveSlot()));
		std::string active;
		if (it != AllSlots.end() && it->second.type == Json::Obj)
			for (auto& [k, v] : it->second.o)
			{
				if (k == "$active") active = v.s;
				else SlotData[k] = v;
			}
		if (!Defs.empty()) Extend(true);
		WarlordsChecked = -1;                                    // (its rewards as saved: checked again)
		ForEachContent([](Def& d)                               // (as saved: no run new; its legacies again; its auras off -
		{                                                        // the game's own are off after a load too)
			d.MissionSeen.clear(); d.LegaciesRestored = false; d.AurasOn.clear(); d.AurasPending.clear();
		});
		for (size_t i = 0; i < Defs.size(); ++i)
			if (Defs[i].Key() == active)
			{
				ActiveKey = active;
				PendingHero = static_cast<int>(3 + i);
				if (RValue* h = CurrentHero()) *h = RValue(static_cast<double>(PendingHero));
			}
		SyncElements();
	}

	// scr_hero_switch(hero, ...): after a load, the switch to the saved hero (the base) becomes the custom hero
	bool PreSwitch(CInstance*, int ArgCount, RValue** Args, RValue&)
	{
		if (PendingHero < 3 || ArgCount < 1 || !Args[0] || !IsNumeric(*Args[0])) return false;
		int to = static_cast<int>(Args[0]->ToDouble());
		if (to == Defs[static_cast<size_t>(PendingHero) - 3].Base) *Args[0] = RValue(static_cast<double>(PendingHero));
		PendingHero = -1;
		return false;
	}
	// ... and when a switch ends: the hero object made for it (the game sets it by a switch on 0 / 1 / 2, so a class's
	// hero would keep the previous hero's), and the slot remembers which hero is played
	const char* const kHeroObjects[] = { "obj_hero_knight", "obj_hero_huntress", "obj_hero_mage" };
	void PostSwitch(CInstance*, int, RValue**, RValue&)
	{
		RValue* h = CurrentHero();
		if (!h || !IsNumeric(*h) || SwappedAt) return;
		ResumeHero = -1;
		int hero = static_cast<int>(h->ToDouble());
		if (hero >= 3 && static_cast<size_t>(hero) - 3 < Defs.size())
			if (RValue* obj = FindMember(GlobalRValue(), "object_hero"))
				*obj = AssetIndex(kHeroObjects[Defs[static_cast<size_t>(hero) - 3].Base]);
		SyncElements();
		std::string key = hero >= 3 && static_cast<size_t>(hero) - 3 < Defs.size() ? Defs[static_cast<size_t>(hero) - 3].Key() : "";
		if (key == ActiveKey) return;
		ActiveKey = key;
		int slot = SaveSlot();
		if (slot < 0) return;
		LoadFile();
		Json& s = AllSlots[std::to_string(slot)];
		if (s.type != Json::Obj) s = Json::Object();
		std::vector<std::pair<std::string, Json>> keep;
		for (auto& kv : s.o) if (kv.first != "$active") keep.push_back(kv);
		s.o = std::move(keep);
		if (!key.empty()) s.Add("$active", key);
		WriteFile();
	}

	// The game's globals Exchange swaps (kGlobals, kColumns) are made at its start, never removed, and none is a built-in:
	// once one is found, only its member is looked up, not FindMember's checks (its existence through CallBuiltin, the
	// built-ins by name). (A base-kit class exchanges twice a frame: 259 us a frame before this and ArrayLen's fix, 20 after.)
	RValue* GameGlobal(const RValue& Global, const char* Name)
	{
		static std::unordered_set<const char*> found;      // (by the name's address: the tables' own strings)
		RValue* m = nullptr;
		if (found.contains(Name)) return AurieSuccess(g_Yytk->GetInstanceMember(Global, Name, m)) ? m : nullptr;
		if ((m = FindMember(Global, Name))) found.insert(Name);
		return m;
	}

	// ---- scr_general_skill_cast switches on the hero number (0 / 1 / 2), then the skill: a custom hero casts as its base
	// hero's number - with its own data in the base hero's entries meanwhile (its skill table, masteries, upgrades,
	// runes...), so the game's skill code reads the class's, not the base hero's. Exchanged again after the call, so
	// what the cast changed (mastery gained, ...) stays the class's.
	void Exchange(int Base, int Hero)
	{
		PerfScope perf(&g_PerfExchangeUs);
		if (g_Perf) ++g_PerfExchanges;
		RValue global = GlobalRValue();
		for (const char* name : kGlobals)
		{
			RValue* arr = GameGlobal(global, name);
			if (!arr || KindOf(*arr) != VALUE_ARRAY) continue;
			RValue* a = ArrayAt(*arr, static_cast<size_t>(Base));
			RValue* b = ArrayAt(*arr, static_cast<size_t>(Hero));
			if (a && b) std::swap(*a, *b);
		}
		for (const char* name : kColumns)
		{
			RValue* rows = GameGlobal(global, name);
			if (!rows || KindOf(*rows) != VALUE_ARRAY) continue;
			for (size_t r = 0, n = ArrayLen(*rows); r < n; ++r)
			{
				RValue* row = ArrayAt(*rows, r);
				if (!row || KindOf(*row) != VALUE_ARRAY) continue;
				RValue* a = ArrayAt(*row, kColumn0 + static_cast<size_t>(Base));
				RValue* b = ArrayAt(*row, kColumn0 + static_cast<size_t>(Hero));
				if (a && b) std::swap(*a, *b);
			}
		}
	}

	// kit "own": the game's cast runs as the class's hero (cost, pose, timers, cooldown, procs); its class switch has no
	// case for it - at that moment (after scr_element_determine, and scr_skill_cast_list_add_all unless forced) the mod's
	// skill code gets "class_cast"; each hit of its skills "class_hit"
	struct OwnCast { int Depth; int Hero; double Skill; bool Forced; bool HaveElement = false, Sent = false; RValue Element; };
	std::vector<OwnCast> OwnCasts;

	Json InstJson(const RValue& V) { return KindOf(V) == VALUE_OBJECT && V.m_Instance ? InstanceStub(V.m_Instance) : ToJson(V, 0); }

	void EmitCast(OwnCast& C)
	{
		if (C.Sent) return;
		C.Sent = true;
		RValue inst = g_Yytk->CallBuiltin("instance_find", { AssetIndex("obj_parent_hero"), RValue(0.0) });
		if (KindOf(inst) == VALUE_UNDEFINED || inst.ToDouble() < 0) return;
		Json d = Json::Object();
		d.Add("class", Defs[static_cast<size_t>(C.Hero) - 3].Key());
		d.Add("skill", C.Skill);
		d.Add("forced", C.Forced);
		d.Add("element", KindOf(C.Element) == VALUE_STRING ? C.Element.ToString() : std::string("none"));
		Json hero = Json::Object();
		hero.Add("$inst", static_cast<double>(inst.ToInt64()));
		d.Add("hero", hero);
		for (const char* v : { "x", "y", "precast_x", "precast_y", "facing" })
		{
			RValue x = g_Yytk->CallBuiltin("variable_instance_get", { inst, RValue(v) });
			if (IsNumeric(x)) d.Add(v, x.ToDouble());
		}
		if (RValue* sk = FindMember(GlobalRValue(), "skills"))
			if (RValue* t = ArrayAt(*sk, static_cast<size_t>(C.Hero)))
				if (RValue* row = ArrayAt(*t, static_cast<size_t>(C.Skill)))
					if (RValue* dt = ArrayAt(*row, 16); dt && KindOf(*dt) == VALUE_STRING) d.Add("dmg_type", dt->ToString());
		g_DirectEvents.emplace_back("class_cast", std::move(d));
	}

	void PostElementDetermine(CInstance*, int, RValue**, RValue& Result)
	{
		if (OwnCasts.empty() || OwnCasts.back().Depth != Depth || OwnCasts.back().HaveElement) return;
		OwnCast& c = OwnCasts.back();
		c.HaveElement = true;
		if (KindOf(Result) == VALUE_ARRAY) if (RValue* e = ArrayAt(Result, 0)) c.Element = *e;
		if (c.Forced) EmitCast(c);
	}
	void PostCastListAdd(CInstance*, int, RValue**, RValue&)
	{
		if (OwnCasts.empty() || OwnCasts.back().Depth != Depth || !OwnCasts.back().HaveElement || OwnCasts.back().Forced) return;
		EmitCast(OwnCasts.back());
	}

	// scr_general_skill_damage(skill, target, source, primary): a hit of the class's skill
	void PostSkillDamage(CInstance*, int ArgCount, RValue** Args, RValue&)
	{
		Def* d = nullptr;
		RValue* h = CurrentHero();
		int hero = h && IsNumeric(*h) ? static_cast<int>(h->ToDouble()) : -1;
		if (hero >= 3 && static_cast<size_t>(hero) - 3 < Defs.size() && Defs[static_cast<size_t>(hero) - 3].Own) d = &Defs[static_cast<size_t>(hero) - 3];
		if (!d || ArgCount < 2 || !Args[0] || !IsNumeric(*Args[0]) || !Args[1]) return;
		Json e = Json::Object();
		e.Add("class", d->Key());
		e.Add("skill", Args[0]->ToDouble());
		e.Add("target", InstJson(*Args[1]));
		e.Add("primary", ArgCount > 3 && Args[3] && Args[3]->ToBoolean());
		RValue inst = g_Yytk->CallBuiltin("instance_find", { AssetIndex("obj_parent_hero"), RValue(0.0) });
		if (KindOf(inst) != VALUE_UNDEFINED && inst.ToDouble() >= 0) { Json hero = Json::Object(); hero.Add("$inst", static_cast<double>(inst.ToInt64())); e.Add("hero", hero); }
		g_DirectEvents.emplace_back("class_hit", std::move(e));
	}

	bool PreCast(CInstance*, int ArgCount, RValue** Args, RValue&)
	{
		++Depth;
		RValue* h = CurrentHero();
		if (SwappedAt || !h || !IsNumeric(*h) || !Took.empty()) return false;
		int hero = static_cast<int>(h->ToDouble());
		if (hero < 3 || hero - 3 >= static_cast<int>(Defs.size())) return false;
		if (Defs[static_cast<size_t>(hero) - 3].Own)
		{
			if (ArgCount >= 1 && Args[0] && IsNumeric(*Args[0]))
				OwnCasts.push_back({ Depth, hero, Args[0]->ToDouble(), ArgCount > 1 && Args[1] && Args[1]->ToBoolean() });
			return false;
		}
		Saved = hero;
		SwappedAt = Depth;
		int base = Defs[static_cast<size_t>(hero - 3)].Base;
		Exchange(base, hero);
		*h = RValue(static_cast<double>(base));
		return false;
	}
	void PostCast(CInstance*, int, RValue**, RValue&)
	{
		if (!OwnCasts.empty() && OwnCasts.back().Depth == Depth) OwnCasts.pop_back();
		if (SwappedAt == Depth)
		{
			if (RValue* h = CurrentHero()) *h = RValue(static_cast<double>(Saved));
			Exchange(Defs[static_cast<size_t>(Saved - 3)].Base, Saved);
			SwappedAt = 0;
			Saved = -1;
		}
		--Depth;
	}

	// The skills menu's texts (scr_ui_skill_full_description, and scr_ui_skill_text it and the menu search call) fill
	// each skill's values and keywords in a switch (current_hero) of cases 0-2: for a class, its texts kept their
	// placeholders (Wall of Omen: "Slowed by <placeholder>% for <placeholder> seconds") and lost their hero keywords
	// (Temporal Clone, Remnants). A class on its base hero's kit gets them as its base hero, its data in the base hero's
	// entries meanwhile (as for a cast, without the cast's events); an own kit's texts are its own.
	bool PreSkillText(CInstance*, int, RValue**, RValue&)
	{
		++Depth;
		RValue* h = CurrentHero();
		if (SwappedAt || !h || !IsNumeric(*h) || !Took.empty()) return false;
		int hero = static_cast<int>(h->ToDouble());
		if (hero < 3 || hero - 3 >= static_cast<int>(Defs.size()) || Defs[static_cast<size_t>(hero) - 3].Own) return false;
		Saved = hero;
		SwappedAt = Depth;
		int base = Defs[static_cast<size_t>(hero - 3)].Base;
		Exchange(base, hero);
		*h = RValue(static_cast<double>(base));
		return false;
	}
	void PostSkillText(CInstance*, int, RValue**, RValue&)
	{
		if (SwappedAt == Depth)
		{
			if (RValue* h = CurrentHero()) *h = RValue(static_cast<double>(Saved));
			Exchange(Defs[static_cast<size_t>(Saved - 3)].Base, Saved);
			SwappedAt = 0;
			Saved = -1;
		}
		--Depth;
	}
	// at the start of a frame (no script running): a cast's or a text's window whose Post did not run (a GML error
	// inside) closed - its data back in its own entries, its number back - and the nesting count back to 0
	void CloseCallWindow()
	{
		if (SwappedAt > 0 && Saved >= 3 && static_cast<size_t>(Saved) - 3 < Defs.size())
		{
			if (RValue* h = CurrentHero()) *h = RValue(static_cast<double>(Saved));
			Exchange(Defs[static_cast<size_t>(Saved) - 3].Base, Saved);
		}
		if (SwappedAt > 0) { SwappedAt = 0; Saved = -1; }
		Depth = 0;
		OwnCasts.clear();
	}

	const char* const kHeroNames[] = { "knight", "huntress", "mage" };

	// a dialogue line (global.dialogue + dialogue_correspondance: [key, type, ?, text per language...]) with this key - made
	// in the form of an existing line (Like) - its text in every language: "[speaker]page/" pages, "#" a line break,
	// "<Action?>" a Yes / No question (Yes: scr_citizen_action(Action))
	constexpr size_t kDialogueText = 3;
	bool DialogueLine(const std::string& Key, const std::string& Like, const std::function<std::string(size_t)>& Text)
	{
		RValue global = GlobalRValue();
		RValue* dial = FindMember(global, "dialogue");
		RValue* corr = FindMember(global, "dialogue_correspondance");
		if (!dial || KindOf(*dial) != VALUE_ARRAY || !corr) return false;
		RValue index = g_Yytk->CallBuiltin("ds_map_find_value", { *corr, RStr(Key) });
		if (!IsNumeric(index))
		{
			RValue likeIndex = g_Yytk->CallBuiltin("ds_map_find_value", { *corr, RStr(Like) });
			if (!IsNumeric(likeIndex)) return false;
			RValue* likeLine = ArrayAt(*dial, static_cast<size_t>(likeIndex.ToDouble()));
			if (!likeLine) return false;
			std::vector<RValue> row;
			for (size_t k = 0, n = ArrayLen(*likeLine); k < n; ++k) row.push_back(*ArrayAt(*likeLine, k));
			row[0] = RStr(Key);
			std::vector<RValue> lines;
			for (size_t k = 0, n = ArrayLen(*dial); k < n; ++k) lines.push_back(*ArrayAt(*dial, k));
			lines.push_back(RValue(row));
			*dial = RValue(lines);
			index = RValue(static_cast<double>(lines.size() - 1));
			g_Yytk->CallBuiltin("ds_map_add", { *corr, RStr(Key), index });
		}
		RValue* line = ArrayAt(*dial, static_cast<size_t>(index.ToDouble()));
		if (!line) return false;
		for (size_t k = kDialogueText, n = ArrayLen(*line); k < n; ++k)                       // one column per language
			if (KindOf(*ArrayAt(*line, k)) == VALUE_STRING) *ArrayAt(*line, k) = RStr(Text(k - kDialogueText));
		return true;
	}

	// ---- in the hub: each class is a citizen like the three heroes (citizens 13-15), whose "Yes" switches to it
	constexpr size_t kCitizenNames = 10, kCitizenNameEnd = 32;   // [10, 12, ...]: its name in each language

	RValue CopyOf(const RValue& V) { return FromJson(ToJson(V, 8)); }
	constexpr size_t kCitizenTag = 3;                            // the citizen's tag: [tag] in dialogues, spr_npc_<tag>_* sprites
	std::string CitizenTag(const Def& D) { return D.TalkSelf ? "slormapi_" + std::to_string(D.Cid) : kHeroNames[D.Base]; }

	void EnsureCitizens()
	{
		RValue global = GlobalRValue();
		RValue* cit = FindMember(global, "citizen");
		RValue* dial = FindMember(global, "dialogue");
		RValue* corr = FindMember(global, "dialogue_correspondance");
		if (!cit || KindOf(*cit) != VALUE_ARRAY || !dial || KindOf(*dial) != VALUE_ARRAY || !corr) return;
		for (size_t i = 0; i < Defs.size(); ++i)
		{
			Def& d = Defs[i];
			RValue* baseRow = ArrayAt(*cit, static_cast<size_t>(13 + d.Base));
			if (!baseRow || ArrayLen(*baseRow) <= 5) return;
			if (d.Cid < 0 || d.CidBase != d.Base)                  // made from its base hero's row (again when the base changed)
			{
				RValue row = CopyOf(*baseRow);
				if (d.HubX < 0 || (d.AutoSpot && d.CidBase != d.Base))   // right of its base hero's spot, past the lamp post, two per row
				{
					int k = 0;
					for (size_t j = 0; j < i; ++j) k += Defs[j].Base == d.Base && Defs[j].AutoSpot;
					d.AutoSpot = true;
					d.HubX = ArrayAt(row, 4)->ToDouble() + 100 + 50 * (k % 2);
					d.HubY = ArrayAt(row, 5)->ToDouble() + 56 * (k / 2);
				}
				if (d.Cid < 0)
				{
					std::vector<RValue> rows;
					for (size_t k = 0, n = ArrayLen(*cit); k < n; ++k) rows.push_back(*ArrayAt(*cit, k));
					d.Cid = static_cast<int>(rows.size());
					rows.push_back(row);
					*cit = RValue(rows);
				}
				else *ArrayAt(*cit, static_cast<size_t>(d.Cid)) = row;
				RValue* mine = ArrayAt(*cit, static_cast<size_t>(d.Cid));
				*ArrayAt(*mine, 0) = RValue(static_cast<double>(d.Cid));
				*ArrayAt(*mine, 4) = RValue(d.HubX);
				*ArrayAt(*mine, 5) = RValue(d.HubY);
				for (size_t k = kCitizenNames; k < kCitizenNameEnd && k < ArrayLen(*mine); k += 2) *ArrayAt(*mine, k) = RStr(ClassName(d, (k - kCitizenNames) / 2));
				d.CidBase = d.Base;
			}
			if (RValue* row = ArrayAt(*cit, static_cast<size_t>(d.Cid)))
				if (RValue* tag = ArrayAt(*row, kCitizenTag)) *tag = RStr(CitizenTag(d));
			std::string like = std::to_string(13 + d.Base) + " - Default";   // (its lines in the form of its base hero's)
			auto say = [&](const std::string& Key, const std::function<std::string(size_t)>& Text) { DialogueLine(Key, like, Text); };
			// "<cid> - Default": what is said when you talk to it - its "talk", by Bryan (like the heroes' lines: "You want
			// to make way for the Knight?") or by itself; "<cid> - Pick": its line in the new game's hero picker
			std::string self = d.TalkSelf ? CitizenTag(d) : std::string("bryan"), picker = d.TalkSelf ? CitizenTag(d) : std::string(kHeroNames[d.Base]);
			say(std::to_string(d.Cid) + " - Default", [&](size_t L)
			{
				std::string said = TalkPages(d, L);
				return "[" + self + "]" + (!said.empty() ? said : "Shall we let the " + ClassName(d, L) + " take over?")
					+ "<Hero Switcher " + std::to_string(3 + i) + "?>/";
			});
			say(std::to_string(d.Cid) + " - Pick", [&](size_t L)
			{
				std::string said = TalkPages(d, L), first = said.substr(0, said.find('/'));
				return "[" + picker + "]" + (!first.empty() ? first : "I am the " + ClassName(d, L) + ".") + "/";
			});
		}
	}

	// a save played as a class, with the class registered after the game loaded it (the game made its base hero
	// meanwhile): the game's own switch to the class, as its citizen's "Yes" does - at once, while the main menu is up
	// (the switch reloads the hub, which opens on the main menu again), so its CONTINUE goes in as the class like the
	// game's own. (Switched during CONTINUE, the reload brought the main menu back: CONTINUE had to be pressed twice.)
	void Resume()
	{
		if (ResumeHero < 3 || static_cast<size_t>(ResumeHero) - 3 >= Defs.size()) return;
		RValue* h = CurrentHero();
		int hero = ResumeHero;
		if (!h || !IsNumeric(*h) || static_cast<int>(h->ToDouble()) != Defs[static_cast<size_t>(hero) - 3].Base) { ResumeHero = -1; return; }
		CInstance* self = FirstInstance(AssetIndex("obj_parent_hero"));
		if (!self) return;                                       // (its hero is made yet)
		ResumeHero = -1;
		RValue r;
		g_Yytk->CallGameScriptEx(r, "gml_Script_scr_hero_switcher", self, self, { RValue(static_cast<double>(hero)), RValue(true) });
	}

	// "Hero Switcher N" (a hero citizen's Yes): the game knows 0-2, a class's hero is switched to here
	// "SlormAPI Choice <n>" (a "say" question): n -> its owner and id. A say's line is made again each time, so only the
	// last few questions can still be answered: older ones are dropped, an answered one too
	std::map<int, std::pair<std::string, std::string>> SayChoices;
	int NextChoice = 1;
	constexpr int kChoicesKept = 16;
	int AddChoice(const std::string& Owner, const std::string& Id)
	{
		SayChoices[NextChoice] = { Owner, Id };
		SayChoices.erase(SayChoices.begin(), SayChoices.lower_bound(NextChoice - kChoicesKept + 1));
		return NextChoice++;
	}
	bool PreCitizenAction(CInstance* Self, int ArgCount, RValue** Args, RValue&)
	{
		if (ArgCount < 1 || !Args[0] || KindOf(*Args[0]) != VALUE_STRING) return false;
		std::string a = Args[0]->ToString();
		if (a.starts_with("SlormAPI Choice "))                        // a "say" question's Yes
		{
			if (auto it = SayChoices.find(std::atoi(a.c_str() + 16)); it != SayChoices.end())
			{
				Json e = Json::Object();
				e.Add("owner", it->second.first);
				e.Add("id", it->second.second);
				g_DirectEvents.emplace_back("dialogue_choice", std::move(e));
				SayChoices.erase(it);
			}
			return true;
		}
		if (!a.starts_with("Hero Switcher ")) return false;
		int hero = std::atoi(a.c_str() + 14);
		if (hero < 3 || hero >= static_cast<int>(Wanted())) return false;
		RValue r;
		g_Yytk->CallGameScriptEx(r, "gml_Script_scr_hero_switcher", Self, Self, { RValue(static_cast<double>(hero)), RValue(true) });
		return true;
	}

	// a class's citizen is unlocked when its base hero's is (the game unlocks hero citizens by its story)
	void PostCitizenUnlocked(CInstance* Self, int ArgCount, RValue** Args, RValue& Result)
	{
		if (ArgCount < 1 || !Args[0] || !IsNumeric(*Args[0])) return;
		int cid = static_cast<int>(Args[0]->ToDouble());
		for (auto& d : Defs)
			if (d.Cid == cid)
			{
				RValue r;
				g_Yytk->CallGameScriptEx(r, "gml_Script_scr_citizen_check_if_unlocked", Self, Self, { RValue(static_cast<double>(13 + d.Base)) });
				Result = r;
				return;
			}
	}

	// the hero being played has no citizen in the hub (the game does the same for its three heroes)
	// a class speaking for itself has a tag of its own (its name and portrait in dialogues): its NPC is made with its base
	// hero's (its sprites, spr_npc_<tag>_*)
	int TagLent = -1;
	bool PreCreateCitizen(CInstance*, int ArgCount, RValue** Args, RValue&)
	{
		if (ArgCount < 1 || !Args[0] || !IsNumeric(*Args[0])) return false;
		int cid = static_cast<int>(Args[0]->ToDouble());
		RValue* h = CurrentHero();
		int hero = h && IsNumeric(*h) ? static_cast<int>(h->ToDouble()) : -1;
		for (size_t i = 0; i < Defs.size(); ++i)
			if (Defs[i].Cid == cid)
			{
				if (hero == static_cast<int>(3 + i)) return true;
				if (!Defs[i].TalkSelf) return false;
				RValue* cit = FindMember(GlobalRValue(), "citizen");
				RValue* row = cit && KindOf(*cit) == VALUE_ARRAY ? ArrayAt(*cit, static_cast<size_t>(cid)) : nullptr;
				if (RValue* tag = row ? ArrayAt(*row, 3) : nullptr) { *tag = RStr(kHeroNames[Defs[i].Base]); TagLent = static_cast<int>(i); }
			}
		return false;
	}
	void RestoreLentTag()
	{
		if (TagLent < 0 || static_cast<size_t>(TagLent) >= Defs.size()) { TagLent = -1; return; }
		const Def& d = Defs[static_cast<size_t>(TagLent)];
		RValue* cit = FindMember(GlobalRValue(), "citizen");
		RValue* row = cit && KindOf(*cit) == VALUE_ARRAY ? ArrayAt(*cit, static_cast<size_t>(d.Cid)) : nullptr;
		if (RValue* tag = row ? ArrayAt(*row, 3) : nullptr) *tag = RStr("slormapi_" + std::to_string(d.Cid));
		TagLent = -1;
	}
	void PostCreateCitizen(CInstance*, int, RValue**, RValue&) { RestoreLentTag(); }

	// arriving by a switch, the game puts you at your own citizen's spot - found by a switch on 0 / 1 / 2: a class's
	// hero arrives at the hub's usual spot instead, then moves next to its citizen's spot
	int PlaceAt = -1;
	bool PreHubLoadHero(CInstance*, int, RValue**, RValue&)
	{
		RValue* h = CurrentHero();
		RValue* spot = FindMember(GlobalRValue(), "load_on_spot");
		if (!h || !IsNumeric(*h) || h->ToDouble() < 3 || !spot || !spot->ToBoolean()) return false;
		PlaceAt = static_cast<int>(h->ToDouble()) - 3;
		*spot = RValue(false);
		return false;
	}
	void PostHubLoadHero(CInstance*, int, RValue**, RValue&)
	{
		if (PlaceAt < 0 || PlaceAt >= static_cast<int>(Defs.size())) { PlaceAt = -1; return; }
		RValue hero = g_Yytk->CallBuiltin("instance_find", { AssetIndex("obj_parent_hero"), RValue(0.0) });
		if (KindOf(hero) != VALUE_UNDEFINED && hero.ToDouble() >= 0 && Defs[PlaceAt].HubX >= 0)
		{
			g_Yytk->CallBuiltin("variable_instance_set", { hero, RValue("x"), RValue(Defs[PlaceAt].HubX) });
			g_Yytk->CallBuiltin("variable_instance_set", { hero, RValue("y"), RValue(Defs[PlaceAt].HubY + 24) });
		}
		PlaceAt = -1;
	}

	// ---- the class's own look and damage
	// the class being played: during a cast its hero number is lent to the base hero (PreCast)
	Def* Playing()
	{
		int hero = Saved >= 0 ? Saved : -1;
		if (hero < 0) { RValue* h = CurrentHero(); hero = h && IsNumeric(*h) ? static_cast<int>(h->ToDouble()) : -1; }
		return hero >= 3 && static_cast<size_t>(hero) - 3 < Defs.size() ? &Defs[static_cast<size_t>(hero) - 3] : nullptr;
	}
	// the content of the hero being played: a class's (as Playing), or a game hero's from mods
	int PlayedHero()
	{
		if (Saved >= 0) return Saved;
		RValue* h = CurrentHero();
		return h && IsNumeric(*h) ? static_cast<int>(h->ToDouble()) : -1;
	}
	Def* Content() { return ContentOf(PlayedHero()); }


	// an icon option: a frame of the base hero's skill icons (number), "sprite" / "sprite:frame", or {sprite, frame}
	struct IconSrc { int Sprite = -1, Frame = 0; };
	IconSrc ParseIcon(const Json* J, int BaseSprite, int Default)
	{
		IconSrc s{ BaseSprite, Default };
		if (!J) return s;
		auto named = [&](const std::string& Name)
		{
			size_t c = Name.find(':');
			int spr = static_cast<int>(AssetIndex(Name.substr(0, c).c_str()).ToDouble());
			if (spr >= 0) { s.Sprite = spr; s.Frame = c == std::string::npos ? 0 : std::atoi(Name.c_str() + c + 1); }
		};
		if (J->type == Json::Num) s.Frame = static_cast<int>(J->n);
		else if (J->type == Json::Str) named(J->s);
		else if (J->type == Json::Obj)
		{
			if (const Json* sp = J->Find("sprite"); sp && sp->type == Json::Str) named(sp->s);
			else if (sp && sp->type == Json::Num && g_Yytk->CallBuiltin("sprite_exists", { RValue(sp->n) }).ToBoolean()) { s.Sprite = static_cast<int>(sp->n); s.Frame = 0; }
			s.Frame = static_cast<int>(J->GetNum("frame", s.Frame));
		}
		return s;
	}

	// a sprite with one frame per source (each drawn at its origin), sized and placed like Like
	int MakeStrip(const std::vector<IconSrc>& Src, int Like)
	{
		auto num = [&](const char* fn, int Spr) { return g_Yytk->CallBuiltin(fn, { RValue(static_cast<double>(Spr)) }).ToDouble(); };
		double w = num("sprite_get_width", Like), h = num("sprite_get_height", Like);
		RValue surf = g_Yytk->CallBuiltin("surface_create", { RValue(w), RValue(h) });
		int out = -1;
		for (const IconSrc& s : Src)
		{
			g_Yytk->CallBuiltin("surface_set_target", { surf });
			g_Yytk->CallBuiltin("draw_clear_alpha", { RValue(0.0), RValue(0.0) });
			if (s.Sprite >= 0)                                       // (no sprite: a blank frame)
			{
				double xo = num("sprite_get_xoffset", s.Sprite), yo = num("sprite_get_yoffset", s.Sprite);
				g_Yytk->CallBuiltin("draw_sprite", { RValue(static_cast<double>(s.Sprite)), RValue(static_cast<double>(s.Frame)), RValue(xo), RValue(yo) });
			}
			g_Yytk->CallBuiltin("surface_reset_target", {});
			if (out < 0)
				out = static_cast<int>(g_Yytk->CallBuiltin("sprite_create_from_surface", { surf, RValue(0.0), RValue(0.0), RValue(w), RValue(h),
					RValue(false), RValue(false), RValue(num("sprite_get_xoffset", Like)), RValue(num("sprite_get_yoffset", Like)) }).ToDouble());
			else
				g_Yytk->CallBuiltin("sprite_add_from_surface", { RValue(static_cast<double>(out)), surf, RValue(0.0), RValue(0.0), RValue(w), RValue(h),
					RValue(false), RValue(false) });
		}
		g_Yytk->CallBuiltin("surface_free", { surf });
		return out;
	}

	// kit "own": its skill icons, one 44x44 frame per row of its table (skills: their "icon", else the base hero's same
	// frame; entries: their "icon", else their skill's icon; other rows: the base hero's same frame)
	int IconStrip(Def& D, int BaseSprite, size_t Rows)
	{
		if (auto a = D.Art.find(BaseSprite); a != D.Art.end()) BaseSprite = a->second;   // its art for the base hero's icons
		std::vector<IconSrc> src(Rows);
		for (size_t r = 0; r < Rows; ++r) src[r] = { BaseSprite, static_cast<int>(r) };
		for (size_t k = 0; k < 11 && k < Rows; ++k)
			if (const Json* o = D.Skills.Find(std::to_string(k)); o && o->type == Json::Obj) src[k] = ParseIcon(o->Find("icon"), BaseSprite, static_cast<int>(k));
		size_t first = Rows - D.Entries.a.size();
		for (size_t i = 0; i < D.Entries.a.size(); ++i)
		{
			const Json& e = D.Entries.a[i];
			size_t skill = static_cast<size_t>(e.GetNum("skill", 3));
			src[first + i] = e.Find("icon") ? ParseIcon(e.Find("icon"), BaseSprite, 0) : (skill < 11 ? src[skill] : src[first + i]);
		}
		int out = MakeStrip(src, BaseSprite);
		if (D.Icons >= 0) g_Yytk->CallBuiltin("sprite_delete", { RValue(static_cast<double>(D.Icons)) });
		return D.Icons = out;
	}

	// the reapers' strips by hero number: the three heroes', then two the game made for heroes it never shipped
	const char* const kReaperSprites[] = { "spr_reapers_swords", "spr_reapers_bows", "spr_reapers_staffs", "spr_reapers_axes", "spr_reapers_scythes" };

	// the sprites whose names start with one of Prefixes
	std::vector<int> SpritesNamed(const std::vector<std::string>& prefixes)
	{
		std::vector<int> out;
		for (int i = 0; i < 4000; ++i)
		{
			if (!g_Yytk->CallBuiltin("sprite_exists", { RValue(static_cast<double>(i)) }).ToBoolean()) continue;
			std::string name = g_Yytk->CallBuiltin("sprite_get_name", { RValue(static_cast<double>(i)) }).ToString();
			for (auto& p : prefixes)
				if (name.starts_with(p)) { out.push_back(i); break; }
		}
		return out;
	}

	// the skills menu's per-hero sprites (class / mechanic icons, supports, locked skills): the class's look
	const char* const kLookSlots[] = { "sprite_class", "sprite_skills_support", "sprite_skills_los" };
	RValue LookSlot(const Def& D, const RValue& Base)
	{
		if (!IsNumeric(Base) || KindOf(Base) == VALUE_STRING) return Base;
		auto it = D.Sprites.find(static_cast<int>(Base.ToDouble()));
		return it != D.Sprites.end() ? RValue(static_cast<double>(it->second)) : Base;
	}
	void LookSlots(size_t I)
	{
		for (const char* name : kLookSlots)
			if (RValue* arr = FindMember(GlobalRValue(), name); arr && KindOf(*arr) == VALUE_ARRAY)
				if (RValue* b = ArrayAt(*arr, static_cast<size_t>(Defs[I].Base)), *mine = ArrayAt(*arr, 3 + I); b && mine)
					*mine = LookSlot(Defs[I], *b);
		if (!Defs[I].Own)                                          // its art for the base hero's skill icons (kit "own": IconStrip)
			if (RValue* arr = FindMember(GlobalRValue(), "sprite_skills"); arr && KindOf(*arr) == VALUE_ARRAY)
				if (RValue* b = ArrayAt(*arr, static_cast<size_t>(Defs[I].Base)), *mine = ArrayAt(*arr, 3 + I); b && mine && IsNumeric(*b))
				{
					auto a = Defs[I].Art.find(static_cast<int>(b->ToDouble()));
					*mine = a != Defs[I].Art.end() ? RValue(static_cast<double>(a->second)) : *b;
				}
	}

	std::string SpriteName(int Spr) { return g_Yytk->CallBuiltin("sprite_get_name", { RValue(static_cast<double>(Spr)) }).ToString(); }
	int Frames(int Spr) { return static_cast<int>(g_Yytk->CallBuiltin("sprite_get_number", { RValue(static_cast<double>(Spr)) }).ToDouble()); }
	int Named(const std::string& Name) { return static_cast<int>(AssetIndex(Name.c_str()).ToDouble()); }

	// a game sprite as it shipped: a copy made before any class's art is assigned into it
	int Original(int Spr)
	{
		static std::map<int, int> copies;
		auto it = copies.find(Spr);
		if (it == copies.end())
			it = copies.emplace(Spr, static_cast<int>(g_Yytk->CallBuiltin("sprite_duplicate", { RValue(static_cast<double>(Spr)) }).ToDouble())).first;
		return it->second;
	}

	// its body as the hero's and its citizen's sprite variables draw it, and its skills menu's sprites (per-hero slots)
	std::vector<std::string> BodyPrefixes(const Def& D)
	{
		std::string b = kHeroNames[D.Base];
		return { "spr_npc_" + b + "_", "spr_" + b + "_death", "spr_" + b + "_cast_", "spr_" + b + "_arm", "spr_class_" + b,
			"spr_supports_" + b, "spr_skills_" + b + "_los" };
	}

	// a blank base: each sprite of its template (the Knight) its look would show - body, skills-menu sprites, spec art,
	// banners, reapers, skill icons, its widget, class icon, portrait, category icon, legacy icons - that it gives no art
	// for, drawn as nothing: an empty sprite of that one's size, origin and frames (one frame for the strips a class takes
	// one frame of), made once per sprite; the log names them
	int EmptyLike(int Spr, int Frames_)
	{
		static std::map<std::pair<int, int>, int> made;
		auto key = std::make_pair(Spr, Frames_);
		if (auto it = made.find(key); it != made.end()) return it->second;
		int out = MakeStrip(std::vector<IconSrc>(static_cast<size_t>((std::max)(1, Frames_)), IconSrc{ -1, 0 }), Spr);
		return made[key] = out;
	}
	std::vector<std::string> BodyPrefixes(const Def& D);
	int Named(const std::string& Name);
	int Frames(int Spr);
	std::string SpriteName(int Spr);
	std::vector<int> SpritesNamed(const std::vector<std::string>& prefixes);
	void BlankArt(Def& D)
	{
		if (!D.Blank) return;
		std::vector<std::pair<int, bool>> want;                  // (sprite, all its frames)
		for (int spr : SpritesNamed(BodyPrefixes(D))) want.push_back({ spr, true });
		for (int spr : SpritesNamed({ "spr_specialization_knight", "spr_knight_banner_" })) want.push_back({ spr, true });
		for (const char* n : { "spr_reapers_swords", "spr_reapers_swords_corrupted", "spr_skills_knight", "spr_knight_ui_back",
				"spr_knight_ui_gem", "spr_knight_ui_ring", "spr_elements" })
			if (int spr = Named(n); spr >= 0) want.push_back({ spr, true });
		for (const char* n : { "spr_class_icon_small", "spr_class_icon_mid", "spr_ui_dialogue_citizen", "spr_achievement_back" })
			if (int spr = Named(n); spr >= 0) want.push_back({ spr, false });
		std::string lacking;
		size_t count = 0;
		for (auto [spr, all] : want)
		{
			if (D.BaseArt.count(spr)) continue;
			int empty = EmptyLike(spr, all ? Frames(spr) : 1);
			if (empty < 0) continue;
			D.BaseArt[spr] = empty;
			++count;
			if (lacking.size() < 600) lacking += (lacking.empty() ? "" : ", ") + SpriteName(spr);
		}
		D.Art = D.BaseArt;
		if (count)
			DbgPrintEx(LOG_SEVERITY_INFO, "[SlormAPI] class %s (base \"none\"): %zu sprites drawn as nothing - its art for: %s%s",
				D.Name.c_str(), count, lacking.c_str(), lacking.size() >= 600 ? ", ..." : "");
	}

	int ArtFor = -1;                                             // the hero whose art is assigned in (-1: none)
	std::vector<Def::Swap> ArtApplied;                           // ... what was assigned (put back from this)
	void Portraits();
	// its look: its base hero's, and its "art" - its own sprite in place of any game sprite. Drawn: its body by
	// sprite variable (ApplyLook), its skills menu's slots (LookSlots), its reapers in hand (BeforeEvent), its widget
	// (DrawMeter), its portrait (Portraits); the art drawn by name - spec art, banners, its class icon (frame <hero> of
	// the strip, which is extended past its frames), the menus' reapers (its base hero's strips and primordial strips:
	// the menus take its base hero's case, ReaperCases) and any other sprite - is assigned into the game's sprite while it
	// is played. A game hero's content (D.Game) the same, but for the sprites the game draws as that hero's own (its skills
	// menu's slots, skill icons, HUD widget): its art assigned in while it is played
	void BuildLook(Def& D)
	{
		std::string b = kHeroNames[D.Base];
		int hero = HeroOf(D);
		std::string reaperName = kReaperSprites[D.Base];
		int reapers = Named(reaperName), ancient = Named(reaperName + "_corrupted");
		auto art = [&](int Spr) { auto it = D.Art.find(Spr); return it != D.Art.end() ? it->second : -1; };
		for (auto& [game, had] : D.Sprites) D.SpritesBack[had] = game;   // (the sprite variables holding them: back first)
		D.Sprites.clear();
		std::vector<std::string> body = BodyPrefixes(D);
		if (D.Game) body.resize(4);                              // (a game hero: its body only - its own skills-menu slots: assigned in)
		auto isBody = [&](const std::string& Name) { for (auto& p : body) if (Name.starts_with(p)) return true; return false; };
		for (auto& [spr, a] : D.Art)
			if (isBody(SpriteName(spr))) D.Sprites[spr] = a;
		// its reapers (art without primordial art: its reapers for both)
		D.Reapers = reapers >= 0 && art(reapers) >= 0 ? art(reapers) : reapers;
		D.ReapersAncient = ancient >= 0 && art(ancient) >= 0 ? art(ancient) : reapers >= 0 && art(reapers) >= 0 ? art(reapers)
			: ancient >= 0 ? ancient : D.Reapers;
		// the art drawn by name
		std::vector<Def::Swap> swaps;
		std::vector<int> made;
		// a reaper's own art ("reapers": {ref: {art, primordial_art}}): its frame in its strips
		auto framed = [&](int Strip, const std::map<int, int>& Over) -> int
		{
			if (Strip < 0 || Over.empty()) return Strip;
			std::vector<IconSrc> src;
			for (int f = 0, n = Frames(Strip); f < n; ++f)
			{
				auto it = Over.find(f);
				src.push_back(it != Over.end() ? IconSrc{ it->second, 0 } : IconSrc{ Strip, f });
			}
			int out = MakeStrip(src, Strip);
			if (out >= 0) made.push_back(out);
			return out >= 0 ? out : Strip;
		};
		D.Reapers = framed(D.Reapers, D.ReaperArt);
		D.ReapersAncient = framed(D.ReapersAncient, D.ReaperArtPrimordial);
		auto swap = [&](int Spr, int Mine) { if (Spr >= 0 && Mine >= 0 && Mine != Spr) swaps.push_back({ Spr, Original(Spr), Mine }); };
		for (const char* name : { "spr_class_icon_small", "spr_class_icon_mid" })
			if (int spr = Named(name); spr >= 0 && Frames(spr) > 0 && (!D.Game || art(spr) >= 0))
			{
				int orig = Original(spr), n = Frames(orig);
				std::vector<IconSrc> src;
				for (int f = 0; f < (std::max)(n, hero + 1); ++f)
					src.push_back(f != hero ? IconSrc{ orig, f < n ? f : 0 } : art(spr) >= 0 ? IconSrc{ art(spr), 0 } : IconSrc{ orig, D.Base });
				int strip = MakeStrip(src, orig);
				if (strip >= 0) made.push_back(strip);
				swap(spr, strip);
			}
		if (reapers >= 0 && D.Reapers >= 0) swap(reapers, D.Reapers);               // the menus draw its base hero's strips
		if (ancient >= 0 && D.ReapersAncient >= 0) swap(ancient, D.ReapersAncient);  // (ReaperCases), its primordial ones too
		for (auto& [spr, a] : D.Art)
		{
			std::string n = SpriteName(spr);
			if (isBody(n) || spr == reapers || spr == ancient || n == "spr_class_icon_small" || n == "spr_class_icon_mid"
				|| n == "spr_ui_dialogue_citizen" || (!D.Game && (n == "spr_skills_" + b || n.starts_with("spr_" + b + "_ui")))
				|| n == "spr_achievement_back"                       // (one frame: its achievement category's icon, AchIcon)
				|| (n == "spr_elements" && (!D.Legacies.a.empty() || D.LegacyBaseHidden)))    // (its legacy tree's icons: BuildElementIcons, from it)
				continue;
			swap(spr, a);
		}
		if (ArtFor == hero)                                      // played: the game's back first, then the new ones (SyncArt)
		{
			for (auto& w : ArtApplied) g_Yytk->CallBuiltin("sprite_assign", { RValue(static_cast<double>(w.Sprite)), RValue(static_cast<double>(w.Orig)) });
			ArtApplied.clear();
			ArtFor = -1;
		}
		D.Swaps = std::move(swaps);
		for (int old : D.Made)                                     // the previous build's (the game's sprites hold copies)
			if (g_Yytk->CallBuiltin("sprite_exists", { RValue(static_cast<double>(old)) }).ToBoolean())
				g_Yytk->CallBuiltin("sprite_delete", { RValue(static_cast<double>(old)) });
		D.Made = std::move(made);
		D.MeterArt.clear();
		D.CategoryIcon = -1;
		if (!D.Game) LookSlots(static_cast<size_t>(hero - 3));
		Portraits();
		DbgPrintEx(LOG_SEVERITY_INFO, "[SlormAPI] %s %s: %zu sprites in its look (%zu its own art)", D.Game ? "hero" : "class",
			D.Name.c_str(), D.Sprites.size() + D.Swaps.size(), D.Art.size());
	}

	// dialogue portraits are frames of spr_ui_dialogue_citizen by citizen: a class's citizen has its own - its art
	// "spr_ui_dialogue_citizen" (one frame), else its base hero's. The strip is made again
	// from the game's frames when that changes.
	std::string PortraitsMade;
	int PortraitStrip = -1;                                       // the strip assigned last (the game's sprite holds a copy)
	void Portraits()
	{
		int spr = Named("spr_ui_dialogue_citizen");
		if (spr < 0) return;
		std::string sig;
		bool gameArt = false;                                         // (a game hero's portrait art: its frame made again)
		for (auto& d : Defs)
		{
			auto a = d.Art.find(spr);
			sig += std::to_string(d.Cid) + ":" + std::to_string(a != d.Art.end() ? a->second : -1) + ":" + std::to_string(d.Base) + ";";
		}
		for (int h = 0; h < 3; ++h)
			if (auto a = GameDefs[h].Art.find(spr); GameUsed[h] && a != GameDefs[h].Art.end())
			{
				sig += "h" + std::to_string(h) + ":" + std::to_string(a->second) + ";";
				gameArt = true;
			}
		if (sig == PortraitsMade) return;
		int orig = Original(spr), n = Frames(orig), total = n;
		for (auto& d : Defs) total = (std::max)(total, d.Cid + 1);
		if (total <= n && !gameArt && PortraitStrip < 0) return;
		std::vector<IconSrc> src;
		for (int f = 0; f < total; ++f) src.push_back({ orig, f < n ? f : 0 });
		for (auto& d : Defs)
			if (d.Cid >= n)
			{
				auto a = d.Art.find(spr);
				src[static_cast<size_t>(d.Cid)] = a != d.Art.end() ? IconSrc{ a->second, 0 } : IconSrc{ orig, 13 + d.Base };
			}
		for (int h = 0; h < 3; ++h)
			if (auto a = GameDefs[h].Art.find(spr); GameUsed[h] && a != GameDefs[h].Art.end() && 13 + h < total)
				src[static_cast<size_t>(13 + h)] = IconSrc{ a->second, 0 };
		if (int strip = MakeStrip(src, orig); strip >= 0)
		{
			g_Yytk->CallBuiltin("sprite_assign", { RValue(static_cast<double>(spr)), RValue(static_cast<double>(strip)) });
			if (PortraitStrip >= 0) g_Yytk->CallBuiltin("sprite_delete", { RValue(static_cast<double>(PortraitStrip)) });
			PortraitStrip = strip;
			PortraitsMade = sig;
		}
	}

	// the game picks the reapers' strip by hero number with a switch over 0-4, in hand and in each menu that draws one;
	// YYC keeps each switch's case values in a static table set on its first run: [value (an RValue real), case index] x 5.
	// While a class is played, its base hero's case (0-2: its strip, and its primordial strip for a primordial reaper -
	// both assigned in, BuildLook) has the class's hero number, so the menus draw its reapers like its base hero's. A table
	// is changed only when it is [0-or-a-class, 1-or-a-class, 2-or-a-class, 3, 4] with cases 0-4 (not run yet, or another
	// build of the game: left alone). The last is scr_loot_weapon_name's (a reaper on the ground: its sprite, none past
	// hero 2 - the hub's "First Reaper" for a class with none stopped scr_create_sprite_loot on an unset sprite_index).
	// A table whose first run comes while a class is played takes no case that run. Where its guard is known (MSVC's
	// thread-safe static: 0 before the first run), a table not run yet is filled here as that run fills it, and the guard
	// set to INT_MIN (done for every thread) - only once the script's code is seen to use that table and that guard (a
	// RIP-relative reference to each in its first 16 KB: another build of the game is left alone).
	struct ReaperSwitch { uint32_t Table; uint32_t Guard = 0; const char* Script = nullptr; };
	const ReaperSwitch kReaperSwitches[] = { { 0x2115910 }, { 0x20fb7b0 }, { 0x20ffac0 }, { 0x20fad30 }, { 0x20fde20 },
		{ 0x21184f0 }, { 0x2118040 }, { 0x211e460 }, { 0x2109b20, 0x2109b84, "gml_Script_scr_loot_weapon_name" } };
	// a RIP-relative reference (an instruction's disp32, then 0, 1 or 4 bytes of immediate) to Target in Len bytes from Code
	bool CodeRefers(const uint8_t* Code, size_t Len, uintptr_t Target)
	{
		for (size_t i = 0; i + 4 <= Len; ++i)
		{
			int32_t disp;
			std::memcpy(&disp, Code + i, 4);
			for (size_t tail : { 0, 1, 4 })
				if (reinterpret_cast<uintptr_t>(Code + i + 4 + tail) + static_cast<intptr_t>(disp) == Target) return true;
		}
		return false;
	}
	bool GuardVerified(const ReaperSwitch& S, uintptr_t Base, size_t Image)
	{
		CScript* cs = nullptr;
		if (!S.Script || !AurieSuccess(g_Yytk->GetNamedRoutinePointer(S.Script, reinterpret_cast<PVOID*>(&cs))) || !cs
			|| !cs->m_Functions || !cs->m_Functions->m_ScriptFunction) return false;
		auto code = reinterpret_cast<const uint8_t*>(cs->m_Functions->m_ScriptFunction);
		uintptr_t start = reinterpret_cast<uintptr_t>(code);
		if (start < Base || start + 0x4000 > Base + Image) return false;
		return CodeRefers(code, 0x4000, Base + S.Table) && CodeRefers(code, 0x4000, Base + S.Guard);
	}
	void FillReaperTable(uint8_t* T, int32_t* Guard)
	{
		for (int k = 0; k < 5; ++k)
			if (std::any_of(T + 20 * k, T + 20 * k + 20, [](uint8_t b) { return b != 0; })) return;   // (part filled: not ours)
		for (int k = 0; k < 5; ++k)
		{
			double v = k;
			int32_t index = k;
			std::memcpy(T + 20 * k, &v, 8);                 // (flags and kind 0: a real)
			std::memcpy(T + 20 * k + 16, &index, 4);
		}
		*Guard = INT32_MIN;
	}
	void ReaperCases()
	{
		static uintptr_t base = ExeBase();
		static size_t image = 0;
		static int writable[std::size(kReaperSwitches)] = {};       // 0 unknown, 1 yes, -1 no
		static int guarded[std::size(kReaperSwitches)] = {};        // its guard: 0 not checked yet, 1 verified, -1 not
		if (!image)
		{
			auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			image = nt->OptionalHeader.SizeOfImage;
		}
		Def* d = Playing();
		double want[3] = { 0, 1, 2 };
		if (d) want[d->Base] = 3.0 + static_cast<double>(d - Defs.data());
		static bool found = false, told = false;
		static int waited = 0;                                        // (the weapon's switch runs when the hero is made)
		if (d && !found && !told && ++waited > 600)
		{
			told = true;
			DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormAPI] reapers: none of the game's reaper switches found in this game build; "
				"classes show no reaper art in the menus");
		}
		for (size_t i = 0; i < std::size(kReaperSwitches); ++i)
		{
			uint32_t rva = kReaperSwitches[i].Table;
			if (writable[i] < 0 || rva + 100 > image) continue;
			uint8_t* t = reinterpret_cast<uint8_t*>(base + rva);
			if (!writable[i])
			{
				MEMORY_BASIC_INFORMATION mbi{};
				writable[i] = VirtualQuery(t, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT
					&& (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)) ? 1 : -1;
				if (writable[i] < 0) continue;
			}
			if (uint32_t g = kReaperSwitches[i].Guard; g && g + 4 <= image && guarded[i] >= 0)
			{
				if (!guarded[i]) guarded[i] = GuardVerified(kReaperSwitches[i], base, image) ? 1 : -1;
				if (auto* guard = reinterpret_cast<int32_t*>(base + g); guarded[i] > 0 && *guard == 0) FillReaperTable(t, guard);
			}
			bool ok = true;
			for (int k = 0; k < 5 && ok; ++k)
			{
				double v; uint32_t kind; int32_t index;
				std::memcpy(&v, t + 20 * k, 8);
				std::memcpy(&kind, t + 20 * k + 12, 4);
				std::memcpy(&index, t + 20 * k + 16, 4);
				ok = (kind & 0xFFFFFF) == 0 && index == k && (v == k || (k < 3 && v >= 3 && v < 64 && v == std::floor(v)));
			}
			found |= ok;
			if (ok)
				for (int k = 0; k < 3; ++k)
				{
					double cur;
					std::memcpy(&cur, t + 20 * k, 8);
					if (cur != want[k]) std::memcpy(t + 20 * k, &want[k], 8);
				}
		}
	}

	// ---- its mechanic on the HUD, where the game draws its heroes' (the body's GUI draw shows nothing for a class): its
	// base hero's widget - the Knight's gems lit by value / max (all of them and the lit back while "lit"), the Huntress's
	// faces (red over green by value / max, the ring lit while "lit"), the Mage's emblems (lit by value / max) - with the
	// value in its middle. Its art for the widget's sprites, else the game's.
	bool MeterShown() { Def* d = Playing(); return d && d->MeterValue >= 0; }
	int MeterSprite(Def& D, const char* Name)
	{
		int spr = Named(Name);
		if (spr < 0) return -1;
		if (auto it = D.MeterArt.find(spr); it != D.MeterArt.end()) return it->second;
		int out = spr;
		if (auto a = D.Art.find(spr); a != D.Art.end()) out = a->second;
		return D.MeterArt[spr] = out;
	}
	void DrawMeter(CInstance* Ui)
	{
		Def* d = Playing();
		if (!d || d->MeterValue < 0 || !Ui) return;
		RValue menu = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Ui), RValue("menu_show") });
		if (KindOf(menu) == VALUE_STRING && menu.ToString() != "none") return;
		if (g_Yytk->CallBuiltin("instance_number", { AssetIndex("obj_hub_controller") }).ToDouble() > 0) return;   // in battle only
		double sc = Num(g_Yytk->CallBuiltin("variable_instance_get", { RValue(Ui), RValue("ui_scale") }));
		if (sc <= 0) sc = 1;
		double x = Num(g_Yytk->CallBuiltin("variable_instance_get", { RValue(Ui), RValue("class_mec_x") })),
			y = Num(g_Yytk->CallBuiltin("variable_instance_get", { RValue(Ui), RValue("class_mec_y") })), k = 2 * sc;   // the game's spot, pixel art x2
		auto sprite = [&](int Spr, double Frame, double X, double Y)
		{
			if (Spr >= 0) g_Yytk->CallBuiltin("draw_sprite_ext", { RValue(static_cast<double>(Spr)), RValue(Frame), RValue(X), RValue(Y), RValue(k), RValue(k),
				RValue(0.0), RValue(16777215.0), RValue(1.0) });
		};
		double part = std::clamp(d->MeterValue / (std::max)(1.0, d->MeterMax), 0.0, 1.0);
		int gems = d->MeterLit ? 4 : static_cast<int>(std::floor(4 * part));
		if (d->Base == 1)                                          // Huntress: the faces, red over green, in the ring
		{
			int ring = MeterSprite(*d, "spr_huntress_ui"), faces = MeterSprite(*d, "spr_huntress_ui_faces");
			sprite(ring, d->MeterLit && ring >= 0 && Frames(ring) > 1 ? 1 : 0, x, y);
			if (faces >= 0)
			{
				auto num = [&](const char* Fn) { return g_Yytk->CallBuiltin(Fn, { RValue(static_cast<double>(faces)) }).ToDouble(); };
				double w = num("sprite_get_width"), h = num("sprite_get_height"), left = x - num("sprite_get_xoffset") * k, top = y - num("sprite_get_yoffset") * k;
				double red = std::round(h * (1 - (d->MeterLit ? 1.0 : part)));
				auto piece = [&](double Frame, double From, double Rows)
				{
					if (Rows > 0) g_Yytk->CallBuiltin("draw_sprite_part_ext", { RValue(static_cast<double>(faces)), RValue(Frame), RValue(0.0), RValue(From), RValue(w), RValue(Rows),
						RValue(left), RValue(top + From * k), RValue(k), RValue(k), RValue(16777215.0), RValue(1.0) });
				};
				piece(1, 0, red);
				piece(0, red, h - red);
			}
		}
		else if (d->Base == 2)                                     // Mage: the emblems around its back
		{
			int back = MeterSprite(*d, "spr_mage_ui_back"), gem = MeterSprite(*d, "spr_mage_ui_gem");
			sprite(back, 0, x, y);
			const double at[4][2] = { { 0, -16 }, { 16, 0 }, { 0, 16 }, { -16, 0 } };
			for (int g = 0; g < 4; ++g) sprite(gem, g < gems ? 0 : 3, x + at[g][0] * k, y + at[g][1] * k);
		}
		else                                                       // Knight: the gems around the ring
		{
			int back = MeterSprite(*d, "spr_knight_ui_back"), ring = MeterSprite(*d, "spr_knight_ui_ring"), gem = MeterSprite(*d, "spr_knight_ui_gem");
			sprite(back, d->MeterLit ? 1 : 0, x, y);
			sprite(ring, 0, x, y);
			const double at[4][2] = { { 0, -15 }, { 15, 0 }, { 0, 15 }, { -15, 0 } };
			for (int g = 0; g < 4; ++g) sprite(gem, g < gems ? 1 : 0, x + at[g][0] * k, y + at[g][1] * k);
		}
		std::vector<RValue> a = { RValue(x), RValue(y), RStr(std::to_string(static_cast<int>(d->MeterValue))), RValue(1.0), RValue(1.0),
			RStr("ft_standard_13_b"), RValue(1.0), RValue(1.0), RValue(0.0), RValue(1.0), RStr(d->MeterLit ? "item_highlight" : "white"),
			RStr("black"), RValue(1.0), RValue(false) };
		RValue r;
		g_Yytk->CallGameScriptEx(r, "gml_Script_scr_draw_text", Ui, Ui, a);
	}

	// the art drawn by name: the played class's, the game's otherwise
	// its reapers ("reapers": {ref: {...}}) in the game's reaper table while it is played - the game's back otherwise.
	// global.reapers, 120 rows: [0] REF, [1] ORDER, [2] CONSTANT_CHECK, [3] VALUE_BASE, [4] VALUE_TYPE, [5] VALUE_LEVEL,
	// [6] VALUE_STAT, [7] VALUE_REAL, [8] LOOTABLE (undefined: not), [9] BASE_DMG_MIN, [10] BASE_DMG_MAX, [11] MIN_DMG_LVL,
	// [12] MAX_DMG_LVL, [13] DMG_MULTIPLIER, [14] MAX_LVL, [15] EVOLVE_IN (undefined: none), [16] BLACKSMITH, [17] SPRITE
	// (its frame in spr_reapers_<swords|bows|staffs>), [18 + 3 x language] name ("$": the weapon's noun), description,
	// lore (11 languages). A description: its stat lines ("*+@ £" each: VALUE_BASE / VALUE_TYPE "%" / VALUE_LEVEL "rl"
	// x the reaper's level / VALUE_STAT, "|" between them), then "|", then its effect ("*" a line, "@" the next value,
	// "#" a break), "/" the primordial part. Each hero owns its own copy of the 120 (reapers_data[hero][ref]), so a
	// class's version of a reaper is its own; a reaper's number stays (current_weapon past weapon_ancient_offset = 120 is
	// its primordial one, the save keeps numbers), which is why a class gives its reapers in the place of game ones.
	int TextsFor = -1;
	struct HeldText { size_t Ref, Col; RValue Was; };
	std::vector<HeldText> TextsHeld;
	RValue ReaperValue(const Json& V)                         // "none" / false: none (undefined), else as given
	{
		if ((V.type == Json::Str && V.s == "none") || (V.type == Json::Bool && !V.b)) return RValue();
		if (V.type == Json::Bool) return RValue(1.0);
		return FromJson(V);
	}
	void SyncReaperTexts(bool Force = false)
	{
		Def* d = Content();
		int want = d && !d->ReaperCells.empty() ? HeroOf(*d) : -1;
		if (want == TextsFor && !Force) return;
		RValue* rows = FindMember(GlobalRValue(), "reapers");
		auto cell = [&](size_t Ref, size_t Col) -> RValue* { RValue* r = rows ? ArrayAt(*rows, Ref) : nullptr; return r ? ArrayAt(*r, Col) : nullptr; };
		for (auto& h : TextsHeld)
			if (RValue* c = cell(h.Ref, h.Col)) *c = h.Was;
		TextsHeld.clear();
		if (want >= 0)
			for (auto& t : d->ReaperCells)
				if (RValue* c = cell(t.Ref, t.Col)) { TextsHeld.push_back({ t.Ref, t.Col, *c }); *c = ReaperValue(t.Mine); }
		TextsFor = want;
		if (Force)                                               // (registered again while played: the worn one's numbers)
			if (CInstance* hero = FirstInstance(AssetIndex("obj_parent_hero")))
			{
				RValue r;
				g_Yytk->CallGameScriptEx(r, "gml_Script_scr_hero_fast_check_reset", hero, hero, {});
				for (const char* flag : { "self_refresh_weapon", "self_refresh_weapon_synergies", "self_refresh_stats" })
					g_Yytk->CallBuiltin("variable_instance_set", { RValue(hero), RValue(flag), RValue(true) });
			}
	}
	void SyncArt()
	{
		if (!Took.empty() || Loading || Saving) return;
		SyncReaperTexts();
		Def* d = Content();
		int want = d && !d->Swaps.empty() ? HeroOf(*d) : -1;
		if (want == ArtFor) return;
		auto assign = [](int To, int From) { g_Yytk->CallBuiltin("sprite_assign", { RValue(static_cast<double>(To)), RValue(static_cast<double>(From)) }); };
		for (auto& w : ArtApplied) assign(w.Sprite, w.Orig);
		ArtApplied.clear();
		if (want >= 0)
		{
			for (auto& w : d->Swaps) assign(w.Sprite, w.Mine);
			ArtApplied = d->Swaps;
		}
		ArtFor = want;
	}

	// before the hero's / a citizen's Draw: the variables that hold its sprites point at the class's
	const char* const kSpriteVars[] = { "sprite_index", "idle_sprite", "idle_sprite_armless", "run_sprite", "run_sprite_armless",
		"old_idle_sprite", "old_run_sprite", "death_animation_sprite", "dist_sprites", "melee_sprites", "supp_sprites",
		"walk_sprite", "special_sprite" };
	void ApplyLook(CInstance* Self, const Def& D)
	{
		if ((D.Sprites.empty() && D.SpritesBack.empty()) || !Self) return;
		auto look = [&](int S)                                   // (a sprite of a look it had: the game's, then this look's)
		{
			if (auto b = D.SpritesBack.find(S); b != D.SpritesBack.end()) S = b->second;
			auto it = D.Sprites.find(S);
			return it != D.Sprites.end() ? it->second : S;
		};
		RValue self(Self);
		for (const char* name : kSpriteVars)
		{
			RValue* v = FindMember(self, name);
			if (!v || !IsNumeric(*v)) continue;
			int now = static_cast<int>(v->ToDouble()), want = look(now);
			if (want != now) *v = RValue(static_cast<double>(want));
		}
		RValue spr = g_Yytk->CallBuiltin("variable_instance_get", { self, RValue("sprite_index") });   // a built-in
		if (IsNumeric(spr))
			if (int now = static_cast<int>(spr.ToDouble()), want = look(now); want != now)
				g_Yytk->CallBuiltin("variable_instance_set", { self, RValue("sprite_index"), RValue(static_cast<double>(want)) });
	}

	// a game hero's content gone: the sprite variables of its hero and its citizen that hold its look's sprites, back to
	// the game's now (nothing draws them through its content any more)
	void LookBack(const Def& D)
	{
		if (D.SpritesBack.empty()) return;
		if (PlayedHero() == D.Base)
			if (CInstance* h = FirstInstance(AssetIndex("obj_parent_hero"))) ApplyLook(h, D);
		RValue npc = AssetIndex("obj_npc");
		double n = g_Yytk->CallBuiltin("instance_number", { npc }).ToDouble();
		for (int k = 0; k < static_cast<int>(n); ++k)
		{
			RValue in = g_Yytk->CallBuiltin("instance_find", { npc, RValue(static_cast<double>(k)) });
			RValue cid = g_Yytk->CallBuiltin("variable_instance_get", { in, RValue("cid") });
			if (IsNumeric(cid) && static_cast<int>(cid.ToDouble()) == 13 + D.Base) ApplyLook(in.ToInstance(), D);
		}
	}

	enum class DrawKind { Unknown, Other, Hero, Npc, UiStep, Weapon, EntranceEnter, EntranceDraw };
	std::map<CCode*, DrawKind> DrawKinds;
	bool FillerRow(const RValue& Row);
	void BeforeEvent(CInstance* Self, CCode* Code)
	{
		if (!AnyContent() || !Code || !Self) return;
		auto it = DrawKinds.find(Code);
		DrawKind kind = it != DrawKinds.end() ? it->second : DrawKind::Unknown;
		if (kind == DrawKind::Unknown)
		{
			std::string n = Code->GetName() ? Code->GetName() : "";
			kind = n == "gml_Object_obj_parent_hero_Draw_0" ? DrawKind::Hero
				: (n.starts_with("gml_Object_obj_npc_Draw") || n.starts_with("gml_Object_obj_parent_npc_Draw")) ? DrawKind::Npc
				: n == "gml_Object_obj_ui_next_gen_Step_0" ? DrawKind::UiStep
				: n.starts_with("gml_Object_obj_parent_weapon_") ? DrawKind::Weapon
				: n == "gml_Object_obj_hub_parent_environment_entrance_Other_25" ? DrawKind::EntranceEnter
				: n == "gml_Object_obj_hub_parent_environment_entrance_Draw_64" ? DrawKind::EntranceDraw : DrawKind::Other;
			DrawKinds[Code] = kind;
		}
		if (AreaNaming >= 0 && kind != DrawKind::EntranceDraw) AreaNaming = -1;
		if (kind == DrawKind::Hero)
		{
			if (Def* d = Content()) ApplyLook(Self, *d);
		}
		else if (kind == DrawKind::EntranceEnter || kind == DrawKind::EntranceDraw) AreaEntranceEvent(Self, kind == DrawKind::EntranceEnter);
		else if (kind == DrawKind::Weapon)          // the hero's reaper: the game takes its strip by hero number (none past 4)
		{
			RValue self(Self);
			RValue* h = FindMember(self, "hero");
			int hero = h && IsNumeric(*h) ? static_cast<int>(h->ToDouble()) : -1;
			if (hero >= 3 && static_cast<size_t>(hero) - 3 < Defs.size() && Defs[static_cast<size_t>(hero) - 3].Reapers >= 0)
			{
				const Def& d = Defs[static_cast<size_t>(hero) - 3];
				double want = d.Reapers;
				if (d.ReapersAncient >= 0 && d.ReapersAncient != d.Reapers)   // a primordial one: id past weapon_ancient_offset
				{
					RValue* worn = FindMember(GlobalRValue(), "current_weapon");
					RValue* from = FindMember(GlobalRValue(), "weapon_ancient_offset");
					RValue* id = worn && KindOf(*worn) == VALUE_ARRAY ? ArrayAt(*worn, static_cast<size_t>(hero)) : nullptr;
					if (id && from && IsNumeric(*id) && IsNumeric(*from) && id->ToDouble() >= from->ToDouble()) want = d.ReapersAncient;
				}
				RValue cur = g_Yytk->CallBuiltin("variable_instance_get", { self, RValue("sprite_index") });
				if (!IsNumeric(cur) || cur.ToDouble() != want)
					g_Yytk->CallBuiltin("variable_instance_set", { self, RValue("sprite_index"), RValue(want) });
			}
		}
		else if (kind == DrawKind::UiStep)                           // (the menus are called directly: no hook sees them)
		{
			RValue self(Self);
			if (RValue* tip = FindMember(self, "skills_tooltip_panel"); tip && FillerRow(*tip)) *tip = RValue(-1.0);
		}
		else if (kind == DrawKind::Npc)
		{
			RValue cid = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Self), RValue("cid") });
			if (IsNumeric(cid))
			{
				int c = static_cast<int>(cid.ToDouble());
				for (auto& d : Defs)
					if (d.Cid == c) ApplyLook(Self, d);
				if (c >= 13 && c <= 15 && GameUsed[c - 13]) ApplyLook(Self, GameDefs[c - 13]);   // a game hero's citizen
			}
		}
	}

	// kit "own": an entry's rank (the game's own check: equipped, mastery reached, passives with their skill in a slot)
	double EntryRank(CInstance* Hero, int Entry)
	{
		RValue r;
		if (!AurieSuccess(g_Yytk->CallGameScriptEx(r, "gml_Script_scr_skill_check_if_equipped", Hero, Hero, { RValue(static_cast<double>(Entry)) })))
			return 0;
		return IsNumeric(r) ? r.ToDouble() : 0;
	}

	// the entries' effects on its skill rows, from the table as built: value x (1 + percent x rank / 100) + add x rank
	void ApplyEffects(Def& D, CInstance* Hero, int HeroNumber)
	{
		if (D.Effects.empty()) return;
		std::map<std::tuple<size_t, size_t, size_t>, std::pair<double, double>> sum;   // (row, col, index) -> percent, add
		std::map<int, double> ranks;
		for (auto& e : D.Effects)
		{
			if (!ranks.count(e.Entry)) ranks[e.Entry] = EntryRank(Hero, e.Entry);
			auto& s = sum[{ e.Row, e.Col, e.Index }];
			s.first += e.Percent * ranks[e.Entry];
			s.second += e.Add * ranks[e.Entry];
		}
		RValue* skills = FindMember(GlobalRValue(), "skills");
		RValue* table = skills ? ArrayAt(*skills, static_cast<size_t>(HeroNumber)) : nullptr;
		RValue* current = FindMember(GlobalRValue(), "hero_skills");
		for (auto& [at, s] : sum)
		{
			auto [row, col, index] = at;
			auto p0 = D.Pristine.find({ row, col });
			if (p0 == D.Pristine.end()) continue;
			RValue original = p0->second;
			RValue* c0 = &original;
			RValue value;
			if (IsNumeric(*c0) && index == 0) value = RValue(c0->ToDouble() * (1 + s.first / 100) + s.second);
			else
			{
				std::vector<std::string> parts;
				std::string text = CellText(*c0);
				for (size_t p = 0;;) { size_t q = text.find('|', p); parts.push_back(text.substr(p, q - p)); if (q == std::string::npos) break; p = q + 1; }
				if (index >= parts.size()) continue;
				double v = 0;
				try { v = std::stod(parts[index]); } catch (...) { continue; }
				v = v * (1 + s.first / 100) + s.second;
				char buf[64];
				snprintf(buf, sizeof buf, "%g", std::round(v * 1000) / 1000);
				parts[index] = buf;
				std::string out;
				for (size_t i = 0; i < parts.size(); ++i) out += (i ? "|" : "") + parts[i];
				value = RStr(out);
			}
			for (RValue* t : { table, current })
				if (RValue* r = t ? ArrayAt(*t, row) : nullptr)
					if (RValue* c = ArrayAt(*r, col)) *c = value;
		}
	}

	// scr_hero_step switches on the hero number (0 / 1 / 2) for the hero's own part of a step: its step check
	// (scr_<hero>_step_constant_check: its mechanic, and every synergy period - synergy_refresh_time reaching
	// synergy_refresh_timer - the time back to 0 and its passives refreshed) and, when self_refresh_passives, its passives
	// (scr_<hero>_refresh_passives). A class had no case: its time stayed at the timer, so every step refreshed all its
	// synergies, then all its stats (scr_reset_stats) and the open menu (baked again each frame; the game's bake makes
	// ds_lists that piled up). Done for it before the step: a class on its base hero's kit by the base hero's scripts (its
	// data in the base hero's entries meanwhile, as for a cast), an own kit by its own passives at the game's times.
	const char* const kStepCheck[] = { "gml_Script_scr_knight_step_constant_check", "gml_Script_scr_huntress_step_constant_check",
		"gml_Script_scr_mage_step_constant_check" };
	const char* const kRefreshPassives[] = { "gml_Script_scr_knight_refresh_passives", "gml_Script_scr_huntress_refresh_passives",
		"gml_Script_scr_mage_refresh_passives" };
	constexpr int kStepWindow = -1;                          // SwappedAt while PreHeroStep has the class's data in place
	// its data back in its own entries, its number back (also at the next frame, when the call did not come back)
	void CloseStepWindow()
	{
		if (SwappedAt != kStepWindow || Saved < 3 || static_cast<size_t>(Saved) - 3 >= Defs.size()) return;
		int hero = Saved;
		SwappedAt = 0;
		Saved = -1;
		if (RValue* now = CurrentHero()) *now = RValue(static_cast<double>(hero));
		Exchange(Defs[static_cast<size_t>(hero) - 3].Base, hero);
	}
	bool PreHeroStep(CInstance* Self, int, RValue**, RValue&)
	{
		Def* d = Playing();
		RValue* h = CurrentHero();
		if (!d || !Self || !h || !IsNumeric(*h) || Saved >= 0 || SwappedAt || !Took.empty()) return false;
		RValue self(Self);
		RValue* pause = FindMember(GlobalRValue(), "game_pause");
		bool paused = pause && pause->ToBoolean();
		RValue* flag = FindMember(self, "self_refresh_passives");
		bool passives = flag && flag->ToBoolean();
		RValue r;
		if (!d->Own)
		{
			int hero = static_cast<int>(h->ToDouble());
			Exchange(d->Base, hero);
			*h = RValue(static_cast<double>(d->Base));
			Saved = hero;                                       // (as for a cast: still the class for the other hooks)
			SwappedAt = kStepWindow;                            // (a cast inside: its data already)
			try
			{
				if (!paused) g_Yytk->CallGameScriptEx(r, kStepCheck[d->Base], Self, Self, {});
				if (passives) g_Yytk->CallGameScriptEx(r, kRefreshPassives[d->Base], Self, Self, {});
			}
			catch (...) {}
			CloseStepWindow();
			return false;
		}
		RValue* time = FindMember(self, "synergy_refresh_time");
		RValue* timer = FindMember(self, "synergy_refresh_timer");
		bool period = !paused && time && timer && IsNumeric(*time) && IsNumeric(*timer) && time->ToDouble() >= timer->ToDouble();
		if (period) *time = RValue(0.0);
		if ((period || passives) && !d->Passives.empty())
			g_Yytk->CallGameScriptEx(r, "gml_Script_scr_skill_refresh_passives", Self, Self, { RStr(d->Passives) });
		return false;
	}

	void AuraLocks(Def& D, CInstance* Hero);
	void PostStatsRefresh(CInstance* Self, int, RValue**, RValue&)
	{
		bool hero = false;
		try                                                       // (an exception out of a hook ends the game)
		{
			hero = Self && ObjectNameOf(Self).starts_with("obj_hero_");
			if (!hero) return;
			Def* d = Playing();
			if (d && d->Own)
				if (RValue* h = CurrentHero(); h && IsNumeric(*h)) ApplyEffects(*d, Self, static_cast<int>(h->ToDouble()));
			if (Def* c = Content()) AuraLocks(*c, Self);
		}
		catch (const std::exception& e) { DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormAPI] stats refresh: %s", e.what()); }
		catch (...) {}
		if (hero) lua::StatsRefreshed(Self);                      // the Lua mods' on_stats, last (guarded there)
	}

	extern int ProfileSwap;                                   // the class whose card the Profile draws now (below)

	// ---- its name in the menus: the game's text "hero_<n>" ("The Mighty Knight") has no entry for a class's hero
	bool AchCategoryString(CInstance* Self, const std::string& Key, RValue& Result);
	void PostString(CInstance* Self, int ArgCount, RValue** Args, RValue& Result)
	{
		if (ArgCount < 1 || !Args[0] || KindOf(*Args[0]) != VALUE_STRING) return;
		std::string key = Args[0]->ToString();
		if (key.starts_with("achievement_cat_") && AchCategoryString(Self, key, Result)) return;
		if (key.starts_with("environment_") && AreaString(key, Result)) return;
		bool weapon = key.starts_with("weapon_");               // ... and "weapon_<n>" is the game's unused 4th, "Axe"
		size_t at = weapon ? 7 : 5;
		if (!(weapon || key.starts_with("hero_")) || key.size() <= at || key.size() > at + 2) return;
		int hero = std::atoi(key.c_str() + at);
		if (!weapon && ProfileSwap >= 0 && hero == Defs[static_cast<size_t>(ProfileSwap)].Base) hero = 3 + ProfileSwap;   // its card
		if (hero < 3 || static_cast<size_t>(hero) - 3 >= Defs.size()) return;
		const Def& d = Defs[static_cast<size_t>(hero) - 3];
		if (!weapon) Result = RStr(ClassTitle(d, CurrentLang()));
		else if (!d.Weapon.empty()) Result = RStr(d.Weapon);
		else
		{
			RValue r;
			g_Yytk->CallGameScriptEx(r, "gml_Script_scr_string", Self, Self, { RStr("weapon_" + std::to_string(d.Base)) });
			Result = r;
		}
	}

	// code that sees the three heroes only: the Profile (room for three cards; a 4th is drawn past the panel - a class's
	// card is chosen in its dropdown, below), the class selector, and the account's stats (scr_global_fill_stats:
	// achievements, Steam unlocks - the classes stay out)
	RValue ThreeHeroes;
	int ThreeDepth = 0;
	bool PreThreeHeroes(CInstance*, int, RValue**, RValue&)
	{
		if (ThreeDepth++ > 0) return false;
		RValue* hi = FindMember(GlobalRValue(), "heroes_ingame");
		if (hi && IsNumeric(*hi) && hi->ToDouble() > 3) { ThreeHeroes = *hi; *hi = RValue(3.0); }
		return false;
	}
	void PostThreeHeroes(CInstance*, int, RValue**, RValue&)
	{
		if (ThreeDepth > 0 && --ThreeDepth > 0) return;
		if (KindOf(ThreeHeroes) == VALUE_UNDEFINED) return;
		if (RValue* hi = FindMember(GlobalRValue(), "heroes_ingame")) *hi = ThreeHeroes;
		ThreeHeroes = RValue();
	}

	// ---- the Profile's cards: any character in any of its three slots. Clicking a card's name bar opens a dropdown
	// (the game's thin grey buttons) of the three heroes and the classes. A card is always the game's own: after the
	// Profile's draw, the draw runs again into a surface - for a class with every per-hero entry of it and its base hero
	// exchanged (its title answered for the base hero's "hero_<n>") - and the chosen card's rectangle is copied over the
	// slot's. The account's totals in the same draw stay the three heroes'. A card's figure (spr_npc_<hero>_idle by its
	// slot, x4) is drawn outside the Profile's script, later: so the cards are laid down after the UI's GUI draw, with
	// the shown character's figure (a class's in its look) and the dropdowns on top. (Assigning other art to the
	// game's figure sprites each frame broke them.)
	int SlotShow[3] = { 0, 1, 2 };       // the character in each slot: hero 0-2, or 3 + class index
	int SlotList = -1;                   // the slot whose dropdown is open
	int ProfileDepth = 0, ProfileSwap = -1;
	void SwapHeroes(size_t A, size_t B)
	{
		auto swap = [&](RValue* Arr)
		{
			if (!Arr || KindOf(*Arr) != VALUE_ARRAY) return;
			RValue* a = ArrayAt(*Arr, A), *b = ArrayAt(*Arr, B);
			if (a && b) std::swap(*a, *b);
		};
		RValue global = GlobalRValue();
		for (const char* name : kGlobals) swap(FindMember(global, name));
		RValue ui = UiValue();
		if (KindOf(ui) == VALUE_UNDEFINED) return;
		for (const char* name : kUi) swap(FindMember(ui, name));
		if (RValue* wd = FindMember(ui, "weapons_discovered"); wd && KindOf(*wd) == VALUE_ARRAY)
			for (size_t k = 0; k < kReaperBlock; ++k)
			{
				RValue* a = ArrayAt(*wd, A * kReaperBlock + k), *b = ArrayAt(*wd, B * kReaperBlock + k);
				if (a && b) std::swap(*a, *b);
			}
	}
	bool ValidCharacter(int C) { return C >= 0 && (C < 3 || static_cast<size_t>(C) - 3 < Defs.size()); }
	std::string CharacterName(int C, CInstance* Ui)
	{
		if (C >= 3) return ClassTitle(Defs[static_cast<size_t>(C) - 3], CurrentLang());
		RValue r;
		g_Yytk->CallGameScriptEx(r, "gml_Script_scr_string", Ui, Ui, { RStr("hero_" + std::to_string(C)) });
		return KindOf(r) == VALUE_STRING ? r.ToString() : "Hero " + std::to_string(C + 1);
	}

	void AchEnter();
	void AchLeave();
	bool PreProfile(CInstance* Self, int ArgCount, RValue** Args, RValue& Result)
	{
		PreThreeHeroes(Self, ArgCount, Args, Result);
		AchEnter();
		++ProfileDepth;
		return false;
	}
	void PrepareProfileCards(CInstance* Ui);
	void PushProfileEntries(CInstance* Ui);
	void PostProfileDraw(CInstance* Self, int ArgCount, RValue** Args, RValue& Result)
	{
		PostThreeHeroes(Self, ArgCount, Args, Result);
		AchLeave();
		if (ProfileDepth > 0 && --ProfileDepth > 0) return;         // (a card's own pass)
		if (Defs.empty()) return;
		PrepareProfileCards(Self);
		PushProfileEntries(Self);
	}

	constexpr double kCardX = 1456, kCardY = 148, kCardW = 398, kCardH = 266;   // slot k: kCardY + k * kCardH
	constexpr double kBarX = 1462, kBarY = 152, kBarW = 388, kBarH = 56;        // its name bar
	constexpr double kRowW = 336, kRowH = 52;                                  // a dropdown row (spr_menu_thin_grey_button)
	int CardSurface = -1, CleanSurface = -1;   // a pass of the Profile's draw; the slots' cards (no figures) for this frame
	bool CardsReady = false;                   // CleanSurface holds the shown slots' cards
	// the cards are drawn again (a whole pass of the Profile's draw per source: some ms) only when what they show may
	// have changed - other characters in the slots, another screen or language, a surface lost - or kCardsRefreshMs on
	// (their play time counts the seconds); the game draws the Profile every frame
	constexpr double kCardsRefreshMs = 250;
	double CardsAt = 0;
	std::string CardsKey;
	int FigureSprite(int Hero) { return static_cast<int>(AssetIndex("spr_npc_" + std::string(kHeroNames[Hero]) + "_idle").ToDouble()); }
	constexpr double kFigX = 1504, kFigY = 308, kFigScale = 4;              // slot 0's figure (its origin), per slot + kCardH
	int GuiSurface(int& Surf, bool& Made)
	{
		double gw = g_Yytk->CallBuiltin("display_get_gui_width", {}).ToDouble(), gh = g_Yytk->CallBuiltin("display_get_gui_height", {}).ToDouble();
		auto exists = [](int S) { return S >= 0 && g_Yytk->CallBuiltin("surface_exists", { RValue(static_cast<double>(S)) }).ToBoolean(); };
		auto size = [](const char* Fn, int S) { return g_Yytk->CallBuiltin(Fn, { RValue(static_cast<double>(S)) }).ToDouble(); };
		if (exists(Surf) && (size("surface_get_width", Surf) != gw || size("surface_get_height", Surf) != gh))
			g_Yytk->CallBuiltin("surface_free", { RValue(static_cast<double>(Surf)) });
		if (!exists(Surf))
		{
			Surf = static_cast<int>(g_Yytk->CallBuiltin("surface_create", { RValue(gw), RValue(gh) }).ToDouble());
			Made = true;
		}
		return exists(Surf) ? Surf : -1;
	}
	bool ProfileShown(CInstance* Ui)
	{
		if (Defs.empty() || !Ui) return false;
		RValue menu = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Ui), RValue("menu_show") });
		return KindOf(menu) == VALUE_STRING && menu.ToString() == "profile";
	}
	// the Profile's panel: 0 the cards, 2 the achievements
	int ProfilePanel(CInstance* Ui)
	{
		RValue p = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Ui), RValue("profile_panel") });
		return IsNumeric(p) ? static_cast<int>(p.ToDouble()) : -1;
	}
	void DrawAchOverlay(CInstance* Ui);
	void PushAchEntries(CInstance* Ui, const std::function<void(const std::string&, double, double, double, double)>& Entry);
	bool Controller()
	{
		RValue* pad = FindMember(GlobalRValue(), "input_control");
		return pad && IsNumeric(*pad) && pad->ToDouble() == 1;
	}
	// the cursor in layout units (the widgets' 1920x1080 coordinates), which lights a dropdown's row: the mouse, or with
	// a controller the game's own cursor (obj_ui_next_gen mx / my: the selected entry's middle). Both are on the screen:
	// less the menu's place (C is an overlay's)
	void GuiCursor(EvalCtx& C, double& Mx, double& My)
	{
		bool pad = Controller() && C.Self;
		double sc = UiScale(C);
		Mx = ((pad ? g_Yytk->CallBuiltin("variable_instance_get", { RValue(C.Self), RValue("mx") })
			: g_Yytk->CallBuiltin("device_mouse_x_to_gui", { RValue(0.0) })).ToDouble() - C.OffX) / sc;
		My = ((pad ? g_Yytk->CallBuiltin("variable_instance_get", { RValue(C.Self), RValue("my") })
			: g_Yytk->CallBuiltin("device_mouse_y_to_gui", { RValue(0.0) })).ToDouble() - C.OffY) / sc;
	}
	// the game's selector arrow at K of its size around (X, Y): down on a closed dropdown, up on an open one
	void DrawListArrow(EvalCtx& C, double X, double Y, double K, bool Open)
	{
		double sc = UiScale(C);
		g_Yytk->CallBuiltin("draw_sprite_ext", { AssetIndex("spr_selector_btn_r_v2"), RValue(0.0), RValue(GuiX(C, Open ? X - 18 * K : X + 18 * K)),
			RValue(GuiY(C, Open ? Y + 12 * K : Y - 12 * K)), RValue(K * sc), RValue(K * sc), RValue(Open ? 90.0 : 270.0), RValue(16777215.0), RValue(1.0) });
	}
	// an open dropdown's N rows from (X, Y) down, the game's thin grey buttons as its menus use them: frame 1 under the
	// cursor, 2 while the mouse button is held on it, 3 (gold) the current pick (Lit), whose name is highlighted
	void DrawListRows(EvalCtx& C, double X, double Y, int N, int Lit, const std::function<std::string(int)>& Name)
	{
		double mx, my;
		GuiCursor(C, mx, my);
		bool held = !Controller() && g_Yytk->CallBuiltin("mouse_check_button", { RValue(1.0) }).ToBoolean();
		RValue button = AssetIndex("spr_menu_thin_grey_button");
		for (int k = 0; k < N; ++k, Y += kRowH)
		{
			bool hover = mx >= X && mx < X + kRowW && my >= Y && my < Y + kRowH, lit = k == Lit;
			WidgetSprite(C, button, hover && held ? 2 : lit ? 3 : hover ? 1 : 0, X, Y);
			WidgetText(C, X + kRowW / 2, Y + kRowH / 2, Name(k), "ft_standard_12", lit ? "item_highlight" : "white", 1, 1);
		}
	}

	// after the Profile's draw: the shown characters' cards into CleanSurface, at their slots. scr_ui_menus_profile draws
	// at the layout times ui_scale only (the game adds menu_x_pos / menu_y_pos when it puts the menu on screen), so the
	// surfaces hold the cards there and DrawProfileOverlay adds the offset
	void PrepareProfileCards(CInstance* Ui)
	{
		bool ready = CardsReady;
		CardsReady = false;
		if (!Ui) return;
		for (int& c : SlotShow) if (!ValidCharacter(c)) c = static_cast<int>(&c - SlotShow);
		if (ProfilePanel(Ui) != 0) return;
		bool any = false;
		for (int k = 0; k < 3; ++k) any |= SlotShow[k] != k;
		if (!any) return;
		EvalCtx ctx{ Ui, Ui, 0, nullptr };
		double sc = UiScale(ctx);
		bool made = false;
		if (GuiSurface(CardSurface, made) < 0 || GuiSurface(CleanSurface, made) < 0) return;
		std::string key = std::to_string(SlotShow[0]) + "," + std::to_string(SlotShow[1]) + "," + std::to_string(SlotShow[2]) + "|" +
			std::to_string(sc) + "|" + std::to_string(CurrentLang());
		double now = MicrosSinceStart() / 1000.0;
		if (ready && !made && key == CardsKey && now - CardsAt < kCardsRefreshMs) { CardsReady = true; return; }
		CardsKey = key;
		CardsAt = now;
		RValue surf(static_cast<double>(CardSurface)), clean(static_cast<double>(CleanSurface));
		g_Yytk->CallBuiltin("surface_set_target", { clean });
		g_Yytk->CallBuiltin("draw_clear_alpha", { RValue(0.0), RValue(0.0) });
		g_Yytk->CallBuiltin("surface_reset_target", {});
		RValue mba = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Ui), RValue("mba") });
		if (KindOf(mba) == VALUE_ARRAY) MbaRemovePrefix(mba, "slormapi:class:");     // (the game draws its entries in that draw)
		// one pass per source: -1 the heroes as they are, else a class (index)
		std::set<int> passes;
		for (int k = 0; k < 3; ++k) if (SlotShow[k] != k) passes.insert(SlotShow[k] < 3 ? -1 : SlotShow[k] - 3);
		for (int pass : passes)
		{
			Def* d = pass >= 0 ? &Defs[static_cast<size_t>(pass)] : nullptr;
			size_t base = d ? static_cast<size_t>(d->Base) : 0, hero = d ? 3 + static_cast<size_t>(pass) : 0;
			g_Yytk->CallBuiltin("surface_set_target", { surf });
			g_Yytk->CallBuiltin("draw_clear_alpha", { RValue(0.0), RValue(0.0) });
			if (d) { ProfileSwap = pass; SwapHeroes(base, hero); }
			++ProfileDepth;                                       // its hooks: no cards or dropdowns of their own
			try { RValue r; g_Yytk->CallGameScriptEx(r, "gml_Script_scr_ui_menus_profile", Ui, Ui, {}); }
			catch (...) {}
			ProfileDepth = (std::max)(0, ProfileDepth - 1);
			if (d) { SwapHeroes(base, hero); ProfileSwap = -1; }
			g_Yytk->CallBuiltin("surface_reset_target", {});
			g_Yytk->CallBuiltin("surface_set_target", { clean });
			for (int k = 0; k < 3; ++k)
			{
				int c = SlotShow[k];
				if (c == k || (c < 3 ? -1 : c - 3) != pass) continue;
				double from = kCardY + static_cast<double>(d ? base : static_cast<size_t>(c)) * kCardH, to = kCardY + k * kCardH;
				g_Yytk->CallBuiltin("draw_surface_part", { surf, RValue(kCardX * sc), RValue(from * sc), RValue(kCardW * sc), RValue(kCardH * sc),
					RValue(kCardX * sc), RValue(to * sc) });
			}
			g_Yytk->CallBuiltin("surface_reset_target", {});
		}
		CardsReady = true;
	}

	// the open dropdown's rows: n, under its name bar (above it when there is no room)
	int ListRows(double& X, double& Y)
	{
		int n = 3 + static_cast<int>(Defs.size());
		X = kBarX + (kBarW - kRowW) / 2;
		Y = kBarY + SlotList * kCardH + kBarH;
		if (Y + n * kRowH > 1060) Y = kBarY + SlotList * kCardH - n * kRowH;
		return n;
	}

	// the dropdowns' clickable entries: each name bar (an empty sprite: the game draws an entry's sprite and text when
	// the mouse is over it) and, for the open one, a row per character
	int BlankSprite = -1;
	void ClassEntry(EvalCtx& C, const std::string& Action, double X, double Y, double W, double H)
	{
		if (BlankSprite < 0 || !g_Yytk->CallBuiltin("sprite_exists", { RValue(static_cast<double>(BlankSprite)) }).ToBoolean())
		{
			RValue s = g_Yytk->CallBuiltin("surface_create", { RValue(1.0), RValue(1.0) });
			g_Yytk->CallBuiltin("surface_set_target", { s });
			g_Yytk->CallBuiltin("draw_clear_alpha", { RValue(0.0), RValue(0.0) });
			g_Yytk->CallBuiltin("surface_reset_target", {});
			BlankSprite = static_cast<int>(g_Yytk->CallBuiltin("sprite_create_from_surface", { s, RValue(0.0), RValue(0.0), RValue(1.0), RValue(1.0),
				RValue(false), RValue(false), RValue(0.0), RValue(0.0) }).ToDouble());
			g_Yytk->CallBuiltin("surface_free", { s });
		}
		RValue e = g_Yytk->CallBuiltin("array_create", { RValue(10.0), RValue(0.0) });
		double sc = UiScale(C);
		RValue fields[10] = { RStr("slormapi_class_" + Action), RValue(static_cast<double>(BlankSprite)), RValue(X * sc), RValue(Y * sc),
			RValue(W * sc), RValue(H * sc), RStr(""), AssetIndex("scr_blacksmith_open_filter_from_inventory"), RStr("slormapi:class:" + Action), RStr("") };
		for (int k = 0; k < 10; ++k) g_Yytk->CallBuiltin("array_set", { e, RValue(static_cast<double>(k)), fields[k] });
		MbaPushEntry(C.Self, e);
	}
	void PushProfileEntries(CInstance* Ui)
	{
		if (!Ui) return;
		EvalCtx c{ Ui, Ui, 0, nullptr };
		RValue mba = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Ui), RValue("mba") });
		if (KindOf(mba) == VALUE_ARRAY) MbaRemovePrefix(mba, "slormapi:class:");     // this draw adds the current ones
		auto entry = [&](const std::string& Action, double X, double Y, double W, double H) { ClassEntry(c, Action, X, Y, W, H); };
		if (ProfilePanel(Ui) == 2) { PushAchEntries(Ui, entry); return; }
		if (ProfilePanel(Ui) != 0) return;
		for (int k = 0; k < 3; ++k) entry("slot:" + std::to_string(k), kBarX, kBarY + k * kCardH, kBarW, kBarH);
		if (SlotList < 0 || SlotList > 2) return;
		double x, y;
		int n = ListRows(x, y);
		for (int ch = 0; ch < n; ++ch) entry("pick:" + std::to_string(SlotList) + ":" + std::to_string(ch), x, y + ch * kRowH, kRowW, kRowH);
	}

	// after the UI's GUI draw (everything the game draws, the cards' figures included): the shown characters' cards,
	// their figures (a class's in its look), then the dropdowns (the game's thin grey buttons, lit under the mouse)
	void DrawProfileOverlay(CInstance* Ui)
	{
		if (!ProfileShown(Ui)) return;
		if (ProfilePanel(Ui) == 2) { DrawAchOverlay(Ui); return; }
		if (ProfilePanel(Ui) != 0) return;
		EvalCtx c{ Ui, Ui, 0, nullptr };
		c.Overlay = true;
		double sc = UiScale(c);
		if (CardsReady && CleanSurface >= 0 && g_Yytk->CallBuiltin("surface_exists", { RValue(static_cast<double>(CleanSurface)) }).ToBoolean())
		{
			double now = MicrosSinceStart() / 1000.0;
			for (int k = 0; k < 3; ++k)
			{
				int ch = SlotShow[k];
				if (ch == k || !ValidCharacter(ch)) continue;
				double top = kCardY + k * kCardH;
				g_Yytk->CallBuiltin("draw_surface_part", { RValue(static_cast<double>(CleanSurface)), RValue(kCardX * sc), RValue(top * sc),
					RValue(kCardW * sc), RValue(kCardH * sc), RValue(GuiX(c, kCardX)), RValue(GuiY(c, top)) });   // (the surfaces: no menu offset)
				int spr = -1;
				if (ch < 3) spr = FigureSprite(ch);
				else
				{
					const Def& d = Defs[static_cast<size_t>(ch) - 3];
					auto it = d.Sprites.find(FigureSprite(d.Base));                  // its art, else its base hero's
					spr = it != d.Sprites.end() ? it->second : FigureSprite(d.Base);
				}
				if (spr < 0) continue;
				double frames = (std::max)(1.0, g_Yytk->CallBuiltin("sprite_get_number", { RValue(static_cast<double>(spr)) }).ToDouble());
				g_Yytk->CallBuiltin("draw_sprite_ext", { RValue(static_cast<double>(spr)), RValue(std::fmod(std::floor(now / 125.0), frames)),
					RValue(GuiX(c, kFigX)), RValue(GuiY(c, kFigY + k * kCardH)), RValue(kFigScale * sc), RValue(kFigScale * sc), RValue(0.0),
					RValue(16777215.0), RValue(1.0) });
			}
		}
		for (int k = 0; k < 3; ++k)                    // on each name bar's lower edge
			DrawListArrow(c, kBarX + kBarW / 2, kBarY + k * kCardH + kBarH - 2, 0.75, SlotList == k);
		if (SlotList < 0 || SlotList > 2) return;
		double x, y;
		int n = ListRows(x, y);
		DrawListRows(c, x, y, n, SlotShow[SlotList], [&](int Ch) { return CharacterName(Ch, Ui); });
	}

	// ---- the new game's hero picker ("Choose your Hero": menu "class", obj_ui_next_gen.selected_class): a dropdown on the
	// hero's name lists the three heroes and the classes. A class picked is the picker's selected_class - the intro
	// switches to it at its end (scr_hero_switcher) - with its line said ("<cid> - Pick": its talk, by itself, else by its
	// base hero) and its portrait drawn over the picker's (which knows heroes 0-2)
	bool PickOpen = false;
	constexpr double kPickNameX = 100, kPickNameY = 466, kPickNameW = 330, kPickNameH = 44;   // the hero's name
	constexpr double kPickPortraitX = 216, kPickPortraitY = 340, kPickListX = 540, kPickListY = 340;
	bool PickerShown(CInstance* Ui)
	{
		if (Defs.empty() || !Ui) return false;
		RValue menu = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Ui), RValue("menu_show") });
		return KindOf(menu) == VALUE_STRING && menu.ToString() == "class";
	}
	int PickerCharacter(CInstance* Ui)
	{
		RValue c = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Ui), RValue("selected_class") });
		return IsNumeric(c) && KindOf(c) != VALUE_STRING ? static_cast<int>(c.ToDouble()) : -1;
	}
	void PostPicker(CInstance* Self, int ArgCount, RValue** Args, RValue& Result)
	{
		PostThreeHeroes(Self, ArgCount, Args, Result);
		if (!PickerShown(Self)) return;
		EvalCtx c{ Self, Self, 0, nullptr };
		RValue mba = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Self), RValue("mba") });
		if (KindOf(mba) == VALUE_ARRAY) MbaRemovePrefix(mba, "slormapi:class:");
		ClassEntry(c, "kpick:open", kPickNameX, kPickNameY, kPickNameW, kPickNameH);
		if (PickOpen)
			for (int ch = 0; ch < 3 + static_cast<int>(Defs.size()); ++ch)
				ClassEntry(c, "kpick:" + std::to_string(ch), kPickListX, kPickListY + ch * kRowH, kRowW, kRowH);
	}
	void DrawPickerOverlay(CInstance* Ui)
	{
		if (!PickerShown(Ui)) { PickOpen = false; return; }
		EvalCtx c{ Ui, Ui, 0, nullptr };
		c.Overlay = true;
		int shown = PickerCharacter(Ui);
		if (shown >= 3 && ValidCharacter(shown))                     // its portrait
			WidgetSprite(c, AssetIndex("spr_ui_dialogue_citizen"), Defs[static_cast<size_t>(shown) - 3].Cid, kPickPortraitX, kPickPortraitY);
		DrawListArrow(c, kPickNameX + kPickNameW - 24, kPickNameY + kPickNameH / 2, 0.75, PickOpen);
		if (PickOpen)
			DrawListRows(c, kPickListX, kPickListY, 3 + static_cast<int>(Defs.size()), shown, [&](int Ch) { return CharacterName(Ch, Ui); });
	}
	void PickerClick(const std::string& What)
	{
		if (What == "open") { PickOpen = !PickOpen; return; }
		PickOpen = false;
		int ch = std::atoi(What.c_str());
		RValue uiv = UiValue();
		if (!ValidCharacter(ch) || KindOf(uiv) == VALUE_UNDEFINED) return;
		CInstance* ui = uiv.ToInstance();
		RValue r;                                                     // the game's own change (its hero's line for 0-2)
		g_Yytk->CallGameScriptEx(r, "gml_Script_scr_ui_class_selector_change_class", ui, ui, { RValue(static_cast<double>(ch)) });
		if (ch < 3) return;
		g_Yytk->CallGameScriptEx(r, "gml_Script_scr_clear_dialogue", ui, ui, {});
		g_Yytk->CallGameScriptEx(r, "gml_Script_scr_create_dialogue", ui, ui, { RStr(std::to_string(Defs[static_cast<size_t>(ch) - 3].Cid) + " - Pick") });
	}

	int AchList = -1;                    // the achievements panel's open dropdown (its hero category, 0-2)
	// a character into a row (shown in another row: the two change places)
	void PickCharacter(int Row, int Character)
	{
		if (Row < 0 || Row > 2 || !ValidCharacter(Character)) return;
		for (int j = 0; j < 3; ++j)
			if (j != Row && SlotShow[j] == Character) SlotShow[j] = SlotShow[Row];
		SlotShow[Row] = Character;
	}
	void ProfileClickIn(const std::string& Action);
	// a click on the class's UI (dropdowns, picks): the menu is baked again, so the entries it adds (a dropdown's rows)
	// are there and its look follows, as for the game's own buttons
	void ProfileClick(const std::string& Action)
	{
		ProfileClickIn(Action);
		UiRefresh();
	}
	void ProfileClickIn(const std::string& Action)
	{
		if (Action.starts_with("kpick:")) { PickerClick(Action.substr(6)); return; }
		if (Action.starts_with("kskin:")) { SkinClick(Action.substr(6)); return; }
		if (Action.starts_with("aslot:"))
		{
			int k = std::atoi(Action.c_str() + 6);
			AchList = AchList == k || k < 0 || k > 2 ? -1 : k;
			return;
		}
		if (Action.starts_with("apick:"))
		{
			int k = std::atoi(Action.c_str() + 6);
			size_t colon = Action.find(':', 6);
			int ch = colon == std::string::npos ? -1 : std::atoi(Action.c_str() + colon + 1);
			if (k >= 0 && k <= 2 && ValidCharacter(ch) && SlotShow[k] != ch)
			{
				PickCharacter(k, ch);
				RValue ui = UiValue();                               // its first page
				if (KindOf(ui) != VALUE_UNDEFINED) g_Yytk->CallBuiltin("variable_instance_set", { ui, RValue("achievement_page"), RValue(0.0) });
			}
			AchList = -1;
			return;
		}
		if (Action.starts_with("slot:"))
		{
			int k = std::atoi(Action.c_str() + 5);
			SlotList = SlotList == k || k < 0 || k > 2 ? -1 : k;
		}
		else if (Action.starts_with("pick:"))
		{
			int k = std::atoi(Action.c_str() + 5);
			size_t colon = Action.find(':', 5);
			int ch = colon == std::string::npos ? -1 : std::atoi(Action.c_str() + colon + 1);
			if (k >= 0 && k <= 2 && ValidCharacter(ch)) SlotShow[k] = ch;
			SlotList = -1;
		}
	}

	// ---- its skins ("skins": [{id, name, art}]): other looks for it, picked in the Character menu (menu
	// "inventory") from a dropdown on its name bar - its own look first ("skin_name", default "Default"). A skin's art
	// goes over its own (a game sprite given as itself: the game's); its look is built again (BuildLook) when the skin
	// picked changes. The pick is kept per class in its save data ("skins": {"picked": id}). The game's skins
	// (skin_data[hero] = [skin, colour], sprites named *_skin) are its base hero's: a class leaves skin_data alone.
	bool SkinOpen = false;
	constexpr double kSkinBarX = 218, kSkinBarY = 156, kSkinBarW = 224, kSkinBarH = 82;   // its name bar
	std::string PickedSkin(const Def& D)
	{
		const Json* v = AchData(D, "skins").Find("picked");
		return v && v->type == Json::Str ? v->s : "";
	}
	void ApplySkin(Def& D, bool Force = false)
	{
		std::string id = PickedSkin(D);
		const Json* skin = nullptr;
		for (auto& sk : D.Skins.a)
			if (sk.GetStr("id") == id) skin = &sk;
		if (!skin) id = "";
		if (!Force && id == D.SkinShown) return;
		D.Art = D.BaseArt;
		if (skin)
		{
			if (const Json* a = skin->Find("art"); a && a->type == Json::Obj)
				for (auto& [name, v] : a->o)
				{
					int from = static_cast<int>(AssetIndex(name.c_str()).ToDouble());
					int to = v.type == Json::Num ? static_cast<int>(v.n) : v.type == Json::Str ? static_cast<int>(AssetIndex(v.s.c_str()).ToDouble()) : -1;
					if (from >= 0 && to >= 0) D.Art[from] = to;
				}
		}
		D.SkinShown = id;
		BuildLook(D);
	}
	std::string SkinName(const Def& D, size_t Row)                // row 0: its own look
	{
		if (Row == 0) { std::string t = LangText(&D.SkinDefault, CurrentLang()); return t.empty() ? "Default" : t; }
		const Json& sk = D.Skins.a[Row - 1];
		std::string t = LangText(sk.Find("name"), CurrentLang());
		return t.empty() ? sk.GetStr("id") : t;
	}
	bool SkinsShown(CInstance* Ui)
	{
		Def* d = Content();
		if (!d || d->Skins.a.empty() || !Ui) return false;
		RValue menu = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Ui), RValue("menu_show") });
		return KindOf(menu) == VALUE_STRING && menu.ToString() == "inventory";
	}
	void PostInventory(CInstance* Self, int, RValue**, RValue&)
	{
		if (!Self) return;
		RValue mba = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Self), RValue("mba") });
		if (KindOf(mba) == VALUE_ARRAY) MbaRemovePrefix(mba, "slormapi:class:kskin:");
		if (!SkinsShown(Self)) { SkinOpen = false; return; }
		EvalCtx c{ Self, Self, 0, nullptr };
		ClassEntry(c, "kskin:open", kSkinBarX, kSkinBarY, kSkinBarW, kSkinBarH);
		if (SkinOpen)
			for (size_t k = 0; k <= Content()->Skins.a.size(); ++k)
				ClassEntry(c, "kskin:" + std::to_string(k), kSkinBarX + (kSkinBarW - kRowW) / 2, kSkinBarY + kSkinBarH + k * kRowH, kRowW, kRowH);
	}
	void DrawSkinOverlay(CInstance* Ui)
	{
		if (!SkinsShown(Ui)) { SkinOpen = false; return; }
		Def& d = *Content();
		EvalCtx c{ Ui, Ui, 0, nullptr };
		c.Overlay = true;
		DrawListArrow(c, kSkinBarX + kSkinBarW - 20, kSkinBarY + kSkinBarH - 16, 0.6, SkinOpen);
		if (!SkinOpen) return;
		int lit = 0;                                                  // the default look, else the picked skin
		for (size_t k = 0; k < d.Skins.a.size(); ++k)
			if (!d.SkinShown.empty() && d.Skins.a[k].GetStr("id") == d.SkinShown) lit = static_cast<int>(k) + 1;
		DrawListRows(c, kSkinBarX + (kSkinBarW - kRowW) / 2, kSkinBarY + kSkinBarH, static_cast<int>(d.Skins.a.size()) + 1, lit,
			[&](int K) { return SkinName(d, static_cast<size_t>(K)); });
	}
	void SkinClick(const std::string& What)
	{
		if (What == "open") { SkinOpen = !SkinOpen; return; }
		SkinOpen = false;
		Def* d = Content();
		size_t k = static_cast<size_t>(std::atoi(What.c_str()));
		if (!d || k > d->Skins.a.size()) return;
		AchData(*d, "skins").Set("picked", k == 0 ? std::string() : d->Skins.a[k - 1].GetStr("id"));
		ApplySkin(*d);
	}

	// ---- its achievements. The game's table (global.achievements, 225 rows): [0] REF (= its index), [1] ORDER, [2] CAT
	// (category: 0-3, the heroes 4-6, villagers 7), [5..9] steps, [10] STEAM_ID, [13] STAT, [14] ICON, [15] HERO,
	// [16] UPON, [17] name, [18] description (+2 per language: "$" the step, [hero_<n>] a hero's name, [<n>-<k>] a skill's).
	// Its count: scr_achievement_get_progression(row) = [steps done, steps, value, next step], worked out from the save's
	// data each time (no progress is kept). A class's achievements are rows after the game's - its base hero's category
	// mirrored (counted by the game's own code with its data exchanged with its base hero's, SwapHeroes) and its own -
	// in a view of the table that is global.achievements only while the Profile, its step and the notifications run
	// (AchEnter): there the three hero categories show the characters of the Profile's three slots (SlotShow; a dropdown
	// on each category), a class's own category is 8 + its index otherwise. The game's code elsewhere (its checks, Steam,
	// the account's totals) sees its 225 rows. Steps done are kept per class in its save data ("achievements_done");
	// a new one shows the game's "New Achievement Completed" (never Steam) and raises "class_achievement".
	constexpr size_t kAchCat = 2, kAchSteps = 5, kAchSteam = 10, kAchStat = 13, kAchIcon = 14, kAchHero = 15, kAchUpon = 16,
		kAchText = 17;
	extern RValue OrigElements;                              // (the legacy tree's, below)
	extern bool HaveOrigElements;
	extern int ElementsFor;
	RValue AchGame;                      // the game's table
	size_t AchGameRows = 0;
	RValue AchView, AchHeld;
	std::string AchViewMade;
	int AchDepth = 0;
	bool AchIn = false, AchPass = false, AchNaming = false;
	int AchVersion = 0;
	// a row's copy, cell by cell (through JSON its undefined cells would come back as empty arrays)
	RValue RowCopy(const RValue& Row)
	{
		std::vector<RValue> v;
		for (size_t k = 0, n = ArrayLen(Row); k < n; ++k) v.push_back(*ArrayAt(const_cast<RValue&>(Row), k));
		return RValue(v);
	}
	bool CaptureAchievements()
	{
		if (AchGameRows) return true;
		RValue* g = FindMember(GlobalRValue(), "achievements");
		if (!g || KindOf(*g) != VALUE_ARRAY || ArrayLen(*g) == 0) return false;
		AchGame = *g;
		AchGameRows = ArrayLen(AchGame);
		Anchor("achievements_game", AchGame);
		return true;
	}
	// the category a character's rows show in: its slot's hero category, else none - "parked" (the Profile sums every row
	// into an array of the game's 8 categories by its category number, and lists as many: a parked row is in category 0,
	// counts nothing there, and is out of the pages - category -1 while scr_achievement_pagination makes them)
	int AchCategory(int Character)
	{
		for (int k = 0; k < 3; ++k)
			if (SlotShow[k] == Character) return 4 + k;
		return -1;
	}
	std::vector<bool> AchParked;                                 // by row of the view
	// the UI's arrays by achievement row (its "new" marks, its shown step): as long as the view
	void AchUiRows(size_t Rows)
	{
		RValue ui = UiValue();
		if (KindOf(ui) == VALUE_UNDEFINED) return;
		for (const char* name : { "achievements_discovered", "single_achievement_step" })
			if (RValue* a = FindMember(ui, name); a && KindOf(*a) == VALUE_ARRAY && ArrayLen(*a) < Rows)
			{
				std::vector<RValue> v;
				for (size_t k = 0, n = ArrayLen(*a); k < n; ++k) v.push_back(*ArrayAt(*a, k));
				while (v.size() < Rows) v.push_back(RValue(0.0));
				*a = RValue(v);
			}
	}
	void MakeAchView()
	{
		for (int& c : SlotShow) if (!ValidCharacter(c)) c = static_cast<int>(&c - SlotShow);
		std::string made = std::to_string(AchVersion) + ":" + std::to_string(SlotShow[0]) + "," + std::to_string(SlotShow[1]) + "," + std::to_string(SlotShow[2]);
		if (made != AchViewMade)
		{
			std::vector<RValue> rows;
			AchParked.clear();
			auto category = [](int C) { return RValue(static_cast<double>((std::max)(C, 0))); };
			for (size_t r = 0; r < AchGameRows; ++r)
			{
				RValue row = *ArrayAt(AchGame, r);
				RValue* cat = ArrayAt(row, kAchCat);
				int c = cat && IsNumeric(*cat) ? static_cast<int>(cat->ToDouble()) : -1;
				bool moved = c >= 4 && c <= 6 && AchCategory(c - 4) != c;
				if (moved)
				{
					row = RowCopy(row);
					*ArrayAt(row, kAchCat) = category(AchCategory(c - 4));
				}
				AchParked.push_back(moved && AchCategory(c - 4) < 0);
				rows.push_back(row);
			}
			ForEachContent([&](Def& d)                             // (the classes', then the game heroes': in their own category)
			{
				for (auto& a : d.Achievements)
				{
					int c = AchCategory(HeroOf(d));
					*ArrayAt(a.Row, 0) = RValue(static_cast<double>(rows.size()));
					*ArrayAt(a.Row, kAchCat) = category(c);
					AchParked.push_back(c < 0);
					rows.push_back(a.Row);
				}
			});
			AchView = RValue(rows);
			Anchor("achievements_view", AchView);
			AchViewMade = made;
		}
		AchUiRows(ArrayLen(AchView));
	}
	bool AchNeeded()
	{
		if (!AchGameRows) return false;
		for (int k = 0; k < 3; ++k) if (SlotShow[k] != k) return true;
		bool any = false;
		ForEachContent([&](Def& d) { any |= !d.Achievements.empty(); });
		return any;
	}
	void AchEnter()
	{
		if (AchDepth++ > 0 || AchIn || !AchNeeded()) return;
		RValue* g = FindMember(GlobalRValue(), "achievements");
		if (!g || KindOf(*g) != VALUE_ARRAY) return;
		MakeAchView();
		AchHeld = *g;
		*g = AchView;
		AchIn = true;
	}
	void AchLeave()
	{
		if (AchDepth > 0 && --AchDepth > 0) return;
		if (!AchIn) return;
		if (RValue* g = FindMember(GlobalRValue(), "achievements")) *g = AchHeld;
		AchHeld = RValue();
		AchIn = false;
	}
	// the pages: without the parked rows
	int PagingDepth = 0;
	bool PagingParked = false;
	void ParkedCategory(double From, double To)
	{
		for (size_t r = 0; r < AchParked.size(); ++r)
			if (AchParked[r])
				if (RValue* row = ArrayAt(AchView, r); row && KindOf(*row) == VALUE_ARRAY)
					if (RValue* c = ArrayAt(*row, kAchCat); c && IsNumeric(*c) && c->ToDouble() == From) *c = RValue(To);
	}
	bool PrePaging(CInstance*, int, RValue**, RValue&) { if (PagingDepth++ == 0 && AchIn) { ParkedCategory(0, -1); PagingParked = true; } return false; }
	void PostPaging(CInstance*, int, RValue**, RValue&) { if (--PagingDepth <= 0) { PagingDepth = 0; if (PagingParked) ParkedCategory(-1, 0); PagingParked = false; } }
	// a notice clicked opens the Achievements at its row's category: while the notices run, a parked class's rows are in
	// category 100 + its index; clicked, the class takes its base hero's row (as its dropdown does) and that category
	bool AchMarked = false;
	void MarkParkedClasses(bool On)
	{
		size_t r = AchGameRows;
		for (size_t i = 0; i < Defs.size(); ++i)
			for (size_t k = 0; k < Defs[i].Achievements.size(); ++k, ++r)
				if (r < AchParked.size() && AchParked[r])
					if (RValue* row = ArrayAt(AchView, r); row && KindOf(*row) == VALUE_ARRAY)
						if (RValue* c = ArrayAt(*row, kAchCat)) *c = RValue(On ? 100.0 + static_cast<double>(i) : 0.0);
		AchMarked = On;
	}
	void PickCharacter(int Row, int Character);
	// at the start of each frame (the UI's step, outside any script): a window whose Post did not run - a GML error inside
	// the hooked script - is closed, the parked rows put back, a lent citizen tag restored
	void CloseStepWindow();
	void ResetWindows()
	{
		CloseStepWindow();                                       // a class's step window left open (its call did not return)
		CloseCallWindow();                                       // a cast's or a skill text's, the same
		if (!Took.empty() || Saving)                             // a save's Post did not run: its custom heroes' entries back
		{                                                        // (else every later save skips the trim: a save past 3 heroes)
			RValue none;
			PostSave(nullptr, 0, nullptr, none);
		}
		if (AchMarked) MarkParkedClasses(false);
		if (PagingParked) ParkedCategory(-1, 0);
		PagingParked = false;
		PagingDepth = 0;
		if (AchDepth > 0 || AchIn) { AchDepth = 1; AchLeave(); }
		AchNaming = false;
		RestoreLentTag();
	}
	bool PreAchWindow(CInstance*, int, RValue**, RValue&)
	{
		AchEnter();
		if (AchIn && AchDepth == 1) MarkParkedClasses(true);
		return false;
	}
	void PostAchWindow(CInstance*, int, RValue**, RValue&)
	{
		if (AchMarked && AchDepth == 1)
		{
			MarkParkedClasses(false);
			RValue ui = UiValue();
			RValue cat = KindOf(ui) == VALUE_UNDEFINED ? RValue() : g_Yytk->CallBuiltin("variable_instance_get", { ui, RValue("selected_ach_cat") });
			double c = IsNumeric(cat) ? cat.ToDouble() : -1;
			if (c >= 100 && c < 100 + static_cast<double>(Defs.size()))      // a parked class's notice was clicked
			{
				const Def& d = Defs[static_cast<size_t>(c - 100)];
				int row = d.Base;
				for (int k = 0; k < 3; ++k) if (SlotShow[k] == d.Base) row = k;
				PickCharacter(row, 3 + static_cast<int>(c - 100));
				g_Yytk->CallBuiltin("variable_instance_set", { ui, RValue("selected_ach_cat"), RValue(4.0 + row) });
			}
		}
		AchLeave();
	}

	// a class's achievement by its row in the view
	Def::Achievement* AchievementAt(double Ref, Def** Of = nullptr)
	{
		size_t r = static_cast<size_t>(Ref);
		if (Ref < static_cast<double>(AchGameRows) || !AchGameRows) return nullptr;
		r -= AchGameRows;
		for (auto& d : Defs)
		{
			if (r < d.Achievements.size()) { if (Of) *Of = &d; return &d.Achievements[r]; }
			r -= d.Achievements.size();
		}
		return nullptr;
	}

	// the game's count of a row of its own table (with the game's legacy table and the three heroes in place)
	RValue GameProgression(CInstance* Self, double Ref)
	{
		RValue r;
		RValue* el = FindMember(GlobalRValue(), "elements");
		RValue heldElements;
		bool elements = el && HaveOrigElements && ElementsFor >= 0;
		if (elements) { heldElements = *el; *el = OrigElements; }
		RValue* ach = FindMember(GlobalRValue(), "achievements");
		RValue heldView;
		if (AchIn && ach) { heldView = *ach; *ach = AchHeld; }
		RValue dummy;
		PreThreeHeroes(nullptr, 0, nullptr, dummy);
		AchPass = true;
		struct Call { CInstance* Self; double Ref; RValue* Out; } call{ Self, Ref, &r };
		DWORD code = RunGuarded([](void* P)                       // (an access violation in the game's code: no count)
		{
			auto* c = static_cast<Call*>(P);
			try { g_Yytk->CallGameScriptEx(*c->Out, "gml_Script_scr_achievement_get_progression", c->Self, c->Self, { RValue(c->Ref) }); }
			catch (...) { *c->Out = RValue(); }
		}, &call);
		if (code)
		{
			static int told = 0;
			if (told++ < 3) DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormAPI] achievements: row %.0f's count failed (0x%08lX)", Ref, code);
			r = RValue();
		}
		AchPass = false;
		PostThreeHeroes(nullptr, 0, nullptr, dummy);
		if (AchIn && ach) *ach = heldView;
		if (elements) *el = heldElements;
		if (KindOf(r) == VALUE_ARRAY && ArrayLen(r) >= 4) return r;
		static int odd = 0;
		if (odd++ < 5) DbgPrintEx(LOG_SEVERITY_TRACE, "[SlormAPI] achievements: row %.0f counted as %s", Ref, sj::Dump(ToJson(r, 2)).c_str());
		return RValue(std::vector<RValue>{ RValue(0.0), RValue(1.0), RValue(0.0), RValue(1.0) });   // (its callers read 4 values)
	}

	// its rows: its base hero's category (copies: Steam id none, its name for its base hero's) then its own
	void BuildAchievements(Def& D)
	{
		D.Achievements.clear();
		++AchVersion;
		if (!CaptureAchievements()) return;
		int hero = HeroOf(D);
		auto texts = [&](RValue& Row, const std::function<std::string(size_t, const std::string&)>& Text)
		{
			for (size_t c = kAchText, n = ArrayLen(Row); c < n; ++c)
				if (RValue* v = ArrayAt(Row, c); v && KindOf(*v) == VALUE_STRING) *v = RStr(Text(c, v->ToString()));
		};
		auto replace = [](std::string S, const std::string& From, const std::string& To)
		{
			for (size_t at = S.find(From); at != std::string::npos; at = S.find(From, at + To.size())) S.replace(at, From.size(), To);
			return S;
		};
		if (D.AchievementsMirror)
			for (size_t r = 0; r < AchGameRows; ++r)
			{
				RValue* src = ArrayAt(AchGame, r);
				RValue* cat = src ? ArrayAt(*src, kAchCat) : nullptr;
				if (!cat || !IsNumeric(*cat) || static_cast<int>(cat->ToDouble()) != 4 + D.Base) continue;
				Def::Achievement a;
				a.Id = "base_" + std::to_string(r);
				a.Source = static_cast<int>(r);
				a.Row = RowCopy(*src);
				*ArrayAt(a.Row, kAchSteam) = RValue();
				std::string b = std::to_string(D.Base), h = std::to_string(hero);
				texts(a.Row, [&](size_t, const std::string& T) { return replace(replace(T, "[hero_" + b + "]", "[hero_" + h + "]"), "[" + b + "-", "[" + h + "-"); });
				D.Achievements.push_back(std::move(a));
			}
		RValue* tmpl = ArrayAt(AchGame, 2);                      // a one-step row of the game's, as the form
		if (!tmpl) return;
		int order = 100;
		auto baseRef = [](const Json& J) -> int
		{
			const Json* b = J.Find("base");
			if (!b) return -1;
			if (b->type == Json::Num) return static_cast<int>(b->n);
			return b->type == Json::Str && b->s.starts_with("base_") ? std::atoi(b->s.c_str() + 5) : -2;
		};
		for (auto& j : D.AchievementDefs.a)                         // its base hero's, renamed
		{
			int ref = j.type == Json::Obj ? baseRef(j) : -1;
			if (ref < 0) continue;
			for (auto& a : D.Achievements)
			{
				if (a.Source != ref) continue;
				texts(a.Row, [&](size_t C, const std::string& T)
				{
					std::string t = LangText(j.Find((C - kAchText) % 2 == 0 ? "name" : "description"), (C - kAchText) / 2);
					return t.empty() ? T : t;
				});
				if (const Json* icon = j.Find("icon"); icon && icon->type == Json::Num) *ArrayAt(a.Row, kAchIcon) = RValue(icon->n);
			}
		}
		for (auto& j : D.AchievementDefs.a)
		{
			if (j.type != Json::Obj || j.Find("base")) continue;
			Def::Achievement a;
			a.Id = j.GetStr("id");
			if (a.Id.empty()) Fail("achievements: each needs an id");
			a.Stat = j.GetStr("stat");
			if (const Json* st = j.Find("steps"); st && st->type == Json::Arr)
				for (auto& v : st->a) if (v.type == Json::Num && a.Steps.size() < kAchSteps) a.Steps.push_back(v.n);
			if (const Json* st = j.Find("steps"); st && st->type == Json::Num) a.Steps.push_back(st->n);
			if (a.Steps.empty()) a.Steps.push_back(1);
			a.Row = RowCopy(*tmpl);
			*ArrayAt(a.Row, 1) = RValue(static_cast<double>(order++));
			for (size_t k = 0; k < kAchSteps; ++k) *ArrayAt(a.Row, 5 + k) = k < a.Steps.size() ? RValue(a.Steps[k]) : RValue();
			*ArrayAt(a.Row, kAchSteam) = RValue();
			*ArrayAt(a.Row, kAchStat) = RStr("");
			*ArrayAt(a.Row, kAchIcon) = RValue(j.GetNum("icon", 13));
			*ArrayAt(a.Row, kAchHero) = RValue();
			*ArrayAt(a.Row, kAchUpon) = RValue();
			texts(a.Row, [&](size_t C, const std::string&)
			{
				std::string t = LangText(j.Find((C - kAchText) % 2 == 0 ? "name" : "description"), (C - kAchText) / 2);
				return t.empty() && (C - kAchText) % 2 == 0 ? a.Id : t;
			});
			D.Achievements.push_back(std::move(a));
		}
		for (auto& a : D.Achievements)
			if (a.Source < 0) { a.Progress[1] = static_cast<double>(a.Steps.size()); a.Progress[3] = a.Steps[0]; }
		D.AchDirty = true;
		AchViewMade.clear();
		MakeAchView();
	}

	// the categories' names in the view: a slot's character's (a class: its title), a class's own category
	bool AchCategoryString(CInstance* Self, const std::string& Key, RValue& Result)
	{
		if (!AchIn || AchNaming) return false;
		int n = std::atoi(Key.c_str() + 16);
		int c = n >= 4 && n <= 6 ? SlotShow[n - 4] : -1;
		if (!ValidCharacter(c) || c == n - 4) return false;
		if (c >= 3) { Result = RStr(ClassTitle(Defs[static_cast<size_t>(c) - 3], CurrentLang())); return true; }
		struct Naming { Naming() { AchNaming = true; } ~Naming() { AchNaming = false; } } naming;
		RValue r;
		g_Yytk->CallGameScriptEx(r, "gml_Script_scr_string", Self, Self, { RStr("achievement_cat_" + std::to_string(4 + c)) });
		Result = r;
		return true;
	}

	// its value of its own achievement (its save data, or its player stat)
	Json& AchData(const Def& D, const char* Group)
	{
		Json& d = SlotData[D.Key()];
		if (d.type != Json::Obj) d = Json::Object();
		return Child(d, Group);
	}
	double AchValue(const Def& D, const Def::Achievement& A)
	{
		if (!A.Stat.empty())
		{
			RValue* track = FindMember(GlobalRValue(), "stats_track");
			RValue* stats = FindMember(GlobalRValue(), "player_stats");
			if (!track || !stats || KindOf(*stats) != VALUE_ARRAY) return 0;
			RValue at = g_Yytk->CallBuiltin("ds_map_find_value", { *track, RStr(A.Stat) });
			RValue* mine = ArrayAt(*stats, static_cast<size_t>(HeroOf(D)));
			RValue* v = mine && IsNumeric(at) && KindOf(*mine) == VALUE_ARRAY ? ArrayAt(*mine, static_cast<size_t>(at.ToDouble())) : nullptr;
			return v && IsNumeric(*v) ? v->ToDouble() : 0;
		}
		const Json* v = AchData(D, "achievements").Find(A.Id);
		return v && v->type == Json::Num ? v->n : 0;
	}

	// every half second, in game: each class's counts, and its newly done steps
	void RefreshAchievements()
	{
		if (!AchGameRows || !Took.empty() || Loading || Saving || SaveSlot() < 0 || !FirstInstance(AssetIndex("obj_parent_hero"))) return;
		RValue uiv = UiValue();
		if (KindOf(uiv) == VALUE_UNDEFINED) return;
		RValue menu = g_Yytk->CallBuiltin("variable_instance_get", { uiv, RValue("menu_show") });
		if (KindOf(menu) == VALUE_STRING && menu.ToString() == "main") return;   // (in game only)
		static bool told = false;
		if (!told) { told = true; DbgPrintEx(LOG_SEVERITY_INFO, "[SlormAPI] achievements: counting %zu classes and the game heroes' mods", Defs.size()); }

		RValue id = modspage::UiInstance();
		CInstance* ui = KindOf(id) == VALUE_UNDEFINED ? nullptr : InstanceById(static_cast<int32_t>(id.ToInt64()));
		if (!ui) return;
		bool profile = KindOf(menu) == VALUE_STRING && menu.ToString() == "profile";
		Def* played = Content();
		double first = static_cast<double>(AchGameRows);            // its first row in the view (MakeAchView's order)
		std::vector<Def*> all;
		ForEachContent([&](Def& d) { all.push_back(&d); });
		for (Def* dp : all)
		{
			Def& d = *dp;
			double mine = first;
			first += static_cast<double>(d.Achievements.size());
			if (d.Achievements.empty() || !(&d == played || profile || d.AchDirty)) continue;   // (counts change while played)
			d.AchDirty = false;
			bool mirrored = false;
			for (auto& a : d.Achievements) mirrored |= a.Source >= 0;
			if (mirrored)
			{
				SwapHeroes(static_cast<size_t>(d.Base), static_cast<size_t>(HeroOf(d)));
				for (auto& a : d.Achievements)
					if (a.Source >= 0)
					{
						RValue r = GameProgression(ui, a.Source);
						for (size_t k = 0; k < 4; ++k)
						{
							RValue* v = ArrayAt(r, k);
							a.Progress[k] = v && IsNumeric(*v) && KindOf(*v) != VALUE_STRING ? v->ToDouble() : (k % 2 ? 1.0 : 0.0);
						}
					}
				SwapHeroes(static_cast<size_t>(d.Base), static_cast<size_t>(HeroOf(d)));
			}
			for (auto& a : d.Achievements)
			{
				if (a.Source >= 0) continue;
				double v = AchValue(d, a), done = 0;
				for (double st : a.Steps) done += v >= st;
				double next = done < a.Steps.size() ? a.Steps[static_cast<size_t>(done)] : a.Steps.back();
				a.Progress[0] = done; a.Progress[1] = static_cast<double>(a.Steps.size()); a.Progress[2] = v; a.Progress[3] = next;
			}
			Json& rec = AchData(d, "achievements_done");
			for (size_t k = 0; k < d.Achievements.size(); ++k)
			{
				auto& a = d.Achievements[k];
				double done = a.Progress[0];
				const Json* was = rec.Find(a.Id);
				if (!was || was->type != Json::Num) { rec.Set(a.Id, done); continue; }   // first seen: as it is, no notice
				if (done <= was->n) continue;
				rec.Set(a.Id, done);
				double ref = mine + static_cast<double>(k);
				AchEnter();                                           // the game's notice (it reads the row in the view)
				RValue r;
				try { g_Yytk->CallGameScriptEx(r, "gml_Script_scr_add_notification", ui, ui, { RStr("achievement"), RValue(ref) }); }
				catch (...) {}
				AchLeave();
				Json e = Json::Object();
				e.Add("class", d.Key());
				e.Add("id", a.Id);
				e.Add("step", done);
				e.Add("steps", a.Progress[1]);
				g_DirectEvents.emplace_back("class_achievement", std::move(e));
			}
		}
	}

	// the achievements panel: each hero category's dropdown (a selector arrow on its row) and the open one's characters
	constexpr double kAchRowY = 156, kAchRowH = 96;                       // category c's row: kAchRowY + c * kAchRowH
	constexpr double kAchIconX = 141, kAchIconY = 169;                      // its icon (spr_achievement_back, 64x64)
	constexpr double kAchArrowX = 440, kAchArrowW = 84, kAchListX = 532;
	int AchListRows(double& X, double& Y)
	{
		int n = 3 + static_cast<int>(Defs.size());
		X = kAchListX;
		Y = kAchRowY + (4 + AchList) * kAchRowH;
		if (Y + n * kRowH > 1060) Y = 1060 - n * kRowH;
		return n;
	}
	void PushAchEntries(CInstance*, const std::function<void(const std::string&, double, double, double, double)>& Entry)
	{
		for (int k = 0; k < 3; ++k) Entry("aslot:" + std::to_string(k), kAchArrowX, kAchRowY + (4 + k) * kAchRowH + 6, kAchArrowW, 44);
		if (AchList < 0 || AchList > 2) return;
		double x, y;
		int n = AchListRows(x, y);
		for (int ch = 0; ch < n; ++ch) Entry("apick:" + std::to_string(AchList) + ":" + std::to_string(ch), x, y + ch * kRowH, kRowW, kRowH);
	}
	// a class's category icon: its art (one frame), else its base hero's
	int AchIcon(Def& D)
	{
		int spr = Named("spr_achievement_back");
		if (spr < 0) return -1;
		if (D.CategoryIcon >= 0) return D.CategoryIcon;
		if (auto a = D.Art.find(spr); a != D.Art.end()) return D.CategoryIcon = a->second;
		// as the Profile draws a category: its background (spr_achievement_back, frame = the category) and its emblem over it
		// (spr_achievement_high x4, frame by category: Knight 13, Huntress 1, Mage 19) - its base hero's
		const double emblem[3] = { 13, 1, 19 };
		RValue surf = g_Yytk->CallBuiltin("surface_create", { RValue(64.0), RValue(64.0) });
		g_Yytk->CallBuiltin("surface_set_target", { surf });
		g_Yytk->CallBuiltin("draw_clear_alpha", { RValue(0.0), RValue(0.0) });
		g_Yytk->CallBuiltin("draw_sprite", { RValue(static_cast<double>(Original(spr))), RValue(4.0 + D.Base), RValue(0.0), RValue(0.0) });
		g_Yytk->CallBuiltin("draw_sprite_ext", { AssetIndex("spr_achievement_high"), RValue(emblem[D.Base]), RValue(0.0), RValue(0.0), RValue(4.0), RValue(4.0),
			RValue(0.0), RValue(16777215.0), RValue(1.0) });
		g_Yytk->CallBuiltin("surface_reset_target", {});
		D.CategoryIcon = static_cast<int>(g_Yytk->CallBuiltin("sprite_create_from_surface", { surf, RValue(0.0), RValue(0.0), RValue(64.0), RValue(64.0),
			RValue(false), RValue(false), RValue(0.0), RValue(0.0) }).ToDouble());
		g_Yytk->CallBuiltin("surface_free", { surf });
		if (D.CategoryIcon >= 0) D.Made.push_back(D.CategoryIcon);
		return D.CategoryIcon;
	}
	void DrawAchOverlay(CInstance* Ui)
	{
		EvalCtx c{ Ui, Ui, 0, nullptr };
		c.Overlay = true;
		for (int k = 0; k < 3; ++k)                                   // the shown characters' icons over the slots' heroes'
		{
			int ch = SlotShow[k];
			if (ch == k || !ValidCharacter(ch)) continue;
			RValue spr;
			double frame = 0;
			if (ch < 3) { spr = AssetIndex("spr_achievement_back"); frame = 4 + ch; }
			else if (int icon = AchIcon(Defs[static_cast<size_t>(ch) - 3]); icon >= 0) spr = RValue(static_cast<double>(icon));
			if (IsNumeric(spr)) WidgetSprite(c, spr, frame, kAchIconX, kAchIconY + (4 + k) * kAchRowH);
		}
		for (int k = 0; k < 3; ++k)                                   // at each row's right
			DrawListArrow(c, kAchArrowX + kAchArrowW / 2 + 20, kAchRowY + (4 + k) * kAchRowH + 28, 0.75, AchList == k);
		if (AchList < 0 || AchList > 2) return;
		double x, y;
		int n = AchListRows(x, y);
		DrawListRows(c, x, y, n, SlotShow[AchList], [&](int Ch) { return CharacterName(Ch, Ui); });
	}

	// ---- its Ancestral Legacy tree
	// global.elements rows: [1] REALM, [2] REF, [3] GENRE, [4] REALM_COLOR, [5] COST, [6] COST_LEVEL, [7] COST_TYPE,
	// [9] DESC_VALUE, [10] base, [11] per rank, [12] value type, [13] DESC_VALUE_REAL, [14] COOLDOWN, [15] DMG_TYPE,
	// [18] UPGRADE_NUMBER (max rank), [19] TYPE, [20] AURA_BUFF_NAME, [22] EXTRA_NBR, [23] name, [24] description (+2 per
	// language). The tree shows per realm the first rows (in row order) whose REALM equals it, 1 or 2 by the realm's
	// size: a hidden game legacy gets REALM + 0.5 - equal to no realm, still realm r where game code indexes by it (the
	// game's check then answers 0). The class's legacies come after the game's 150 rows.
	const std::map<std::string, size_t> kLegacyColumns = { { "realm", 1 }, { "genre", 3 }, { "colour", 4 }, { "cost", 5 },
		{ "cost_per_rank", 6 }, { "cost_type", 7 }, { "stat", 9 }, { "value", 10 }, { "per_rank", 11 }, { "value_type", 12 },
		{ "real", 13 }, { "cooldown", 14 }, { "dmg_type", 15 }, { "max", 18 }, { "kind", 19 }, { "buff", 20 }, { "extra", 22 } };
	constexpr size_t kLegacyRealm = 1, kLegacyRef = 2, kLegacyType = 19, kLegacyName = 23, kGameLegacies = 150;

	RValue OrigElements;                 // the game's table
	bool HaveOrigElements = false;
	int OrigElementIcons = -1;           // spr_elements as the game made it (a copy)
	int ElementsFor = -1;                // the hero whose content's legacy table is in place

	double LegacyRealm(RValue& Row)
	{
		RValue* c = ArrayAt(Row, kLegacyRealm);
		if (!c) return -1;
		if (IsNumeric(*c) && KindOf(*c) != VALUE_STRING) return c->ToDouble();
		try { return std::stod(CellText(*c)); } catch (...) { return -1; }
	}

	std::string LegacyBuffKey(const Def& D, size_t Row)
	{
		return "slormapi_legacy_" + (D.Game ? "h" + std::to_string(D.Base) : std::to_string(&D - Defs.data())) + "_" + std::to_string(Row);
	}
	void BuildElements(Def& D)
	{
		RValue base = FromJson(ToJson(OrigElements, 4));
		size_t n = (std::min)(ArrayLen(base), kGameLegacies);
		std::set<int> used;
		for (auto& l : D.Legacies.a) if (l.type == Json::Obj) used.insert(static_cast<int>(l.GetNum("realm", -1)));
		std::map<std::string, RValue> tpl;                        // first game row of each TYPE
		std::vector<RValue> rows;
		D.HiddenLegacies.clear();
		for (size_t r = 0; r < n; ++r)
		{
			RValue row = FromJson(ToJson(*ArrayAt(base, r), 2));
			RValue* t = ArrayAt(row, kLegacyType);
			std::string type = t && KindOf(*t) == VALUE_STRING ? t->ToString() : "";
			if (!tpl.count(type)) tpl[type] = FromJson(ToJson(row, 2));
			double realm = LegacyRealm(row);
			if (realm >= 0 && (D.LegacyBaseHidden || used.count(static_cast<int>(realm))))
				if (RValue* c = ArrayAt(row, kLegacyRealm)) { *c = RValue(std::floor(realm) + 0.5); D.HiddenLegacies.insert(static_cast<int>(r)); }
			rows.push_back(row);
		}
		D.ElementPassives.clear();
		for (auto& l : D.Legacies.a)
		{
			if (l.type != Json::Obj) continue;
			std::string kind = l.GetStr("kind", "stat");
			auto t = tpl.find(kind);
			if (t == tpl.end()) t = tpl.find("stat");
			if (t == tpl.end()) break;
			RValue row = FromJson(ToJson(t->second, 2));
			for (size_t c : { 3, 9, 10, 11, 12, 13, 20, 22 }) Blank(row, c);
			size_t id = rows.size();
			SetTexts(row, l, kLegacyName);
			for (auto& [k, v] : l.o)
			{
				auto col = kLegacyColumns.find(k);
				if (col == kLegacyColumns.end()) continue;
				Json val = v;
				if (v.type == Json::Arr)
				{
					std::string s;
					for (size_t i = 0; i < v.a.size(); ++i) s += (i ? "|" : "") + (v.a[i].type == Json::Str ? v.a[i].s : sj::Dump(v.a[i]));
					val = Json(s);
				}
				if (RValue* cell = ArrayAt(row, col->second)) *cell = CellValue(*cell, val);
			}
			if (RValue* ref = ArrayAt(row, kLegacyRef)) *ref = CellValue(*ref, Json(static_cast<double>(id)));
			if (kind == "aura")
				if (RValue* buff = ArrayAt(row, 20)) *buff = RStr(LegacyBuffKey(D, id));
			AlignValues(row, 9);
			if (kind == "stat") D.ElementPassives += "," + std::to_string(id);
			rows.push_back(row);
		}
		D.Elements = RValue(rows);
		Anchor("elements_hero_" + std::to_string(HeroOf(D)), D.Elements);
	}

	// spr_elements as the class sees it: the game's 150 icons, then its own ("icon" as for skills; default: the icon of
	// the first game legacy of its realm) - each from its art for spr_elements when it has one (frames it lacks: the game's)
	int BuildElementIcons(Def& D)
	{
		int spr = static_cast<int>(AssetIndex("spr_elements").ToDouble());
		if (OrigElementIcons < 0) OrigElementIcons = static_cast<int>(g_Yytk->CallBuiltin("sprite_duplicate", { RValue(static_cast<double>(spr)) }).ToDouble());
		int games = static_cast<int>(g_Yytk->CallBuiltin("sprite_get_number", { RValue(static_cast<double>(OrigElementIcons)) }).ToDouble());
		int from = OrigElementIcons, fromFrames = games;
		if (auto a = D.Art.find(spr); a != D.Art.end())
		{
			from = a->second;
			fromFrames = static_cast<int>(g_Yytk->CallBuiltin("sprite_get_number", { RValue(static_cast<double>(from)) }).ToDouble());
		}
		auto icon = [&](int Frame) { return Frame < fromFrames ? IconSrc{ from, Frame } : IconSrc{ OrigElementIcons, Frame }; };
		std::vector<IconSrc> src;
		for (int f = 0; f < games; ++f) src.push_back(icon(f));
		std::map<int, int> realmIcon;                            // realm -> its first game legacy
		for (size_t r = 0, n = (std::min)(ArrayLen(OrigElements), kGameLegacies); r < n; ++r)
		{
			double realm = LegacyRealm(*ArrayAt(OrigElements, r));
			if (realm >= 0 && !realmIcon.count(static_cast<int>(realm))) realmIcon[static_cast<int>(realm)] = static_cast<int>(r);
		}
		while (static_cast<int>(src.size()) < static_cast<int>(kGameLegacies)) src.push_back(icon(0));
		for (auto& l : D.Legacies.a)
		{
			int realm = static_cast<int>(l.GetNum("realm", -1));
			IconSrc def = icon(realmIcon.count(realm) ? realmIcon[realm] : 0);
			src.push_back(l.Find("icon") ? ParseIcon(l.Find("icon"), def.Sprite, def.Frame) : def);
		}
		if (D.ElementIcons >= 0) g_Yytk->CallBuiltin("sprite_delete", { RValue(static_cast<double>(D.ElementIcons)) });
		return D.ElementIcons = MakeStrip(src, OrigElementIcons);
	}

	// the class's per-legacy arrays (ranks; the menu's copy) as long as its legacy table
	// every hero's per-legacy arrays (ranks; the menu's copy) as long as the legacy table in place - the game's checks walk
	// the table's rows for each hero (scr_achievement_get_progression: elements_rank[hero][row], "every legacy at its
	// max"); the others' new ones are 0. (Hero: the one it is for - its own; the rest as long too)
	void PadLegacyRows(int Hero, size_t Rows)
	{
		(void)Hero;
		auto pad = [&](RValue* All)
		{
			if (!All || KindOf(*All) != VALUE_ARRAY) return;
			for (size_t h = 0, nh = ArrayLen(*All); h < nh; ++h)
			{
				RValue* mine = ArrayAt(*All, h);
				if (!mine || KindOf(*mine) != VALUE_ARRAY || ArrayLen(*mine) >= Rows) continue;
				std::vector<RValue> items;
				for (size_t k = 0, n = ArrayLen(*mine); k < n; ++k) items.push_back(*ArrayAt(*mine, k));
				while (items.size() < Rows) items.push_back(RValue(0.0));
				*mine = RValue(items);
			}
		};
		pad(FindMember(GlobalRValue(), "elements_rank"));
		RValue ui = UiValue();
		if (KindOf(ui) != VALUE_UNDEFINED) pad(FindMember(ui, "temp_elems"));
	}

	// a game hero's mods' legacies in its save: the game's save keeps the game's 150 ranks of each hero and its skill bar's
	// game legacies - a mod's legacy there would be a number past the game's table once the mod is gone. Its mods'
	// legacies' ranks and skill-bar places are kept in its SlormAPI save data by legacy ("legacies": {rank_of: {key: rank},
	// bar_of: [key | null per slot]}, key "<its mod>|<its id>": their rows move when its mods change), put back when its
	// table is in place again. Out of the game's arrays around the game's save for its time (Took), or for good when its
	// legacies change (AroundSave false: their ranks there 0, their places on the bar empty)
	std::string LegacyKey(const Def& D, size_t K)
	{
		std::string id = K < D.Legacies.a.size() ? D.Legacies.a[K].GetStr("id") : "";
		std::pair<std::string, int> from = K < D.LegacyFrom.size() ? D.LegacyFrom[K] : std::pair<std::string, int>{ D.Owner, static_cast<int>(K) };
		return from.first + "|" + (id.empty() ? "#" + std::to_string(from.second) : id);
	}
	// how many of the game's rows a legacy table has (BuildElements copies them; its own come after)
	size_t GameLegacyRows() { return HaveOrigElements ? (std::min)(ArrayLen(OrigElements), kGameLegacies) : kGameLegacies; }
	void KeepGameLegacies(Def& D, bool AroundSave)
	{
		if (!D.Game) return;
		const size_t games = GameLegacyRows();
		int h = D.Base;
		RValue g = GlobalRValue();
		Json& data = SlotData[D.Key()];
		if (data.type != Json::Obj) data = Json::Object();
		const Json* had = data.Find("legacies");
		// (the game's arrays hold its ranks once its saved ones are back, or when none were saved: else those stay)
		bool live = D.LegaciesRestored || !had || had->type != Json::Obj;
		Json rec = Json::Object();
		if (had && had->type == Json::Obj)
			for (auto& [k, v] : had->o)
				if (!live || (k != "ranks" && k != "bar")) rec.Set(k, v);    // (by row, saved before: as by legacy now)
		Json rankOf = rec.Find("rank_of") && rec.Find("rank_of")->type == Json::Obj ? *rec.Find("rank_of") : Json::Object();
		size_t n = D.Legacies.a.size();
		if (RValue* ranks = FindMember(g, "elements_rank"))
			if (RValue* mine = ArrayAt(*ranks, static_cast<size_t>(h)); mine && KindOf(*mine) == VALUE_ARRAY && ArrayLen(*mine) > games)
			{
				size_t len = ArrayLen(*mine);
				if (live)
					for (size_t k = 0; k < n && games + k < len; ++k)
						if (RValue* c = ArrayAt(*mine, games + k); c && IsNumeric(*c)) rankOf.Set(LegacyKey(D, k), c->ToDouble());
				if (AroundSave)
				{
					std::vector<RValue> keep;
					for (size_t k = 0; k < games; ++k) keep.push_back(*ArrayAt(*mine, k));
					Took.push_back({ mine, *mine });
					*mine = RValue(keep);
				}
				else if (live)
					for (size_t k = 0; k < n && games + k < len; ++k) *ArrayAt(*mine, games + k) = RValue(0.0);
			}
		Json barOf = Json::Array();
		if (RValue* bar = FindMember(g, "aura_array"))
			if (RValue* mine = ArrayAt(*bar, static_cast<size_t>(h)); mine && KindOf(*mine) == VALUE_ARRAY)
			{
				std::vector<RValue> keep;
				bool any = false;
				for (size_t k = 0, nb = ArrayLen(*mine); k < nb; ++k)
				{
					RValue v = *ArrayAt(*mine, k);
					bool mod = IsNumeric(v) && v.ToDouble() >= static_cast<double>(games);
					size_t li = mod ? static_cast<size_t>(v.ToDouble()) - games : 0;
					barOf.Push(mod && li < n ? Json(LegacyKey(D, li)) : Json());
					any |= mod;
					keep.push_back(mod ? RValue(-1.0) : v);
				}
				if (any)
				{
					if (AroundSave) Took.push_back({ mine, *mine });
					*mine = RValue(keep);
				}
			}
		if (live)
		{
			// a gone mod's legacies keep their places on the bar, as their ranks: back with it (into a slot still empty)
			std::set<std::string> have;
			for (size_t k = 0; k < n; ++k) have.insert(LegacyKey(D, k));
			if (const Json* old = rec.Find("bar_of"); old && old->type == Json::Arr)
				for (size_t k = 0; k < old->a.size(); ++k)
					if (old->a[k].type == Json::Str && !have.count(old->a[k].s))
					{
						while (barOf.a.size() <= k) barOf.Push(Json());
						if (barOf.a[k].type != Json::Str) barOf.a[k] = old->a[k];
					}
			rec.Set("rank_of", std::move(rankOf));
			rec.Set("bar_of", std::move(barOf));
		}
		data.Set("legacies", std::move(rec));
	}
	// a game hero's rank row past the game's 150 (padded while a table is in place): as the game's around a save
	void TrimGameRanks(int Hero)
	{
		RValue* ranks = FindMember(GlobalRValue(), "elements_rank");
		RValue* mine = ranks ? ArrayAt(*ranks, static_cast<size_t>(Hero)) : nullptr;
		const size_t games = GameLegacyRows();
		if (!mine || KindOf(*mine) != VALUE_ARRAY || ArrayLen(*mine) <= games) return;
		std::vector<RValue> keep;
		for (size_t k = 0; k < games; ++k) keep.push_back(*ArrayAt(*mine, k));
		Took.push_back({ mine, *mine });
		*mine = RValue(keep);
	}
	void RestoreGameLegacies(Def& D)
	{
		const Json* rec = SlotData.count(D.Key()) ? SlotData[D.Key()].Find("legacies") : nullptr;
		if (!rec || rec->type != Json::Obj || D.LegaciesRestored) return;
		D.LegaciesRestored = true;
		RValue g = GlobalRValue();
		size_t n = D.Legacies.a.size();
		const size_t games = GameLegacyRows();
		std::map<std::string, size_t> at;                         // a legacy's key -> its place in its table now
		for (size_t k = 0; k < n; ++k) at[LegacyKey(D, k)] = k;
		RValue* ranks = FindMember(g, "elements_rank");
		RValue* mine = ranks ? ArrayAt(*ranks, static_cast<size_t>(D.Base)) : nullptr;
		auto rank = [&](size_t K, const Json& V)
		{
			if (mine && KindOf(*mine) == VALUE_ARRAY && K < n && games + K < ArrayLen(*mine) && V.type == Json::Num)
				if (RValue* c = ArrayAt(*mine, games + K)) *c = RValue(V.n);
		};
		if (const Json* of = rec->Find("rank_of"); of && of->type == Json::Obj)
		{
			for (auto& [key, v] : of->o)
				if (auto it = at.find(key); it != at.end()) rank(it->second, v);
		}
		else if (const Json* tail = rec->Find("ranks"); tail && tail->type == Json::Arr)   // (saved before: by row)
			for (size_t k = 0; k < tail->a.size(); ++k) rank(k, tail->a[k]);
		RValue* bar = FindMember(g, "aura_array");
		RValue* slots = bar ? ArrayAt(*bar, static_cast<size_t>(D.Base)) : nullptr;
		auto place = [&](size_t Slot, size_t K)                   // (into an empty slot only)
		{
			if (slots && KindOf(*slots) == VALUE_ARRAY && Slot < ArrayLen(*slots) && K < n)
				if (RValue* c = ArrayAt(*slots, Slot); c && IsNumeric(*c) && c->ToDouble() < 0) *c = RValue(static_cast<double>(games + K));
		};
		if (const Json* of = rec->Find("bar_of"); of && of->type == Json::Arr)
		{
			for (size_t k = 0; k < of->a.size(); ++k)
				if (of->a[k].type == Json::Str)
					if (auto it = at.find(of->a[k].s); it != at.end()) place(k, it->second);
		}
		else if (const Json* old = rec->Find("bar"); old && old->type == Json::Arr)
			for (size_t k = 0; k < old->a.size(); ++k)
				if (old->a[k].type == Json::Num && old->a[k].n >= static_cast<double>(games))
					place(k, static_cast<size_t>(old->a[k].n) - games);
	}

	// the hero's stat legacies (hero var elements_passives, set by scr_hero_element_create) with the class's
	void AddElementPassives(CInstance* Hero, const Def& D)
	{
		if (!Hero || D.ElementPassives.empty()) return;
		RValue self(Hero);
		RValue* v = FindMember(self, "elements_passives");
		if (!v || KindOf(*v) != VALUE_STRING) return;
		std::string list = v->ToString();
		if (list.find(D.ElementPassives) == std::string::npos) *v = RStr(list + D.ElementPassives);
	}
	void ReapplyAuras(CInstance* Hero);
	void ApplyPendingAuras(Def& D, CInstance* Hero);
	void PostHeroElementCreate(CInstance* Self, int, RValue**, RValue&)
	{
		if (Def* d = ContentOf(ElementsFor); d && Self) AddElementPassives(Self, *d);
		ReapplyAuras(Self);
	}

	// the legacy table and icons in place: the class's while it is played, the game's otherwise and around the game's
	// saves and loads
	void SyncElements()
	{
		SyncArt();
		SyncMissions();
		RValue* g = FindMember(GlobalRValue(), "elements");
		if (!g || KindOf(*g) != VALUE_ARRAY) return;
		Def* d = Content();
		// (its table: its own legacies, or none of the game's - legacy_base "none" with no legacies of its own: an empty tree)
		Def* want = d && (!d->Legacies.a.empty() || d->LegacyBaseHidden) && Took.empty() && !Loading && !Saving ? d : nullptr;
		int wantIndex = want ? HeroOf(*want) : -1;
		if (wantIndex == ElementsFor) return;
		if (!HaveOrigElements) { OrigElements = *g; HaveOrigElements = true; Anchor("elements_game", OrigElements); }
		RValue spr = AssetIndex("spr_elements");
		CInstance* hero = FirstInstance(AssetIndex("obj_parent_hero"));
		if (want)
		{
			if (KindOf(want->Elements) != VALUE_ARRAY) BuildElements(*want);
			if (want->ElementIcons < 0) BuildElementIcons(*want);
			*g = want->Elements;
			g_Yytk->CallBuiltin("sprite_assign", { spr, RValue(static_cast<double>(want->ElementIcons)) });
			PadLegacyRows(wantIndex, ArrayLen(want->Elements));
			if (want->Game) RestoreGameLegacies(*want);             // (its ranks and skill-bar legacies from its save data)
			ApplyPendingAuras(*want, hero);
			AddElementPassives(hero, *want);
		}
		else
		{
			*g = OrigElements;
			if (OrigElementIcons >= 0) g_Yytk->CallBuiltin("sprite_assign", { spr, RValue(static_cast<double>(OrigElementIcons)) });
		}
		ElementsFor = wantIndex;
		if (hero) { RValue r; g_Yytk->CallGameScriptEx(r, "gml_Script_scr_hero_fast_check_reset", hero, hero, {}); }
	}

	// ---- its story missions. The game's story is one table for all heroes (global.mission_array, 55 rows): [0] REF (its
	// index), [1] OBJECTIVE ("mission_objective_<kind>"), [2] ENVIRONMENT (its act: 0 Graveyard, 1 Prison, 2 Academy,
	// 3 Gardens, 4 Royal Wing; 99 the end), [3] ENEMY_LEVEL, [4] CATACLYSMS "a|b", [5] LINKED_FROM "a|b" (any one done
	// opens it; "": open from the start), [6] NUMBER (shown I, II, ...), [7] MAP_LAYOUT, [8] BREACH_LAYOUT (its map, in the
	// game's encoding), [9] CHAIN_TO, [10 + 2 x language] name, [11 + 2 x language] description (11 languages). A hero's
	// runs of each are in mission_data[hero][ref], and one is done when any hero ran it (scr_story_mission_check). An act's
	// page (menu "story", story_wanted = the act) draws the act's rows in row order: a node at
	// scr_story_mission_coordinates()[ref] (53 of them; x from the map's middle, y from its top - the map is 592 x 556),
	// and the k-th row's room on spr_menu_story_<act>_maps (done) / _maps_hover (selected), frame k. A run of one (START:
	// scr_run_start(ref), menu "story") reads its row; back in the hub after it is done, mission_data[hero][ref] counts it.
	// A class's own missions come after the game's while it is played (refs 55 + k): every hero's mission_data row is as
	// long as the table meanwhile, its hero's runs of its missions come from its save data ("missions": {id: runs}), the
	// node table gets theirs, and the act maps a frame for each (the room of the game mission whose map it plays). Out of
	// play, and around the game's save and load, the game's table and rows are back.
	const char* const kActs[] = { "graveyard", "prison", "academy", "garden", "wing" };
	const char* const kObjectives[] = { "enemies", "breach", "breaches", "waves", "bonus_breakables", "leave", "boss", "exit",
		"lever", "escort", "named", "emblem" };
	constexpr size_t kMisRef = 0, kMisObjective = 1, kMisAct = 2, kMisLevel = 3, kMisCataclysms = 4, kMisAfter = 5,
		kMisNumber = 6, kMisLayout = 7, kMisBreach = 8, kMisChain = 9, kMisText = 10;
	RValue OrigMissions;                                     // the game's table
	bool HaveOrigMissions = false;
	size_t GameMissions = 0;
	int MissionsFor = -1;                                    // the hero whose content's missions are in the table
	int ActArtFor = -1;                                      // the hero whose act maps are in place (kept around saves)
	std::map<int, int> OrigActArt;                           // the act maps' sprites as the game has them (copies)

	RValue* GameMissionTable()
	{
		if (HaveOrigMissions) return &OrigMissions;
		RValue* g = FindMember(GlobalRValue(), "mission_array");
		return g && KindOf(*g) == VALUE_ARRAY ? g : nullptr;
	}
	size_t GameMissionCount()
	{
		if (HaveOrigMissions) return GameMissions;
		RValue* t = GameMissionTable();
		return t ? ArrayLen(*t) : 0;
	}
	RValue MissionCell(size_t Ref, size_t Col)
	{
		RValue* t = GameMissionTable();
		RValue* row = t && Ref < ArrayLen(*t) ? ArrayAt(*t, Ref) : nullptr;
		RValue* c = row && KindOf(*row) == VALUE_ARRAY ? ArrayAt(*row, Col) : nullptr;
		return c ? *c : RValue();
	}
	// "enemies" or "mission_objective_enemies" -> the game's objective ("" if it is none of them)
	std::string Objective(const Json* V)
	{
		std::string o = V && V->type == Json::Str ? V->s : "";
		if (o.starts_with("mission_objective_")) o = o.substr(18);
		for (const char* k : kObjectives)
			if (o == k) return "mission_objective_" + o;
		return "";
	}
	// "after": one of its missions' ids or a game mission's number, or a list of those -> the refs, "a|b"
	std::vector<const Json*> ListOf(const Json* V)
	{
		std::vector<const Json*> out;
		if (V && V->type == Json::Arr) for (auto& x : V->a) out.push_back(&x);
		else if (V) out.push_back(V);
		return out;
	}
	std::string AfterRefs(const Def& D, const Json* V)
	{
		std::string out;
		for (const Json* x : ListOf(V))
		{
			double ref = x->type == Json::Num ? x->n : -1;
			if (x->type == Json::Str)
				for (size_t k = 0; k < D.Missions.a.size(); ++k)
					if (D.Missions.a[k].GetStr("id") == x->s) ref = static_cast<double>(GameMissions + k);
			if (ref >= 0) out += (out.empty() ? "" : "|") + std::to_string(static_cast<int>(ref));
		}
		return out;
	}

	// the table with its missions after the game's (each a copy of the row of the game mission whose map it plays, with
	// its own cells); their nodes; its act maps with a frame for each
	RValue BuildMissionTable(Def& D)
	{
		RValue* game = GameMissionTable();
		std::vector<RValue> rows;
		for (size_t r = 0; r < GameMissions; ++r) rows.push_back(*ArrayAt(*game, r));
		RValue coords;
		g_Yytk->CallGameScriptEx(coords, "gml_Script_scr_story_mission_coordinates", nullptr, nullptr, {});
		std::map<int, double> topNumber;                        // an act's last number shown
		std::map<int, std::vector<size_t>> actRows;             // an act's game rows in order (its maps' frames)
		for (size_t r = 0; r < GameMissions; ++r)
		{
			int act = static_cast<int>(MissionCell(r, kMisAct).ToDouble());
			actRows[act].push_back(r);
			topNumber[act] = (std::max)(topNumber[act], MissionCell(r, kMisNumber).ToDouble());
		}
		D.MissionPos.clear();
		std::map<int, std::vector<int>> rooms;                  // act -> its missions' rooms (its maps' frames; -1 none)
		int areaCount = 0;
		for (size_t k = 0; k < D.Missions.a.size(); ++k)
		{
			const Json& m = D.Missions.a[k];
			int map = static_cast<int>(m.GetNum("map", -1));
			int mapAct = map >= 0 ? static_cast<int>(MissionCell(static_cast<size_t>(map), kMisAct).ToDouble()) : -1;
			bool onMap = map >= 0 && !m.Find("layout");               // a game mission's map: its act, its objective
			int act = onMap ? mapAct : static_cast<int>(m.GetNum("act", mapAct));
			bool area = m.GetBool("area", false);                     // on its area's page (its act's tiles for the run)
			RValue* like = ArrayAt(*game, map >= 0 ? static_cast<size_t>(map) : 0);
			std::vector<RValue> row;
			for (size_t c = 0, n = ArrayLen(*like); c < n; ++c) row.push_back(*ArrayAt(*like, c));
			row[kMisRef] = RValue(static_cast<double>(GameMissions + k));
			if (std::string o = Objective(m.Find("objective")); !o.empty() && !onMap) row[kMisObjective] = RStr(o);
			row[kMisAct] = RValue(area ? act + 0.5 : static_cast<double>(act));   // (no act page shows act + 0.5)
			row[kMisNumber] = RValue(m.GetNum("number", area ? static_cast<double>(++areaCount) : topNumber[act] + 1 + static_cast<double>(rooms[act].size())));
			if (const Json* c = m.Find("cataclysms"))
			{
				std::string list;
				for (const Json* x : ListOf(c))
					list += (list.empty() ? "" : "|") + (x->type == Json::Num ? std::to_string(static_cast<int>(x->n)) : x->s);
				row[kMisCataclysms] = RStr(list);
			}
			row[kMisAfter] = RStr(AfterRefs(D, m.Find("after")));
			row[kMisLevel] = RValue(m.GetNum("level", map >= 0 ? MissionCell(static_cast<size_t>(map), kMisLevel).ToDouble() : 1));
			if (const Json* l = m.Find("layout"); l && l->type == Json::Str) row[kMisLayout] = RStr(l->s);
			if (const Json* l = m.Find("breach_layout"); l && l->type == Json::Str) row[kMisBreach] = RStr(l->s);
			row[kMisChain] = RValue(false);
			for (size_t lang = 0; kMisText + 2 * lang + 1 < row.size(); ++lang)
			{
				row[kMisText + 2 * lang] = RStr(LangText(m.Find("name"), lang));
				row[kMisText + 2 * lang + 1] = RStr(LangText(m.Find("description"), lang));
			}
			rows.push_back(RValue(row));
			// its node: as given, else 32 px right of its map's
			RValue* at = map >= 0 && KindOf(coords) == VALUE_ARRAY ? ArrayAt(coords, static_cast<size_t>(map)) : nullptr;
			bool known = at && KindOf(*at) == VALUE_ARRAY && ArrayLen(*at) >= 2;
			double right = area ? 0 : 32;                            // (on its area's page its map's spot is free)
			D.MissionPos.push_back({ m.GetNum("x", known ? ArrayAt(*at, 0)->ToDouble() + right : 0), m.GetNum("y", known ? ArrayAt(*at, 1)->ToDouble() : 278) });
			if (area) continue;                                       // (its area's page has its own rooms)
			int room = -1;
			if (mapAct == act)
				for (size_t f = 0; f < actRows[act].size(); ++f)
					if (actRows[act][f] == static_cast<size_t>(map)) room = static_cast<int>(f);
			rooms[act].push_back(room);
		}
		if (D.ActArt.empty())                                    // its act maps: the game's frames, then its missions' rooms
			for (auto& [act, mine] : rooms)
			{
				if (act < 0 || act >= static_cast<int>(std::size(kActs))) continue;
				for (const char* kind : { "_maps", "_maps_hover" })
				{
					int spr = static_cast<int>(AssetIndex(("spr_menu_story_" + std::string(kActs[act]) + kind).c_str()).ToDouble());
					if (spr < 0) continue;
					if (!OrigActArt.count(spr)) OrigActArt[spr] = static_cast<int>(g_Yytk->CallBuiltin("sprite_duplicate", { RValue(static_cast<double>(spr)) }).ToDouble());
					int orig = OrigActArt[spr];
					int frames = static_cast<int>(g_Yytk->CallBuiltin("sprite_get_number", { RValue(static_cast<double>(orig)) }).ToDouble());
					std::vector<IconSrc> src;
					for (size_t f = 0; f < actRows[act].size(); ++f) src.push_back({ static_cast<int>(f) < frames ? orig : -1, static_cast<int>(f) });
					for (int room : mine) src.push_back({ room >= 0 && room < frames ? orig : -1, room });
					D.ActArt[spr] = MakeStrip(src, orig);
				}
			}
		return RValue(rows);
	}
	void DropActArt(Def& D)
	{
		for (auto& [game, mine] : D.ActArt) g_Yytk->CallBuiltin("sprite_delete", { RValue(static_cast<double>(mine)) });
		D.ActArt.clear();
		if (ActArtFor == HeroOf(D)) ActArtFor = -2;            // (the game's maps hold copies of them: assigned again)
	}
	// the act maps: the class's (D) or the game's
	void ActMaps(const Def* D)
	{
		for (auto& [spr, orig] : OrigActArt)
		{
			int src = orig;
			if (D)
				if (auto it = D->ActArt.find(spr); it != D->ActArt.end()) src = it->second;
			g_Yytk->CallBuiltin("sprite_assign", { RValue(static_cast<double>(spr)), RValue(static_cast<double>(src)) });
		}
	}
	// every hero's runs as long as the table (D's hero's of D's missions: as counted), or back to the game's (D null)
	void PadMissionData(const Def* D, size_t Rows)
	{
		RValue* md = FindMember(GlobalRValue(), "mission_data");
		if (!md || KindOf(*md) != VALUE_ARRAY) return;
		size_t mine = D ? static_cast<size_t>(HeroOf(*D)) : SIZE_MAX;
		for (size_t h = 0, n = ArrayLen(*md); h < n; ++h)
		{
			RValue* row = ArrayAt(*md, h);
			if (!row || KindOf(*row) != VALUE_ARRAY || ArrayLen(*row) == Rows) continue;
			size_t len = ArrayLen(*row);
			std::vector<RValue> items;
			for (size_t r = 0; r < Rows; ++r)
			{
				size_t k = r - GameMissions;
				if (r < GameMissions) items.push_back(r < len ? *ArrayAt(*row, r) : RValue(0.0));
				else items.push_back(RValue(h == mine && k < D->MissionRuns.size() ? D->MissionRuns[k] : 0.0));
			}
			*row = RValue(items);
		}
	}
	// its hero's runs of its missions (while its table is in place) into its save data
	void KeepMissionRuns(Def& D)
	{
		RValue* md = FindMember(GlobalRValue(), "mission_data");
		RValue* row = md && KindOf(*md) == VALUE_ARRAY ? ArrayAt(*md, static_cast<size_t>(HeroOf(D))) : nullptr;
		Json& rec = AchData(D, "missions");
		for (size_t k = 0; k < D.Missions.a.size(); ++k)
		{
			RValue* v = row && KindOf(*row) == VALUE_ARRAY ? ArrayAt(*row, GameMissions + k) : nullptr;
			double runs = v && IsNumeric(*v) ? v->ToDouble() : k < D.MissionRuns.size() ? D.MissionRuns[k] : 0;
			rec.Set(D.Missions.a[k].GetStr("id"), runs);
		}
	}
	void SyncMissions(bool Out)
	{
		RValue* g = FindMember(GlobalRValue(), "mission_array");
		if (!g || KindOf(*g) != VALUE_ARRAY) return;
		if (Out || !Took.empty() || Loading || Saving) AreaViewOff();
		Def* d = Content();
		Def* played = d && !d->Missions.a.empty() ? d : nullptr;
		Def* want = !Out && played && Took.empty() && !Loading && !Saving ? played : nullptr;
		int wantIndex = want ? HeroOf(*want) : -1;
		if (wantIndex == MissionsFor)
		{
			if (want) PadMissionData(want, ArrayLen(*g));          // (a hero's row made meanwhile)
		}
		else
		{
			AreaViewOff();
			if (!HaveOrigMissions) { OrigMissions = *g; HaveOrigMissions = true; GameMissions = ArrayLen(*g); Anchor("missions_game", OrigMissions); }
			if (Def* was = ContentOf(MissionsFor)) KeepMissionRuns(*was);
			*g = OrigMissions;
			PadMissionData(nullptr, GameMissions);
			MissionsFor = -1;
			if (want)
			{
				want->MissionRuns.clear();
				const Json& rec = AchData(*want, "missions");
				for (auto& m : want->Missions.a)
				{
					const Json* v = rec.Find(m.GetStr("id"));
					want->MissionRuns.push_back(v && v->type == Json::Num ? v->n : 0);
				}
				if (want->MissionSeen.size() != want->MissionRuns.size()) want->MissionSeen = want->MissionRuns;   // (loaded: not new)
				*g = BuildMissionTable(*want);
				MissionsFor = wantIndex;
				PadMissionData(want, ArrayLen(*g));
			}
		}
		// the act maps: by the class played only (sprites are not saved)
		int artIndex = played && !played->ActArt.empty() ? HeroOf(*played) : -1;
		if (artIndex != ActArtFor)
		{
			ActMaps(artIndex >= 0 ? played : nullptr);
			ActArtFor = artIndex;
		}
	}

	// the game's scripted reaper effects ask scr_weapon_check_if_available_effect(ref): is that reaper worn, or one it
	// evolved from. A class's reaper with code "own" answers no (its Lua does its effect: class_reaper {effect})
	bool RawReaperCheck = false;
	bool PreReaperEffect(CInstance*, int ArgCount, RValue** Args, RValue& Result)
	{
		if (RawReaperCheck || ArgCount < 1 || !Args[0] || !IsNumeric(*Args[0]) || KindOf(*Args[0]) == VALUE_STRING) return false;
		Def* d = Content();
		if (!d || !d->ReaperOwnCode.count(static_cast<int>(Args[0]->ToDouble()))) return false;
		Result = RValue(false);
		return true;
	}
	// a reaper's effect on (class_reaper {effect}: asked by its Lua handlers on each hit) - the game's check, kept until the
	// hero, its worn reaper (evolutions follow from it) or the reaper cells in place change
	struct EffectCache { int Hero = -1, Cells = -2; double Worn = -1; std::map<double, bool> On; };
	EffectCache ReaperEffects;
	bool ReaperEffectOn(CInstance* Hero, double Ref)
	{
		RValue* h = CurrentHero();
		RValue* cw = FindMember(GlobalRValue(), "current_weapon");
		int hero = h && IsNumeric(*h) ? static_cast<int>(h->ToDouble()) : -1;
		RValue* worn = cw && KindOf(*cw) == VALUE_ARRAY && hero >= 0 ? ArrayAt(*cw, static_cast<size_t>(hero)) : nullptr;
		double w = worn && IsNumeric(*worn) ? worn->ToDouble() : -1;
		EffectCache& c = ReaperEffects;
		if (c.Hero != hero || c.Worn != w || c.Cells != TextsFor) c = EffectCache{ hero, TextsFor, w, {} };
		if (auto it = c.On.find(Ref); it != c.On.end()) return it->second;
		RValue r;
		RawReaperCheck = true;
		try { g_Yytk->CallGameScriptEx(r, "gml_Script_scr_weapon_check_if_available_effect", Hero, Hero, { RValue(Ref) }); }
		catch (...) {}
		RawReaperCheck = false;
		return c.On[Ref] = r.ToBoolean();
	}

	// the stat lines of its own-code reapers (the game applies a reaper's after the effect check, which says no for
	// them): a passive buff while one is worn - value x its level where per_level - made again when the worn reaper or
	// its level changes
	std::string ReaperBuffFor;                                   // "ref:level" the buff was made for ("" none)
	const char* const kReaperBuff = "slormapi_class_reaper";
	bool WornReaper(double& Ref, bool& Primordial, double& Level)
	{
		RValue* h = CurrentHero();
		RValue* cw = FindMember(GlobalRValue(), "current_weapon");
		RValue* off = FindMember(GlobalRValue(), "weapon_ancient_offset");
		RValue* worn = h && IsNumeric(*h) && cw && KindOf(*cw) == VALUE_ARRAY ? ArrayAt(*cw, static_cast<size_t>(h->ToDouble())) : nullptr;
		CInstance* hero = FirstInstance(AssetIndex("obj_parent_hero"));
		if (!worn || !IsNumeric(*worn) || !hero) return false;
		double id = worn->ToDouble(), offset = off && IsNumeric(*off) ? off->ToDouble() : 120;
		Primordial = id >= offset;
		Ref = Primordial ? id - offset : id;
		RValue level;
		g_Yytk->CallGameScriptEx(level, "gml_Script_scr_get_weapon_level", hero, hero, { RValue(Ref), *h });
		RValue* lv = KindOf(level) == VALUE_ARRAY ? ArrayAt(level, 0) : &level;   // ([level, xp into it, next, max, over])
		Level = lv && IsNumeric(*lv) ? lv->ToDouble() : 0;
		return true;
	}
	void ReaperPassives()
	{
		if (!Took.empty() || Loading || Saving) return;
		RValue hero = HeroInstance();
		if (KindOf(hero) == VALUE_UNDEFINED) { ReaperBuffFor.clear(); return; }
		RValue target(InstanceById(static_cast<int32_t>(hero.ToInt64())));
		Def* d = Content();
		double ref = -1, level = 0;
		bool primordial = false;
		std::string want;
		if (d && !d->ReaperStats.empty() && WornReaper(ref, primordial, level) && d->ReaperStats.count(static_cast<int>(ref)))
			want = Fmt(ref) + ":" + Fmt(level);
		if (want == ReaperBuffFor && (want.empty() || BuffStacks(target, kReaperBuff) > 0)) return;
		BuffRemove(target, kReaperBuff);
		if (!want.empty())
		{
			std::vector<std::pair<std::string, double>> stats;
			for (auto& st : d->ReaperStats[static_cast<int>(ref)]) stats.emplace_back(st.Stat, st.PerLevel ? st.Value * level : st.Value);
			BuffAdd(target, kReaperBuff, stats, -1, 1, 1, "", "", "", false);
		}
		ReaperBuffFor = want;
		g_Yytk->CallBuiltin("variable_instance_set", { target, RValue("self_refresh_stats"), RValue(true) });
	}

	// the hero (obj_parent_hero or one of its children)
	bool IsHeroObject(double Obj)
	{
		static double hero = -1;
		static std::map<int, bool> seen;                          // (an object's parents do not change)
		if (hero < 0) hero = AssetIndex("obj_parent_hero").ToDouble();
		if (hero < 0 || Obj < 0) return false;
		int o = static_cast<int>(Obj);
		if (auto it = seen.find(o); it != seen.end()) return it->second;
		return seen[o] = Obj == hero || g_Yytk->CallBuiltin("object_is_ancestor", { RValue(Obj), RValue(hero) }).ToBoolean();
	}
	bool IsHero(CInstance* I)
	{
		RValue obj;
		return I && AurieSuccess(g_Yytk->GetBuiltin("object_index", I, NULL_INDEX, obj)) && IsNumeric(obj) && IsHeroObject(obj.ToDouble());
	}

	// the sound effects its hero makes (obj_single_sound_effect: scr_create_emitter(x, y, sound, team, follow_up) -> it,
	// played in its step): made by the hero's own code, or by an attack's hitbox when the hero dodges it
	// (scr_damage_dodge_check(attacker, target): its own "dodge" - not what the procs it sets off make, an enemy's death
	// among them). Marked slormapi_hero there, while its hero's sounds matter
	std::vector<bool> DodgeChecks;                            // (each running check: its target the hero; cleared each Tick)
	bool HeroSounds() { Def* d = Content(); return d && (!d->Sounds.empty() || d->Blank); }
	bool PreDodgeCheck(CInstance*, int ArgCount, RValue** Args, RValue&)
	{
		bool hero = false;
		Def* d = Content();
		if (d && (d->Blank || d->Sounds.count("dodge")) && ArgCount >= 2 && Args[1] && KindOf(*Args[1]) != VALUE_UNDEFINED
			&& g_Yytk->CallBuiltin("instance_exists", { *Args[1] }).ToBoolean())
		{
			RValue obj = g_Yytk->CallBuiltin("variable_instance_get", { *Args[1], RValue("object_index") });
			hero = IsNumeric(obj) && IsHeroObject(obj.ToDouble());
		}
		DodgeChecks.push_back(hero);
		return false;
	}
	void PostDodgeCheck(CInstance*, int, RValue**, RValue&) { if (!DodgeChecks.empty()) DodgeChecks.pop_back(); }
	void PostCreateEmitter(CInstance* Self, int ArgCount, RValue** Args, RValue& Result)
	{
		if (KindOf(Result) == VALUE_UNDEFINED || !HeroSounds()) return;
		bool dodge = !DodgeChecks.empty() && DodgeChecks.back() && ArgCount >= 3 && Args[2] && KindOf(*Args[2]) == VALUE_STRING
			&& Args[2]->ToString() == "dodge";
		if (dodge || IsHero(Self))
			g_Yytk->CallBuiltin("variable_instance_set", { Result, RValue("slormapi_hero"), RValue(true) });
	}

	// its hero's sounds ("sounds": {name: its sound | false | true}). The game plays a sound by name, scr_play_sound(volume
	// group, name, team) picking a variant of snd_<name>_NN: its hero calls it itself for its swings ("large_sword"),
	// steps ("footstep_<floor>"), death, level up, auras, and through the sound effects it makes (above) or that follow
	// it (parent: the hero - its skills' "whirlwind", ...). A class's for a name plays instead (at the sound effects
	// volume), or none (false), or the game's (true); only its hero's (an enemy's swing, a hit's impact keep theirs).
	// A blank base's hero plays none of the game's but those (the log names each one it plays none of, once); its own
	// (sound_file, played by S.play / a piece's o.sound) always play
	bool PrePlaySound(CInstance* Self, int ArgCount, RValue** Args, RValue& Result)
	{
		Def* d = Content();
		if (!d || (d->Sounds.empty() && !d->Blank) || !Self || ArgCount < 2 || !Args[1] || KindOf(*Args[1]) != VALUE_STRING) return false;
		std::string name = Args[1]->ToString();
		auto it = d->Sounds.find(name);
		if (it == d->Sounds.end() ? !d->Blank : it->second == -2) return false;
		if (!IsHero(Self))                                    // a sound effect: its hero made it, or it follows its hero
		{
			RValue self(Self);
			static double effect = -1;
			if (effect < 0) effect = AssetIndex("obj_single_sound_effect").ToDouble();
			RValue obj;
			if (effect < 0 || !AurieSuccess(g_Yytk->GetBuiltin("object_index", Self, NULL_INDEX, obj)) || !IsNumeric(obj) || obj.ToDouble() != effect)
				return false;
			RValue* mine = FindMember(self, "slormapi_hero");
			RValue* parent = FindMember(self, "parent");             // what it follows: an object ("the hero") or an instance
			double p = parent && IsNumeric(*parent) ? parent->ToDouble() : -1;
			bool follows = p >= 100000 ? g_Yytk->CallBuiltin("instance_exists", { RValue(p) }).ToBoolean()
					&& IsHeroObject(g_Yytk->CallBuiltin("variable_instance_get", { RValue(p), RValue("object_index") }).ToDouble())
				: IsHeroObject(p);
			if (!(mine && mine->ToBoolean()) && !follows) return false;
		}
		Result = RValue(-1.0);
		if (it == d->Sounds.end())
		{
			if (d->Silenced.insert(name).second)
				DbgPrintEx(LOG_SEVERITY_INFO, "[SlormAPI] class %s (base \"none\"): no sound for \"%s\" (its hero's: \"sounds\" {%s = its sound | true})",
					d->Name.c_str(), name.c_str(), name.c_str());
			return true;
		}
		if (it->second >= 0 && g_Yytk->CallBuiltin("audio_exists", { RValue(it->second) }).ToBoolean())
		{
			Result = g_Yytk->CallBuiltin("audio_play_sound", { RValue(it->second), RValue(10.0), RValue(false) });
			RValue* sfx = FindMember(GlobalRValue(), "sc_sfx");
			g_Yytk->CallBuiltin("audio_sound_gain", { Result, RValue(sfx && IsNumeric(*sfx) ? sfx->ToDouble() : 1.0), RValue(0.0) });
		}
		return true;
	}

	// a dialogue's "[hero]" pages while a class is played: its own name and portrait. (The box takes the hero's citizen
	// 13-15 by hero number; for a class's it kept the index of the line before - "The King" - or none: an error each
	// frame.) dial_chars: the pages' speaker tags
	void PostCreateDialogue(CInstance*, int, RValue**, RValue&)
	{
		Def* d = Playing();
		RValue ui = UiValue();
		RValue* chars = d && d->Cid >= 0 && KindOf(ui) != VALUE_UNDEFINED ? FindMember(ui, "dial_chars") : nullptr;
		for (size_t k = 0, n = chars && KindOf(*chars) == VALUE_ARRAY ? ArrayLen(*chars) : 0; k < n; ++k)
			if (RValue* c = ArrayAt(*chars, k); c && KindOf(*c) == VALUE_STRING && c->ToString() == "hero")
				*c = RStr("slormapi_" + std::to_string(d->Cid));
	}

	// ---- its own story area ("area": {name, map, rooms, entrance, hub_x, hub_y, act}). An entrance of its own stands in
	// the hub while it is played (an obj_hub_graveyard_entrance made at hub_x / hub_y, default right of its citizen's spot;
	// its art "entrance"). Its user event 15 - the game's "Enter" - opens the act page of "act" (default 0), which is then
	// the area's: the table seen through a view (its missions with "area" on that act, the act's own hidden at act + 0.5),
	// the act's sprites its art (map: spr_menu_story_<act>_overmap, 592 x 556; rooms: one frame per area mission, for
	// _maps / _maps_hover), its title "name" (scr_string "environment_<act>"). The view goes when the page closes, a run
	// starts (an area mission's row has its act for the run: its tiles), and around saves and loads.
	int AreaFor = -1;                                         // the class whose area page is shown
	RValue AreaHeld;                                          // the class's table meanwhile
	int AreaNaming = -1;                                      // its entrance draws now (its "Enter <name>")
	int AreaAct(const Def& D) { int a = static_cast<int>(D.Area.GetNum("act", 0)); return a >= 0 && a <= 4 ? a : 0; }
	bool AreaMission(const Def& D, size_t K) { return K < D.Missions.a.size() && D.Missions.a[K].GetBool("area", false); }
	int ActSprite(int Act, const char* Kind) { return Named("spr_menu_story_" + std::string(kActs[Act]) + Kind); }
	void AreaViewOff()
	{
		if (AreaFor < 0) return;
		if (RValue* g = FindMember(GlobalRValue(), "mission_array"); g && KindOf(AreaHeld) == VALUE_ARRAY) *g = AreaHeld;
		AreaHeld = RValue();
		Anchor("missions_area", RValue());
		int act = static_cast<size_t>(AreaFor) < Defs.size() ? AreaAct(Defs[static_cast<size_t>(AreaFor)]) : 0;
		if (int spr = ActSprite(act, "_overmap"); spr >= 0) g_Yytk->CallBuiltin("sprite_assign", { RValue(static_cast<double>(spr)), RValue(static_cast<double>(Original(spr))) });
		for (const char* kind : { "_maps", "_maps_hover" })
			if (int spr = ActSprite(act, kind); spr >= 0 && OrigActArt.count(spr))
				g_Yytk->CallBuiltin("sprite_assign", { RValue(static_cast<double>(spr)), RValue(static_cast<double>(OrigActArt[spr])) });
		ActArtFor = -2;                                        // (its act maps again: SyncMissions)
		AreaFor = -1;
	}
	void AreaViewOn(size_t I)
	{
		AreaViewOff();
		Def& d = Defs[I];
		RValue* g = FindMember(GlobalRValue(), "mission_array");
		if (!g || KindOf(*g) != VALUE_ARRAY || MissionsFor != 3 + static_cast<int>(I) || d.AreaMap < 0) return;
		int act = AreaAct(d);
		std::vector<RValue> rows;
		for (size_t r = 0, n = ArrayLen(*g); r < n; ++r)
		{
			RValue* row = ArrayAt(*g, r);
			bool mine = r >= GameMissions && AreaMission(d, r - GameMissions);
			RValue* env = row && KindOf(*row) == VALUE_ARRAY ? ArrayAt(*row, kMisAct) : nullptr;
			bool there = env && IsNumeric(*env) && env->ToDouble() == act;
			if (!mine && !there) { rows.push_back(*row); continue; }
			std::vector<RValue> copy;
			for (size_t c = 0, nc = ArrayLen(*row); c < nc; ++c) copy.push_back(*ArrayAt(*row, c));
			copy[kMisAct] = RValue(mine ? static_cast<double>(act) : act + 0.5);
			rows.push_back(RValue(copy));
		}
		AreaHeld = *g;
		Anchor("missions_area", AreaHeld);
		*g = RValue(rows);
		// its art in the act's sprites (the game's kept: the act maps' copies, made before the first change)
		int over = ActSprite(act, "_overmap");
		if (over >= 0) { Original(over); g_Yytk->CallBuiltin("sprite_assign", { RValue(static_cast<double>(over)), RValue(static_cast<double>(d.AreaMap)) }); }
		for (const char* kind : { "_maps", "_maps_hover" })
		{
			int spr = ActSprite(act, kind);
			if (spr < 0) continue;
			if (!OrigActArt.count(spr)) OrigActArt[spr] = static_cast<int>(g_Yytk->CallBuiltin("sprite_duplicate", { RValue(static_cast<double>(spr)) }).ToDouble());
			if (d.AreaStrip < 0)                                     // its rooms, one frame per area mission (none: blank)
			{
				int rooms = d.AreaRooms, n = rooms >= 0 ? Frames(rooms) : 0;
				std::vector<IconSrc> src;
				for (size_t k = 0, order = 0; k < d.Missions.a.size(); ++k)   // (its "room": a frame of its rooms; default its order)
					if (AreaMission(d, k))
					{
						int f = static_cast<int>(d.Missions.a[k].GetNum("room", static_cast<double>(order++)));
						src.push_back({ f >= 0 && f < n ? rooms : -1, f });
					}
				if (src.empty()) src.push_back({ -1, 0 });
				d.AreaStrip = MakeStrip(src, OrigActArt[spr]);
			}
			if (d.AreaStrip >= 0) g_Yytk->CallBuiltin("sprite_assign", { RValue(static_cast<double>(spr)), RValue(static_cast<double>(d.AreaStrip)) });
		}
		AreaFor = static_cast<int>(I);
	}
	void AreaDoorArt(Def& D);
	// its entrance in the hub while it is played (made again when the hub is)
	void AreaEntrances()
	{
		Def* d = Playing();
		if (!d || d->Area.type != Json::Obj) return;
		if (d->AreaEntrance >= 0 && g_Yytk->CallBuiltin("instance_exists", { RValue(static_cast<double>(d->AreaEntrance)) }).ToBoolean())
		{
			AreaDoorArt(*d);
			return;
		}
		d->AreaEntrance = -1;
		if (g_Yytk->CallBuiltin("instance_number", { AssetIndex("obj_hub_controller") }).ToDouble() < 1) return;
		if (!FirstInstance(AssetIndex("obj_parent_hero"))) return;
		double x = d->Area.GetNum("hub_x", d->HubX >= 0 ? d->HubX + 70 : -1), y = d->Area.GetNum("hub_y", d->HubY >= 0 ? d->HubY - 10 : -1);
		if (x < 0 || y < 0) return;
		RValue inst = g_Yytk->CallBuiltin("instance_create_depth", { RValue(x), RValue(y), RValue(-30000.0), AssetIndex("obj_hub_graveyard_entrance") });
		int64_t id = KindOf(inst) == VALUE_UNDEFINED ? -1 : inst.ToInt64();   // (an instance reference: its number)
		if (id <= 0) return;
		g_Yytk->CallBuiltin("variable_instance_set", { inst, RValue("environment"), RValue(static_cast<double>(AreaAct(*d))) });
		d->AreaEntrance = static_cast<int>(id);
		AreaDoorArt(*d);
	}
	// its art on its entrance (the entrance sets its sprite from its state: kept on it)
	void AreaDoorArt(Def& D)
	{
		if (D.AreaDoor < 0 || D.AreaEntrance < 0) return;
		for (int k = 0, n = static_cast<int>(g_Yytk->CallBuiltin("instance_number", { AssetIndex("obj_hub_parent_environment_entrance") }).ToDouble()); k < n; ++k)
		{
			RValue e = g_Yytk->CallBuiltin("instance_find", { AssetIndex("obj_hub_parent_environment_entrance"), RValue(static_cast<double>(k)) });
			if (KindOf(e) == VALUE_UNDEFINED || e.ToInt64() != D.AreaEntrance) continue;
			RValue spr = g_Yytk->CallBuiltin("variable_instance_get", { e, RValue("sprite_index") });
			if (!IsNumeric(spr) || static_cast<int>(spr.ToDouble()) != D.AreaDoor)
				g_Yytk->CallBuiltin("variable_instance_set", { e, RValue("sprite_index"), RValue(static_cast<double>(D.AreaDoor)) });
		}
	}
	// the entrance's events: its draw names it (its "Enter", a user event, is seen by AreaFrame)
	void AreaEntranceEvent(CInstance* Self, bool Enter)
	{
		AreaNaming = -1;
		int64_t id = InstanceId(Self);
		for (size_t i = 0; i < Defs.size(); ++i)
			if (Defs[i].AreaEntrance >= 0 && Defs[i].AreaEntrance == id)
			{
				if (Enter) AreaViewOn(i);
				else AreaNaming = static_cast<int>(i);
			}
	}
	// "environment_<act>": the area's name on its page and on its entrance
	bool AreaString(const std::string& Key, RValue& Result)
	{
		int who = AreaFor >= 0 ? AreaFor : AreaNaming;
		if (who < 0 || static_cast<size_t>(who) >= Defs.size() || Key != "environment_" + std::to_string(AreaAct(Defs[static_cast<size_t>(who)]))) return false;
		std::string t = LangText(Defs[static_cast<size_t>(who)].Area.Find("name"), CurrentLang());
		if (t.empty()) return false;
		Result = RStr(t);
		return true;
	}
	// each frame: its page opened from its gate - the story page of its act opening while its gate is nearer the hero than
	// the act's own gate (its "Enter" runs as a user event, which no event hook sees) - shows its area; the page closed
	// (or another class played): the view goes
	void AreaWatch();
	bool StoryWasOpen = false;
	void AreaFrame()
	{
		RValue ui = UiValue();
		RValue menu = KindOf(ui) == VALUE_UNDEFINED ? RValue() : g_Yytk->CallBuiltin("variable_instance_get", { ui, RValue("menu_show") });
		RValue next = KindOf(ui) == VALUE_UNDEFINED ? RValue() : g_Yytk->CallBuiltin("variable_instance_get", { ui, RValue("menu_wanted") });
		// (asked for: menu_wanted, before the menu's first frame builds its entries)
		bool story = (KindOf(menu) == VALUE_STRING && menu.ToString() == "story") || (KindOf(next) == VALUE_STRING && next.ToString() == "story");
		bool opened = story && !StoryWasOpen;
		StoryWasOpen = story;
		if (AreaFor >= 0) { AreaWatch(); return; }
		Def* d = Playing();
		if (!opened || !d || d->AreaEntrance < 0) return;
		RValue wanted = g_Yytk->CallBuiltin("variable_instance_get", { ui, RValue("story_wanted") });
		if (!IsNumeric(wanted) || static_cast<int>(wanted.ToDouble()) != AreaAct(*d)) return;
		CInstance* hero = FirstInstance(AssetIndex("obj_parent_hero"));
		if (!hero) return;
		RValue hx, hy;
		g_Yytk->GetBuiltin("x", hero, NULL_INDEX, hx);
		g_Yytk->GetBuiltin("y", hero, NULL_INDEX, hy);
		double mine = -1, theirs = -1;
		RValue obj = AssetIndex("obj_hub_parent_environment_entrance");
		for (int k = 0, n = static_cast<int>(g_Yytk->CallBuiltin("instance_number", { obj }).ToDouble()); k < n; ++k)
		{
			RValue e = g_Yytk->CallBuiltin("instance_find", { obj, RValue(static_cast<double>(k)) });
			RValue env = g_Yytk->CallBuiltin("variable_instance_get", { e, RValue("environment") });
			if (!IsNumeric(env) || static_cast<int>(env.ToDouble()) != AreaAct(*d)) continue;
			double dist = g_Yytk->CallBuiltin("point_distance", { hx, hy, g_Yytk->CallBuiltin("variable_instance_get", { e, RValue("x") }),
				g_Yytk->CallBuiltin("variable_instance_get", { e, RValue("y") }) }).ToDouble();
			double& slot = e.ToInt64() == d->AreaEntrance ? mine : theirs;
			if (slot < 0 || dist < slot) slot = dist;
		}
		if (mine >= 0 && (theirs < 0 || mine < theirs))
		{
			AreaViewOn(static_cast<size_t>(d - Defs.data()));
			RValue r;                                                  // (its entries built again from the view)
			if (AreaFor >= 0) g_Yytk->CallGameScriptEx(r, "gml_Script_scr_ui_menu_reset", ui.ToInstance(), ui.ToInstance(), {});
		}
	}
	void AreaWatch()
	{
		if (AreaFor < 0) return;
		RValue ui = UiValue();
		RValue menu = KindOf(ui) == VALUE_UNDEFINED ? RValue() : g_Yytk->CallBuiltin("variable_instance_get", { ui, RValue("menu_show") });
		Def* d = Playing();
		RValue next = KindOf(ui) == VALUE_UNDEFINED ? RValue() : g_Yytk->CallBuiltin("variable_instance_get", { ui, RValue("menu_wanted") });
		bool shown = (KindOf(menu) == VALUE_STRING && menu.ToString() == "story") || (KindOf(next) == VALUE_STRING && next.ToString() == "story");
		if (!shown || !d || static_cast<int>(d - Defs.data()) != AreaFor) AreaViewOff();
	}

	// the act page's nodes: its missions' too
	void PostMissionCoordinates(CInstance*, int, RValue**, RValue& Result)
	{
		const Def* dp = ContentOf(MissionsFor);
		if (!dp || KindOf(Result) != VALUE_ARRAY) return;
		const Def& d = *dp;
		std::vector<RValue> rows;
		for (size_t r = 0, n = ArrayLen(Result); r < n; ++r) rows.push_back(*ArrayAt(Result, r));
		while (rows.size() < GameMissions) rows.push_back(RValue(std::vector<RValue>{ RValue(0.0), RValue(0.0) }));
		for (auto& [x, y] : d.MissionPos) rows.push_back(RValue(std::vector<RValue>{ RValue(x), RValue(y) }));
		Result = RValue(rows);
	}

	// its missions' starts ("mission_started" {class, mission, ref}) and runs done ("mission_done" {..., runs}); their
	// "intro" said in the map, their "outro" back in the hub (a dialogue's text: "[speaker]page/...", one per language)
	struct MissionLine { int Hero; size_t K; bool Intro; double At; };
	std::vector<MissionLine> MissionLines;
	void MissionEvent(const char* Name, const Def& D, size_t K, double Runs)
	{
		Json e = Json::Object();
		e.Add("class", D.Key());
		e.Add("mission", D.Missions.a[K].GetStr("id"));
		e.Add("owner", K < D.MissionFrom.size() ? D.MissionFrom[K] : D.Owner);
		e.Add("ref", static_cast<double>(GameMissions + K));
		if (Runs >= 0) e.Add("runs", Runs);
		g_DirectEvents.emplace_back(Name, std::move(e));
	}
	// A run keeps its mission's number in run_data["Preset Run"]: the map's waves, boss, escort and the game's dialogues of
	// the mission come by it (the run controller's Create, scr_story_trigger_dialogue_mission_start / _end), and when the
	// floor ends (scr_floor_end) mission_data[hero][that number] counts the run. A mission of the class's on a game
	// mission's map runs as that mission (its number there after scr_run_start, which read the class's row: name, level,
	// cataclysms), with the game's dialogues of it left out, and gets its own number back when the run ends.
	struct RunOf { int Hero = -1; size_t K = 0; double Map = -1; };
	RunOf Running;
	size_t AreaRowRef = SIZE_MAX;                             // an area mission's row, its act during scr_run_start
	// an area mission's act cell in the story table now (by its row: the table may be another array by then)
	RValue* AreaActCell(size_t Ref)
	{
		RValue* g = FindMember(GlobalRValue(), "mission_array");
		RValue* row = g && KindOf(*g) == VALUE_ARRAY ? ArrayAt(*g, Ref) : nullptr;
		RValue* env = row && KindOf(*row) == VALUE_ARRAY ? ArrayAt(*row, kMisAct) : nullptr;
		return env && IsNumeric(*env) && KindOf(*env) != VALUE_STRING ? env : nullptr;
	}
	void PresetRun(double Ref)
	{
		if (RValue* rd = FindMember(GlobalRValue(), "run_data"))
			g_Yytk->CallBuiltin("ds_map_replace", { *rd, RValue("Preset Run"), RValue(Ref) });
	}
	bool PreRunStart(CInstance*, int ArgCount, RValue** Args, RValue&)
	{
		Running = {};
		AreaRowRef = SIZE_MAX;
		AreaViewOff();                                           // (its row as the class's table has it)
		if (!ContentOf(MissionsFor) || ArgCount < 1 || !Args[0] || !IsNumeric(*Args[0]) || KindOf(*Args[0]) == VALUE_STRING) return false;
		RValue ui = UiValue();
		if (KindOf(ui) == VALUE_UNDEFINED) return false;
		RValue menu = g_Yytk->CallBuiltin("variable_instance_get", { ui, RValue("menu_show") });
		if (KindOf(menu) != VALUE_STRING || menu.ToString() != "story") return false;
		const Def& d = *ContentOf(MissionsFor);
		double k = Args[0]->ToDouble() - static_cast<double>(GameMissions);
		if (k < 0 || k >= static_cast<double>(d.Missions.a.size())) return false;
		const Json& m = d.Missions.a[static_cast<size_t>(k)];
		Running = { MissionsFor, static_cast<size_t>(k), m.Find("layout") ? -1 : m.GetNum("map", -1) };
		if (AreaMission(d, static_cast<size_t>(k)))                // its act's tiles for the run (act + 0.5 hides it from the acts)
			if (RValue* env = AreaActCell(GameMissions + static_cast<size_t>(k)))
			{
				*env = RValue(std::floor(env->ToDouble()));
				AreaRowRef = GameMissions + static_cast<size_t>(k);
			}
		MissionEvent("mission_started", d, static_cast<size_t>(k), -1);
		MissionLines.push_back({ MissionsFor, static_cast<size_t>(k), true, MicrosSinceStart() / 1e6 });
		return false;
	}
	void PostRunStart(CInstance*, int, RValue**, RValue&)
	{
		if (AreaRowRef != SIZE_MAX)                              // hidden from the acts again (unless the table was made anew)
			if (RValue* env = AreaActCell(AreaRowRef); env && env->ToDouble() == std::floor(env->ToDouble())) *env = RValue(env->ToDouble() + 0.5);
		AreaRowRef = SIZE_MAX;
		if (Running.Hero >= 0 && Running.Map >= 0) PresetRun(Running.Map);
	}
	bool PreRunOver(CInstance*, int, RValue**, RValue&)
	{
		if (Running.Hero >= 0 && Running.Map >= 0) PresetRun(static_cast<double>(GameMissions + Running.K));
		Running = {};
		return false;
	}
	bool PreMapMissionDialogue(CInstance*, int, RValue**, RValue& Result)
	{
		if (Running.Hero < 0) return false;
		Result = RValue(0.0);
		return true;
	}
	void WatchMissions()
	{
		if (Def* dp = ContentOf(MissionsFor))
		{
			Def& d = *dp;
			RValue* md = FindMember(GlobalRValue(), "mission_data");
			RValue* row = md && KindOf(*md) == VALUE_ARRAY ? ArrayAt(*md, static_cast<size_t>(MissionsFor)) : nullptr;
			for (size_t k = 0; row && KindOf(*row) == VALUE_ARRAY && k < d.MissionRuns.size() && k < d.MissionSeen.size(); ++k)
			{
				RValue* v = ArrayAt(*row, GameMissions + k);
				if (!v || !IsNumeric(*v) || v->ToDouble() <= d.MissionSeen[k]) continue;
				d.MissionRuns[k] = d.MissionSeen[k] = v->ToDouble();
				AchData(d, "missions").Set(d.Missions.a[k].GetStr("id"), d.MissionRuns[k]);
				MissionEvent("mission_done", d, k, d.MissionRuns[k]);
				MissionLines.push_back({ MissionsFor, k, false, MicrosSinceStart() / 1e6 });
			}
		}
		double now = MicrosSinceStart() / 1e6;
		std::erase_if(MissionLines, [&](const MissionLine& L) { return now - L.At > 120; });   // (never there: dropped)
		if (MissionLines.empty()) return;
		// said once its hero is there with no menu nor dialogue up: an intro in a map, an outro in the hub
		RValue ui = UiValue();
		if (KindOf(ui) == VALUE_UNDEFINED || !FirstInstance(AssetIndex("obj_parent_hero"))) return;
		RValue menu = g_Yytk->CallBuiltin("variable_instance_get", { ui, RValue("menu_show") });
		RValue dial = g_Yytk->CallBuiltin("variable_instance_get", { ui, RValue("dial_ref") });
		if (KindOf(menu) != VALUE_STRING || menu.ToString() != "none" || (KindOf(dial) == VALUE_STRING && !dial.ToString().empty())) return;
		RValue room;
		std::string where = AurieSuccess(g_Yytk->GetBuiltin("room", nullptr, NULL_INDEX, room)) ? GmlString(g_Yytk->CallBuiltin("room_get_name", { room })) : "";
		MissionLine l = MissionLines.front();
		if (l.Intro == (where == "rm_the_hub") || !ContentOf(l.Hero)) return;
		MissionLines.erase(MissionLines.begin());
		const Def& d = *ContentOf(l.Hero);
		if (l.K >= d.Missions.a.size()) return;
		const Json* text = d.Missions.a[l.K].Find(l.Intro ? "intro" : "outro");
		if (!text) return;
		std::string speaker = d.TalkSelf ? CitizenTag(d) : "hero";
		// (a key as the game's, "<citizen> - ...": in a map the dialogue reads a game citizen's number from it - 13, the
		// Knight's; a class's citizen there leaves its box without a portrait index: an error each frame)
		std::string key = "13 - SlormAPI " + d.Key() + " " + d.Missions.a[l.K].GetStr("id") + (l.Intro ? " - In" : " - Back");
		bool made = DialogueLine(key, "13 - Default", [&](size_t Lang)
		{
			std::string t = LangText(text, Lang);
			while (!t.empty() && (t.back() == '/' || t.back() == '\n')) t.pop_back();
			return (t.starts_with("[") ? "" : "[" + speaker + "]") + t + "/";
		});
		if (!made) return;
		CInstance* u = ui.ToInstance();
		RValue r;
		g_Yytk->CallGameScriptEx(r, "gml_Script_scr_clear_dialogue", u, u, {});
		g_Yytk->CallGameScriptEx(r, "gml_Script_scr_create_dialogue", u, u, { RStr(key) });
	}

	// ---- its activable legacies (kind "aura" / "active"): the game puts them on the skill bar (aura_array, keys 1-4),
	// checks their cooldown and cost from the table, and calls scr_aura_toggle(id, slot), whose effects are its own
	// code by legacy number. A class's (ids from 150) are done here: an aura toggles a buff with its stats (value +
	// per_rank x rank, until toggled off) and locks its cost of mana / life (cost_type mana_lock / life_lock); an active
	// pays its cost (mana, life, life_percent), gives its stats for "duration" seconds and starts its cooldown. Either
	// raises "legacy_used" {class, legacy, slot, kind, on, hero} for the class's own effects.
	std::vector<std::pair<std::string, double>> LegacyStats(const Json& L, double Rank)
	{
		std::vector<std::pair<std::string, double>> out;
		auto list = [](const Json* J)
		{
			std::vector<Json> v;
			if (!J) return v;
			if (J->type == Json::Arr) v = J->a;
			else if (J->type == Json::Str)
				for (size_t at = 0; at <= J->s.size();)
				{
					size_t bar = J->s.find('|', at);
					v.push_back(Json(J->s.substr(at, bar == std::string::npos ? std::string::npos : bar - at)));
					if (bar == std::string::npos) break;
					at = bar + 1;
				}
			else v.push_back(*J);
			return v;
		};
		std::vector<Json> stats = list(L.Find("stat")), values = list(L.Find("value")), per = list(L.Find("per_rank"));
		for (size_t i = 0; i < stats.size(); ++i)
		{
			if (stats[i].type != Json::Str || stats[i].s.empty()) continue;
			bool known = false;
			for (auto& c : gd::kStatCatalog) known |= stats[i].s == c.Key;
			if (!known) continue;
			double v = i < values.size() && values[i].type == Json::Num ? values[i].n : 0;
			double r = i < per.size() && per[i].type == Json::Num ? per[i].n : 0;
			out.emplace_back(stats[i].s, v + r * Rank);
		}
		return out;
	}
	size_t LegacyFirst(const Def& D) { return KindOf(D.Elements) == VALUE_ARRAY ? ArrayLen(D.Elements) - D.Legacies.a.size() : 0; }
	// the row of the K-th of its own legacies (its table built: after the game's rows it copies - BuildElements; else
	// where they will be)
	size_t LegacyRow(const Def& D, size_t K)
	{
		size_t first = LegacyFirst(D);
		return (first ? first : GameLegacyRows()) + K;
	}
	double LegacyRankOf(int Hero, size_t Row)
	{
		RValue* ranks = FindMember(GlobalRValue(), "elements_rank");
		RValue* mine = ranks ? ArrayAt(*ranks, static_cast<size_t>(Hero)) : nullptr;
		RValue* r = mine && KindOf(*mine) == VALUE_ARRAY ? ArrayAt(*mine, Row) : nullptr;
		return r && IsNumeric(*r) ? r->ToDouble() : 0;
	}
	void AuraBuff(Def& D, CInstance* Hero, size_t Row, bool On)
	{
		size_t li = Row - LegacyFirst(D);
		if (!Hero || li >= D.Legacies.a.size()) return;
		const Json& L = D.Legacies.a[li];
		RValue hero(Hero);
		std::string key = LegacyBuffKey(D, Row);
		if (!On) { BuffRemove(hero, key); return; }
		int h = HeroOf(D);
		BuffAdd(hero, key, LegacyStats(L, LegacyRankOf(h, Row)), -1, 1, 1, L.GetStr("name"), "", L.GetStr("buff_icon"), false);
	}
	// the costs its toggled auras lock, in % of the maximum: added to the hero's mana_lock / health_lock after each
	// stats refresh (the game counts those there from its own auras only), the current mana / life cut to what is left
	void AuraLocks(Def& D, CInstance* Hero)
	{
		if (D.AurasOn.empty() || !Hero) return;
		size_t first = LegacyFirst(D);
		double mana = 0, life = 0;
		for (int row : D.AurasOn)
		{
			if (static_cast<size_t>(row) < first || static_cast<size_t>(row) - first >= D.Legacies.a.size()) continue;
			const Json& L = D.Legacies.a[static_cast<size_t>(row) - first];
			std::string type = L.GetStr("cost_type", "mana_lock");
			if (type == "mana_lock") mana += L.GetNum("cost", 0);
			else if (type == "life_lock") life += L.GetNum("cost", 0);
		}
		RValue self(Hero);
		auto add = [&](const char* Lock, const char* Cur, const char* Max, double Amount)
		{
			RValue* lock = FindMember(self, Lock);
			if (Amount <= 0 || !lock || !IsNumeric(*lock)) return;
			double v = (std::min)(95.0, lock->ToDouble() + Amount);
			*lock = RValue(v);
			RValue* cur = FindMember(self, Cur);
			RValue* max = FindMember(self, Max);
			if (cur && max && IsNumeric(*cur) && IsNumeric(*max))
				*cur = RValue((std::min)(cur->ToDouble(), (std::max)(1.0, max->ToDouble() * (1 - v / 100))));
		};
		add("mana_lock", "the_mana", "final_the_max_mana", mana);
		add("health_lock", "the_health", "final_the_max_health", life);
	}
	void RefreshStats(CInstance* Hero)
	{
		if (!Hero) return;
		RValue self(Hero);
		if (RValue* f = FindMember(self, "self_refresh_stats")) *f = RValue(true);
	}
	bool AuraToggle(CInstance* Self, int ArgCount, RValue** Args);
	bool PreAuraToggle(CInstance* Self, int ArgCount, RValue** Args, RValue&)
	{
		try { return AuraToggle(Self, ArgCount, Args); }            // (an exception out of a hook ends the game)
		catch (const std::exception& e) { DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormAPI] class legacy: %s", e.what()); return true; }
		catch (...) { return true; }
	}
	bool AuraToggle(CInstance* Self, int ArgCount, RValue** Args)
	{
		Def* d = Content();
		if (!d || ArgCount < 1 || !Args[0] || !IsNumeric(*Args[0]) || KindOf(*Args[0]) == VALUE_STRING) return false;
		double id = Args[0]->ToDouble();
		size_t first = LegacyFirst(*d);
		if (first == 0 || id < static_cast<double>(first) || id >= static_cast<double>(first + d->Legacies.a.size())) return false;
		size_t row = static_cast<size_t>(id);
		const Json& L = d->Legacies.a[row - first];
		std::string kind = L.GetStr("kind", "stat");
		double slot = ArgCount > 1 && Args[1] && IsNumeric(*Args[1]) ? Args[1]->ToDouble() : -1;
		CInstance* hero = Self ? Self : FirstInstance(AssetIndex("obj_parent_hero"));
		if (!hero) return true;
		RValue self(hero);
		auto num = [&](const char* V) { RValue* v = FindMember(self, V); return v && IsNumeric(*v) ? v->ToDouble() : 0.0; };
		auto set = [&](const char* V, double X) { if (RValue* v = FindMember(self, V)) *v = RValue(X); };
		bool on = false;
		if (kind == "aura")
		{
			on = !d->AurasOn.count(static_cast<int>(row));
			if (on) d->AurasOn.insert(static_cast<int>(row)); else d->AurasOn.erase(static_cast<int>(row));
			AuraBuff(*d, hero, row, on);
			RefreshStats(hero);                                  // (its lock: AuraLocks)
			RValue r;
			g_Yytk->CallGameScriptEx(r, "gml_Script_scr_elements_start_cooldown", hero, hero, { RValue(id), RValue(0.0) });
		}
		else
		{
			std::string type = L.GetStr("cost_type", "mana");
			double cost = L.GetNum("cost", 0);
			if (type == "mana")
			{
				RValue c;
				g_Yytk->CallGameScriptEx(c, "gml_Script_scr_modified_element_cost", hero, hero, { RValue(cost) });
				double pay = IsNumeric(c) ? c.ToDouble() : cost;
				if (num("the_mana") < pay) return true;
				set("the_mana", num("the_mana") - pay);
			}
			else if (type == "life" || type == "life_percent")
			{
				double pay = type == "life" ? cost : num("final_the_max_health") * cost / 100;
				if (num("the_health") <= pay) return true;
				set("the_health", num("the_health") - pay);
			}
			auto stats = LegacyStats(L, LegacyRankOf(HeroOf(*d), row));
			if (!stats.empty())
				BuffAdd(self, LegacyBuffKey(*d, row), stats, L.GetNum("duration", 5), 1, 1, L.GetStr("name"), "", L.GetStr("buff_icon"), false);
			RValue r;
			g_Yytk->CallGameScriptEx(r, "gml_Script_scr_elements_start_cooldown", hero, hero, { RValue(id), RValue(std::round(L.GetNum("cooldown", 0) * 60)) });
		}
		Json e = Json::Object();
		e.Add("class", d->Key());
		e.Add("legacy", static_cast<double>(row));
		if (const Json* lid = L.Find("id"); lid && lid->type == Json::Str) e.Add("id", lid->s);
		size_t li = row - first;                                     // (its mod's: its own list's place - rows move)
		e.Add("owner", li < d->LegacyFrom.size() ? d->LegacyFrom[li].first : d->Owner);
		e.Add("index", static_cast<double>(li < d->LegacyFrom.size() ? d->LegacyFrom[li].second : static_cast<int>(li)));
		e.Add("kind", kind);
		e.Add("on", on);
		e.Add("slot", slot);
		Json h = Json::Object();
		h.Add("$inst", static_cast<double>(g_Yytk->CallBuiltin("variable_instance_get", { self, RValue("id") }).ToDouble()));
		e.Add("hero", h);
		g_DirectEvents.emplace_back("legacy_used", std::move(e));
		return true;                                             // (the game's toggle knows only its own legacies)
	}
	// a hero made again (a new room, a switch): its toggled auras' buffs again (their locks stay on the class)
	void ReapplyAuras(CInstance* Hero)
	{
		Def* d = Content();
		if (!d || !Hero) return;
		for (int row : d->AurasOn)
			try
			{
				AuraBuff(*d, Hero, static_cast<size_t>(row), true);
			}
			catch (...) {}
	}
	// its auras toggled on before its mods' parts changed (by legacy: their rows may have moved), on again once its table
	// is in place; a gone mod's stay pending until it is back
	void ApplyPendingAuras(Def& D, CInstance* Hero)
	{
		size_t first = LegacyFirst(D);
		if (D.AurasPending.empty() || !first) return;
		bool any = false;
		for (size_t k = 0; k < D.Legacies.a.size(); ++k)
			if (D.AurasPending.erase(LegacyKey(D, k)))
			{
				D.AurasOn.insert(static_cast<int>(first + k));
				if (Hero) AuraBuff(D, Hero, first + k, true);
				any = true;
			}
		if (any && Hero) RefreshStats(Hero);
	}

	// the game's legacy legendaries (HERO 99 / 999, working on the legacies in their SKILL column) do not drop for a
	// class whose tree hides all of those legacies: out of the pool (HERO -2) while an item is made (scr_loot_roll_legendary
	// is called directly - no hook sees it - so around its callers)
	std::vector<std::pair<RValue*, RValue>> LegacyLegs;
	int RollDepth = 0;
	// its HERO cell when the row is a legacy legendary working only on legacies D's tree hides
	RValue* HiddenLegacyLegendary(const Def& D, RValue* Row)
	{
		RValue* hero = Row && KindOf(*Row) == VALUE_ARRAY ? ArrayAt(*Row, 9) : nullptr;
		RValue* skill = hero ? ArrayAt(*Row, 10) : nullptr;
		if (!skill || !IsNumeric(*hero) || KindOf(*hero) == VALUE_STRING || (hero->ToDouble() != 99 && hero->ToDouble() != 999)) return nullptr;
		std::string ids = CellText(*skill);
		bool any = false, all = true;
		for (size_t at = 0; at <= ids.size();)
		{
			size_t bar = ids.find('|', at);
			std::string id = ids.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
			if (!id.empty()) { any = true; all = all && D.HiddenLegacies.count(std::atoi(id.c_str())); }
			if (bar == std::string::npos) break;
			at = bar + 1;
		}
		return any && all ? hero : nullptr;
	}
	bool PreLegendaryRoll(CInstance*, int, RValue**, RValue&)
	{
		if (RollDepth++ > 0) return false;
		Def* d = Content();
		RValue* table = FindMember(GlobalRValue(), "legendary");
		if (!d || d->HiddenLegacies.empty() || ElementsFor != HeroOf(*d) || !table || KindOf(*table) != VALUE_ARRAY) return false;
		for (size_t r = 0, n = ArrayLen(*table); r < n; ++r)
			if (RValue* hero = HiddenLegacyLegendary(*d, ArrayAt(*table, r))) { LegacyLegs.push_back({ hero, *hero }); *hero = RValue(-2.0); }
		if (!d->LegsLogged)
		{
			d->LegsLogged = true;
			DbgPrintEx(LOG_SEVERITY_INFO, "[SlormAPI] %s: %zu legacy legendaries left out of its drops", d->Name.c_str(), LegacyLegs.size());
		}
		return false;
	}
	void PostLegendaryRoll(CInstance*, int, RValue**, RValue&)
	{
		if (RollDepth > 0 && --RollDepth > 0) return;
		for (auto& [where, was] : LegacyLegs) *where = was;
		LegacyLegs.clear();
	}
	// the Netherworld's rewards (warlords_data[hero]: per node of its map, a Warlord's legendaries) are made by
	// scr_ui_warlords_legendary_list_create from the same rows as a drop (HERO -1, 99, 999 or the current hero), so it
	// goes through the filter above too. Rewards made otherwise (an older build, a tree that hides more since) are made
	// again when they hold a legendary its tree hides - checked each time its legacy table goes in place
	bool HidesWarlordReward(const Def& D, RValue& Mine)
	{
		RValue* nodes = FindMember(GlobalRValue(), "warlords");
		RValue* legs = FindMember(GlobalRValue(), "legendary");
		if (!nodes || !legs || KindOf(*nodes) != VALUE_ARRAY || KindOf(*legs) != VALUE_ARRAY) return false;
		for (size_t w = 0, n = (std::min)(ArrayLen(*nodes), ArrayLen(Mine)); w < n; ++w)
		{
			RValue* node = ArrayAt(*nodes, w);                      // a Warlord's node: its affixes; a cataclysm's: ""
			RValue* affixes = node && KindOf(*node) == VALUE_ARRAY ? ArrayAt(*node, 2) : nullptr;
			if (!affixes || KindOf(*affixes) != VALUE_STRING || affixes->ToString().empty()) continue;
			RValue* list = ArrayAt(Mine, w);                        // (ids: numbers, or texts as a save loads them)
			for (size_t k = 0, m = list && KindOf(*list) == VALUE_ARRAY ? ArrayLen(*list) : 0; k < m; ++k)
			{
				double id = std::strtod(CellText(*ArrayAt(*list, k)).c_str(), nullptr);
				if (id >= 0 && id < static_cast<double>(ArrayLen(*legs)) && HiddenLegacyLegendary(D, ArrayAt(*legs, static_cast<size_t>(id))))
					return true;
			}
		}
		return false;
	}
	void CheckWarlordRewards()
	{
		Def* d = Content();
		int me = d ? HeroOf(*d) : -1;
		if (!d || d->HiddenLegacies.empty() || ElementsFor != me) { WarlordsChecked = -1; return; }
		if (WarlordsChecked == me) return;
		RValue ui = modspage::UiInstance();
		if (KindOf(ui) == VALUE_UNDEFINED) return;
		WarlordsChecked = me;
		RValue* data = FindMember(GlobalRValue(), "warlords_data");
		RValue* mine = data && KindOf(*data) == VALUE_ARRAY ? ArrayAt(*data, static_cast<size_t>(me)) : nullptr;
		if (!mine || KindOf(*mine) != VALUE_ARRAY || !HidesWarlordReward(*d, *mine)) return;
		CInstance* self = InstanceById(static_cast<int32_t>(ui.ToInt64()));
		RValue r;
		g_Yytk->CallGameScriptEx(r, "gml_Script_scr_ui_warlords_legendary_list_create", self, self, {});
		DbgPrintEx(LOG_SEVERITY_INFO, "[SlormAPI] %s: its Warlords' rewards made again (a legendary its tree hides)", d->Name.c_str());
	}

	// a filler row of the played class's own kit (TYPE "none": the base hero's rows 11.. it does not use). Game code
	// that reaches one by the base hero's numbers - the achievements' jump to a skill's upgrade (the Knight's
	// achievements name his rows) - would show or rank it: the game takes a row it does not know for a skill
	// (index out of bounds in scr_get_extra_stats_from_loot) and its tooltip has no values
	bool FillerRow(const RValue& Row)
	{
		Def* d = Playing();
		if (!d || !d->Own || !IsNumeric(Row) || KindOf(Row) == VALUE_STRING || Row.ToDouble() < 11) return false;
		RValue* skills = FindMember(GlobalRValue(), "skills");
		RValue* h = CurrentHero();
		RValue* table = skills && h && IsNumeric(*h) ? ArrayAt(*skills, static_cast<size_t>(h->ToDouble())) : nullptr;
		RValue* r = table && KindOf(*table) == VALUE_ARRAY ? ArrayAt(*table, static_cast<size_t>(Row.ToDouble())) : nullptr;
		RValue* type = r && KindOf(*r) == VALUE_ARRAY ? ArrayAt(*r, 14) : nullptr;
		return type && KindOf(*type) == VALUE_STRING && type->ToString() == "none";
	}
	// its level is 0
	bool PreSkillLevel(CInstance*, int ArgCount, RValue** Args, RValue& Result)
	{
		if (ArgCount < 1 || !Args[0] || !FillerRow(*Args[0])) return false;
		Result = RValue(0.0);
		return true;
	}
	// (and the skills menu shows no tooltip for it: BeforeEvent, before the UI's step)

	// while a class is played the game's achievements are left alone: no checks, no Steam unlocks (their checks also
	// index the game's 150 legacies / 3 heroes with the class's numbers); nor in the view (its rows are the classes')
	bool PreAchievements(CInstance*, int, RValue**, RValue& Result)
	{
		if (!Playing() && !AchIn) return false;
		Result = RValue(0.0);
		return true;
	}
	// an achievement's count [steps done, steps, value, next step]: a class's row - its count from Tick; a game row while
	// a class is played - the game's own (on the game's legacy table and the three heroes) where it is shown (the view),
	// "nothing done yet" for the account's stats (scr_global_fill_stats, which sets Steam achievements)
	int FillStatsDepth = 0;
	// (the account's stats also set Steam achievements: not while SlormAPI counts - a class's data may be in a hero's place)
	bool PreFillStats(CInstance* Self, int ArgCount, RValue** Args, RValue& Result)
	{
		if (AchPass) return true;                                // (answered here: its Post does not run)
		// a stat the game does not track - a dialogue of SlormAPI's closing counts as read under its key - is skipped
		// (the game indexes player_stats by stats_track[name] unchecked: an error)
		if (ArgCount >= 1 && Args[0] && KindOf(*Args[0]) == VALUE_STRING)
			if (RValue* track = FindMember(GlobalRValue(), "stats_track"))
				if (KindOf(g_Yytk->CallBuiltin("ds_map_find_value", { *track, *Args[0] })) == VALUE_UNDEFINED) return true;
		++FillStatsDepth;
		return PreThreeHeroes(Self, ArgCount, Args, Result);
	}
	void PostFillStats(CInstance* Self, int ArgCount, RValue** Args, RValue& Result)
	{
		FillStatsDepth = (std::max)(0, FillStatsDepth - 1);
		PostThreeHeroes(Self, ArgCount, Args, Result);
	}
	bool PreAchievementProgression(CInstance* Self, int ArgCount, RValue** Args, RValue& Result)
	{
		if (AchPass) return false;
		double ref = ArgCount > 0 && Args[0] && IsNumeric(*Args[0]) && KindOf(*Args[0]) != VALUE_STRING ? Args[0]->ToDouble() : -1;
		if (AchIn && ref >= 0 && static_cast<size_t>(ref) < AchParked.size() && AchParked[static_cast<size_t>(ref)])
		{
			Result = RValue(std::vector<RValue>{ RValue(0.0), RValue(0.0), RValue(0.0), RValue(0.0) });
			return true;
		}
		if (AchGameRows && ref >= static_cast<double>(AchGameRows))
		{
			Def::Achievement* a = AchievementAt(ref);
			const double none[4] = { 0, 1, 0, 1 };
			const double* p = a ? a->Progress : none;
			Result = RValue(std::vector<RValue>{ RValue(p[0]), RValue(p[1]), RValue(p[2]), RValue(p[3]) });
			return true;
		}
		if (!Playing()) return false;
		if (AchIn && FillStatsDepth == 0) { Result = GameProgression(Self, ref); return true; }
		Result = RValue(std::vector<RValue>{ RValue(0.0), RValue(1.0), RValue(0.0), RValue(1.0) });
		return true;
	}
	bool PreProfileStep(CInstance* Self, int ArgCount, RValue** Args, RValue& Result) { AchEnter(); return PreThreeHeroes(Self, ArgCount, Args, Result); }
	void PostProfileStep(CInstance* Self, int ArgCount, RValue** Args, RValue& Result) { PostThreeHeroes(Self, ArgCount, Args, Result); AchLeave(); }

	void EnsureHooks()
	{
		if (Hooked) return;
		Hooked = true;
		legend::EnsureHooks();                                    // theirs first: custom legendaries of custom heroes are
		NativeHookInstall("scr_general_skill_cast", { "SlormAPI.classes", PreCast, PostCast });   // swapped like the rest
		for (const char* text : { "scr_ui_skill_full_description", "scr_ui_skill_text" })
			NativeHookInstall(text, { "SlormAPI.classes", PreSkillText, PostSkillText });
		NativeHookInstall("scr_save_save", { "SlormAPI.classes", PreSave, PostSave });
		NativeHookInstall("scr_achievements_check_progression", { "SlormAPI.classes", PreAchievements, nullptr });
		NativeHookInstall("scr_achievement_get_progression", { "SlormAPI.classes", PreAchievementProgression, nullptr });
		NativeHookInstall("scr_save_load", { "SlormAPI.classes", PreLoad, PostLoad });
		NativeHookInstall("scr_hero_switch", { "SlormAPI.classes", PreSwitch, PostSwitch });
		NativeHookInstall("scr_citizen_action", { "SlormAPI.classes", PreCitizenAction, nullptr });
		NativeHookInstall("scr_create_citizen", { "SlormAPI.classes", PreCreateCitizen, PostCreateCitizen });
		NativeHookInstall("scr_citizen_check_if_unlocked", { "SlormAPI.classes", nullptr, PostCitizenUnlocked });
		NativeHookInstall("scr_hub_load_hero", { "SlormAPI.classes", PreHubLoadHero, PostHubLoadHero });
		NativeHookInstall("scr_stats_refresh", { "SlormAPI.classes", nullptr, PostStatsRefresh });
		NativeHookInstall("scr_string", { "SlormAPI.classes", nullptr, PostString });
		NativeHookInstall("scr_ui_menus_class_selector", { "SlormAPI.classes", PreThreeHeroes, PostPicker });
		NativeHookInstall("scr_global_fill_stats", { "SlormAPI.classes", PreFillStats, PostFillStats });
		// the main menu's save panels: the save's three heroes (it draws a 4th head as the Knight - portraits 13, 14, 15 by index)
		NativeHookInstall("scr_ui_menus_main", { "SlormAPI.classes", PreThreeHeroes, PostThreeHeroes });
		NativeHookInstall("scr_save_get_level", { "SlormAPI.classes", PreThreeHeroes, PostThreeHeroes });
		NativeHookInstall("scr_save_get_profile", { "SlormAPI.classes", PreThreeHeroes, PostThreeHeroes });
		NativeHookInstall("scr_ui_menus_profile", { "SlormAPI.classes", PreProfile, PostProfileDraw });
		NativeHookInstall("scr_ui_menus_profile_step", { "SlormAPI.classes", PreProfileStep, PostProfileStep });
		NativeHookInstall("scr_ui_notifications", { "SlormAPI.classes", PreAchWindow, PostAchWindow });
		NativeHookInstall("scr_achievement_pagination", { "SlormAPI.classes", PrePaging, PostPaging });
		NativeHookInstall("scr_element_determine", { "SlormAPI.classes", nullptr, PostElementDetermine });
		NativeHookInstall("scr_skill_cast_list_add_all", { "SlormAPI.classes", nullptr, PostCastListAdd });
		NativeHookInstall("scr_general_skill_damage", { "SlormAPI.classes", nullptr, PostSkillDamage });
		NativeHookInstall("scr_hero_step", { "SlormAPI.classes", PreHeroStep, nullptr });
		NativeHookInstall("scr_hero_element_create", { "SlormAPI.classes", nullptr, PostHeroElementCreate });
		NativeHookInstall("scr_get_skill_level", { "SlormAPI.classes", PreSkillLevel, nullptr });
		NativeHookInstall("scr_aura_toggle", { "SlormAPI.classes", PreAuraToggle, nullptr });
		NativeHookInstall("scr_story_mission_coordinates", { "SlormAPI.classes", nullptr, PostMissionCoordinates });
		NativeHookInstall("scr_run_start", { "SlormAPI.classes", PreRunStart, PostRunStart });
		NativeHookInstall("scr_create_dialogue", { "SlormAPI.classes", nullptr, PostCreateDialogue });
		NativeHookInstall("scr_floor_end", { "SlormAPI.classes", PreRunOver, nullptr });
		NativeHookInstall("scr_weapon_check_if_available_effect", { "SlormAPI.classes", PreReaperEffect, nullptr });
		NativeHookInstall("scr_play_sound", { "SlormAPI.classes", PrePlaySound, nullptr });
		NativeHookInstall("scr_create_emitter", { "SlormAPI.classes", nullptr, PostCreateEmitter });
		NativeHookInstall("scr_damage_dodge_check", { "SlormAPI.classes", PreDodgeCheck, PostDodgeCheck });
		NativeHookInstall("scr_ui_menus_inventory", { "SlormAPI.classes", nullptr, PostInventory });
		NativeHookInstall("scr_run_end", { "SlormAPI.classes", PreRunOver, nullptr });
		for (const char* name : { "scr_story_trigger_dialogue_mission_start", "scr_story_trigger_dialogue_mission_end" })
			NativeHookInstall(name, { "SlormAPI.classes", PreMapMissionDialogue, nullptr });
		// the reaper in hand is set when a hero is made (a switch, a load): the switch tables for the hero now, not the
		// one before (else a base hero after its class finds no case: no sprite)
		NativeHookInstall("scr_inventory_equip_weapon", { "SlormAPI.classes", [](CInstance*, int, RValue**, RValue&) { ReaperCases(); return false; }, nullptr });
		for (const char* name : { "scr_create_loot_equipment", "scr_loot_roll_legendary", "scr_ui_warlords_legendary_list_create" })
			NativeHookInstall(name, { "SlormAPI.classes", PreLegendaryRoll, PostLegendaryRoll });
	}

	// a periodic check (the UI object is made again with each room): every structure has the custom heroes' entries
	double NextCheck = 0;
	void Tick()
	{
		DodgeChecks.clear();                                      // (a check stopped by an error: no Post)
		if (!AnyContent()) return;
		double now = MicrosSinceStart() / 1e6;
		if (now < NextCheck) return;
		NextCheck = now + 0.5;
		if (!Defs.empty())
		{
			if (Took.empty()) Extend();
			Resume();
		}
		SyncElements();
		CheckWarlordRewards();
		WatchMissions();
		ReaperPassives();
		ForEachContent([](Def& d) { ApplySkin(d); });
		AreaEntrances();
		RValue ui = UiValue();
		RValue menu = KindOf(ui) == VALUE_UNDEFINED ? RValue() : g_Yytk->CallBuiltin("variable_instance_get", { ui, RValue("menu_show") });
		bool profile = KindOf(menu) == VALUE_STRING && menu.ToString() == "profile";
		if (!profile) SlotList = AchList = -1;
		RefreshAchievements();
	}
}

// {"owner", "id", "name", "title", "base": 0|1|2, "hub_x", "hub_y", "skills": {"<row>": {"name", "description",
// <dat_cla field>: value}}} -> {"hero": its hero number, "citizen": its citizen}. title: its name in the menus ("The
// Mighty Knight" for the Knight; default: name). weapon: its reapers' noun (default: the base's). attribute: the
// attribute (0-7) a new hero starts with a point in (default: the base's). Its look: its base hero's, with its "art".
// a hero's content options - art, achievements, missions, skins, sounds, reapers (register_class, register_hero): checked
// (Fail, before anything changes) and the ones kept as data parsed. Base: the class's base hero (its game achievements
// "base" names); -1 for a game hero's content
struct ContentOptions
{
	std::map<int, int> Art;
	std::map<std::string, double> Sounds;
	std::vector<classes::Def::ReaperCell> ReaperCells;
	std::map<int, int> ReaperArt, ReaperArtPrimordial;
	std::set<int> ReaperOwn;
	std::map<int, std::vector<classes::Def::ReaperStat>> ReaperStats;
};
static ContentOptions ParseContentOptions(const Json& Req, int base)
{
	ContentOptions o;
	auto& art = o.Art;
	auto& sounds = o.Sounds;
	auto& reaperCells = o.ReaperCells;
	auto& reaperArt = o.ReaperArt;
	auto& reaperArtPrimordial = o.ReaperArtPrimordial;
	auto& reaperOwn = o.ReaperOwn;
	auto& reaperStats = o.ReaperStats;
	if (const Json* a = Req.Find("art"); a && a->type == Json::Obj)
		for (auto& [name, v] : a->o)
		{
			int from = static_cast<int>(AssetIndex(name.c_str()).ToDouble());
			int to = v.type == Json::Num ? static_cast<int>(v.n) : v.type == Json::Str ? static_cast<int>(AssetIndex(v.s.c_str()).ToDouble()) : -1;
			if (from < 0) Fail("art: no game sprite " + name);
			if (to < 0 || !g_Yytk->CallBuiltin("sprite_exists", { RValue(static_cast<double>(to)) }).ToBoolean()) Fail("art: " + name + ": no such sprite");
			art[from] = to;
		}
	if (const Json* list = Req.Find("achievements"); list && list->type == Json::Arr)
	{
		std::set<std::string> ids;
		for (auto& j : list->a)
		{
			if (j.type == Json::Obj && j.Find("base"))                // renames one of its base hero's
			{
				const Json* b = j.Find("base");
				int ref = b->type == Json::Num ? static_cast<int>(b->n) : b->type == Json::Str && b->s.starts_with("base_") ? std::atoi(b->s.c_str() + 5) : -1;
				RValue* rows = FindMember(GlobalRValue(), "achievements");
				RValue* row = rows && KindOf(*rows) == VALUE_ARRAY && ref >= 0 ? ArrayAt(*rows, static_cast<size_t>(ref)) : nullptr;
				if (classes::AchIn && classes::AchGameRows) row = ref >= 0 ? ArrayAt(classes::AchGame, static_cast<size_t>(ref)) : nullptr;
				RValue* cat = row ? ArrayAt(*row, classes::kAchCat) : nullptr;
				if (base < 0) Fail("achievements: base: a class renames its base hero's (a game hero's own stay the game's)");
				if (!cat || !IsNumeric(*cat) || static_cast<int>(cat->ToDouble()) != 4 + base)
					Fail("achievements: base " + std::to_string(ref) + " is not one of its base hero's");
				continue;
			}
			std::string aid = j.type == Json::Obj ? j.GetStr("id") : "";
			if (aid.empty()) Fail("achievements: each needs an id");
			if (aid.starts_with("base_")) Fail("achievements: " + aid + ": ids base_* are its base hero's");
			if (!ids.insert(aid).second) Fail("achievements: " + aid + " twice");
		}
	}
	if (const Json* ms = Req.Find("missions"))
	{
		size_t games = classes::GameMissionCount();
		if (ms->type != Json::Arr) Fail("missions: a list");
		if (!games) Fail("missions: no story table yet");
		std::set<std::string> ids;
		for (auto& m : ms->a)
		{
			std::string mid = m.type == Json::Obj ? m.GetStr("id") : "";
			if (mid.empty()) Fail("missions: each needs an id");
			if (!ids.insert(mid).second) Fail("missions: " + mid + " twice");
		}
		for (auto& m : ms->a)
		{
			std::string mid = "missions: " + m.GetStr("id") + ": ";
			double map = m.GetNum("map", -1);
			bool layout = m.Find("layout") && m.Find("layout")->type == Json::Str;
			if (map >= static_cast<double>(games) || (map < 0 && !layout)) Fail(mid + "map: a game mission (0-" + std::to_string(games - 1) + "), or a layout");
			double mapAct = map >= 0 ? classes::MissionCell(static_cast<size_t>(map), classes::kMisAct).ToDouble() : -1;
			double act = map >= 0 && !layout ? mapAct : m.GetNum("act", mapAct);
			if (map >= 0 && (m.Find("act") || m.Find("objective")) && !layout)
				Fail(mid + "a game mission's map plays its act and objective (act / objective: with a layout)");
			if (act < 0 || act > 4) Fail(mid + (map >= 0 && !layout ? "map: a mission of the five acts (0-52)" : "act: 0 Graveyard, 1 Prison, 2 Academy, 3 Gardens, 4 Royal Wing"));
			if (map < 0 && classes::Objective(m.Find("objective")).empty())
				Fail(mid + "objective: enemies, breach, breaches, waves, bonus_breakables, leave, boss, exit, lever, escort, named or emblem");
			for (const Json* a : classes::ListOf(m.Find("after")))
				if (!(a->type == Json::Num && a->n >= 0 && a->n < static_cast<double>(games)) && !(a->type == Json::Str && ids.count(a->s)))
					Fail(mid + "after: one of its missions' ids or a game mission (0-" + std::to_string(games - 1) + ")");
		}
	}
	// "skins": [{id, name, art: {game sprite: its sprite}}]
	if (const Json* sk = Req.Find("skins"))
	{
		if (sk->type != Json::Arr) Fail("skins: a list");
		std::set<std::string> ids;
		for (auto& k : sk->a)
		{
			std::string sid = k.type == Json::Obj ? k.GetStr("id") : "";
			if (sid.empty()) Fail("skins: each needs an id");
			if (!ids.insert(sid).second) Fail("skins: " + sid + " twice");
			if (k.Find("look")) Fail("skins: " + sid + ": no \"look\" any more: its \"art\" (a game sprite given as itself: the game's)");
			if (const Json* a = k.Find("art"); a && a->type == Json::Obj)
				for (auto& [name, v] : a->o)
				{
					int to = v.type == Json::Num ? static_cast<int>(v.n) : v.type == Json::Str ? static_cast<int>(AssetIndex(v.s.c_str()).ToDouble()) : -1;
					if (AssetIndex(name.c_str()).ToDouble() < 0) Fail("skins: " + sid + ": art: no game sprite " + name);
					if (to < 0 || !g_Yytk->CallBuiltin("sprite_exists", { RValue(static_cast<double>(to)) }).ToBoolean()) Fail("skins: " + sid + ": art: " + name + ": no such sprite");
				}
		}
	}
	// "sounds": {game sound name: its sound (sound_file) | false (none) | true (the game's: a blank base plays none else)}
	if (const Json* ss = Req.Find("sounds"))
	{
		if (ss->type != Json::Obj) Fail("sounds: {game sound name: its sound | false | true}");
		for (auto& [name, v] : ss->o)
		{
			if (v.type == Json::Bool) { sounds[name] = v.b ? -2 : -1; continue; }
			if (v.type != Json::Num || !g_Yytk->CallBuiltin("audio_exists", { RValue(v.n) }).ToBoolean()) Fail("sounds: " + name + ": no such sound");
			sounds[name] = v.n;
		}
	}
	// "reapers": {ref: {name, description, lore, stats, values, damage, multiplier, max_level, evolve_in, lootable,
	// blacksmith, <column>: value, art, primordial_art, code}} (a list: [{ref, ...}])
	if (const Json* rs = Req.Find("reapers"))
	{
		RValue* rows = FindMember(GlobalRValue(), "reapers");
		size_t n = rows && KindOf(*rows) == VALUE_ARRAY ? ArrayLen(*rows) : 0;
		auto num = [](double V) { std::ostringstream o; o << V; return o.str(); };
		auto add = [&](double Ref, const Json& O)
		{
			std::string what = "reapers: " + std::to_string(static_cast<int>(Ref)) + ": ";
			if (Ref < 0 || Ref >= static_cast<double>(n) || O.type != Json::Obj) Fail("reapers: no reaper " + std::to_string(static_cast<int>(Ref)));
			size_t ref = static_cast<size_t>(Ref);
			auto set = [&](size_t Col, const Json& V) { reaperCells.push_back({ ref, Col, V }); };
			// its stats (applied, x its level with per_level) and its effect's values: built as the game's columns
			const Json* stats = O.Find("stats");
			const Json* values = O.Find("values");
			size_t nstats = stats && stats->type == Json::Arr ? stats->a.size() : 0;
			if (stats || values)
			{
				std::string base, type, level, stat;
				auto value = [&](const Json& V, bool Stat)
				{
					double v = V.type == Json::Num ? V.n : V.GetNum("value", 0);
					bool pct = V.type == Json::Obj && V.GetBool("percent", false), per = V.type == Json::Obj && V.GetBool("per_level", false);
					if (!base.empty()) { base += "|"; type += "|"; level += "|"; }
					base += num(v); type += pct ? "%" : ""; level += per ? "rl" : "";
					if (Stat)
					{
						std::string key = V.GetStr("stat");
						if (key.empty()) Fail(what + "stats: each needs a stat");
						stat += (stat.empty() ? "" : "|") + key;
					}
				};
				for (size_t k = 0; k < nstats; ++k)
				{
					const Json& st = stats->a[k];
					if (st.type != Json::Obj) Fail(what + "stats: {stat, value, per_level, percent}");
					value(st, true);
					reaperStats[static_cast<int>(ref)].push_back({ st.GetStr("stat"), st.GetNum("value", 0), st.GetBool("per_level", false) });
				}
				if (values && values->type == Json::Arr) for (auto& v : values->a) value(v, false);
				set(3, Json(base)); set(4, Json(type)); set(5, Json(level)); set(6, Json(stat));
				if (!O.Find("value_real") && !O.Find("VALUE_REAL")) set(7, Json(std::string("")));
			}
			for (size_t lang = 0; lang < std::size(classes::kLangs); ++lang)
				for (auto [key, k] : { std::pair{ "name", 0 }, std::pair{ "description", 1 }, std::pair{ "lore", 2 } })
				{
					std::string t = classes::LangText(O.Find(key), lang);
					if (k == 1 && (stats || values))                  // its stat lines, then its effect
					{
						std::string lines;
						for (size_t s2 = 0; s2 < nstats; ++s2) lines += "*+@ \xc2\xa3";
						t = lines + "|" + (t.empty() ? "" : "*" + t);
					}
					if (!t.empty()) set(18 + 3 * lang + static_cast<size_t>(k), Json(t));
				}
			if (const Json* dmg = O.Find("damage"); dmg && dmg->type == Json::Obj)
				for (auto [key, col] : { std::pair{ "min", 9 }, std::pair{ "max", 10 }, std::pair{ "min_level", 11 }, std::pair{ "max_level", 12 } })
					if (const Json* v = dmg->Find(key)) set(static_cast<size_t>(col), *v);
			for (auto [key, col] : { std::pair{ "multiplier", 13 }, std::pair{ "max_level", 14 }, std::pair{ "evolve_in", 15 },
					std::pair{ "lootable", 8 }, std::pair{ "blacksmith", 16 } })
				if (const Json* v = O.Find(key)) set(static_cast<size_t>(col), *v);
			// any column by the game's name (dat_rea.json)
			for (auto [key, col] : { std::pair{ "order", 1 }, std::pair{ "constant_check", 2 }, std::pair{ "value_base", 3 },
					std::pair{ "value_type", 4 }, std::pair{ "value_level", 5 }, std::pair{ "value_stat", 6 }, std::pair{ "value_real", 7 },
					std::pair{ "base_dmg_min", 9 }, std::pair{ "base_dmg_max", 10 }, std::pair{ "min_dmg_lvl", 11 },
					std::pair{ "max_dmg_lvl", 12 }, std::pair{ "dmg_multiplier", 13 }, std::pair{ "max_lvl", 14 } })
			{
				std::string upper = key;
				for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
				if (const Json* v = O.Find(key) ? O.Find(key) : O.Find(upper)) set(static_cast<size_t>(col), *v);
			}
			if (const Json* e = O.Find("evolve_in"); e && e->type == Json::Num && (e->n < 0 || e->n >= static_cast<double>(n)))
				Fail(what + "evolve_in: a reaper (0-" + std::to_string(n - 1) + "), or \"none\"");
			// its art: a frame of its strips (the reaper's SPRITE frame)
			RValue* row = ArrayAt(*rows, ref);
			RValue* frameCell = row && KindOf(*row) == VALUE_ARRAY ? ArrayAt(*row, 17) : nullptr;
			int frame = frameCell && IsNumeric(*frameCell) ? static_cast<int>(frameCell->ToDouble()) : static_cast<int>(ref);
			auto sprite = [&](const char* Key) -> int
			{
				const Json* a = O.Find(Key);
				if (!a) return -1;
				int spr = a->type == Json::Num ? static_cast<int>(a->n) : a->type == Json::Str ? static_cast<int>(AssetIndex(a->s.c_str()).ToDouble()) : -1;
				if (spr < 0 || !g_Yytk->CallBuiltin("sprite_exists", { RValue(static_cast<double>(spr)) }).ToBoolean()) Fail(what + Key + ": no such sprite");
				return spr;
			};
			int art = sprite("art"), prim = sprite("primordial_art");
			if (art >= 0) reaperArt[frame] = art;
			if (prim >= 0 || art >= 0) reaperArtPrimordial[frame] = prim >= 0 ? prim : art;   // (its art for both)
			// its values changed: the game's code of that reaper reads the game's (it errors on others) - its own code then
			bool changed = stats || values;
			for (const char* k : { "value_base", "value_type", "value_level", "value_stat", "value_real", "constant_check",
					"VALUE_BASE", "VALUE_TYPE", "VALUE_LEVEL", "VALUE_STAT", "VALUE_REAL", "CONSTANT_CHECK" })
				changed |= O.Find(k) != nullptr;
			std::string code = O.GetStr("code");
			if (!code.empty() && code != "own" && code != "game") Fail(what + "code: \"own\" (its Lua does its effect) or \"game\"");
			if (code == "game" && changed) Fail(what + "its values changed: the game's code of it reads the game's (code \"own\")");
			if (code == "own" || changed) reaperOwn.insert(static_cast<int>(ref));
		};
		if (rs->type == Json::Arr) for (auto& o : rs->a) add(o.GetNum("ref", -1), o);
		else if (rs->type == Json::Obj) for (auto& [k, o] : rs->o) add(std::atof(k.c_str()), o);
	}
	return o;
}

static Json CmdRegisterClass(const Json& Req)
{
	std::string owner = Req.GetStr("owner"), id = Req.GetStr("id");
	if (owner.empty() || id.empty()) Fail("needs owner and id");
	const Json* baseOpt = Req.Find("base");
	bool blank = baseOpt && baseOpt->type == Json::Str && baseOpt->s == "none";   // nothing of a game hero's (the Knight its frame)
	int base = blank ? 0 : static_cast<int>(Req.GetNum("base", 0));
	if (!blank && (base < 0 || base > 2 || (baseOpt && baseOpt->type != Json::Num))) Fail("base: 0 Knight, 1 Huntress, 2 Mage, or \"none\"");
	if (blank && Req.GetStr("kit") != "own") Fail("base \"none\": kit \"own\" (its skills are its own: there is no base hero's)");
	if (blank && Req.GetStr("weapon").empty()) Fail("base \"none\": weapon (its reapers' noun: there is no base hero's)");
	if (Req.Find("look")) Fail("look: not an option any more - its look is its base hero's with its \"art\" (game sprite -> its sprite)");
	if (Req.Find("damage")) Fail("damage: not an option any more - its skills' damage types are its own (kit \"own\") or its base hero's");
	ContentOptions opts = ParseContentOptions(Req, blank ? -1 : base);
	auto& art = opts.Art;
	auto& sounds = opts.Sounds;
	auto& reaperCells = opts.ReaperCells;
	auto& reaperArt = opts.ReaperArt;
	auto& reaperArtPrimordial = opts.ReaperArtPrimordial;
	auto& reaperOwn = opts.ReaperOwn;
	auto& reaperStats = opts.ReaperStats;
	// "area": {name, map: sprite (592 x 556), rooms: sprite (a frame per area mission), entrance: sprite, hub_x, hub_y, act}
	int areaMap = -1, areaRooms = -1, areaDoor = -1;
	if (const Json* ar = Req.Find("area"))
	{
		if (ar->type != Json::Obj) Fail("area: {name, map, rooms, entrance, hub_x, hub_y, act}");
		auto sprite = [&](const char* Key, bool Needed) -> int
		{
			const Json* v = ar->Find(Key);
			if (!v) { if (Needed) Fail(std::string("area: ") + Key + ": its sprite"); return -1; }
			int spr = v->type == Json::Num ? static_cast<int>(v->n) : v->type == Json::Str ? static_cast<int>(AssetIndex(v->s.c_str()).ToDouble()) : -1;
			if (spr < 0 || !g_Yytk->CallBuiltin("sprite_exists", { RValue(static_cast<double>(spr)) }).ToBoolean()) Fail(std::string("area: ") + Key + ": no such sprite");
			return spr;
		};
		areaMap = sprite("map", true);
		areaRooms = sprite("rooms", false);
		areaDoor = sprite("entrance", false);
		if (double act = ar->GetNum("act", 0); act < 0 || act > 4) Fail("area: act: the act page it shows itself on (0-4)");
	}
	size_t i = 0;
	for (; i < classes::Defs.size(); ++i)
		if (classes::Defs[i].Owner == owner && classes::Defs[i].Id == id) break;
	if (i == classes::Defs.size()) { classes::Defs.push_back({}); classes::Defs[i].Owner = owner; classes::Defs[i].Id = id; }
	classes::Def& def = classes::Defs[i];
	def.NameText = Req.Find("name") ? *Req.Find("name") : Json(id);
	def.TitleText = Req.Find("title") ? *Req.Find("title") : Json("");
	def.Name = classes::LangText(&def.NameText, 0);             // (its English name: logs, keys)
	if (def.Name.empty()) def.Name = id;
	def.Title = classes::LangText(&def.TitleText, 0);
	def.Weapon = Req.GetStr("weapon");
	def.Attribute = static_cast<int>(Req.GetNum("attribute", blank ? 0 : -1));
	def.Base = base;
	def.Blank = blank;
	if (const Json* x = Req.Find("hub_x")) def.HubX = x->n;
	if (const Json* y = Req.Find("hub_y")) def.HubY = y->n;
	def.Art = def.BaseArt = std::move(art);
	classes::BlankArt(def);                                    // (a blank base: what it gives no art for, nothing)
	def.Skins = Req.Find("skins") && Req.Find("skins")->type == Json::Arr ? *Req.Find("skins") : Json::Array();
	if (classes::AreaFor == static_cast<int>(i)) classes::AreaViewOff();
	def.Area = Req.Find("area") && Req.Find("area")->type == Json::Obj ? *Req.Find("area") : Json();
	def.AreaMap = areaMap; def.AreaRooms = areaRooms; def.AreaDoor = areaDoor;
	if (def.AreaStrip >= 0) g_Yytk->CallBuiltin("sprite_delete", { RValue(static_cast<double>(def.AreaStrip)) });
	def.AreaStrip = -1;
	def.SkinDefault = Req.Find("skin_name") ? *Req.Find("skin_name") : Json("");
	def.ReaperCells = std::move(reaperCells);
	def.ReaperArt = std::move(reaperArt);
	def.ReaperArtPrimordial = std::move(reaperArtPrimordial);
	def.ReaperOwnCode = std::move(reaperOwn);
	def.ReaperStats = std::move(reaperStats);
	def.Sounds = std::move(sounds);
	classes::ReaperBuffFor.clear();                              // (its stats may have changed: made again)
	if (classes::TextsFor == 3 + static_cast<int>(i)) classes::SyncReaperTexts(true);   // played: its new texts at once
	def.TalkText = Req.Find("talk") ? *Req.Find("talk") : Json("");   // its pages: "a/b", ["a", "b"], or one per language
	def.Talk = classes::TalkPages(def, 0);
	def.TalkSelf = Req.GetStr("talk_as") == "self";
	def.AchievementsMirror = !blank && Req.GetStr("achievements_base") != "none";
	def.AchievementDefs = Req.Find("achievements") && Req.Find("achievements")->type == Json::Arr ? *Req.Find("achievements") : Json::Array();
	def.Skills = Req.Find("skills") && Req.Find("skills")->type == Json::Obj ? *Req.Find("skills") : Json::Object();
	def.Own = Req.GetStr("kit") == "own";
	def.Entries = Req.Find("entries") && Req.Find("entries")->type == Json::Arr ? *Req.Find("entries") : Json::Array();
	def.Texts = Req.Find("texts") && Req.Find("texts")->type == Json::Obj ? *Req.Find("texts") : Json::Object();
	if (classes::ElementsFor == 3 + static_cast<int>(i))         // its legacy table again from the new options
	{
		if (RValue* g = FindMember(GlobalRValue(), "elements"); g && classes::HaveOrigElements) *g = classes::OrigElements;
		if (classes::OrigElementIcons >= 0)
			g_Yytk->CallBuiltin("sprite_assign", { AssetIndex("spr_elements"), RValue(static_cast<double>(classes::OrigElementIcons)) });
		classes::ElementsFor = -1;
	}
	def.Legacies = Req.Find("legacies") && Req.Find("legacies")->type == Json::Arr ? *Req.Find("legacies") : Json::Array();
	def.LegacyFrom.clear();
	for (size_t k = 0; k < def.Legacies.a.size(); ++k) def.LegacyFrom.push_back({ owner, static_cast<int>(k) });
	++classes::ContentVer;
	if (classes::MissionsFor == 3 + static_cast<int>(i)) classes::SyncMissions(true);   // its story table again from the new options
	def.Missions = Req.Find("missions") && Req.Find("missions")->type == Json::Arr ? *Req.Find("missions") : Json::Array();
	def.MissionFrom.assign(def.Missions.a.size(), owner);
	def.MissionSeen.clear();
	classes::DropActArt(def);
	def.LegacyBaseHidden = blank || Req.GetStr("legacy_base") == "none";
	def.Elements = RValue();
	classes::LoadFile();
	if (auto it = classes::AllSlots.find(std::to_string(classes::SaveSlot())); it != classes::AllSlots.end() && !classes::SlotData.count(def.Key()))
		if (const Json* d = it->second.Find(def.Key())) classes::SlotData[def.Key()] = *d;   // (registered again: as it is now)
	classes::EnsureHooks();
	classes::Extend();
	// its skill table from its options (also when it registers again with new ones); while it is played, the game's
	// copy of the current table too
	if (RValue* skills = FindMember(GlobalRValue(), "skills"); skills && ArrayLen(*skills) > 3 + i)
		if (RValue* baseTable = ArrayAt(*skills, static_cast<size_t>(base)))
		{
			RValue table = classes::ClassTable(def, *baseTable);
			*ArrayAt(*skills, 3 + i) = table;
			if (classes::Playing() == &def)
				if (RValue* cur = FindMember(GlobalRValue(), "hero_skills")) *cur = FromJson(ToJson(table, 4));
			classes::PadRows(i);
			if (def.Own)                                       // its icons, one per row of its table
				if (RValue* icons = FindMember(GlobalRValue(), "sprite_skills"))
					if (RValue* baseIcons = ArrayAt(*icons, static_cast<size_t>(base)); baseIcons && IsNumeric(*baseIcons))
						if (RValue* mine = ArrayAt(*icons, 3 + i))
							*mine = RValue(static_cast<double>(classes::IconStrip(def, static_cast<int>(baseIcons->ToDouble()), ArrayLen(table))));
		}
	classes::EnsureCitizens();
	classes::ApplySkin(def, true);                             // its look (with the skin picked)
	classes::BuildAchievements(def);
	legend::Restore();                                         // its saved Epics that were custom legendaries (registered first)
	// registered while the hub is up (a mod loading after it): its citizen joins now, not at the next hub load
	if (classes::Playing() != &def && g_Yytk->CallBuiltin("instance_number", { AssetIndex("obj_hub_controller") }).ToDouble() > 0)
	{
		bool there = false;
		RValue n = g_Yytk->CallBuiltin("instance_number", { AssetIndex("obj_npc") });
		for (int k = 0; k < static_cast<int>(n.ToDouble()) && !there; ++k)
		{
			RValue npc = g_Yytk->CallBuiltin("instance_find", { AssetIndex("obj_npc"), RValue(static_cast<double>(k)) });
			RValue cid = g_Yytk->CallBuiltin("variable_instance_get", { npc, RValue("cid") });
			there = IsNumeric(cid) && static_cast<int>(cid.ToDouble()) == def.Cid;
		}
		RValue unlocked, r;
		g_Yytk->CallGameScriptEx(unlocked, "gml_Script_scr_citizen_check_if_unlocked", nullptr, nullptr, { RValue(static_cast<double>(def.Cid)) });
		if (!there && unlocked.ToBoolean())
			g_Yytk->CallGameScriptEx(r, "gml_Script_scr_create_citizen", nullptr, nullptr, { RValue(static_cast<double>(def.Cid)) });
	}
	// the save was being played with this class, and the game loaded it before this registration: with no hero yet it
	// starts as this class; else (the game made its base hero already, at the main menu) CONTINUE switches to it
	if (auto it = classes::AllSlots.find(std::to_string(classes::SaveSlot())); it != classes::AllSlots.end())
		if (it->second.GetStr("$active") == classes::Defs[i].Key())
		{
			RValue* h = classes::CurrentHero();
			double heroes = g_Yytk->CallBuiltin("instance_number", { AssetIndex("obj_parent_hero") }).ToDouble();
			if (h && IsNumeric(*h) && static_cast<int>(h->ToDouble()) == base)
			{
				if (heroes == 0)
				{
					*h = RValue(static_cast<double>(3 + i));
					classes::ActiveKey = classes::Defs[i].Key();
				}
				else classes::ResumeHero = static_cast<int>(3 + i);
			}
		}
	classes::SyncElements();
	Json j = Json::Object();
	j.Add("hero", static_cast<double>(3 + i));
	j.Add("citizen", static_cast<double>(def.Cid));
	j.Add("tag", classes::CitizenTag(def));                     // its speaker tag in dialogues
	j.Add("key", def.Key());
	if (!def.Legacies.a.empty()) j.Add("legacy_first", static_cast<double>(classes::LegacyRow(def, 0)));
	if (!def.Missions.a.empty())                               // its missions' numbers in the story table while it is played
	{
		j.Add("mission_first", static_cast<double>(classes::GameMissionCount()));
		j.Add("missions", static_cast<double>(def.Missions.a.size()));
	}
	if (!def.Achievements.empty())                             // its achievements' rows in the Profile's view
	{
		double first = static_cast<double>(classes::AchGameRows);
		for (size_t k = 0; k < i; ++k) first += static_cast<double>(classes::Defs[k].Achievements.size());
		j.Add("achievement_first", first);
		j.Add("achievements", static_cast<double>(def.Achievements.size()));
	}
	if (def.Own)
	{
		j.Add("rows", static_cast<double>(ArrayLen(def.Table)));
		j.Add("effects", static_cast<double>(def.Effects.size()));
		j.Add("passives", def.Passives);
	}
	return j;
}

// {"entry": row} -> the playing class's rank of that entry (0: not taken / not live)
static Json CmdClassRank(const Json& Req)
{
	CInstance* hero = FirstInstance(AssetIndex("obj_parent_hero"));
	if (!hero) return Json(0.0);
	return Json(classes::EntryRank(hero, static_cast<int>(Req.GetNum("entry", -1))));
}

// {"legacy": id} -> the playing hero's rank of that Ancestral Legacy (0: not taken, or its realm is closed)
// ---- a game hero's content from mods: each mod's part (its register_hero request) kept, and the hero's content (a Def,
// classes::GameDefs) made again from all of them in order whenever one changes. Art, sounds, a reaper's cells: the later
// mod's where two give the same (a log line); skins, achievements, missions, legacies: added (an id given again: the
// later one in the earlier one's place, a log line)
static std::vector<std::pair<std::string, Json>> g_HeroParts[3];
static void RebuildGameHero(int H)
{
	using namespace classes;
	Def& D = GameDefs[H];
	std::string who = kHeroNames[H];
	// each part parsed first: one whose sprites / sounds are gone meanwhile is left out (logged), nothing half changed
	std::vector<std::pair<const std::pair<std::string, Json>*, ContentOptions>> parts;
	for (auto& part : g_HeroParts[H])
		try { parts.push_back({ &part, ParseContentOptions(part.second, -1) }); }
		catch (const std::exception& e)
		{
			DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormAPI] %s: %s's part left out: %s", who.c_str(), part.first.c_str(), e.what());
		}
	++ContentVer;
	// its mods' legacies - ranks, skill-bar places, auras toggled on - kept by legacy and out of the game's arrays (their
	// rows may move), back once its table is in place again (auras: ApplyPendingAuras, also when it is played later)
	if (GameUsed[H] && !D.Legacies.a.empty())
	{
		CInstance* hero = PlayedHero() == H ? FirstInstance(AssetIndex("obj_parent_hero")) : nullptr;
		size_t first = LegacyFirst(D);
		for (int row : D.AurasOn)
		{
			if (first && static_cast<size_t>(row) >= first && static_cast<size_t>(row) - first < D.Legacies.a.size())
				D.AurasPending.insert(LegacyKey(D, static_cast<size_t>(row) - first));
			if (hero) AuraBuff(D, hero, static_cast<size_t>(row), false);
		}
		KeepGameLegacies(D, false);
		D.LegaciesRestored = false;
	}
	D.AurasOn.clear();
	// what of it is in place goes back to the game's first
	if (ElementsFor == H)
	{
		if (RValue* g = FindMember(GlobalRValue(), "elements"); g && HaveOrigElements) *g = OrigElements;
		if (OrigElementIcons >= 0) g_Yytk->CallBuiltin("sprite_assign", { AssetIndex("spr_elements"), RValue(static_cast<double>(OrigElementIcons)) });
		ElementsFor = -1;
	}
	if (MissionsFor == H) SyncMissions(true);
	DropActArt(D);
	if (D.ElementIcons >= 0) g_Yytk->CallBuiltin("sprite_delete", { RValue(static_cast<double>(D.ElementIcons)) });
	D.ElementIcons = -1;
	D.Game = true;
	D.Base = H;
	D.Owner = "hero";
	D.Id = std::to_string(H);
	D.Name = who;
	D.Art.clear(); D.Sounds.clear(); D.ReaperCells.clear(); D.ReaperArt.clear(); D.ReaperArtPrimordial.clear();
	D.ReaperOwnCode.clear(); D.ReaperStats.clear();
	D.Skins = Json::Array();
	D.SkinDefault = Json("");
	D.AchievementsMirror = false;
	D.AchievementDefs = Json::Array();
	D.Missions = Json::Array();
	D.MissionFrom.clear();
	D.MissionSeen.clear();
	D.Legacies = Json::Array();
	D.LegacyFrom.clear();
	D.LegacyBaseHidden = false;
	D.Elements = RValue();
	// a list's items added (an id given again: the later one in the earlier one's place, a log line); Placed(at, its
	// place in its mod's list)
	// (Same(k): item k of the list is the one this id names - legacies: of the same mod too)
	auto merge = [&](Json& Into, const Json* From, const char* What, const std::string& Owner, auto&& Placed, auto&& Same)
	{
		if (!From || From->type != Json::Arr) return;
		for (size_t i = 0; i < From->a.size(); ++i)
		{
			const Json& item = From->a[i];
			std::string iid = item.GetStr("id");
			size_t at = Into.a.size();
			for (size_t k = 0; k < Into.a.size() && !iid.empty(); ++k)
				if (Into.a[k].GetStr("id") == iid && Same(k)) { at = k; break; }
			if (at < Into.a.size())
			{
				Into.a[at] = item;
				DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormAPI] %s: %s %s given again by %s (its in place of the earlier)", D.Name.c_str(), What, iid.c_str(), Owner.c_str());
			}
			else Into.Push(item);
			Placed(at, i);
		}
	};
	auto none = [](size_t, size_t) {};
	auto any = [](size_t) { return true; };
	for (auto& [part, o] : parts)
	{
		const std::string& owner = part->first;
		const Json& req = part->second;
		for (auto& [from, to] : o.Art)
		{
			if (D.Art.count(from) && D.Art[from] != to)
				DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormAPI] %s: art for %s given again by %s (its in place of the earlier)", D.Name.c_str(), SpriteName(from).c_str(), owner.c_str());
			D.Art[from] = to;
		}
		for (auto& [name, v] : o.Sounds) D.Sounds[name] = v;
		for (auto& c : o.ReaperCells) D.ReaperCells.push_back(c);
		for (auto& [f, spr] : o.ReaperArt) D.ReaperArt[f] = spr;
		for (auto& [f, spr] : o.ReaperArtPrimordial) D.ReaperArtPrimordial[f] = spr;
		D.ReaperOwnCode.insert(o.ReaperOwn.begin(), o.ReaperOwn.end());
		for (auto& [ref, st] : o.ReaperStats) D.ReaperStats[ref] = st;
		merge(D.Skins, req.Find("skins"), "skin", owner, none, any);
		if (std::string(LangText(&D.SkinDefault, 0)).empty() && req.Find("skin_name")) D.SkinDefault = *req.Find("skin_name");
		merge(D.AchievementDefs, req.Find("achievements"), "achievement", owner, none, any);
		merge(D.Missions, req.Find("missions"), "mission", owner, [&](size_t At, size_t)
			{
				if (At >= D.MissionFrom.size()) D.MissionFrom.resize(At + 1);
				D.MissionFrom[At] = owner;
			}, any);
		merge(D.Legacies, req.Find("legacies"), "legacy", owner, [&](size_t At, size_t I)
			{
				if (At >= D.LegacyFrom.size()) D.LegacyFrom.resize(At + 1);
				D.LegacyFrom[At] = { owner, static_cast<int>(I) };
			}, [&](size_t K) { return K < D.LegacyFrom.size() && D.LegacyFrom[K].first == owner; });
	}
	D.BaseArt = D.Art;
	GameUsed[H] = !parts.empty();
	ReaperBuffFor.clear();
	if (!GameUsed[H])                                          // nothing left: its art and reaper cells out
	{
		if (ArtFor == H) { for (auto& w : ArtApplied) g_Yytk->CallBuiltin("sprite_assign", { RValue(static_cast<double>(w.Sprite)), RValue(static_cast<double>(w.Orig)) }); ArtApplied.clear(); ArtFor = -1; }
		if (TextsFor == H) SyncReaperTexts(true);
		D.Swaps.clear();
		for (auto& [game, had] : D.Sprites) D.SpritesBack[had] = game;
		D.Sprites.clear();
		LookBack(D);                                           // (its hero's and citizen's sprite variables: the game's)
		D.Achievements.clear();
		++AchVersion;
		SyncElements();
		return;
	}
	LoadFile();
	if (auto it = AllSlots.find(std::to_string(SaveSlot())); it != AllSlots.end() && !SlotData.count(D.Key()))
		if (const Json* d = it->second.Find(D.Key())) SlotData[D.Key()] = *d;
	EnsureHooks();
	ApplySkin(D, true);                                        // its look (with the skin picked)
	BuildAchievements(D);
	if (TextsFor == H) SyncReaperTexts(true);
	SyncElements();                                            // (played: its table built, its auras on again)
}
static int HeroArg(const Json& Req)
{
	const Json* h = Req.Find("hero");
	if (h && h->type == Json::Num && h->n >= 0 && h->n <= 2 && h->n == std::floor(h->n)) return static_cast<int>(h->n);
	if (h && h->type == Json::Str)
		for (int k = 0; k < 3; ++k) if (h->s == classes::kHeroNames[k]) return k;
	Fail("hero: 0 (\"knight\"), 1 (\"huntress\") or 2 (\"mage\")");
	return -1;
}
// {"owner", "hero": 0|1|2|"knight"|"huntress"|"mage", "art", "skins", "skin_name", "reapers", "sounds", "achievements",
// "missions", "legacies"} -> {"hero", "key": "hero:<n>", "legacy_first", "mission_first"}: a game hero's content from a
// mod, as a class's same options, while that hero is played (its legacies take the realms they are in from the game's).
// Again from the same owner: its part replaced. legacy_first / mission_first: its first legacy's row / mission's number
// now (another mod's part changing moves them: content_row)
static Json CmdRegisterHero(const Json& Req)
{
	std::string owner = Req.GetStr("owner");
	if (owner.empty()) Fail("needs owner");
	int h = HeroArg(Req);
	for (const char* k : { "name", "title", "base", "kit", "skills", "entries", "texts", "weapon", "attribute", "talk", "talk_as",
			"area", "legacy_base", "achievements_base", "hub_x", "hub_y", "look", "damage" })
		if (Req.Find(k))
			Fail(std::string(k) + ": not for a game hero (that is the game's) - art, skins, skin_name, reapers, sounds, achievements, missions, legacies");
	(void)ParseContentOptions(Req, -1);                         // (checked before anything changes)
	auto& parts = g_HeroParts[h];
	auto it = std::find_if(parts.begin(), parts.end(), [&](auto& p) { return p.first == owner; });
	if (it != parts.end()) it->second = Req; else parts.push_back({ owner, Req });
	RebuildGameHero(h);
	Json j = Json::Object();
	j.Add("hero", static_cast<double>(h));
	j.Add("key", classes::GameDefs[h].Key());
	// its own first legacy / mission in the hero's content now (they move when another mod's part changes: content_row)
	const classes::Def& d = classes::GameDefs[h];
	size_t legacies = d.Legacies.a.size(), missions = d.Missions.a.size();
	for (size_t k = 0; k < d.LegacyFrom.size(); ++k) if (d.LegacyFrom[k].first == owner) { legacies = k; break; }
	for (size_t k = 0; k < d.MissionFrom.size(); ++k) if (d.MissionFrom[k] == owner) { missions = k; break; }
	j.Add("legacy_first", static_cast<double>(classes::LegacyRow(d, legacies)));
	if (classes::GameMissionCount()) j.Add("mission_first", static_cast<double>(classes::GameMissionCount() + missions));
	return j;
}
// {"owner"[, "hero"]} -> how many parts were taken out: that mod's content of that hero (none given: of every hero)
static Json CmdUnregisterHero(const Json& Req)
{
	std::string owner = Req.GetStr("owner");
	if (owner.empty()) Fail("needs owner");
	int only = Req.Find("hero") ? HeroArg(Req) : -1;
	double n = 0;
	for (int h = 0; h < 3; ++h)
	{
		if (only >= 0 && h != only) continue;
		size_t before = g_HeroParts[h].size();
		std::erase_if(g_HeroParts[h], [&](auto& p) { return p.first == owner; });
		if (g_HeroParts[h].size() != before) { n += static_cast<double>(before - g_HeroParts[h].size()); RebuildGameHero(h); }
	}
	return Json(n);
}

// {"class": key, "value": n, "max": n, "lit": bool} -> the class's mechanic on the HUD (value < 0: hidden)
static Json CmdClassMeter(const Json& Req)
{
	std::string key = Req.GetStr("class");
	for (auto& d : classes::Defs)
		if (d.Key() == key)
		{
			d.MeterValue = Req.GetNum("value", -1);
			d.MeterMax = Req.GetNum("max", d.MeterMax);
			d.MeterLit = Req.GetBool("lit", false);
			return Json(true);
		}
	Fail("no class " + key);
	return Json(false);
}

// {"text": "[speaker]page/...", "key": its line's name, "choice": id} -> shows a dialogue (the game's: its speakers'
// names and portraits by citizen tag - a class's: its "tag"; [hero] the hero played). With "choice", its last page asks
// Yes / No and Yes raises "dialogue_choice" {owner, id}. Returns the owner (to match the events).
static Json CmdSay(const Json& Req)
{
	std::string owner = Req.GetStr("owner", "api"), text = Req.GetStr("text");
	if (text.empty()) Fail("needs text");
	while (!text.empty() && (text.back() == '/' || text.back() == '\n')) text.pop_back();
	if (const Json* c = Req.Find("choice"); c && c->type == Json::Str && !c->s.empty())
	{
		text += "<SlormAPI Choice " + std::to_string(classes::AddChoice(owner, c->s)) + "?>";
	}
	text += "/";
	std::string key = "13 - SlormAPI " + owner + " " + Req.GetStr("key", "say");   // ("<citizen> - ...": read in a map)
	if (!classes::DialogueLine(key, "13 - Default", [&](size_t) { return text; })) Fail("no dialogue table yet");
	RValue uiv = classes::UiValue();
	if (KindOf(uiv) == VALUE_UNDEFINED) Fail("no UI yet");
	CInstance* ui = uiv.ToInstance();
	RValue r;
	g_Yytk->CallGameScriptEx(r, "gml_Script_scr_clear_dialogue", ui, ui, {});
	g_Yytk->CallGameScriptEx(r, "gml_Script_scr_create_dialogue", ui, ui, { RStr(key) });
	return Json(owner);
}

// {"class": key, "id": its achievement, "value": n | "add": n} -> its value now (a stat-counted one: read only)
static Json CmdClassAchievement(const Json& Req)
{
	std::string key = Req.GetStr("class"), id = Req.GetStr("id");
	if (classes::Def* dp = classes::ByKey(key))
	{
		classes::Def& d = *dp;
		for (auto& a : d.Achievements)
		{
			if (a.Id != id) continue;
			if (a.Source >= 0) return Json(a.Progress[2]);           // counted: read only
			if (!a.Stat.empty()) return Json(classes::AchValue(d, a));
			double v = classes::AchValue(d, a);
			if (const Json* set = Req.Find("value"); set && set->type == Json::Num) v = set->n;
			if (const Json* add = Req.Find("add"); add && add->type == Json::Num) v += add->n;
			classes::AchData(d, "achievements").Set(id, v);
			d.AchDirty = true;
			return Json(v);
		}
		Fail("no achievement " + id);
	}
	Fail("no class " + key);
	return Json();
}

// {"class": key} -> {ref, primordial, level} of the reaper its hero wears (null when it is not played);
// {"class": key, "effect": ref} -> whether that reaper's effect is on (worn, or one the worn one evolved from)
static Json CmdClassReaper(const Json& Req)
{
	std::string key = Req.GetStr("class");
	if (!classes::ByKey(key)) Fail("no class " + key);
	classes::Def* d = classes::Content();
	CInstance* hero = FirstInstance(AssetIndex("obj_parent_hero"));
	const Json* effect = Req.Find("effect");
	if (!d || d->Key() != key || !hero) return effect ? Json(false) : Json();
	if (effect) return Json(effect->type == Json::Num && classes::ReaperEffectOn(hero, effect->n));
	double ref = -1, level = 0;
	bool primordial = false;
	if (!classes::WornReaper(ref, primordial, level)) return Json();
	Json j = Json::Object();
	j.Add("ref", ref);
	j.Add("primordial", primordial);
	j.Add("level", level);
	return j;
}

// {"class": key, "id": its mission} -> its hero's runs of it (done: 1 or more)
static Json CmdClassMission(const Json& Req)
{
	std::string key = Req.GetStr("class"), id = Req.GetStr("id");
	if (classes::Def* dp = classes::ByKey(key))
	{
		classes::Def& d = *dp;
		for (size_t k = 0; k < d.Missions.a.size(); ++k)
		{
			if (d.Missions.a[k].GetStr("id") != id) continue;
			if (classes::MissionsFor == classes::HeroOf(d) && k < d.MissionRuns.size()) return Json(d.MissionRuns[k]);
			const Json* v = classes::AchData(d, "missions").Find(id);
			return Json(v && v->type == Json::Num ? v->n : 0.0);
		}
		Fail("no mission " + id);
	}
	Fail("no class " + key);
	return Json();
}

static Json CmdLegacyRank(const Json& Req)
{
	CInstance* hero = FirstInstance(AssetIndex("obj_parent_hero"));
	if (!hero) return Json(0.0);
	// only while that class is played and its table is in place, and only for ids the current tables hold (else the
	// game's check indexes past the hero's / the game's 150 legacies)
	classes::Def* d = classes::Content();
	if (!d || classes::ElementsFor != classes::HeroOf(*d)) return Json(0.0);
	double id = Req.GetNum("legacy", -1);
	if (const Json* ix = Req.Find("index"); ix && ix->type == Json::Num)   // {class, index}: the asking mod's legacy, its place in its list
	{
		id = -1;
		std::string owner = Req.GetStr("owner");
		if (d->Key() == Req.GetStr("class"))
			for (size_t k = 0; k < d->LegacyFrom.size(); ++k)
				if (d->LegacyFrom[k].second == static_cast<int>(ix->n) && (owner.empty() || d->LegacyFrom[k].first == owner))
				{
					id = static_cast<double>(classes::LegacyRow(*d, k));
					break;
				}
	}
	RValue* el = FindMember(GlobalRValue(), "elements");
	RValue* ranks = FindMember(GlobalRValue(), "elements_rank");
	RValue* h = classes::CurrentHero();
	RValue* mine = ranks && h && IsNumeric(*h) ? ArrayAt(*ranks, static_cast<size_t>(h->ToDouble())) : nullptr;
	RValue* cache = FindMember(GlobalRValue(), "fast_check_elements_equipped");
	if (id < 0 || !el || !mine || id >= static_cast<double>(ArrayLen(*el)) || id >= static_cast<double>(ArrayLen(*mine))
		|| (cache && KindOf(*cache) == VALUE_ARRAY && id >= static_cast<double>(ArrayLen(*cache))))
		return Json(0.0);
	RValue r;
	g_Yytk->CallGameScriptEx(r, "gml_Script_scr_element_check_if_equipped", hero, hero, { RValue(id) });
	return Json(IsNumeric(r) ? r.ToDouble() : 0.0);
}

// {"class": key, "legacy": its place in the asking mod's list | "mission": its id} -> that legacy's row in the legacy table
// / that mission's number in the story table while its hero is played (a game hero's move when another mod's part of it
// changes); null: none
static Json CmdContentRow(const Json& Req)
{
	classes::Def* d = classes::ByKey(Req.GetStr("class"));
	if (!d) return Json();
	std::string owner = Req.GetStr("owner");
	if (const Json* ix = Req.Find("legacy"); ix && ix->type == Json::Num)
	{
		for (size_t k = 0; k < d->LegacyFrom.size(); ++k)
			if (d->LegacyFrom[k].second == static_cast<int>(ix->n) && (owner.empty() || d->LegacyFrom[k].first == owner))
				return Json(static_cast<double>(classes::LegacyRow(*d, k)));
		return Json();
	}
	std::string id = Req.GetStr("mission");
	if (!classes::GameMissionCount()) return Json();             // (the game's story table not made yet)
	for (size_t k = 0; k < d->Missions.a.size(); ++k)
		if (d->Missions.a[k].GetStr("id") == id && (owner.empty() || k >= d->MissionFrom.size() || d->MissionFrom[k] == owner))
			return Json(static_cast<double>(classes::GameMissionCount() + k));
	return Json();
}

// {"file": path, "frames": n, "xorig", "yorig", "like": sprite} -> a sprite from an image (PNG / GIF / JPG; frames side by
// side: "frames", else as many as the "like" sprite's width fits, else 1), made once per file version and options. Its origin:
// xorig / yorig, else the "like" sprite's, else its middle. The path: relative to the asking Lua mod's folder (others:
// to mods/), and inside mods/.
// a mod's file ("file", relative to "dir" - a Lua mod's folder - else to mods/): inside mods/, one of Exts, existing
static fs::path ModFile(const Json& Req, std::initializer_list<const char*> Exts, const char* What, std::string& Rel, std::string& Ext)
{
	std::string file = Req.GetStr("file");
	if (file.empty()) Fail("needs file");
	fs::path mods = GameDir() / "mods";
	std::string dirOpt = Req.GetStr("dir");
	std::error_code ec;
	fs::path path = fs::weakly_canonical((dirOpt.empty() ? mods : fs::path(dirOpt)) / fs::path(file), ec);
	fs::path root = fs::weakly_canonical(mods, ec);
	Rel = path.lexically_relative(root).generic_string();
	if (Rel.empty() || Rel.starts_with("..") || path.is_relative()) Fail(std::string(What) + " files are read from mods/ only");
	Ext = Lower(path.extension().string());
	bool known = false;
	std::string list;
	for (const char* e : Exts) { known |= Ext == e; list += (list.empty() ? "" : ", ") + std::string(e); }
	if (!known) Fail(std::string("a ") + What + " file is one of " + list);
	if (!fs::is_regular_file(path, ec)) Fail("no file " + Rel);
	return path;
}

static Json CmdSpriteFile(const Json& Req)
{
	static std::map<std::string, int> made;
	std::string rel, ext;
	fs::path path = ModFile(Req, { ".png", ".gif", ".jpg", ".jpeg" }, "sprite", rel, ext);
	std::error_code ec;
	int like = -1;
	if (const Json* l = Req.Find("like"))
		like = l->type == Json::Str ? static_cast<int>(AssetIndex(l->s.c_str()).ToDouble()) : l->type == Json::Num ? static_cast<int>(l->n) : -1;
	if (like >= 0 && !g_Yytk->CallBuiltin("sprite_exists", { RValue(static_cast<double>(like)) }).ToBoolean()) like = -1;
	auto num = [](const char* Fn, int Spr) { return g_Yytk->CallBuiltin(Fn, { RValue(static_cast<double>(Spr)) }).ToDouble(); };
	double frames = 1;                                       // default: as many of the "like" sprite's width as fit
	if (const Json* f = Req.Find("frames"); f && f->type == Json::Num) frames = f->n;
	else if (like >= 0)
	{
		unsigned char head[24] = {};
		std::ifstream in(path, std::ios::binary);
		in.read(reinterpret_cast<char*>(head), sizeof(head));
		double width = ext == ".png" && in.gcount() == 24 ? (head[16] << 24 | head[17] << 16 | head[18] << 8 | head[19])
			: ext == ".gif" && in.gcount() >= 8 ? (head[6] | head[7] << 8) : 0;
		double likeWidth = num("sprite_get_width", like);
		frames = width > 0 && likeWidth > 0 ? (std::max)(1.0, std::floor(width / likeWidth)) : num("sprite_get_number", like);
	}
	if (frames < 1 || frames > 1000) Fail("frames: 1 to 1000");
	const Json* xo = Req.Find("xorig");
	const Json* yo = Req.Find("yorig");
	auto stamp = fs::last_write_time(path, ec).time_since_epoch().count();
	std::string key = path.string() + "|" + std::to_string(stamp) + "|" + std::to_string(frames) + "|" + std::to_string(like) + "|"
		+ (xo && xo->type == Json::Num ? std::to_string(xo->n) : "") + "|" + (yo && yo->type == Json::Num ? std::to_string(yo->n) : "");
	if (auto it = made.find(key); it != made.end() && g_Yytk->CallBuiltin("sprite_exists", { RValue(static_cast<double>(it->second)) }).ToBoolean())
		return Json(static_cast<double>(it->second));
	RValue spr = g_Yytk->CallBuiltin("sprite_add", { RStr(path.string()), RValue(frames), RValue(false), RValue(false), RValue(0.0), RValue(0.0) });
	int id = IsNumeric(spr) ? static_cast<int>(spr.ToDouble()) : -1;
	if (id < 0 || !g_Yytk->CallBuiltin("sprite_exists", { RValue(static_cast<double>(id)) }).ToBoolean()) Fail("could not load " + rel);
	double x = xo && xo->type == Json::Num ? xo->n : like >= 0 ? num("sprite_get_xoffset", like) : std::floor(num("sprite_get_width", id) / 2);
	double y = yo && yo->type == Json::Num ? yo->n : like >= 0 ? num("sprite_get_yoffset", like) : std::floor(num("sprite_get_height", id) / 2);
	g_Yytk->CallBuiltin("sprite_set_offset", { RValue(static_cast<double>(id)), RValue(x), RValue(y) });
	made[key] = id;
	return Json(static_cast<double>(id));
}

// {"file": path} -> a sound from a file: .ogg (streamed) or .wav (PCM, 8 / 16 bit, mono / stereo; loaded into a buffer),
// made once per file version. Same paths as sprite_file. For sound_play.
static Json CmdSoundFile(const Json& Req)
{
	static std::map<std::string, int> made;
	std::string rel, ext;
	fs::path path = ModFile(Req, { ".ogg", ".wav" }, "sound", rel, ext);
	std::error_code ec;
	std::string key = path.string() + "|" + std::to_string(fs::last_write_time(path, ec).time_since_epoch().count());
	if (auto it = made.find(key); it != made.end()) return Json(static_cast<double>(it->second));
	RValue snd;
	if (ext == ".ogg") snd = g_Yytk->CallBuiltin("audio_create_stream", { RStr(path.string()) });
	else
	{
		std::ifstream in(path, std::ios::binary);
		std::vector<unsigned char> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		auto u32 = [&](size_t At) { return At + 4 <= b.size() ? static_cast<uint32_t>(b[At] | b[At + 1] << 8 | b[At + 2] << 16 | b[At + 3] << 24) : 0u; };
		auto u16 = [&](size_t At) { return At + 2 <= b.size() ? static_cast<uint32_t>(b[At] | b[At + 1] << 8) : 0u; };
		if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) Fail(rel + " is not a WAV file");
		uint32_t channels = 0, rate = 0, bits = 0, format = 0, dataAt = 0, dataSize = 0;
		for (size_t at = 12; at + 8 <= b.size();)
		{
			uint32_t size = u32(at + 4);
			if (!std::memcmp(b.data() + at, "fmt ", 4)) { format = u16(at + 8); channels = u16(at + 10); rate = u32(at + 12); bits = u16(at + 22); }
			else if (!std::memcmp(b.data() + at, "data", 4)) { dataAt = static_cast<uint32_t>(at + 8); dataSize = (std::min)(size, static_cast<uint32_t>(b.size() - at - 8)); break; }
			at += 8 + size + (size & 1);
		}
		if (format != 1 || (bits != 8 && bits != 16) || (channels != 1 && channels != 2) || !rate || !dataSize)
			Fail(rel + ": a WAV file is PCM, 8 or 16 bit, mono or stereo");
		RValue loaded = g_Yytk->CallBuiltin("buffer_load", { RStr(path.string()) });
		if (!IsNumeric(loaded) || loaded.ToDouble() < 0) Fail("could not read " + rel);
		RValue size = g_Yytk->CallBuiltin("buffer_get_size", { loaded });   // (a sound takes a fixed buffer, not buffer_load's)
		RValue buf = g_Yytk->CallBuiltin("buffer_create", { size, RValue(0.0), RValue(1.0) });
		g_Yytk->CallBuiltin("buffer_copy", { loaded, RValue(0.0), size, buf, RValue(0.0) });
		g_Yytk->CallBuiltin("buffer_delete", { loaded });
		snd = g_Yytk->CallBuiltin("audio_create_buffer_sound", { buf, RValue(bits == 16 ? 4.0 : 1.0), RValue(static_cast<double>(rate)),   // buffer_s16 / _u8
			RValue(static_cast<double>(dataAt)), RValue(static_cast<double>(dataSize)), RValue(channels == 2 ? 1.0 : 0.0) });       // audio_stereo / _mono
	}
	int id = IsNumeric(snd) ? static_cast<int>(snd.ToDouble()) : -1;
	if (id < 0) Fail("could not load " + rel);
	made[key] = id;
	return Json(static_cast<double>(id));
}

// {"sound": id, "gain": 0-1, "pitch": x, "loop": bool} -> plays a sound (sound_file's or the game's by number), its gain
// times the game's sound effects volume (sc_sfx) -> the playing instance
static Json CmdSoundPlay(const Json& Req)
{
	double id = Req.GetNum("sound", -1);
	if (id < 0 || !g_Yytk->CallBuiltin("audio_exists", { RValue(id) }).ToBoolean()) Fail("no sound");
	RValue inst = g_Yytk->CallBuiltin("audio_play_sound", { RValue(id), RValue(10.0), RValue(Req.GetBool("loop", false)) });
	RValue* sfx = FindMember(GlobalRValue(), "sc_sfx");
	double gain = Req.GetNum("gain", 1) * (sfx && IsNumeric(*sfx) ? sfx->ToDouble() : 1.0);
	g_Yytk->CallBuiltin("audio_sound_gain", { inst, RValue(gain), RValue(0.0) });
	if (const Json* pitch = Req.Find("pitch"); pitch && pitch->type == Json::Num) g_Yytk->CallBuiltin("audio_sound_pitch", { inst, RValue(pitch->n) });
	return Json(IsNumeric(inst) ? inst.ToDouble() : -1.0);
}

// {"hero": n} -> switches to that hero with the game's scr_hero_switcher (saves, then the hub loads with that hero)
static Json CmdSwitchHero(const Json& Req)
{
	int hero = static_cast<int>(Req.GetNum("hero", -1));
	if (hero < 0 || hero >= static_cast<int>(classes::Wanted())) Fail("no hero " + std::to_string(hero));
	classes::Extend();
	CInstance* h = nullptr;
	RValue list = g_Yytk->CallBuiltin("instance_find", { AssetIndex("obj_parent_hero"), RValue(0.0) });
	if (KindOf(list) == VALUE_OBJECT || IsNumeric(list)) h = InstanceById(static_cast<int32_t>(list.ToInt64()));
	RValue r;                                                  // the game's own switch, as a hero NPC's "Yes" does
	g_Yytk->CallGameScriptEx(r, "gml_Script_scr_hero_switcher", h, h, { RValue(static_cast<double>(hero)), RValue(true) });
	return Json(true);
}

static bool CustomLegendary(int Ref, std::string& Name, std::string& Description)
{
	legend::Def* d = legend::ByRef(Ref);
	if (!d) return false;
	Name = d->Name;
	Description = d->Description;
	return true;
}

static Json CmdRegisterLegendary(const Json& Req) { return legend::Register(Req); }
static Json CmdUnregisterLegendary(const Json& Req) { legend::Unregister(Req.GetStr("owner"), Req.GetStr("id")); return Json(true); }
static Json CmdLegendaries() { return legend::List(); }
static Json CmdLegendaryEquipped(const Json& Req)
{
	legend::Def& d = legend::Find(Req);
	legend::ReadEquipped();
	auto it = legend::Equipped.find(d.Ref);
	return it != legend::Equipped.end() && d.Active ? Json(it->second) : Json();
}
static Json CmdLegendaryDrop(const Json& Req) { return legend::Drop(Req); }
static bool LegendaryPrompt(LegendaryPromptView& View)
{
	auto& ask = legend::Ask;
	if (!ask.Open) return false;
	size_t n = ask.List.size();
	std::set<std::string> mods;
	for (auto& w : ask.List) mods.insert(legend::ModOf(w.Id));
	View.Title = mods.size() == 1 ? "A mod is missing" : "Mods are missing";
	static const char* roman[] = { "", "I", "II", "III", "IV", "V" };           // as the Load Game panel names them
	std::string slot = ask.Slot >= 1 && ask.Slot <= 5 ? roman[ask.Slot] : std::to_string(ask.Slot);
	View.Sub = std::string(ask.Slot < 0 ? "This save" : "Save " + slot) + " has " + std::to_string(n)
		+ (n == 1 ? " custom legendary" : " custom legendaries") + " from a mod that is not loaded:";
	View.Rows.clear();
	for (size_t i = 0; i < n && i < 4; ++i) View.Rows.push_back({ ask.List[i].Name, legend::ModOf(ask.List[i].Id) });
	View.More = n > 4 ? n - 4 : 0;
	return true;
}
static void LegendaryPromptClick(const std::string& Action) { legend::Click(Action); }

static std::string LegendaryWaitingNotice()
{
	size_t n = legend::WaitingList.size();
	if (!n) return "";
	return "! " + std::to_string(n) + (n == 1 ? " custom legendary waits" : " custom legendaries wait") + " for its mod: "
		+ legend::WaitingMods();
}

static Json CmdLegendariesWaiting()
{
	legend::CheckWaiting();
	Json out = Json::Array();
	for (auto& w : legend::WaitingList)
	{
		Json j = Json::Object();
		j.Add("name", w.Name);
		j.Add("id", w.Id);
		out.Push(std::move(j));
	}
	return out;
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
	catch (const HeroGone& e) { resp.Add("ok", false); resp.Add("error", e.what()); resp.Add("hero_gone", true); }
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
	if (cmd == "profile") return CmdProfile(Req);
	if (cmd == "catch_errors") return CmdCatchErrors(Req);
	if (cmd == "static_value") return CmdStaticValue(Req);
	if (cmd == "register_class") return CmdRegisterClass(Req);
	if (cmd == "register_hero") return CmdRegisterHero(Req);
	if (cmd == "unregister_hero") return CmdUnregisterHero(Req);
	if (cmd == "content_row") return CmdContentRow(Req);
	if (cmd == "class_rank") return CmdClassRank(Req);
	if (cmd == "legacy_rank") return CmdLegacyRank(Req);
	if (cmd == "class_meter") return CmdClassMeter(Req);
	if (cmd == "sprite_file") return CmdSpriteFile(Req);
	if (cmd == "sound_file") return CmdSoundFile(Req);
	if (cmd == "sound_play") return CmdSoundPlay(Req);
	if (cmd == "class_achievement") return CmdClassAchievement(Req);
	if (cmd == "class_mission") return CmdClassMission(Req);
	if (cmd == "class_reaper") return CmdClassReaper(Req);
	if (cmd == "say") return CmdSay(Req);
	if (cmd == "switch_hero") return CmdSwitchHero(Req);
	if (cmd == "ui_remove") return CmdUiRemove(Req);
	if (cmd == "ui_refresh") { UiRefresh(); return Json(true); }
	if (cmd == "stat_info") return CmdStatInfo(Req);
	if (cmd == "legendary_info") return CmdLegendaryInfo(Req);
	if (cmd == "skill_info") return CmdSkillInfo(Req);
	if (cmd == "slot_info") return CmdSlotInfo(Req);
	if (cmd == "stat_list") return CmdStatList();
	if (cmd == "register_legendary") return CmdRegisterLegendary(Req);
	if (cmd == "unregister_legendary") return CmdUnregisterLegendary(Req);
	if (cmd == "legendaries") return CmdLegendaries();
	if (cmd == "legendary_equipped") return CmdLegendaryEquipped(Req);
	if (cmd == "legendary_drop") return CmdLegendaryDrop(Req);
	if (cmd == "legendaries_waiting") return CmdLegendariesWaiting();
	if (cmd == "buff_add") return CmdBuffAdd(Req);
	if (cmd == "buff_remove") return CmdBuffRemove(Req);
	if (cmd == "buff_stacks") return CmdBuffStacks(Req);
	if (cmd == "hero_stats") return CmdHeroStats();
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
	struct Handler { Mod* M; int Ref; bool OwnClicks; uint64_t Id; bool Dead = false; bool Failed = false; };
	struct Timer { int Id, Ref; double Every, Next; bool Repeat; bool Failed = false; };
	struct StatsFn { int Ref; bool Failed = false; };              // an on_stats handler (Failed: its error logged once)
	struct Mod
	{
		std::string Stem, Name, Version, Author, Description;
		fs::path Path;                       // the script: mods/lua/<stem>.lua, or a pack's entry file
		std::string File;                    // what the Mods page switches: "<stem>.lua" or "<folder>/lua-pack.json"
		std::string Need;                    // a pack that needs a newer SlormAPI: "Needs SlormAPI 1.2.0+"
		std::string PackName, PackVersion, PackAuthor, PackDescription;   // from lua-pack.json, where slorm.mod{} says nothing
		fs::file_time_type Time{};
		bool Enabled = true, Tried = false, Pack = false;
		lua_State* L = nullptr;
		std::string Error;
		std::vector<std::unique_ptr<Handler>> Handlers;
		std::vector<Timer> Timers;
		int NextTimer = 1;
		std::set<std::string> Menus;
		std::string SettingsName;
		std::vector<StatsFn> StatsFns;       // on_stats(fn)
		double CallStart = 0;
		bool TimedOut = false;               // its last call stopped by the 200 ms limit (LimitHook)
		bool GoneSaid = false;               // a request of its on a hero that is gone: said in the log (once)
		size_t MemUsed = 0;
		std::string Owner() const { return "lua:" + Stem; }
		// its name on the Mods page, in settings and in slorm.display_name(): slorm.mod{name}, else the pack's name
		std::string Display() const { return !Name.empty() ? Name : !PackName.empty() ? PackName : Stem; }
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
		if (m && MicrosSinceStart() - m->CallStart > 200000)
		{
			m->TimedOut = true;
			luaL_error(L, "took longer than 200 ms (an endless loop?)");
		}
	}

	int Traceback(lua_State* L)
	{
		const char* msg = lua_tostring(L, 1);
		luaL_traceback(L, L, msg ? msg : "error", 1);
		return 1;
	}

	// calls the function below the NArgs arguments on top; an error is logged and kept for the Mods page (Quiet: not logged
	// again). Its 200 ms are its own: the game's work it asks for is not counted (Request: Paused), and with it the
	// callbacks run inside it (each has its own limit); one of its own mod's run inside it keeps its start and TimedOut.
	// TimedOut: it was stopped by that limit
	bool PCall(Mod& M, int NArgs, bool Quiet = false, bool* TimedOut = nullptr)
	{
		lua_State* L = M.L;
		int base = lua_gettop(L) - NArgs;
		lua_pushcfunction(L, Traceback);
		lua_insert(L, base);
		double was = M.CallStart;                               // (a call of its own running: its start kept for it)
		bool wasOut = M.TimedOut;
		M.CallStart = MicrosSinceStart();
		M.TimedOut = false;
		int st = lua_pcall(L, NArgs, 0, base);
		bool out = M.TimedOut;
		M.CallStart = was; M.TimedOut = wasOut;
		if (TimedOut) *TimedOut = st != LUA_OK && out;
		lua_remove(L, base);
		if (st == LUA_OK) return true;
		std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
		lua_pop(L, 1);
		if (Quiet) return false;
		DbgPrintEx(LOG_SEVERITY_ERROR, "[Lua %s] %s", M.Stem.c_str(), err.c_str());
		M.Error = err.substr(0, err.find('\n'));
		++Version;
		return false;
	}

	// a callback of the mod's that ran into the 200 ms limit is not called again (it would stall the game each time): said
	// in the log and on the Mods page
	void Stopped(Mod& M, const std::string& What)
	{
		DbgPrintEx(LOG_SEVERITY_ERROR, "[Lua %s] %s stopped: it took longer than 200 ms", M.Stem.c_str(), What.c_str());
		M.Error = What + " stopped: it took longer than 200 ms";
		++Version;
	}
	// a callback of the mod's that stays (an event handler, a repeating timer, an on_stats handler): its error logged the
	// first time only (Failed). True: it ran into the 200 ms limit - it would stall the game each time, so it goes (its
	// caller removes it and says so: Stopped)
	bool CallTimedOut(Mod& M, int NArgs, bool& Failed)
	{
		bool out = false;
		if (!PCall(M, NArgs, Failed, &out)) Failed = true;
		return out;
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
		static const std::set<std::string> blocked = { "mod_enable", "mods_page", "perf", "profile", "catch_errors", "static_value", "switch_hero", "overlay", "runner", "routines",
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
		if (cmd == "sprite_file" || cmd == "sound_file")          // its own folder: a pack's, else mods/lua
			Req.Set("dir", (GameDir() / "mods" / "lua" / (M.Pack ? fs::path(M.File).parent_path() : fs::path())).string());
		Req.Set("owner", M.Owner());
		Req.Set("all", false);
	}

	// ---- the C side of the slorm table
	int Request(lua_State* L)
	{
		Mod* m = Of(L);
		std::string err;
		struct Paused                                            // the 200 ms limit counts Lua's own time, not the game's work
		{                                                        // for a request (e.g. register_class making its sprites)
			Mod* M; double At;
			~Paused() { if (M) M->CallStart += MicrosSinceStart() - At; }
		} paused{ m, MicrosSinceStart() };
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
				if (resp.GetBool("hero_gone", false))               // (a hero gone: nothing done on it, nil)
				{
					if (!m->GoneSaid)
					{
						m->GoneSaid = true;
						DbgPrintEx(LOG_SEVERITY_WARNING, "[Lua %s] %s: nothing done, nil (e.g. a cast's hero, its run left; "
							"said once)", m->Stem.c_str(), resp.GetStr("error", "").c_str());
					}
					lua_pushnil(L);
					return 1;
				}
				err = resp.GetStr("error", "error");
			}
		}
		return luaL_error(L, "%s", err.c_str());
	}

	// exists(inst): the instance ({$inst} or its id) still there - as a request on it finds it (e.g. a cast's hero: gone
	// once its run is left). Native: no request
	int InstExists(lua_State* L)
	{
		lua_Integer id = -1;
		if (lua_istable(L, 1))
		{
			lua_getfield(L, 1, "$inst");
			if (lua_isinteger(L, -1) || lua_isnumber(L, -1)) id = lua_tointeger(L, -1);
			lua_pop(L, 1);
		}
		else if (lua_type(L, 1) == LUA_TNUMBER) id = lua_tointeger(L, 1);
		CInstance* inst = nullptr;
		lua_pushboolean(L, id >= 0 && id <= INT32_MAX && AurieSuccess(g_Yytk->GetInstanceObject(static_cast<int32_t>(id), inst))
			&& inst != nullptr);
		return 1;
	}

	// a handler gone (off(), or stopped by the 200 ms limit): unsubscribed, its function freed (the Handler itself with the
	// script: a dispatch may still hold it)
	void Release(Handler& H)
	{
		events::Unsubscribe(H.Id);
		H.Dead = true;
		luaL_unref(H.M->L, LUA_REGISTRYINDEX, H.Ref);
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
		if (!CallTimedOut(*h->M, 2, h->Failed) || h->Dead || !h->M->L) return;   // (off() by itself meanwhile: gone)
		Release(*h);
		Stopped(*h->M, std::string("its ") + Event + " handler");
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
			if (h->Id == id && !h->Dead) Release(*h);
		return 0;
	}

	// on_stats(fn): fn(e) right after each refresh of the hero's stats - inside the game's (Post scr_stats_refresh, after
	// SlormAPI's own), before anything uses them: its final stats can be changed there (S.get / S.set on e.hero).
	// e = {hero = {$inst}, hero_number}. For the script's life. (A handler's own refresh of them runs no handler again)
	int OnStats(lua_State* L)
	{
		Mod* m = Of(L);
		luaL_checktype(L, 1, LUA_TFUNCTION);
		static bool hooked = false;                               // (SlormAPI's own Post, which calls StatsRefreshed)
		if (!hooked) hooked = NativeHookInstall("scr_stats_refresh", { "SlormAPI.classes", nullptr, classes::PostStatsRefresh });
		lua_pushvalue(L, 1);
		m->StatsFns.push_back({ luaL_ref(L, LUA_REGISTRYINDEX) });
		return 0;
	}
	bool InStats = false;
	void StatsRefreshed(CInstance* Hero)
	{
		if (InStats || !Hero) return;
		bool any = false;
		for (auto& m : Mods) any |= m->L && !m->StatsFns.empty();
		if (!any) return;
		InStats = true;
		Json e = Json::Object();
		try
		{
			Json h = Json::Object();
			h.Add("$inst", g_Yytk->CallBuiltin("variable_instance_get", { RValue(Hero), RValue("id") }).ToDouble());
			e.Add("hero", std::move(h));
			e.Add("hero_number", static_cast<double>(classes::PlayedHero()));   // (the one played: also inside a cast's window)
		}
		catch (...) { InStats = false; return; }                  // (an exception out of a hook ends the game)
		for (auto& m : Mods)
		{
			if (!m->L || m->StatsFns.empty()) continue;
			std::vector<int> refs;                                // (as they are now: a handler may add one)
			for (auto& f : m->StatsFns) refs.push_back(f.Ref);
			auto of = [&](int Ref) { return std::find_if(m->StatsFns.begin(), m->StatsFns.end(), [&](const StatsFn& F) { return F.Ref == Ref; }); };
			for (int ref : refs)
			{
				if (!m->L) break;
				auto it = of(ref);
				if (it == m->StatsFns.end()) continue;
				bool failed = it->Failed;
				int top = lua_gettop(m->L);
				try                                               // (each handler on its own: one failing stops no other)
				{
					lua_rawgeti(m->L, LUA_REGISTRYINDEX, ref);
					LuaPush(m->L, e);
					bool stop = CallTimedOut(*m, 1, failed);      // (an endless loop: it would stall each refresh)
					if (m->L && (it = of(ref)) != m->StatsFns.end())
					{
						it->Failed = failed;
						if (stop)
						{
							m->StatsFns.erase(it);
							luaL_unref(m->L, LUA_REGISTRYINDEX, ref);
							Stopped(*m, "its on_stats handler");
						}
					}
				}
				catch (const std::exception& x)
				{
					DbgPrintEx(LOG_SEVERITY_ERROR, "[Lua %s] on_stats: %s", m->Stem.c_str(), x.what());
					if (m->L) lua_settop(m->L, top);
				}
				catch (...) { if (m->L) lua_settop(m->L, top); }
			}
		}
		InStats = false;
	}

	// (_content_version: +1 at each content change, and once the game's legacy / story tables are made - rows counted after them)
	int ContentVersion(lua_State* L)
	{
		lua_pushinteger(L, classes::ContentVer * 4 + (classes::GameMissionCount() ? 2 : 0) + (classes::HaveOrigElements ? 1 : 0));
		return 1;
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
function S.hero_stats() return S.request{ cmd = "hero_stats" } end
-- a custom legendary: data (as register_legendary) plus optional handlers on_hit(e, roll), on_kill, on_boss_kill,
-- on_hero_hit, on_cast, on_status, on_damage, on_cooldown - called only while it is equipped (roll = its 75-100 roll)
local legendary_events = { on_hit = "hit_dealt", on_kill = "enemy_killed", on_boss_kill = "boss_killed",
  on_hero_hit = "hero_hit", on_cast = "skill_cast", on_status = "status_applied", on_damage = "damage_dealt",
  on_cooldown = "cooldown_started" }
function S.legendary(def)
  local data, handlers = { cmd = "register_legendary" }, {}
  for k, v in pairs(def) do
    if type(v) == "function" then
      if not legendary_events[k] then error("unknown legendary handler '" .. k .. "'", 2) end
      handlers[k] = v
    else
      data[k] = v
    end
  end
  local r = S.request(data)
  for k, fn in pairs(handlers) do
    S.on(legendary_events[k], function(e)
      local roll = S.request{ cmd = "legendary_equipped", id = r.id }
      if roll then fn(e, roll) end
    end)
  end
  return r
end
function S.legendary_equipped(id) return S.request{ cmd = "legendary_equipped", id = id } end
-- the game's buffs: S.buff{ key = "my_buff", stats = { attack_speed_percent = 20 }, duration = 4, max_stacks = 3, name = "..." }
function S.buff(b) local r = { cmd = "buff_add" }; for k, v in pairs(b) do r[k] = v end; return S.request(r) end
function S.buff_remove(key, target) return S.request{ cmd = "buff_remove", key = key, target = target } end
function S.buff_stacks(key, target) return S.request{ cmd = "buff_stacks", key = key, target = target } end
function S.stat_list() return S.request{ cmd = "stat_list" } end
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
-- S.say's lines -> a dialogue's text ("[speaker]page/..."): "text" (said by the hero), or a list of pages - "text" (the
-- speaker before) or { speaker, "text" }; "\n" in a text breaks the line
local function pages_of(lines)
  local pages, last = {}, "hero"
  local function page(speaker, text)
    local p = tostring(text):gsub("\n", "#")
    if speaker and speaker ~= last then p = "[" .. speaker .. "]" .. p; last = speaker elseif #pages == 0 then p = "[" .. last .. "]" .. p end
    pages[#pages + 1] = p
  end
  if type(lines) == "string" then page("hero", lines)
  else
    for _, l in ipairs(lines) do if type(l) == "table" then page(l.speaker or l[1], l.text or l[2]) else page(nil, l) end end
  end
  return table.concat(pages, "/\n")
end
local function lines_text(v)                       -- lines, or lines per language ({ en = ..., fr = ... })
  if type(v) ~= "table" or v[1] ~= nil then return pages_of(v) end
  local t = {}
  for lang, l in pairs(v) do t[lang] = pages_of(l) end
  return t
end
)LUA"   /* (one literal holds 16 KB at most: the prelude is two) */   R"LUA(
local reaper_events = { on_hit = "hit_dealt", on_kill = "enemy_killed", on_boss_kill = "boss_killed",
  on_hero_hit = "hero_hit", on_cast = "skill_cast", on_status = "status_applied", on_damage = "damage_dealt",
  on_cooldown = "cooldown_started" }
local reaper_strips = { [0] = "spr_reapers_swords", [1] = "spr_reapers_bows", [2] = "spr_reapers_staffs" }
-- a custom class (register_class); with kit = "own", skills[row].cast(e) runs when the class casts that skill and
-- skills[row].hit(e) on each of its hits (e.target, e.primary)
-- an image file in the mod's folder ("x.png", or {file = "x.png", frames, xorig, yorig, like}) -> its sprite; else as is
local function sprite_of(v, like)
  if type(v) == "string" and (v:lower():match("%.png$") or v:lower():match("%.gif$") or v:lower():match("%.jpe?g$")) then
    return S.sprite(v, { like = like })
  end
  if type(v) == "table" and v.file then return S.sprite(v.file, { frames = v.frames, xorig = v.xorig, yorig = v.yorig, like = v.like or like }) end
  return v
end
local function icon_of(v)                          -- an icon from a file: its first frame
  local s = sprite_of(v)
  if s ~= v then return { sprite = s } end
  if type(v) == "table" and v.sprite then local t = {}; for k, x in pairs(v) do t[k] = x end; t.sprite = sprite_of(v.sprite); return t end
  return v
end
-- a hero's content options both a class and a game hero take (sounds, reapers, missions, art, skins, legacies): their files
-- loaded, their code kept apart (the request carries data only); strips: the reaper strip their reapers' art is sized like
local function content_options(def, data, strip)
  local p = { rhandlers = {}, mstart = {}, mdone = {}, uses = {} }
  if def.sounds then                               -- its hero's sounds: files loaded, false = none, true = the game's
    data.sounds = {}
    for name, v in pairs(def.sounds) do data.sounds[name] = type(v) == "string" and S.sound(v) or v end
  end
  if def.reapers then                              -- its reapers: { [ref] = {...} } -> a list; art from files; handlers: code
    data.reapers = {}
    for ref, rp in pairs(def.reapers) do
      local d = { ref = ref }
      for k, v in pairs(rp) do
        if type(v) == "function" then
          if not reaper_events[k] then error("unknown reaper handler '" .. k .. "'", 3) end
          p.rhandlers[ref] = p.rhandlers[ref] or {}; p.rhandlers[ref][k] = v
        elseif k == "art" or k == "primordial_art" then d[k] = sprite_of(v, strip)
        else d[k] = v end
      end
      data.reapers[#data.reapers + 1] = d
    end
  end
  if def.missions then                             -- a mission's start / done: code; its intro / outro: S.say's lines
    data.missions = {}
    for i, m in ipairs(def.missions) do
      local d = {}
      for k, v in pairs(m) do
        if k == "start" then p.mstart[m.id] = v elseif k == "done" then p.mdone[m.id] = v
        elseif k == "intro" or k == "outro" then d[k] = lines_text(v) else d[k] = v end
      end
      data.missions[i] = d
    end
  end
  if def.art then                                  -- its own sprites: { [game sprite] = file / sprite }
    data.art = {}
    for k, v in pairs(def.art) do data.art[k] = sprite_of(v, k) end
  end
  if def.skins then                                -- its skins: their art the same way
    data.skins = {}
    for i, sk in ipairs(def.skins) do
      local d = {}
      for k, v in pairs(sk) do d[k] = v end
      if sk.art then d.art = {}; for k, v in pairs(sk.art) do d.art[k] = sprite_of(v, k) end end
      data.skins[i] = d
    end
  end
  if def.legacies then                             -- a legacy's "use" (an aura toggled / an active cast): code, not data
    data.legacies = {}
    for i, l in ipairs(def.legacies) do
      local d = {}
      for k, v in pairs(l) do if k == "use" then p.uses[i] = v elseif k == "icon" then d[k] = icon_of(v) else d[k] = v end end
      data.legacies[i] = d
    end
  end
  return p
end
local content_keys = { sounds = true, reapers = true, missions = true, art = true, skins = true, legacies = true }
-- what a registered content (r: the request's answer, its key) offers its mod, and its handlers' events
local function content_bind(r, def, p)
  local warned = {}
  r.run = function(f, e, what)                     -- a failing handler (e.g. its target died meanwhile) is logged once
    local ok, err = pcall(f, e)
    if not ok and not warned[what] then warned[what] = true; S.log(what .. ": " .. tostring(err)) end
  end
  -- its Ancestral Legacies by their "id": r.legacy[id] = its row in the legacy table, r.legacy_rank(id) = its rank now.
  -- (Rows - also r.mission's - asked again only after a content change - a game hero's move when another mod's part of
  -- it changes - or once the game's tables are made; pairs() lists them)
  local lidx, mids, rows, ver = {}, {}, {}, nil
  for i, l in ipairs(def.legacies or {}) do if l.id then lidx[l.id] = i - 1 end end
  for _, m in ipairs(def.missions or {}) do if m.id then mids[m.id] = true end end
  local function row(kind, id)
    local v = S._content_version()
    if v ~= ver then ver, rows = v, { legacy = {}, mission = {} } end
    local c = rows[kind][id]
    if c == nil then
      if kind == "legacy" then c = lidx[id] and S.request{ cmd = "content_row", class = r.key, legacy = lidx[id] }
      else c = mids[id] and S.request{ cmd = "content_row", class = r.key, mission = id } end
      c = c or false
      rows[kind][id] = c
    end
    return c or nil
  end
  local function rows_of(kind, ids)
    return setmetatable({}, { __index = function(_, id) return row(kind, id) end,
      __pairs = function(t)
        return function(_, k) local nk = next(ids, k); if nk ~= nil then return nk, row(kind, nk) end end, t, nil
      end })
  end
  r.legacy = rows_of("legacy", lidx)
  function r.legacy_rank(id) local i = lidx[id]; return i and S.request{ cmd = "legacy_rank", class = r.key, index = i } or 0 end
  -- its own achievements: r.achievement(id) its value, r.achievement(id, value) sets it, r.achievement_add(id, n)
  function r.achievement(id, value) return S.request{ cmd = "class_achievement", class = r.key, id = id, value = value } end
  function r.achievement_add(id, n) return S.request{ cmd = "class_achievement", class = r.key, id = id, add = n or 1 } end
  S.on("legacy_used", function(e)                  -- (its own legacies: by their place in its list)
    if e.class ~= r.key then return end
    local f = p.uses[(e.index or -1) + 1]
    if f then r.run(f, e, "legacy " .. e.legacy) end
  end, true)
  -- its story missions by their "id": r.mission[id] = its number in the story table while it is played, r.mission_runs(id)
  -- its hero's runs of it; a mission's start(e) / done(e) run when it starts / is done (e.runs)
  r.mission = rows_of("mission", mids)
  function r.mission_runs(id) return S.request{ cmd = "class_mission", class = r.key, id = id } end
  -- its reapers: r.reaper() = {ref, primordial, level} worn (nil when it is not played), r.reaper_active(ref) = that
  -- reaper's effect is on (worn, or evolved from); a reaper's on_hit / on_kill / ... run only then
  function r.reaper() return S.request{ cmd = "class_reaper", class = r.key } end
  function r.reaper_active(ref) return S.request{ cmd = "class_reaper", class = r.key, effect = ref } end
  for ref, hs in pairs(p.rhandlers) do
    for k, fn in pairs(hs) do
      S.on(reaper_events[k], function(e) if r.reaper_active(ref) then r.run(fn, e, "reaper " .. ref) end end)
    end
  end
  S.on("mission_started", function(e) local f = e.class == r.key and p.mstart[e.mission]; if f then r.run(f, e, "mission " .. e.mission) end end, true)
  S.on("mission_done", function(e) local f = e.class == r.key and p.mdone[e.mission]; if f then r.run(f, e, "mission " .. e.mission) end end, true)
end

function S.class(def)
  local data, casts, hits = { cmd = "register_class" }, {}, {}
  for k, v in pairs(def) do
    if not content_keys[k] and k ~= "skills" and k ~= "entries" and k ~= "area" then data[k] = v end
  end
  local p = content_options(def, data, reaper_strips[def.base == "none" and 0 or def.base or 0])   -- (a blank one: the Knight's frame)
  if def.area then                                 -- its story area: its art from files (the act page's sizes)
    local a = {}
    for k, v in pairs(def.area) do a[k] = v end
    if a.map then a.map = sprite_of(a.map, "spr_menu_story_graveyard_overmap") end
    if a.rooms then a.rooms = sprite_of(a.rooms, "spr_menu_story_graveyard_maps") end
    if a.entrance then a.entrance = sprite_of(a.entrance, "spr_entrance_graveyard_open") end
    data.area = a
  end
  if def.entries then
    data.entries = {}
    for i, en in ipairs(def.entries) do
      local d = {}
      for k, v in pairs(en) do d[k] = k == "icon" and icon_of(v) or v end
      data.entries[i] = d
    end
  end
  data.skills = {}
  for row, sk in pairs(def.skills or {}) do
    local d = {}
    for k, v in pairs(sk) do
      if k == "cast" then casts[row] = v elseif k == "hit" then hits[row] = v elseif k == "icon" then d[k] = icon_of(v) else d[k] = v end
    end
    data.skills[tostring(row)] = d
  end
  local r = S.request(data)
  -- its entries by their "id" (rows after the base table): r.entry[id] = row, r.rank(id) = its rank now
  r.entry = {}
  if r.rows and def.entries then
    local first = r.rows - #def.entries
    for i, en in ipairs(def.entries) do if en.id then r.entry[en.id] = first + i - 1 end end
  end
  function r.rank(id) local row = r.entry[id]; return row and S.request{ cmd = "class_rank", entry = row } or 0 end
  -- (r.tag: its speaker tag in dialogues - S.say{ { C.tag, "..." } })
  -- its mechanic on the HUD: r.meter(value, max, lit) (value nil: hidden)
  function r.meter(value, max, lit) S.request{ cmd = "class_meter", class = r.key, value = value or -1, max = max, lit = lit and true or false } end
  content_bind(r, def, p)
  S.on("class_cast", function(e) local f = e.class == r.key and casts[e.skill]; if f then r.run(f, e, "cast " .. e.skill) end end)
  S.on("class_hit", function(e) local f = e.class == r.key and hits[e.skill]; if f then r.run(f, e, "hit " .. e.skill) end end)
  return r
end
-- a game hero's content (the Knight's, the Huntress's, the Mage's: hero = 0 / "knight", 1 / "huntress", 2 / "mage"): its
-- art, skins, skin_name, reapers, sounds, achievements, missions, legacies - as a class's same options, while that hero
-- is played. Its answer offers what a class's does for them (legacy, legacy_rank, achievement, mission, mission_runs,
-- reaper, reaper_active) and its key ("hero:<n>")
function S.hero(def)
  local data = { cmd = "register_hero" }
  for k, v in pairs(def) do if not content_keys[k] then data[k] = v end end
  local n = type(def.hero) == "number" and def.hero or ({ knight = 0, huntress = 1, mage = 2 })[def.hero] or 0
  local p = content_options(def, data, reaper_strips[n])
  local r = S.request(data)
  content_bind(r, def, p)
  return r
end
function S.rank(entry) return S.request{ cmd = "class_rank", entry = entry } end
-- a dialogue (the game's box: its speakers' names and portraits). lines: "text" (said by the hero), or a list of pages -
-- "text" (the speaker before) or { speaker, "text" } (a citizen's tag: "bryan", "duke", ...; "hero" the hero played; a
-- class: C.tag); "\n" in a text breaks the line. on_yes: its last page asks Yes / No, on_yes() runs on Yes
local says, say_owner = {}, nil
function S.say(lines, on_yes)
  local id = nil
  if on_yes then id = tostring(#says + 1); says[id] = on_yes end
  say_owner = S.request{ cmd = "say", text = pages_of(lines), choice = id }
  return true
end
S.on("dialogue_choice", function(e)
  local f = e.owner == say_owner and says[e.id]
  if f then says[e.id] = nil; local ok, err = pcall(f); if not ok then S.log("say: " .. tostring(err)) end end
end)
-- a game status on an enemy (slow, daze, blind, stun, root, elemental_broken, ...) for some seconds; value: its % if any
function S.debuff(target, key, seconds, value, stat)
  return S.call("gml_Script_scr_buff_list_add", key, math.floor(seconds * 60), target, 1, true, false,
    value and tostring(value) or "", stat or "")
end
-- a sprite from an image in the mod's folder: frames side by side (o.frames, else as many of o.like's width as fit, else 1); origin
-- o.xorig / o.yorig, else o.like's, else its middle
function S.sprite(file, o)
  o = o or {}
  return S.request{ cmd = "sprite_file", file = file, frames = o.frames, xorig = o.xorig, yorig = o.yorig, like = o.like }
end
-- a sound from a file in the mod's folder (.ogg streamed, .wav PCM) -> its id, for S.play and the pieces' o.sound
function S.sound(file) return S.request{ cmd = "sound_file", file = file } end
-- plays a sound: o.gain (x the game's sound effects volume), o.pitch, o.loop -> its playing instance
function S.play(sound, o) o = o or {}; return S.request{ cmd = "sound_play", sound = sound, gain = o.gain, pitch = o.pitch, loop = o.loop } end
local function snd(v, default)                       -- a piece's sound: the game's by name, or one of S.sound's
  if type(v) == "number" then S.play(v); return "" end
  return v or default
end
-- the game's attack pieces for a class_cast event e (o: options; sprites by name or number); once e's hero is gone (a
-- cast's late piece, its run left): nothing, nil - its sound neither
local HERO = 119                                     -- obj_parent_hero: "the hero" as the attacker
local function spr(v) if type(v) == "string" then return S.asset(v) end return v end
function S.melee(e, o)                               -- a swing (animation + arc hitbox) toward the aim
  if not S.exists(e.hero) then return nil end
  o = o or {}
  return S.call_on(e.hero, "gml_Script_scr_new_attack_state_melee_create", e.forced, o.skill or e.skill, e.element, HERO,
    spr(o.anim or "spr_the_mighty_swing"), o.anim_speed or 0.45, o.dmg_type or e.dmg_type, snd(o.sound, "large_sword"), 0.3,
    spr(o.anim2 or "spr_the_mighty_swing_blow_1"), o.facing or e.facing or 1, spr(o.mask or "spr_aoe_210"), o.scale or 1, 0, false)
end
function S.projectile(e, o)                          -- n projectiles toward the aim (or o.dir degrees)
  if not S.exists(e.hero) then return nil end
  o = o or {}
  return S.call_on(e.hero, "gml_Script_scr_new_attack_state_projectile_create", o.n or 1, S.asset("obj_projectile"),
    snd(o.sound, "spear_light_fire"), o.spread or 15, o.skill or e.skill, HERO, e.element, e.forced, false, -1, -1, o.dir or -1,
    o.dmg_type or e.dmg_type, 0, o.speed or 5.5, spr(o.sprite or "spr_gladius"), 0, 0, true, true)
end
function S.area(e, o)                                -- a hitbox (o.mask scaled by o.scale) at o.x, o.y (default: the aim)
  if not S.exists(e.hero) then return nil end
  o = o or {}
  return S.call_on(e.hero, "gml_Script_scr_damage_create_hitbox", spr(o.mask or "spr_aoe_360"), o.scale or 1, 1, 0,
    o.skill or e.skill, snd(o.sound, ""), spr(o.impact or "spr_impact_melee_base"), "good", HERO, e.forced, e.element,
    o.x or e.precast_x or e.x, o.y or e.precast_y or e.y)
end
function S.anim(x, y, sprite, o)                     -- an effect animation
  o = o or {}
  return S.call("gml_Script_scr_create_anim", x, y, spr(sprite), o.speed or 0.4, o.scale or 1, o.depth or 200,
    o.follow or -1, false, "none", "effect")
end
-- pushes target away from by (an instance, e.g. e.hero) with force; a negative force pulls it toward by
function S.knock(target, force, by) return S.call_on(by or target, "gml_Script_scr_damage_knock", target, by or HERO, force) end
print = S.log
)LUA";

	void Unload(Mod& M, bool Exiting = false)
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
		try { CmdUnhook(owner); CmdUnsubscribe(owner); legend::Unregister(M.Owner()); }
		catch (const std::exception&) {}
		// its game heroes' content goes with it (its code - legacies' uses, reapers' handlers - is gone); a class stays
		// (its saves are its own). At the game's exit nothing changes
		if (!Exiting)
			try { CmdUnregisterHero(owner); }
			catch (const std::exception&) {}
		if (!M.SettingsName.empty()) { g_Settings.erase(M.SettingsName); ++g_SettingsVer; M.SettingsName.clear(); }
		M.Timers.clear();
		M.StatsFns.clear();
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
			{ "_menu", Menu }, { "clock", Clock }, { "on_stats", OnStats }, { "_content_version", ContentVersion }, { "exists", InstExists },
			{ nullptr, nullptr } };
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
			std::string chunk = "@" + (M.Pack ? M.Stem + "/" + M.Path.filename().string() : M.Stem + ".lua");
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

	struct Found { fs::path Path; bool On = true, Pack = false; std::string File, Need, Name, Version, Author, Description; };

	// A Lua pack in the shared Vortex format: mods/lua/<folder>/lua-pack.json + its entry script. Only packs made for
	// SlormAPI ("requires": {"slorm.slormapi": "<min version>"}) run here; packs for another Lua runtime are left alone.
	bool ReadPack(const fs::path& Dir, Found& Out, std::string& Id)
	{
		fs::path man = Dir / "lua-pack.json", off = Dir / (std::string("lua-pack.json") + kOff);
		bool on = Exists(man);
		if (!on && !Exists(off)) return false;
		std::error_code ec;
		auto size = fs::file_size(on ? man : off, ec);
		if (ec || size > 65536) return false;
		std::ifstream f(on ? man : off, std::ios::binary);
		std::stringstream ss;
		ss << f.rdbuf();
		Json j;
		try { j = sj::Parse(ss.str()); } catch (const std::exception&) { return false; }
		if (j.type != Json::Obj || j.GetStr("type") != "slorm-lua") return false;
		const Json* req = j.Find("requires");
		const Json* want = req && req->type == Json::Obj ? req->Find("slorm.slormapi") : nullptr;
		if (!want) return false;                                       // made for another Lua runtime
		Id = j.GetStr("id");
		std::string entry = j.GetStr("entry", "main.lua");
		if (Id.empty() || Id.size() > 64 || Id.find_first_of("/\\:") != std::string::npos || entry.empty()
			|| entry.find(':') != std::string::npos || entry.find("..") != std::string::npos || entry[0] == '/' || entry[0] == '\\'
			|| !Lower(entry).ends_with(".lua"))
			return false;
		Out = { Dir / fs::path(entry), on, true, Dir.filename().string() + "/lua-pack.json", "", j.GetStr("name"), j.GetStr("version"),
			j.GetStr("author"), j.GetStr("description") };
		std::string min = want->type == Json::Str ? want->s : "";
		if (!min.empty() && CompareVersions(kVersion, min) < 0) Out.Need = "Needs SlormAPI " + min + "+";
		return true;
	}

	// mods/lua: new, changed, removed, switched on / off; single scripts and packs (one mod per name: a pack wins)
	std::set<std::string> Shadowed;           // loose scripts already reported as hidden by a pack of the same name
	void Scan()
	{
		fs::path dir = GameDir() / "mods" / "lua";
		std::error_code ec;
		fs::create_directories(dir, ec);
		std::map<std::string, Found> found;
		std::map<std::string, std::string> byLower;                   // the same name in another case is the same mod
		auto rank = [](const Found& F) { return (F.Pack ? 2 : 0) + (F.On ? 1 : 0); };
		auto put = [&](const std::string& Stem, Found F)
		{
			std::string low = Lower(Stem);
			if (auto it = byLower.find(low); it != byLower.end())
			{
				Found& old = found[it->second];
				bool replace = rank(F) > rank(old);
				const Found& hidden = replace ? old : F;
				if (old.Pack != F.Pack && !hidden.Pack && Shadowed.insert(low).second)
					DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormAPI] mods/lua/%s is also installed as a pack; the pack runs",
						hidden.File.c_str());
				if (!replace) return;
				found.erase(it->second);
			}
			byLower[low] = Stem;
			found[Stem] = std::move(F);
		};
		for (auto& f : fs::directory_iterator(dir, ec))
		{
			std::error_code fe;
			if (f.is_directory(fe))
			{
				Found p;
				std::string id;
				if (ReadPack(f.path(), p, id)) put(id, std::move(p));
				continue;
			}
			if (!f.is_regular_file(fe)) continue;
			std::string name = f.path().filename().string(), low = Lower(name);
			if (low.ends_with(".lua")) put(name.substr(0, name.size() - 4), { f.path(), true, false, name, "" });
			else if (low.ends_with(".lua.disabled"))
				put(name.substr(0, name.size() - 13), { f.path(), false, false, name.substr(0, name.size() - 9), "" });
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
			m->Path = pe.Path;
			m->Enabled = pe.On;
			m->Pack = pe.Pack;
			m->File = pe.File;
			m->PackName = pe.Name;
			m->PackVersion = pe.Version;
			m->PackAuthor = pe.Author;
			m->PackDescription = pe.Description;
			auto t = fs::last_write_time(m->Path, ec);
			if (m->Need != pe.Need) { m->Need = pe.Need; m->Tried = false; ++Version; }
			if (!m->Need.empty())                                     // waits for a newer SlormAPI
			{
				if (m->L) Unload(*m);
				m->Time = t;
				continue;
			}
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
		static double lastHero = -1;                              // (the hero instances seen, for Lua mods and TCP clients:
		RValue hero = HeroInstance();                             // a request on one gone - HeroGone)
		if (KindOf(hero) != VALUE_UNDEFINED && hero.ToDouble() != lastHero)
		{
			lastHero = hero.ToDouble();
			g_HeroIds.insert(static_cast<int32_t>(lastHero));
		}
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
				bool repeat = it->Repeat, failed = it->Failed;
				if (repeat) it->Next = now + (std::max)(it->Every, 0.01);
				else m.Timers.erase(it);
				lua_rawgeti(m.L, LUA_REGISTRYINDEX, ref);
				bool stop = CallTimedOut(m, 0, failed);           // (a repeating one's error logged once)
				if (!repeat) { if (m.L) luaL_unref(m.L, LUA_REGISTRYINDEX, ref); continue; }
				if (!m.L) continue;
				it = std::find_if(m.Timers.begin(), m.Timers.end(), [&](const Timer& T) { return T.Id == id; });
				if (it == m.Timers.end()) continue;               // (cancelled by itself)
				it->Failed = failed;
				if (stop)                                         // (an endless loop: it would stall the game each time)
				{
					m.Timers.erase(it);
					luaL_unref(m.L, LUA_REGISTRYINDEX, ref);
					Stopped(m, "its S.every timer");
				}
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
		j.Add("version", m.Version.empty() ? m.PackVersion : m.Version);
		j.Add("author", m.Author.empty() ? m.PackAuthor : m.Author);
		j.Add("description", !m.Description.empty() ? m.Description : !m.PackDescription.empty() ? m.PackDescription : "Lua script");
		j.Add("file", m.File.empty() ? m.Stem + ".lua" : m.File);
		j.Add("kind", "lua");
		j.Add("loaded", m.L != nullptr);
		j.Add("enabled", m.Enabled);
		bool managed = HardLinked(m.Path);                       // deployed by Vortex: switched there
		j.Add("locked", managed);
		if (managed) { j.Add("managed", true); j.Add("note", "Switched on / off in Vortex"); }
		j.Add("pending", false);
		Json problems = Json::Array();
		if (!m.Need.empty()) problems.Push(Json(m.Need));
		else if (!m.Error.empty()) problems.Push(Json("Lua error: " + m.Error));
		j.Add("problems", std::move(problems));
		rows.push_back(std::move(j));
	}
	Out.a.insert(Out.a.begin() + static_cast<std::ptrdiff_t>(at), rows.begin(), rows.end());
}

static bool LuaToggle(const std::string& File, bool On)
{
	fs::path dir = GameDir() / "mods" / "lua", on, off;
	if (Lower(File).ends_with("/lua-pack.json"))                  // a pack: its manifest is switched
	{
		std::string folder = File.substr(0, File.size() - 14);
		if (folder.empty() || folder == "." || folder == ".." || folder.find_first_of("/\\:") != std::string::npos) return false;
		on = dir / folder / "lua-pack.json";
		off = dir / folder / (std::string("lua-pack.json") + kOff);
	}
	else if (Lower(File).ends_with(".lua") && File.find_first_of("/\\:") == std::string::npos) { on = dir / File; off = dir / (File + kOff); }
	else return false;
	if (HardLinked(on)) Fail(File + " is managed by Vortex: switch it on / off there");
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

// obj_ui_next_gen's Step code, once seen (compared by pointer from then on; forgotten if it stops showing up)
static CCode* g_UiStepCode = nullptr;
static CCode* g_UiDrawCode = nullptr;                // the GUI draw of obj_ui_next_gen (the legendary warning)
static uint64_t g_EventsSinceUiStep = 0;

// catch_errors: runs the event, and leaves it (not the game) on a GML error
static void CallCatching(FWCodeEvent& Context, CCode* Code)
{
	std::string name = Code->GetName() ? Code->GetName() : "?";
	auto it = g_CaughtErrors.find(name);
	if (it != g_CaughtErrors.end() && it->second >= kCatchMute) { Context.Override(true); return; }
	try { Context.Call(); }
	catch (...)
	{
		uint64_t n = ++g_CaughtErrors[name];
		DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormAPI] catch_errors: GML error in %s (%llu)%s", name.c_str(),
			static_cast<unsigned long long>(n), n >= kCatchMute ? " - skipped from now on" : "");
	}
}

// ============================================================================
// Game fixes: the game's own bugs (any hero, no mod involved), found while testing
// ============================================================================
namespace gamefix
{
	// scr_play_sound(group, name) returns the sound it played - or, when it plays none (the same sound a moment before,
	// ...), its unset local "". The launcher's Wrath buttons (scr_ui_launcher_change_torment, the one script that hands
	// that result to audio_sound_gain) then stop on a GML error ("argument 1 incorrect type (string)"). There, no
	// sound is -1, which audio_sound_gain leaves alone.
	int InTorment = 0;                                       // (a click's call: back to 0 each frame, also after an error)
	bool PreTorment(CInstance*, int, RValue**, RValue&) { ++InTorment; return false; }
	void PostTorment(CInstance*, int, RValue**, RValue&) { if (InTorment > 0) --InTorment; }
	void PostPlaySound(CInstance*, int, RValue**, RValue& Result)
	{
		if (InTorment > 0 && KindOf(Result) == VALUE_STRING) Result = RValue(-1.0);
	}

	// scr_collision_map_step (obj_controller, each step) builds the room's three collision maps as sprites - its
	// map_collision_sprite / _unlocked / _furnitures (sprite_create_from_surface, the room / 16: the hub's 192x128; the
	// newest also its sprite_index) - again whenever the map changes, and never deletes one: each rebuild and each room
	// left them behind (~5 a hub load). A sprite it no longer holds (in a slot or as sprite_index) is deleted, and a
	// controller's own once a newer room's controller is in its place.
	const char* const kMapSlots[] = { "map_collision_sprite", "map_collision_unlocked", "map_collision_furnitures" };
	using MapSet = std::array<double, 4>;                    // its three maps and its sprite_index
	std::map<double, MapSet> MapSprites;                     // obj_controller instance -> what it holds now
	MapSet MapBefore{ -1, -1, -1, -1 };
	MapSet MapsOf(CInstance* Self)
	{
		MapSet m{ -1, -1, -1, -1 };
		RValue self(Self);
		for (size_t k = 0; k < 3; ++k)
			if (RValue* v = FindMember(self, kMapSlots[k]); v && IsNumeric(*v)) m[k] = v->ToDouble();
		RValue v;
		if (AurieSuccess(g_Yytk->GetBuiltin("sprite_index", Self, NULL_INDEX, v)) && IsNumeric(v)) m[3] = v.ToDouble();
		return m;
	}
	void DeleteSprite(double Spr)
	{
		if (Spr >= 0 && g_Yytk->CallBuiltin("sprite_exists", { RValue(Spr) }).ToBoolean())
			g_Yytk->CallBuiltin("sprite_delete", { RValue(Spr) });
	}
	bool PreCollisionMap(CInstance* Self, int, RValue**, RValue&)
	{
		if (Self) MapBefore = MapsOf(Self);
		return false;
	}
	void PostCollisionMap(CInstance* Self, int, RValue**, RValue&)
	{
		if (!Self) return;
		MapSet now = MapsOf(Self);
		if (now == MapBefore) return;
		std::set<double> gone(MapBefore.begin(), MapBefore.end());   // (what it held and holds no more)
		for (double spr : now) gone.erase(spr);
		for (double spr : gone) DeleteSprite(spr);
		MapSprites[static_cast<double>(InstanceId(Self))] = now;
	}
	void Tick()
	{
		InTorment = 0;
		if (MapSprites.size() < 2) return;                    // (one controller: the one in use)
		// a controller is gone once a newer one (instance ids grow) exists: not one only deactivated, which a newer
		// controller would not stand in for (the game makes one per room)
		double newest = -1;
		for (auto& [id, maps] : MapSprites)
			if (g_Yytk->CallBuiltin("instance_exists", { RValue(id) }).ToBoolean()) newest = (std::max)(newest, id);
		for (auto it = MapSprites.begin(); it != MapSprites.end();)
			if (it->first < newest && !g_Yytk->CallBuiltin("instance_exists", { RValue(it->first) }).ToBoolean())
			{
				for (double spr : std::set<double>(it->second.begin(), it->second.end())) DeleteSprite(spr);
				it = MapSprites.erase(it);
			}
			else ++it;
	}

	// asset_get_index / asset_get_type look a name up in each asset table in turn, and the paths' (after objects,
	// sprites, sounds and tilesets) walks every path slot ever made: the runner never reuses a deleted path's slot,
	// and the AI states make a new path at each repath (scr_AI_state_path / follow / wander / escort / special:
	// path_add, then mp_grid_path), thousands a minute in a fight - ~600,000 slots after a few Battlefield runs, ~0.3 ms
	// a lookup. The hero's Draw looks its palette sprite up by name every frame (none for the Knight: every table
	// walked), so the fps fell with each run. Every slot after the data file's paths is path_add's, named "__newpath"
	// + its slot: the same answer from the data file's paths and, for a "__newpath<n>", slot n. (Reusing slots
	// instead would hand a deleted path's id - still held, and path_exists-checked, by the AI - to another path.)
	namespace paths
	{
		// the 1.0.2c runner's Path_Find and its tables (its first bytes, with their offsets to the tables, checked:
		// another build is left alone)
		constexpr uintptr_t kFind = 0x1441300, kNames = 0x1e63df0, kCount = 0x1e63df8, kSlots = 0x1e63e08;
		constexpr uint8_t kFindBytes[] = {
			0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18, 0x8b, 0x05, 0xe3, 0x2a, 0xa2,
			0x00, 0x45, 0x33, 0xdb, 0x83, 0xe8, 0x01, 0x48, 0x8b, 0xf1, 0x48, 0x63, 0xf8, 0x78, 0x53, 0x4c, 0x8b, 0x0d, 0xc6, 0x2a,
			0xa2, 0x00, 0x45, 0x8b, 0xd3, 0x48, 0x8b, 0x1d, 0xd4, 0x2a, 0xa2, 0x00, 0x49, 0x2b, 0xd9, 0x4a, 0x83, 0x3c, 0x0b, 0x00,
			0x74, 0x29, 0x49, 0x8b };
		using FindFn = int (*)(const char*);
		FindFn Original = nullptr;
		bool Skipped = false;
		char*** Names = nullptr;                             // (a deleted slot: no path, no name)
		void*** Slots = nullptr;
		int* Count = nullptr;
		int DataPaths = 0;                                   // the slots before path_add's
		int Find(const char* Name)
		{
			if (!Name) return Original(Name);
			char** names = *Names;
			void** slots = *Slots;
			const int count = *Count;
			for (int i = 0; i < DataPaths && i < count; ++i)
				if (slots[i] && !strcmp(names[i], Name)) return i;
			if (strncmp(Name, "__newpath", 9)) return -1;
			int64_t n = 0;
			const char* d = Name + 9;
			for (; *d >= '0' && *d <= '9' && n < count; ++d) n = n * 10 + (*d - '0');
			if (d == Name + 9 || *d || n >= count) return -1;
			return slots[n] && !strcmp(names[n], Name) ? static_cast<int>(n) : -1;
		}
		bool Install()
		{
			if (Original || Skipped) return true;
			auto base = reinterpret_cast<uint8_t*>(ExeBase());
			auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
			if (nt->OptionalHeader.SizeOfImage < kSlots + 8 || memcmp(base + kFind, kFindBytes, sizeof(kFindBytes)))
			{
				Skipped = true;
				DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormAPI] gamefix: not the 1.0.2c runner - path lookup left as is");
				return true;
			}
			Names = reinterpret_cast<char***>(base + kNames);
			Slots = reinterpret_cast<void***>(base + kSlots);
			Count = reinterpret_cast<int*>(base + kCount);
			for (int i = 0; i < *Count; ++i)                 // (the data file's are loaded before any GML runs)
				if ((*Slots)[i] && strncmp((*Names)[i], "__newpath", 9)) DataPaths = i + 1;
			PVOID trampoline = nullptr;
			if (!AurieSuccess(MmCreateHook(g_ArSelfModule, "SlormAPI.gamefix.path_find", base + kFind,
				reinterpret_cast<PVOID>(&Find), &trampoline)) || !trampoline) return false;
			Original = reinterpret_cast<FindFn>(trampoline);
			return true;
		}
	}

	int Tries = 0;                                           // (installed once; a failed one tried again a few times)
	void EnsureHooks()
	{
		if (Tries < 0 || Tries >= 5) return;
		++Tries;
		bool ok = NativeHookInstall("scr_ui_launcher_change_torment", { "SlormAPI.gamefix", PreTorment, PostTorment })
			& NativeHookInstall("scr_play_sound", { "SlormAPI.gamefix", nullptr, PostPlaySound })
			& NativeHookInstall("scr_collision_map_step", { "SlormAPI.gamefix", PreCollisionMap, PostCollisionMap })
			& paths::Install();
		if (ok) Tries = -1;
	}
}

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
		if (prof::On) prof::Frame();
		gamefix::EnsureHooks();
		gamefix::Tick();
		{ PerfScope p(&g_PerfPartUs[0]); modspage::Tick(); }
		try { PerfScope p(&g_PerfPartUs[1]); events::Tick(); }
		catch (const std::exception& e) { DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormAPI] events: %s", e.what()); }
		try { legend::Tick(); }
		catch (const std::exception& e) { DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormAPI] legendaries: %s", e.what()); }
		if (classes::AnyContent()) classes::ResetWindows();
		if (!classes::Defs.empty()) { classes::ReaperCases(); classes::AreaFrame(); }
		try { classes::Tick(); }
		catch (const std::exception& e) { DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormAPI] classes: %s", e.what()); }
		try { PerfScope p(&g_PerfPartUs[2]); lua::Tick(); }
		catch (const std::exception& e) { DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormAPI] lua: %s", e.what()); }
	}
	auto& args = Context.Arguments();
	CCode* code = std::get<2>(args);
	if (classes::AnyContent()) classes::BeforeEvent(std::get<0>(args), code);
	bool uiDraw = code && (code == g_UiDrawCode || (!g_UiDrawCode && code->GetName() && !strcmp(code->GetName(), "gml_Object_obj_ui_next_gen_Draw_64")));
	if (uiDraw) g_UiDrawCode = code;
	bool overlay = g_Overlay && !g_InOverlay && code && code->GetName() && g_OverlayEvent == code->GetName();
	bool warn = uiDraw && legend::WarnLeft > 0;
	bool meter = uiDraw && classes::AnyContent() && (classes::MeterShown() || classes::ProfileShown(std::get<0>(args))
		|| classes::PickerShown(std::get<0>(args)) || classes::SkinsShown(std::get<0>(args)));
	if (!overlay && !warn && !meter)
	{
		if (prof::On && code)
			prof::Run(code, [&] { if (g_CatchErrors) CallCatching(Context, code); else Context.Call(); });
		else if (g_CatchErrors && code) CallCatching(Context, code);
		return;
	}
	if (prof::On && code) prof::Run(code, [&] { Context.Call(); });
	else Context.Call();                              // the game's own drawing first, ours on top
	if (overlay) DrawOverlay(std::get<0>(args), std::get<1>(args));
	if (meter)
	{
		try
		{
			classes::DrawMeter(std::get<0>(args)); classes::DrawProfileOverlay(std::get<0>(args)); classes::DrawPickerOverlay(std::get<0>(args));
			classes::DrawSkinOverlay(std::get<0>(args));
		}
		catch (const std::exception&) {}
	}
	if (warn)
		try { legend::DrawWarning(std::get<0>(args)); }
		catch (const std::exception&) { legend::WarnLeft = 0; }
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
	for (auto& mp : lua::Mods) lua::Unload(*mp, true);
	ObDestroyInterface(Module, SLORMAPI_INTERFACE_NAME);
	if (g_ServerStarted) slorm::StopServer();
	for (size_t k = 0; k < kMaxHooks; ++k)
		if (g_Hooks[k].Trampoline) RemoveHookSlot(k);
	return AURIE_SUCCESS;
}
