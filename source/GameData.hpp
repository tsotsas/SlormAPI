// Game tables, read at runtime from the game's own data files (dat_*.json next to the exe; some are base64-wrapped), so
// nothing from the game is redistributed with the mod. (Started in BuildFilter; SlormAPI serves it to every mod.)
#pragma once
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <Windows.h>
#include "Json.hpp"

namespace gd
{
	using sj::Json;

	inline std::string Base64Decode(const std::string& In)
	{
		static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string out;
		int val = 0, bits = -8;
		for (unsigned char c : In)
		{
			size_t p = chars.find(static_cast<char>(c));
			if (p == std::string::npos) continue;
			val = (val << 6) + static_cast<int>(p);
			bits += 6;
			if (bits >= 0) { out.push_back(static_cast<char>((val >> bits) & 0xFF)); bits -= 8; }
		}
		return out;
	}

	inline std::string GameDir()
	{
		char buf[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, buf, MAX_PATH);
		std::string p(buf);
		return p.substr(0, p.find_last_of("\\/"));
	}

	inline Json ReadGameJson(const std::string& Name)
	{
		std::ifstream f(GameDir() + "\\" + Name, std::ios::binary);
		if (!f) throw std::runtime_error("missing game file " + Name);
		std::stringstream ss;
		ss << f.rdbuf();
		std::string raw = ss.str();
		size_t i = raw.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
		if (i == std::string::npos) throw std::runtime_error("empty game file " + Name);
		if (raw[i] != '[' && raw[i] != '{') raw = Base64Decode(raw);
		return sj::Parse(raw);
	}

	inline double Num(const Json& J, double Default = 0)
	{
		if (J.type == Json::Num) return J.n;
		if (J.type == Json::Bool) return J.b ? 1 : 0;
		if (J.type == Json::Str && !J.s.empty())
		{
			try { return std::stod(J.s); }
			catch (...) { return Default; }
		}
		return Default;
	}

	inline std::string Str(const Json& J)
	{
		if (J.type == Json::Str) return J.s;
		if (J.type == Json::Num)
		{
			char b[32];
			snprintf(b, sizeof(b), "%g", J.n);
			return b;
		}
		if (J.type == Json::Bool) return J.b ? "True" : "False";
		return "";
	}

	// Slot keys as the item arrays name them, with the dat_sta.json column of each
	struct SlotInfo { const char* Key; const char* Label; const char* Column; };
	inline const std::vector<SlotInfo>& Slots()
	{
		static const std::vector<SlotInfo> s = {
			{ "helm", "Helmet", "HELM" }, { "body", "Body Armor", "ARMOR" }, { "shoulder", "Shoulders", "SHOULDER" },
			{ "bracer", "Bracers", "BRACER" }, { "glove", "Gloves", "GLOVE" }, { "boot", "Boots", "BOOT" },
			{ "ring", "Rings", "RING" }, { "amulet", "Amulet", "AMULET" }, { "belt", "Belt", "BELT" },
			{ "cape", "Cape", "CAPE" } };
		return s;
	}

	inline const char* kTraits[8] = { "Toughness", "Savagery", "Fury", "Determination", "Zeal", "Willpower", "Dexterity",
		"Bravery" };

	struct GameData
	{
		std::map<std::string, std::string> Text;                    // dat_str.json REF -> English
		std::map<std::string, double> Score;                        // stat -> SCORE
		std::map<std::string, std::vector<std::string>> GoldPool;   // slot -> stats an Epic (gold) line can roll
		struct Leg { int Id; std::string Name, Item; int Hero; bool Lootable; std::string Desc, Value, Type; };
		std::map<int, Leg> Legendaries;
		std::map<int, std::map<int, std::string>> Skills;           // class -> skill id -> name
		bool Loaded = false;

		void Load()
		{
			Json str = ReadGameJson("dat_str.json");
			for (auto& r : str.a)
				if (const Json* ref = r.Find("REF")) Text[Str(*ref)] = r.GetStr("EN");
			Json sta = ReadGameJson("dat_sta.json");
			for (auto& r : sta.a)
			{
				std::string ref = Str(r.At("REF"));
				Score[ref] = Num(r.At("SCORE"));
				for (auto& s : Slots())
				{
					const Json* c = r.Find(s.Column);
					std::string v = c ? Str(*c) : "";
					v.erase(std::remove(v.begin(), v.end(), ' '), v.end());
					if (v == "P" || v == "S" || v == "E") GoldPool[s.Key].push_back(ref);
				}
			}
			Json leg = ReadGameJson("dat_leg.json");
			for (auto& r : leg.a)
			{
				if (!r.Find("REF")) continue;
				Leg l{ static_cast<int>(Num(r.At("REF"))), r.GetStr("EN_NAME"), Str(r.Find("ITEM") ? *r.Find("ITEM") : Json()),
					static_cast<int>(Num(r.Find("HERO") ? *r.Find("HERO") : Json(), -1)), false };
				const Json* lt = r.Find("LOOTABLE");
				l.Lootable = lt && (lt->type == Json::Bool ? lt->b : Str(*lt) == "True");
				l.Desc = r.GetStr("EN_DESC");
				l.Value = r.Find("VALUE") ? Str(*r.Find("VALUE")) : "";
				l.Type = r.Find("TYPE") ? Str(*r.Find("TYPE")) : "";
				Legendaries[l.Id] = l;
			}
			for (int cls = 0; cls < 3; ++cls)
			{
				try
				{
					Json c = ReadGameJson("dat_cla_" + std::to_string(cls) + ".json");
					for (auto& r : c.a)
						if (r.Find("REF")) Skills[cls][static_cast<int>(Num(r.At("REF")))] = r.GetStr("EN_NAME");
				}
				catch (...) {}
			}
			Loaded = true;
		}

		std::string StatName(const std::string& Ref) const
		{
			if (Ref.empty()) return "?";
			auto it = Text.find(Ref);
			if (it != Text.end() && !it->second.empty()) return it->second;
			for (const char* suf : { "_add", "_percent", "_global_mult", "_set" })
			{
				std::string s(suf);
				if (Ref.size() > s.size() && Ref.compare(Ref.size() - s.size(), s.size(), s) == 0)
				{
					auto jt = Text.find(Ref.substr(0, Ref.size() - s.size()));
					if (jt != Text.end() && !jt->second.empty()) return jt->second;
				}
			}
			return Ref;
		}

		// the game's own name, plus " %" when another stat of the slot has the same name (Max Life / Max Life %)
		std::string StatLabel(const std::string& Ref, const std::string& Slot) const
		{
			if (Ref.empty()) return "(no stat)";
			std::string name = StatName(Ref);
			auto it = GoldPool.find(Slot);
			bool twin = false;
			if (it != GoldPool.end())
				for (auto& s : it->second) twin |= s != Ref && StatName(s) == name;
			bool pct = Ref.size() > 8 && Ref.compare(Ref.size() - 8, 8, "_percent") == 0;
			return name + (twin && pct ? " %" : "");
		}

		std::string LegName(int Id) const
		{
			auto it = Legendaries.find(Id);
			return it != Legendaries.end() ? it->second.Name : "Legendary " + std::to_string(Id);
		}

		std::string SkillName(int Cls, int Id) const
		{
			auto c = Skills.find(Cls);
			if (c != Skills.end())
				if (auto s = c->second.find(Id); s != c->second.end()) return s->second;
			return "Skill " + std::to_string(Id);
		}

		// lootable legendaries of a slot a class can use (leg HERO -1 any / class / 99 legacy / 999 reaper), by name
		std::vector<int> LegendaryPool(const std::string& Slot, int Cls) const
		{
			std::vector<int> out;
			for (auto& [id, l] : Legendaries)
				if (l.Item == Slot && l.Lootable && (l.Hero == -1 || l.Hero == Cls || l.Hero == 99 || l.Hero == 999))
					out.push_back(id);
			std::sort(out.begin(), out.end(), [&](int a, int b) { return LegName(a) < LegName(b); });
			return out;
		}

		// scr_loot_stat_from_score: SCORE x roll / 100; flat stats x (1 + L (0.3 + k L)) and rounded (k: damage 0.012,
		// defense 0.009, other flat 0), others rounded to 2 decimals (leech keeps every decimal)
		double AffixValue(const std::string& Stat, double Roll, int Level) const
		{
			static const std::map<std::string, double> levelK = {
				{ "min_basic_damage_add", 0.012 }, { "min_elemental_damage_add", 0.012 }, { "res_phy_add", 0.009 },
				{ "res_mag_add", 0.009 }, { "dodge_add", 0.009 }, { "thorns_add", 0 }, { "the_max_health_add", 0 },
				{ "health_regen_add", 0 }, { "health_on_hit_add", 0 }, { "health_on_kill_add", 0 },
				{ "the_max_mana_add", 0 }, { "mana_regen_add", 0 }, { "mana_on_hit_add", 0 }, { "mana_on_kill_add", 0 } };
			auto sc = Score.find(Stat);
			double v = (sc != Score.end() ? sc->second : 0) * Roll / 100;
			auto k = levelK.find(Stat);
			if (k == levelK.end())
			{
				if (Stat == "health_leech_percent" || Stat == "mana_leech_percent") return v;
				return std::nearbyint(v * 100) / 100;
			}
			v = v * (1 + Level * (0.3 + k->second * Level));
			double r = std::nearbyint(v);
			return (r != 0 || v == 0) ? r : std::ceil(v);
		}
	};
}
