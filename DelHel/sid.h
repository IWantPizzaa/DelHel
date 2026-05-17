#pragma once

#include <string>
#include <map>

struct sidinfo {
	std::string rwy;
	std::string jet_dep;
	std::string prop_dep;
	std::string unlink_jet_dep;
	std::string unlink_prop_dep;
	std::string nap;
	int cfl{};
	int cfl_prop{};
	int prio{};
};

struct sid {
	std::string wp;
	int cfl{};
	std::string north_south;
	struct rfl_constraint {
		int min_fl{};
		int max_fl{};
		int jet_max_fl{};
		int prop_max_fl{};
	};
	rfl_constraint rfl{};
	std::map<std::string, sidinfo> rwys;
};
