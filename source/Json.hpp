// Minimal JSON value, parser and serializer (no external dependencies).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace sj
{
	struct Json
	{
		enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
		bool b = false;
		double n = 0;
		std::string s;
		std::vector<Json> a;
		std::vector<std::pair<std::string, Json>> o;

		Json() = default;
		Json(std::nullptr_t) {}
		Json(bool v) : type(Bool), b(v) {}
		Json(double v) : type(Num), n(v) {}
		template <typename T> requires std::is_integral_v<T> && (!std::is_same_v<T, bool>)
		Json(T v) : type(Num), n(static_cast<double>(v)) {}
		Json(const char* v) : type(Str), s(v) {}
		Json(std::string v) : type(Str), s(std::move(v)) {}
		Json(std::string_view v) : type(Str), s(v) {}

		static Json Array() { Json j; j.type = Arr; return j; }
		static Json Object() { Json j; j.type = Obj; return j; }

		bool IsNull() const { return type == Null; }

		const Json* Find(std::string_view Key) const
		{
			if (type != Obj) return nullptr;
			for (auto& [k, v] : o)
				if (k == Key) return &v;
			return nullptr;
		}

		const Json& At(std::string_view Key) const
		{
			const Json* v = Find(Key);
			if (!v) throw std::runtime_error("missing field '" + std::string(Key) + "'");
			return *v;
		}

		Json& Set(std::string Key, Json Value)
		{
			type = Obj;
			for (auto& [k, v] : o)
				if (k == Key) { v = std::move(Value); return v; }
			o.emplace_back(std::move(Key), std::move(Value));
			return o.back().second;
		}

		// Appends without a duplicate-key check (for keys known to be unique).
		void Add(std::string Key, Json Value) { type = Obj; o.emplace_back(std::move(Key), std::move(Value)); }

		void Push(Json Value) { type = Arr; a.push_back(std::move(Value)); }

		std::string GetStr(std::string_view Key, std::string Default = "") const
		{
			const Json* v = Find(Key);
			return (v && v->type == Str) ? v->s : Default;
		}

		double GetNum(std::string_view Key, double Default) const
		{
			const Json* v = Find(Key);
			return (v && v->type == Num) ? v->n : Default;
		}

		bool GetBool(std::string_view Key, bool Default) const
		{
			const Json* v = Find(Key);
			return (v && v->type == Bool) ? v->b : Default;
		}
	};

	namespace detail
	{
		inline void AppendUtf8(std::string& Out, uint32_t Cp)
		{
			if (Cp < 0x80) Out += static_cast<char>(Cp);
			else if (Cp < 0x800) { Out += static_cast<char>(0xC0 | (Cp >> 6)); Out += static_cast<char>(0x80 | (Cp & 0x3F)); }
			else if (Cp < 0x10000) { Out += static_cast<char>(0xE0 | (Cp >> 12)); Out += static_cast<char>(0x80 | ((Cp >> 6) & 0x3F)); Out += static_cast<char>(0x80 | (Cp & 0x3F)); }
			else { Out += static_cast<char>(0xF0 | (Cp >> 18)); Out += static_cast<char>(0x80 | ((Cp >> 12) & 0x3F)); Out += static_cast<char>(0x80 | ((Cp >> 6) & 0x3F)); Out += static_cast<char>(0x80 | (Cp & 0x3F)); }
		}

		struct Parser
		{
			std::string_view s;
			size_t i = 0;

			[[noreturn]] void Fail(const char* Msg) const
			{
				throw std::runtime_error(std::string("json: ") + Msg + " at offset " + std::to_string(i));
			}

			void Ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }

			bool Lit(std::string_view L) { if (s.substr(i, L.size()) == L) { i += L.size(); return true; } return false; }

			uint32_t Hex4()
			{
				if (i + 4 > s.size()) Fail("bad \\u escape");
				uint32_t v = 0;
				for (int k = 0; k < 4; ++k)
				{
					char c = s[i++];
					v <<= 4;
					if (c >= '0' && c <= '9') v |= c - '0';
					else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
					else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
					else Fail("bad hex digit");
				}
				return v;
			}

			std::string String()
			{
				++i; // opening quote
				std::string out;
				for (;;)
				{
					if (i >= s.size()) Fail("unterminated string");
					char c = s[i++];
					if (c == '"') break;
					if (c != '\\') { out += c; continue; }
					if (i >= s.size()) Fail("bad escape");
					char e = s[i++];
					switch (e)
					{
					case '"': out += '"'; break;
					case '\\': out += '\\'; break;
					case '/': out += '/'; break;
					case 'b': out += '\b'; break;
					case 'f': out += '\f'; break;
					case 'n': out += '\n'; break;
					case 'r': out += '\r'; break;
					case 't': out += '\t'; break;
					case 'u':
					{
						uint32_t cp = Hex4();
						if (cp >= 0xD800 && cp < 0xDC00 && Lit("\\u"))
						{
							uint32_t lo = Hex4();
							cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
						}
						AppendUtf8(out, cp);
						break;
					}
					default: Fail("bad escape");
					}
				}
				return out;
			}

			Json Value(int Depth)
			{
				if (Depth > 256) Fail("nesting too deep");
				Ws();
				if (i >= s.size()) Fail("unexpected end");
				char c = s[i];
				if (c == '{')
				{
					++i;
					Json j = Json::Object();
					Ws();
					if (i < s.size() && s[i] == '}') { ++i; return j; }
					for (;;)
					{
						Ws();
						if (i >= s.size() || s[i] != '"') Fail("expected key");
						std::string k = String();
						Ws();
						if (i >= s.size() || s[i] != ':') Fail("expected ':'");
						++i;
						j.o.emplace_back(std::move(k), Value(Depth + 1));
						Ws();
						if (i < s.size() && s[i] == ',') { ++i; continue; }
						if (i < s.size() && s[i] == '}') { ++i; return j; }
						Fail("expected ',' or '}'");
					}
				}
				if (c == '[')
				{
					++i;
					Json j = Json::Array();
					Ws();
					if (i < s.size() && s[i] == ']') { ++i; return j; }
					for (;;)
					{
						j.a.push_back(Value(Depth + 1));
						Ws();
						if (i < s.size() && s[i] == ',') { ++i; continue; }
						if (i < s.size() && s[i] == ']') { ++i; return j; }
						Fail("expected ',' or ']'");
					}
				}
				if (c == '"') return Json(String());
				if (Lit("true")) return Json(true);
				if (Lit("false")) return Json(false);
				if (Lit("null")) return Json();
				size_t start = i;
				while (i < s.size() && (strchr("+-.eE", s[i]) || (s[i] >= '0' && s[i] <= '9'))) ++i;
				if (start == i) Fail("unexpected character");
				std::string num(s.substr(start, i - start));
				char* end = nullptr;
				double v = strtod(num.c_str(), &end);
				if (!end || *end) Fail("bad number");
				return Json(v);
			}
		};

		inline void DumpString(std::string& Out, const std::string& S)
		{
			Out += '"';
			for (unsigned char c : S)
			{
				switch (c)
				{
				case '"': Out += "\\\""; break;
				case '\\': Out += "\\\\"; break;
				case '\n': Out += "\\n"; break;
				case '\r': Out += "\\r"; break;
				case '\t': Out += "\\t"; break;
				default:
					if (c < 0x20) { char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", c); Out += buf; }
					else Out += static_cast<char>(c);
				}
			}
			Out += '"';
		}

		inline void Dump(std::string& Out, const Json& J)
		{
			switch (J.type)
			{
			case Json::Null: Out += "null"; break;
			case Json::Bool: Out += J.b ? "true" : "false"; break;
			case Json::Num:
				if (std::isnan(J.n)) Out += "\"NaN\"";
				else if (std::isinf(J.n)) Out += J.n > 0 ? "\"Infinity\"" : "\"-Infinity\"";
				else if (J.n == std::floor(J.n) && std::fabs(J.n) < 9007199254740992.0) Out += std::to_string(static_cast<long long>(J.n));
				else { char buf[32]; snprintf(buf, sizeof(buf), "%.17g", J.n); Out += buf; }
				break;
			case Json::Str: DumpString(Out, J.s); break;
			case Json::Arr:
				Out += '[';
				for (size_t k = 0; k < J.a.size(); ++k) { if (k) Out += ','; Dump(Out, J.a[k]); }
				Out += ']';
				break;
			case Json::Obj:
				Out += '{';
				for (size_t k = 0; k < J.o.size(); ++k)
				{
					if (k) Out += ',';
					DumpString(Out, J.o[k].first);
					Out += ':';
					Dump(Out, J.o[k].second);
				}
				Out += '}';
				break;
			}
		}
	}

	inline Json Parse(std::string_view Text)
	{
		detail::Parser p{ Text };
		Json j = p.Value(0);
		p.Ws();
		if (p.i != Text.size()) p.Fail("trailing characters");
		return j;
	}

	inline std::string Dump(const Json& J)
	{
		std::string out;
		detail::Dump(out, J);
		return out;
	}
}
