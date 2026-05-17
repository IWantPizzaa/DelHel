#pragma once

#include <string>
#include <map>
#include <set>
#include <regex>

#include "sid.h"
#include "routing.h"

struct airport {
	std::string icao;
	int elevation;
	bool active;
	std::map<std::string, sid> sids;
	std::map<std::string, bool> rwys;
	std::map<std::string, std::string> rwy_sides;
	std::map<std::string, int> loa_max_fl;
	std::vector<routing> validroutes;
	std::regex rwy_regex;
};
