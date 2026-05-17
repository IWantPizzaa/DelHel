#include "pch.h"

#include "CDelHel.h"

CDelHel* pPlugin;

namespace {
	struct geo_point {
		double lat;
		double lon;
	};

	bool PointInPolygon(const geo_point& p, const std::vector<geo_point>& poly)
	{
		bool inside = false;
		size_t j = poly.size() - 1;
		for (size_t i = 0; i < poly.size(); ++i) {
			const geo_point& pi = poly[i];
			const geo_point& pj = poly[j];
			const bool intersect = ((pi.lat > p.lat) != (pj.lat > p.lat)) &&
				(p.lon < (pj.lon - pi.lon) * (p.lat - pi.lat) / (pj.lat - pi.lat + 0.0) + pi.lon);
			if (intersect) {
				inside = !inside;
			}
			j = i;
		}
		return inside;
	}

	const std::vector<geo_point> LFPG_RM_NORTH = {
		{49.0073325, 2.4936375},
		{49.01041194444445, 2.5481225},
		{49.01231361111111, 2.583878055555556},
		{49.02355722222222, 2.5821525},
		{49.01856166666667, 2.492441111111111}
	};

	const std::vector<geo_point> LFPG_RM_SOUTH = {
		{48.98854333333333, 2.528426944444444},
		{48.98803611111111, 2.539931388888889},
		{48.99624055555556, 2.550860555555556},
		{49.00250361111111, 2.551895833333333},
		{49.01041194444445, 2.5481225},
		{49.01231361111111, 2.583878055555556},
		{49.01163444444444, 2.609878055555556},
		{48.99963611111111, 2.611373611111111},
		{49.00122083333333, 2.526931388888889}
	};
}

CDelHel::CDelHel() : EuroScopePlugIn::CPlugIn(
	EuroScopePlugIn::COMPATIBILITY_CODE,
	PLUGIN_NAME,
	PLUGIN_VERSION,
	PLUGIN_AUTHOR,
	PLUGIN_LICENSE
)
{
	std::ostringstream msg;
	msg << "Version " << PLUGIN_VERSION << " loaded.";

	this->LogMessage(msg.str());

	this->RegisterTagItemType("Flightplan Validation", TAG_ITEM_FP_VALIDATION);
	this->RegisterTagItemFunction("Validation menu", TAG_FUNC_VALIDATION_MENU);
	this->RegisterTagItemFunction("Process FPL", TAG_FUNC_PROCESS_FP);
	this->RegisterTagItemFunction("Process FPL (non-NAP)", TAG_FUNC_PROCESS_FP_NON_NAP);
	this->RegisterTagItemFunction("Process FPL (NAP)", TAG_FUNC_PROCESS_FP_NAP);

	this->RegisterDisplayType(PLUGIN_NAME, false, false, false, false);

	this->debug = false;
	this->updateCheck = false;
	this->assignNap = false;
	this->autoProcess = false;
	this->warnRFLBelowCFL = false;
	this->logMinMaxRFL = false;
	this->checkMinMaxRFL = false;
	this->flashOnMessage = false;
	this->topSkyAvailable = false;
	this->ccamsAvailable = false;
	this->preferTopSkySquawkAssignment = false;
	this->lfpgLinked = true;
	this->lfpgCas = false;
	this->lfpgRm = false;
	this->lfpgRmEnabledAt.reset();
	this->natArrivalIcaoPrefixes = { 'B', 'C', 'K', 'M', 'S', 'T' };
	this->arvlArrivalAirports.clear();
	this->customRunwayConfig = "";

	this->LoadSettings();
	this->lfpgRmEnabledAt.reset();
	this->CheckLoadedPlugins();

	this->ReadAirportConfig();
	this->ReadRoutingConfig();
	this->ReadCustomConfigs();

	if (this->updateCheck) {
		this->latestVersion = std::async(FetchLatestVersion);
	}
}

CDelHel::~CDelHel()
{
}

bool CDelHel::IsAirportLinked(const std::string& icao) const
{
	std::string icaoUpper = trim(icao);
	to_upper(icaoUpper);
	return this->unlinkedDepartureAirports.find(icaoUpper) == this->unlinkedDepartureAirports.end();
}

bool CDelHel::IsNatArrivalAirport(const std::string& icao) const
{
	std::string icaoUpper = trim(icao);
	to_upper(icaoUpper);
	if (icaoUpper.empty()) {
		return false;
	}

	return this->natArrivalIcaoPrefixes.find(icaoUpper[0]) != this->natArrivalIcaoPrefixes.end();
}

bool CDelHel::IsArvlArrivalAirport(const std::string& icao) const
{
	std::string icaoUpper = trim(icao);
	to_upper(icaoUpper);
	if (icaoUpper.empty()) {
		return false;
	}

	return this->arvlArrivalAirports.find(icaoUpper) != this->arvlArrivalAirports.end();
}

void CDelHel::SetAirportLinked(const std::string& icao, bool linked)
{
	std::string icaoUpper = trim(icao);
	to_upper(icaoUpper);
	if (icaoUpper.empty()) {
		return;
	}

	if (linked) {
		this->unlinkedDepartureAirports.erase(icaoUpper);
	}
	else {
		this->unlinkedDepartureAirports.emplace(icaoUpper);
	}

	if (icaoUpper == "LFPG") {
		this->lfpgLinked = linked;
	}
}

std::string CDelHel::SerializeUnlinkedDepartureAirports() const
{
	std::ostringstream ss;
	bool first = true;
	for (const auto& icao : this->unlinkedDepartureAirports) {
		if (!first) {
			ss << ",";
		}
		ss << icao;
		first = false;
	}

	return ss.str();
}

void CDelHel::DeserializeUnlinkedDepartureAirports(const std::string& serialized)
{
	this->unlinkedDepartureAirports.clear();

	for (std::string airport : split(serialized, ',')) {
		airport = trim(airport, " \t\r\n");
		to_upper(airport);
		if (airport.size() == 4) {
			this->unlinkedDepartureAirports.emplace(airport);
		}
	}

	this->lfpgLinked = this->IsAirportLinked("LFPG");
}

bool CDelHel::OnCompileCommand(const char* sCommandLine)
{
	std::vector<std::string> args = split(sCommandLine);
	for (auto& arg : args) {
		arg = trim(arg, " \t\r\n");
	}
	args.erase(std::remove_if(args.begin(), args.end(), [](const std::string& s) { return s.empty(); }), args.end());
	if (args.empty()) {
		return false;
	}

	if (starts_with(args[0], ".delhel")) {
		if (args.size() == 1) {
			std::ostringstream msg;
			msg << "Version " << PLUGIN_VERSION << " loaded. Available commands: auto, debug, nap, reload, reset, update, rflblw, logminmaxrfl, minmaxrfl, flash, ccams, rwycfg, <icao>";

			this->LogMessage(msg.str());

			return true;
		}

		if (args[1] == "debug") {
			if (this->debug) {
				this->LogMessage("Disabling debug mode", "Debug");
			}
			else {
				this->LogMessage("Enabling debug mode", "Debug");
			}

			this->debug = !this->debug;

			this->SaveSettings();

			return true;
		}
		else if (args[1] == "update") {
			if (this->updateCheck) {
				this->LogMessage("Disabling update check", "Update");
			}
			else {
				this->LogMessage("Enabling update check", "Update");
			}

			this->updateCheck = !this->updateCheck;

			this->SaveSettings();

			return true;
		}
		else if (args[1] == "nap") {
			if (this->assignNap) {
				this->LogMessage("No longer assigning NAP SIDs", "Config");
			}
			else {
				this->LogMessage("Assigning NAP SIDs", "Config");
			}

			this->assignNap = !this->assignNap;

			this->SaveSettings();

			return true;
		}
		else if (args[1] == "auto") {
			if (this->autoProcess) {
				this->LogMessage("No longer automatically processing flightplans", "Config");
			}
			else {
				this->LogMessage("Automatically processing flightplans", "Config");
			}

			this->autoProcess = !this->autoProcess;

			return true;
		}
		else if (args[1] == "reload") {
			this->LogMessage("Reloading airport config", "Config");

			this->airports.clear();
			this->ReadAirportConfig();
			this->ReadRoutingConfig();

			return true;
		}
		else if (args[1] == "reset") {
			this->LogMessage("Resetting plugin state", "Config");

			this->autoProcess = false;
			this->processed.clear();
			this->airports.clear();
			this->ReadAirportConfig();
			this->ReadRoutingConfig();

			return true;
		}
		else if (args[1] == "rflblw") {
			if (this->warnRFLBelowCFL) {
				this->LogMessage("No longer displaying warnings for RFLs below inital CFLs for SIDs", "Config");
			}
			else {
				this->LogMessage("Displaying warnings for RFLs below inital CFLs for SIDs", "Config");
			}

			this->warnRFLBelowCFL = !this->warnRFLBelowCFL;

			this->SaveSettings();

			return true;
		}
		else if (args[1] == "logminmaxrfl") {
			if (this->logMinMaxRFL) {
				this->LogMessage("No longer logging min and max RFLs for predefined routings", "Config");
			}
			else {
				this->LogMessage("Logging min and max RFLs for predefined routings", "Config");
			}

			this->logMinMaxRFL = !this->logMinMaxRFL;

			this->SaveSettings();

			return true;
		}
		else if (args[1] == "minmaxrfl") {
			if (this->checkMinMaxRFL) {
				this->LogMessage("No longer checking min and max RFLs for predefined routings", "Config");
			}
			else {
				this->LogMessage("Checking min and max RFLs for predefined routings", "Config");
			}

			this->checkMinMaxRFL = !this->checkMinMaxRFL;

			this->SaveSettings();

			return true;
		}
		else if (args[1] == "flash") {
			if (this->flashOnMessage) {
				this->LogMessage("No longer flashing on DelHel message", "Config");
			}
			else {
				this->LogMessage("Flashing on DelHel message", "Config");
			}

			this->flashOnMessage = !this->flashOnMessage;

			this->SaveSettings();

			return true;
		}
		else if (args[1] == "prefertopsky") {
			if (this->preferTopSkySquawkAssignment) {
				this->LogMessage("No longer preferring TopSky squawk assignment, will use CCAMS if loaded", "Config");
			}
			else {
				this->LogMessage("Preferring TopSky squawk assignment, even if CCAMS is loaded", "Config");
			}

			this->preferTopSkySquawkAssignment = !this->preferTopSkySquawkAssignment;

			this->SaveSettings();
			this->CheckLoadedPlugins();

			return true;
		}
		else if (args[1] == "rwycfg")
		{
			if (args.size() < 3)
			{
				std::ostringstream msg;
				msg << "Select custom runway config to apply. Currently active: " << (this->customRunwayConfig.empty() ? "none" : this->customRunwayConfig) << ". Available values : none(disables custom config)";
				for (auto& [name, config] : this->runwayConfigs)
				{
					msg << ", " << name;
				}
				this->LogMessage(msg.str(), "Config");
				return true;
			}

			std::string cfg = args[2];
			to_upper(cfg);
			if (cfg == "NONE")
			{
				this->LogMessage("No longer using custom runway config", "Config");
				this->customRunwayConfig = "";

				return true;
			}

			auto config = this->runwayConfigs.find(cfg);
			if (config == this->runwayConfigs.end())
			{
				this->LogMessage("Invalid custom runway config specified", "Config");
			}
			else
			{
				this->LogMessage("Switched to custom runway config: " + cfg, "Config");
				this->customRunwayConfig = cfg;
			}

			return true;
		}
		else
		{
			std::string airport = args[1];
			to_upper(airport);
			const bool isLfpg = airport == "LFPG";
			auto airportIt = this->airports.find(airport);
			if (airportIt == this->airports.end())
			{
				return false;
			}

			if (args.size() < 3)
			{
				std::ostringstream msg;
				msg << airport << " departures set to " << (this->IsAirportLinked(airport) ? "linked" : "unlinked");
				if (isLfpg) {
					msg << ", CAS " << (this->lfpgCas ? "enabled" : "disabled")
						<< ", RM " << (this->lfpgRm ? "enabled" : "disabled")
						<< ". Usage: .delhel " << airport << " linked|unlinked|cas|nocas|rm|norm";
				}
				else {
					msg << ". Usage: .delhel " << airport << " linked|unlinked";
				}
				this->LogMessage(msg.str(), "Config");
				return true;
			}

			std::string mode = args[2];
			to_upper(mode);
			if (starts_with(mode, "UNLINKED") || mode == "U")
			{
				this->SetAirportLinked(airport, false);
				this->LogMessage(airport + " departures set to unlinked", "Config");
				this->SaveSettings();
				return true;
			}
			if (starts_with(mode, "LINKED") || mode == "L")
			{
				this->SetAirportLinked(airport, true);
				this->LogMessage(airport + " departures set to linked", "Config");
				this->SaveSettings();
				return true;
			}
			if (isLfpg && starts_with(mode, "CAS"))
			{
				bool enable = true;
				if (args.size() > 3) {
					std::string arg = args[3];
					to_upper(arg);
					if (arg == "OFF" || arg == "0" || arg == "FALSE") {
						enable = false;
					}
				}
				this->lfpgCas = enable;
				if (enable) {
					this->lfpgRm = false;
					this->lfpgRmEnabledAt.reset();
				}
				this->LogMessage(std::string("LFPG CAS mode ") + (enable ? "enabled" : "disabled"), "Config");
				this->SaveSettings();
				return true;
			}
			if (isLfpg && starts_with(mode, "NOCAS"))
			{
				this->lfpgCas = false;
				this->LogMessage("LFPG CAS mode disabled", "Config");
				this->SaveSettings();
				return true;
			}
			if (isLfpg && starts_with(mode, "RM"))
			{
				this->lfpgRm = true;
				this->lfpgRmEnabledAt = std::chrono::steady_clock::now();
				this->lfpgCas = false;
				this->LogMessage("LFPG RM mode enabled", "Config");
				this->SaveSettings();
				return true;
			}
			if (isLfpg && starts_with(mode, "NORM"))
			{
				this->lfpgRm = false;
				this->lfpgRmEnabledAt.reset();
				this->LogMessage("LFPG RM mode disabled", "Config");
				this->SaveSettings();
				return true;
			}

			if (isLfpg) {
				this->LogMessage("Invalid LFPG mode. Use .delhel lfpg linked|unlinked|cas|nocas|rm|norm", "Config");
			}
			else {
				this->LogMessage("Invalid mode. Use .delhel " + airport + " linked|unlinked", "Config");
			}
			return true;
		}
	}

	return false;
}

void CDelHel::OnGetTagItem(EuroScopePlugIn::CFlightPlan FlightPlan, EuroScopePlugIn::CRadarTarget RadarTarget, int ItemCode, int TagData, char sItemString[16], int* pColorCode, COLORREF* pRGB, double* pFontSize)
{
	if (!FlightPlan.IsValid()) {
		return;
	}

	switch (ItemCode) {
	case TAG_ITEM_FP_VALIDATION:
		validation res = this->ProcessFlightPlan(FlightPlan, this->assignNap, true);

		if (res.valid && std::find(this->processed.begin(), this->processed.end(), FlightPlan.GetCallsign()) != this->processed.end()) {
			if (res.tag.empty()) {
				strcpy_s(sItemString, 16, "OK");
			}
			else
			{
				strcpy_s(sItemString, 16, res.tag.c_str());
			}

			*pColorCode = EuroScopePlugIn::TAG_COLOR_RGB_DEFINED;

			if (res.color == TAG_COLOR_NONE) {
				*pRGB = TAG_COLOR_GREEN;
			}
			else {
				*pRGB = res.color;
			}
		}
		else
		{
			strcpy_s(sItemString, 16, res.tag.c_str());

			if (res.color != TAG_COLOR_NONE) {
				*pColorCode = EuroScopePlugIn::TAG_COLOR_RGB_DEFINED;
				*pRGB = res.color;
			}
		}
		
		break;
	}
}

void CDelHel::OnFunctionCall(int FunctionId, const char* sItemString, POINT Pt, RECT Area)
{
	EuroScopePlugIn::CFlightPlan fp = this->FlightPlanSelectASEL();
	if (!fp.IsValid()) {
		return;
	}

	std::string rmSideOverride;
	if (FunctionId == TAG_FUNC_PROCESS_FP || FunctionId == TAG_FUNC_PROCESS_FP_NAP || FunctionId == TAG_FUNC_PROCESS_FP_NON_NAP) {
		rmSideOverride = this->GetRmSideOverride(fp);
	}

	switch (FunctionId) {
	case TAG_FUNC_VALIDATION_MENU:
		this->OpenPopupList(Area, "Validation", 1);
		this->AddPopupListElement("Process FPL (NAP)", NULL, TAG_FUNC_PROCESS_FP_NAP, false, EuroScopePlugIn::POPUP_ELEMENT_NO_CHECKBOX, false, false);
		this->AddPopupListElement("Process FPL (non-NAP)", NULL, TAG_FUNC_PROCESS_FP_NON_NAP, false, EuroScopePlugIn::POPUP_ELEMENT_NO_CHECKBOX, false, false);
		break;
	case TAG_FUNC_PROCESS_FP:
		this->ProcessFlightPlan(fp, this->assignNap, false, rmSideOverride);
		break;
	case TAG_FUNC_PROCESS_FP_NAP:
		this->ProcessFlightPlan(fp, true, false, rmSideOverride);
		break;
	case TAG_FUNC_PROCESS_FP_NON_NAP:
		this->ProcessFlightPlan(fp, false, false, rmSideOverride);
	}
}

void CDelHel::OnTimer(int Counter)
{
	if (this->updateCheck && this->latestVersion.valid() && this->latestVersion.wait_for(0ms) == std::future_status::ready) {
		this->CheckForUpdate();
	}
	if (this->autoProcess && Counter % 5 == 0) {
		this->AutoProcessFlightPlans();
	}
}

void CDelHel::OnFlightPlanDisconnect(EuroScopePlugIn::CFlightPlan FlightPlan)
{
	this->processed.erase(std::remove(this->processed.begin(), this->processed.end(), FlightPlan.GetCallsign()), this->processed.end());
}

void CDelHel::OnAirportRunwayActivityChanged()
{
	this->UpdateActiveAirports();
}

EuroScopePlugIn::CRadarScreen* CDelHel::OnRadarScreenCreated(const char* sDisplayName, bool NeedRadarContent, bool GeoReferenced, bool CanBeSaved, bool CanBeCreated)
{
	this->radarScreen = new RadarScreen();
	return this->radarScreen;
}

void CDelHel::LoadSettings()
{
	const char* settings = this->GetDataFromSettings(PLUGIN_NAME);
	if (settings) {
		std::vector<std::string> splitSettings = split(settings, SETTINGS_DELIMITER);

		if (splitSettings.size() < 8) {
			this->LogMessage("Invalid saved settings found, reverting to default.");

			this->SaveSettings();

			return;
		}

		std::istringstream(splitSettings[0]) >> this->debug;
		std::istringstream(splitSettings[1]) >> this->updateCheck;
		std::istringstream(splitSettings[2]) >> this->assignNap;
		std::istringstream(splitSettings[3]) >> this->warnRFLBelowCFL;
		std::istringstream(splitSettings[4]) >> this->logMinMaxRFL;
		std::istringstream(splitSettings[5]) >> this->checkMinMaxRFL;
		std::istringstream(splitSettings[6]) >> this->flashOnMessage;
		std::istringstream(splitSettings[7]) >> this->preferTopSkySquawkAssignment;
		this->unlinkedDepartureAirports.clear();
		if (splitSettings.size() > 8) {
			std::istringstream(splitSettings[8]) >> this->lfpgLinked;
		}
		this->SetAirportLinked("LFPG", this->lfpgLinked);
		if (splitSettings.size() > 9) {
			std::istringstream(splitSettings[9]) >> this->lfpgCas;
		}
		if (splitSettings.size() > 10) {
			std::istringstream(splitSettings[10]) >> this->lfpgRm;
		}
		if (splitSettings.size() > 11) {
			this->DeserializeUnlinkedDepartureAirports(splitSettings[11]);
		}

		this->LogDebugMessage("Successfully loaded settings.");
	}
	else {
		this->LogMessage("No saved settings found, using defaults.");
	}
}

void CDelHel::SaveSettings()
{
	this->lfpgLinked = this->IsAirportLinked("LFPG");
	const std::string unlinkedAirports = this->SerializeUnlinkedDepartureAirports();

	std::ostringstream ss;
	ss << this->debug << SETTINGS_DELIMITER
		<< this->updateCheck << SETTINGS_DELIMITER
		<< this->assignNap << SETTINGS_DELIMITER
		<< this->warnRFLBelowCFL << SETTINGS_DELIMITER
		<< this->logMinMaxRFL << SETTINGS_DELIMITER
		<< this->checkMinMaxRFL << SETTINGS_DELIMITER
		<< this->flashOnMessage << SETTINGS_DELIMITER
		<< this->preferTopSkySquawkAssignment << SETTINGS_DELIMITER
		<< this->lfpgLinked << SETTINGS_DELIMITER
		<< this->lfpgCas << SETTINGS_DELIMITER
		<< this->lfpgRm;
	if (!unlinkedAirports.empty()) {
		ss << SETTINGS_DELIMITER << unlinkedAirports;
	}

	this->SaveDataToSettings(PLUGIN_NAME, "DelHel settings", ss.str().c_str());
}

void CDelHel::ReadCustomConfigs()
{
	json j;

	try {
		std::filesystem::path base2(GetPluginDirectory());
		base2.append("customconfigs.json");

		// Custom runway config file is optional, skip reading if it doesn't exist
		if (!std::filesystem::exists(base2))
		{
			this->LogDebugMessage("No custom runway config file found, skipping loading.", "Config");
			return;
		}

		std::ifstream ifs(base2.c_str());

		j = json::parse(ifs);
	}
	catch (std::exception e)
	{
		this->LogMessage("Failed to read custom runway configs json. Error: " + std::string(e.what()), "Config");
		return;
	}

	for (auto& [configName, jconfig] : j.items())
	{
		std::string configUpper = configName;
		to_upper(configUpper);
		std::string def = jconfig.value<std::string>("def", "");
		if (def == "")
		{
			this->LogMessage("Missing default runway for \"" + configUpper + "\".", "Config");
			continue;
		}
		rwy_config c{
			def
		};

		json sids;
		try
		{
			sids = jconfig.at("sids");
		}
		catch (std::exception e)
		{
			this->LogMessage("Failed to get SIDs for runway config \"" + configUpper + "\". Error: " + std::string(e.what()), "Config");
			continue;
		}

		for (auto& [wp, jwp] : sids.items())
		{
			rwy_config_sid cs{
				wp
			};

			json rwys;
			try
			{
				rwys = jwp.at("rwys");
			}
			catch (std::exception e)
			{
				this->LogMessage("Failed to get RWYs for WP \"" + wp + "\" in \"" + configUpper + "\". Error: " + std::string(e.what()), "Config");
				continue;
			}

			for (auto& [rwy, jrwy] : rwys.items())
			{
				int prio = jrwy.value<int>("prio", 0);
				cs.rwyPrio.emplace(rwy, prio);
			}

			c.sids.emplace(wp, cs);
		}

		this->runwayConfigs.emplace(configUpper, c);
	}

	this->LogDebugMessage("Loaded " + std::to_string(this->runwayConfigs.size()) + " custom runway config(s).", "Config");
}

void CDelHel::ReadRoutingConfig()
{
	json j;

	try {
		std::filesystem::path base2(GetPluginDirectory());
		base2.append("routing.json");

		std::ifstream ifs(base2.c_str());

		j = json::parse(ifs);
	}
	catch (std::exception e)
	{
		this->LogMessage("Failed to read routing config. Error: " + std::string(e.what()), "Config");
		return;
	}

	for (auto itair = this->airports.begin(); itair != this->airports.end(); itair++) {

		for (auto& obj : j.items()) {		//iterator on outer json object -> key = departure icao
			if (obj.key() == itair->second.icao) {	//if departure icao has already been read in by AirportConfig

				try {
					for (auto& el : obj.value()["entry"].items()) {	//iterator on routes items

						for (auto& in_el : el.value()["routes"].items()) {

							routing ro{
								obj.key(),
								in_el.value()["icao"],
								in_el.value()["maxlvl"],
								in_el.value()["minlvl"],
								{}
							};

							ro.waypts.push_back(el.value()["name"]);	// add entry-point = SID exit as first waypoint of route

							for (auto& inner_el : in_el.value()["waypoints"].items()) {	// add route waypoints
								ro.waypts.push_back(inner_el.value());
							}

							itair->second.validroutes.push_back(ro);	//add routing to airports

							std::string check = "ADEP:" + ro.adep + "-ADEST:" + ro.adest + "-MAX:" + std::to_string(ro.maxlvl) + "-MIN:" + std::to_string(ro.minlvl) + "-#wpts:" + std::to_string(ro.waypts.size());
							this->LogDebugMessage("New Routing added: " + check, "Config");
						}
					}
				}
				catch (std::exception e) {
					this->LogMessage("Failed to read routing config for " + itair->second.icao + "| Error: " + std::string(e.what()), "Config");
					return;
				}
				this->LogDebugMessage("Routing for departure Airport " + itair->second.icao + " has been added.", "Config");
			}

		}
	}
}

void CDelHel::ReadAirportConfig()
{
	json j;
	try {
		std::filesystem::path base(GetPluginDirectory());
		base.append("airports.json");

		std::ifstream ifs(base.c_str());

		j = json::parse(ifs);
	}
	catch (std::exception e)
	{
		this->LogMessage("Failed to read airport config. Error: " + std::string(e.what()), "Config");
		return;
	}

	this->natArrivalIcaoPrefixes = { 'B', 'C', 'K', 'M', 'S', 'T' };
	this->arvlArrivalAirports.clear();
	if (j.contains("_config")) {
		try {
			const auto& jcfg = j.at("_config");
			if (jcfg.contains("nat_arrival_icao_prefixes")) {
				std::set<char> prefixes;
				for (const auto& jprefix : jcfg.at("nat_arrival_icao_prefixes")) {
					std::string prefix = trim(jprefix.get<std::string>(), " \t\r\n");
					to_upper(prefix);
					if (!prefix.empty()) {
						prefixes.emplace(prefix[0]);
					}
				}

				if (!prefixes.empty()) {
					this->natArrivalIcaoPrefixes = prefixes;
				}
			}

			if (jcfg.contains("arvl_arrival_airports")) {
				std::set<std::string> airports;
				for (const auto& jairport : jcfg.at("arvl_arrival_airports")) {
					std::string airportIcao = trim(jairport.get<std::string>(), " \t\r\n");
					to_upper(airportIcao);
					if (!airportIcao.empty()) {
						airports.emplace(airportIcao);
					}
				}

				this->arvlArrivalAirports = airports;
			}
		}
		catch (std::exception e)
		{
			this->LogMessage("Failed to read arrival warning config from airport config. Error: " + std::string(e.what()), "Config");
		}
	}

	for (auto& [icao, jap] : j.items()) {
		if (!icao.empty() && icao[0] == '_') {
			continue;
		}

		airport ap{
			icao, // icao
			jap.value<int>("elevation", 0), // elevation
			false, // active
		};

		if (jap.contains("runways")) {
			try {
				for (auto& [rwy, jrwy] : jap.at("runways").items()) {
					std::string rwyName = rwy;
					to_upper(rwyName);
					std::string side = jrwy.value<std::string>("north_south", "");
					to_upper(side);
					if (!side.empty()) {
						ap.rwy_sides.emplace(rwyName, side);
					}
				}
			}
			catch (std::exception e)
			{
				this->LogMessage("Failed to read runway sides for airport \"" + icao + "\". Error: " + std::string(e.what()), "Config");
			}
		}

		if (jap.contains("loa_max")) {
			try {
				for (auto& [dest, jmax] : jap.at("loa_max").items()) {
					std::string destUpper = dest;
					to_upper(destUpper);
					ap.loa_max_fl[destUpper] = jmax.get<int>();
				}
			}
			catch (std::exception e)
			{
				this->LogMessage("Failed to read LOA max FL for airport \"" + icao + "\". Error: " + std::string(e.what()), "Config");
			}
		}

		json jss;
		try {
			jss = jap.at("sids");
		}
		catch (std::exception e)
		{
			this->LogMessage("Failed to get SIDs for airport \"" + icao + "\". Error: " + std::string(e.what()), "Config");
			continue;
		}

		for (auto& [wp, js] : jss.items()) {
			std::string sidSide = js.value<std::string>("north_south", "");
			to_upper(sidSide);
			sid::rfl_constraint rfl{};
			if (js.contains("RFL")) {
				try {
					const auto& jrfl = js.at("RFL");
					rfl.min_fl = jrfl.value<int>("min", 0);
					rfl.max_fl = jrfl.value<int>("max", 0);
					rfl.jet_max_fl = jrfl.value<int>("jet_max", 0);
					rfl.prop_max_fl = jrfl.value<int>("prop_max", 0);
				}
				catch (std::exception e)
				{
					this->LogMessage("Failed to read RFL constraints for SID \"" + wp + "\" at airport \"" + icao + "\". Error: " + std::string(e.what()), "Config");
				}
			}
			sid s{
				wp, // wp
				js.value<int>("cfl", 0), // cfl
				sidSide, // north_south
				rfl // rfl
			};

			json jrwys;
			try {
				jrwys = js.at("rwys");
			}
			catch (std::exception e)
			{
				this->LogMessage("Failed to get RWYs for SID \"" + wp + "\" for airport \"" + icao + "\". Error: " + std::string(e.what()), "Config");
				continue;
			}

			std::ostringstream rrs;
			rrs << icao << "\\/(";
			for (auto it = jrwys.items().begin(); it != jrwys.items().end(); ++it) {
				std::string dep = it.value().value<std::string>("dep", "");
				std::string jetDep = it.value().value<std::string>("jet_dep", "");
				std::string propDep = it.value().value<std::string>("prop_dep", "");
				std::string unlinkJetDep = it.value().value<std::string>("unlink_jet_dep", "");
				std::string unlinkPropDep = it.value().value<std::string>("unlink_prop_dep", "");
				std::string nap = it.value().value<std::string>("nap", "");
				int rwyCfl = it.value().value<int>("cfl", s.cfl);
				int rwyCflProp = it.value().value<int>("cfl_prop", rwyCfl);
				int prio = it.value().value<int>("prio", 0);

				if (jetDep.empty() && !dep.empty()) {
					jetDep = dep;
				}
				if (propDep.empty() && !dep.empty()) {
					propDep = dep;
				}
				if (jetDep.empty() && !propDep.empty()) {
					jetDep = propDep;
				}
				if (propDep.empty() && !jetDep.empty()) {
					propDep = jetDep;
				}
				if (unlinkJetDep.empty() && !unlinkPropDep.empty()) {
					unlinkJetDep = unlinkPropDep;
				}
				if (unlinkPropDep.empty() && !unlinkJetDep.empty()) {
					unlinkPropDep = unlinkJetDep;
				}

				sidinfo si{
					it.key(), // rwy
					jetDep, // jet_dep
					propDep, // prop_dep
					unlinkJetDep, // unlink_jet_dep
					unlinkPropDep, // unlink_prop_dep
					nap, // nap
					rwyCfl, // cfl
					rwyCflProp, // cfl_prop
					prio // prio
				};

				s.rwys.emplace(si.rwy, si);
				ap.rwys.emplace(si.rwy, false);

				rrs << si.rwy;
				if (std::next(it) != jrwys.items().end()) {
					rrs << '|';
				}
			}
			rrs << ')';

			ap.rwy_regex = std::regex(rrs.str(), std::regex_constants::ECMAScript);

			ap.sids.emplace(wp, s);
		}

		this->airports.emplace(icao, ap);
	}

	this->UpdateActiveAirports();
}

void CDelHel::UpdateActiveAirports()
{
	this->SelectActiveSectorfile();
	for (auto sfe = this->SectorFileElementSelectFirst(EuroScopePlugIn::SECTOR_ELEMENT_RUNWAY); sfe.IsValid(); sfe = this->SectorFileElementSelectNext(sfe, EuroScopePlugIn::SECTOR_ELEMENT_RUNWAY)) {
		std::string ap = trim(sfe.GetAirportName());
		to_upper(ap);

		auto ait = this->airports.find(ap);
		if (ait == this->airports.end()) {
			continue;
		}

		std::string rwy = trim(sfe.GetRunwayName(0));
		to_upper(rwy);

		auto rit = ait->second.rwys.find(rwy);
		if (rit != ait->second.rwys.end()) {
			rit->second = sfe.IsElementActive(true, 0);
		}

		rwy = trim(sfe.GetRunwayName(1));
		to_upper(rwy);

		rit = ait->second.rwys.find(rwy);
		if (rit != ait->second.rwys.end()) {
			rit->second = sfe.IsElementActive(true, 1);
		}

		if (!ait->second.active) {
			ait->second.active = sfe.IsElementActive(true, 0) || sfe.IsElementActive(true, 1);
		}
	}
}


validation CDelHel::ProcessFlightPlan(EuroScopePlugIn::CFlightPlan& fp, bool nap, bool validateOnly, const std::string& sideOverride)
{
	validation res{
		true, // valid
		"", // tag
		TAG_COLOR_NONE // color
	};
	std::string cs = fp.GetCallsign();

	if (!validateOnly) {
		if (nap) {
			this->LogDebugMessage("Processing flightplan using noise abatement procedures", cs);
		}
		else {
			this->LogDebugMessage("Processing flightplan", cs);
		}
	}

	EuroScopePlugIn::CFlightPlanData fpd = fp.GetFlightPlanData();

	std::string dep = fpd.GetOrigin();
	to_upper(dep);

	std::string arr = fpd.GetDestination();
	to_upper(arr);
	const bool natArrival = this->IsNatArrivalAirport(arr);
	const bool arvlArrival = this->IsArvlArrivalAirport(arr);
	auto applyArrivalWarnings = [&]() {
		if (!validateOnly || !res.valid) {
			return;
		}

		if (arvlArrival && (res.tag.empty() || res.tag == "RWY" || res.tag == "VFR" || res.tag == "NAT")) {
			res.tag = "ARVL";
			res.color = TAG_COLOR_YELLOW;
			return;
		}

		if (natArrival && (res.tag.empty() || res.tag == "RWY" || res.tag == "VFR")) {
			res.tag = "NAT";
			res.color = TAG_COLOR_YELLOW;
		}
	};

	auto ait = this->airports.find(dep);
	if (ait == this->airports.end()) {
		if (!validateOnly) {
			this->LogDebugMessage("Failed to process flightplan, did not find departure airport \"" + dep + "\" in airport config", cs);
		}

		res.valid = false;
		res.tag = "ADEP";
		res.color = TAG_COLOR_RED;

		return res;
	}

	airport ap = ait->second;
	EuroScopePlugIn::CFlightPlanControllerAssignedData cad = fp.GetControllerAssignedData();

	if (strcmp(fpd.GetPlanType(), "V") == 0 || strcmp(fpd.GetPlanType(), "Z") == 0) {
		if (!validateOnly) {
			if (!cad.SetClearedAltitude(round_to_closest(ap.elevation + VFR_TRAFFIC_PATTERN_ALTITUDE, 500))) {
				this->LogMessage("Failed to process VFR flightplan, cannot set cleared flightlevel", cs);
				return res;
			}

			if (this->radarScreen != nullptr && this->ccamsAvailable && !this->preferTopSkySquawkAssignment) {
				this->radarScreen->StartTagFunction(cs.c_str(), nullptr, 0, cs.c_str(), CCAMS_PLUGIN_NAME, CCAMS_TAG_FUNC_ASSIGN_SQUAWK_VFR, POINT(), RECT());
				this->LogDebugMessage("Triggered automatic VFR squawk assignment via CCAMS", cs);
			}
			else {
				// TopSky doesn't have a dedicated VFR squawk assignment function. Force hardcoded VFR squawk assignment
				if (!cad.SetSquawk(VFR_SQUAWK)) {
					this->LogDebugMessage("Failed to set VFR squawk", cs);
				}
			}

			this->LogDebugMessage("Skipping processing of VFR flightplan route", cs);

			// Add to list of processed flightplans if not added by auto-processing already
			this->IsFlightPlanProcessed(fp);
		}

		res.tag = "VFR";
		applyArrivalWarnings();

		return res;
	}

	const char engineType = fpd.GetEngineType();
	const bool isPropEngine = (engineType == 'P' || engineType == 'T' || engineType == 'E');
	const bool useLinked = this->IsAirportLinked(dep);

	std::vector<std::string> route = split(fpd.GetRoute());
	const std::vector<std::string> originalRoute = route;
	sid sid;

	auto rit = route.begin();
	while (rit != route.end()) {
		if (std::regex_search(*rit, ap.rwy_regex)) {
			++rit;
			res.tag = "RWY";
			continue;
		}
		
		std::map<std::string, ::sid>::iterator sit;
		std::smatch m;
		if (std::regex_search(*rit, m, REGEX_SPEED_LEVEL_GROUP)) {
			// Try to match waypoint of speed/level group in case SID fix already has one assigned
			sit = ap.sids.find(m[1]);
		}
		else {
			// If no other matchers above yield a result, try to match full route part
			sit = ap.sids.find(*rit);
		}

		if (sit != ap.sids.end()) {
			sid = sit->second;
			break;
		}

		rit = route.erase(rit);
	}

	if (sid.wp == "" || route.size() == 0) {
		if (!validateOnly) {
			this->LogMessage("Invalid flightplan, no valid SID waypoint found in route", cs);
		}

		res.valid = false;
		res.tag = "SID";
		res.color = TAG_COLOR_RED;

		return res;
	}

	if (validateOnly) {
		if (!ap.loa_max_fl.empty()) {
			auto itLoa = ap.loa_max_fl.find(arr);
			if (itLoa != ap.loa_max_fl.end()) {
				int rfl = fpd.GetFinalAltitude();
				if (rfl < 3) {
					rfl = fp.GetFinalAltitude();
				}
				if (rfl > itLoa->second * 100) {
					res.valid = false;
					res.tag = "LOA";
					res.color = TAG_COLOR_RED;

					return res;
				}
			}
		}

		{
			const auto& rc = sid.rfl;
			const bool hasConstraint = rc.min_fl > 0 || rc.max_fl > 0 || rc.jet_max_fl > 0 || rc.prop_max_fl > 0;
			if (hasConstraint) {
				int rfl = fpd.GetFinalAltitude();
				if (rfl < 3) {
					rfl = fp.GetFinalAltitude();
				}
				const int rflFl = rfl / 100;
				bool violated = false;

				if (rc.min_fl > 0 && rflFl < rc.min_fl) {
					violated = true;
				}
				if (rc.max_fl > 0 && rflFl > rc.max_fl) {
					violated = true;
				}
				if (isPropEngine && rc.prop_max_fl > 0 && rflFl > rc.prop_max_fl) {
					violated = true;
				}
				if (!isPropEngine && rc.jet_max_fl > 0 && rflFl > rc.jet_max_fl) {
					violated = true;
				}

				if (violated) {
					res.valid = false;
					res.tag = "RFL";
					res.color = TAG_COLOR_RED;

					return res;
				}
			}
		}

		auto getExpectedSidCfl = [&]() -> int {
			auto cflFromSidInfo = [&](const sidinfo& info) -> int {
				int sidCfl = isPropEngine ? info.cfl_prop : info.cfl;
				if (sidCfl == 0) {
					sidCfl = sid.cfl;
				}

				return sidCfl;
			};

			std::string assignedRwy = trim(fpd.GetDepartureRwy(), " \t\r\n");
			to_upper(assignedRwy);

			auto assignedSidIt = sid.rwys.find(assignedRwy);
			if (assignedSidIt != sid.rwys.end()) {
				return cflFromSidInfo(assignedSidIt->second);
			}

			for (const auto& routePartRaw : originalRoute) {
				size_t slashPos = routePartRaw.find('/');
				if (slashPos == std::string::npos || slashPos + 1 >= routePartRaw.size()) {
					continue;
				}

				std::string routeRwy = trim(routePartRaw.substr(slashPos + 1), " \t\r\n");
				to_upper(routeRwy);
				assignedSidIt = sid.rwys.find(routeRwy);
				if (assignedSidIt != sid.rwys.end()) {
					return cflFromSidInfo(assignedSidIt->second);
				}
			}

			std::optional<int> uniqueCfl;
			for (const auto& rwySidEntry : sid.rwys) {
				int sidCfl = cflFromSidInfo(rwySidEntry.second);
				if (!uniqueCfl.has_value()) {
					uniqueCfl = sidCfl;
				}
				else if (*uniqueCfl != sidCfl) {
					uniqueCfl.reset();
					break;
				}
			}

			if (uniqueCfl.has_value()) {
				return *uniqueCfl;
			}

			return sid.cfl;
		};

		const int expectedSidCfl = getExpectedSidCfl();

		if (this->warnRFLBelowCFL && fp.GetFinalAltitude() < expectedSidCfl) {
			res.tag = "RFL";
			res.color = TAG_COLOR_ORANGE;

			return res;
		}

		int cfl = cad.GetClearedAltitude();
		// If CFL == RFL, EuroScope returns a CFL of 0 and the RFL value should be consulted. Additionally, CFL 1 and 2 indicate ILS and visual approach clearances respectively.
		if (cfl < 3) {
			// If the RFL is not adapted or confirmed by the controller, cad.GetFinalAltitude() will also return 0. As a last source of CFL info, we need to consider the filed RFL.
			cfl = cad.GetFinalAltitude();
			if (cfl < 3) {
				cfl = fp.GetFinalAltitude();
			}
		}

		// Display a warning if the CFL does not match the initial CFL assigned to the SID. No warning is shown if the RFL is below the CFL for the SID as pilots might request a lower initial climb.
		if (cfl != expectedSidCfl && (cfl != fp.GetFinalAltitude() || fp.GetFinalAltitude() >= expectedSidCfl)) {
			res.valid = false;
			res.tag = "CFL";

			return res;
		}
	}
	else {
		if (!fpd.SetRoute(join(route).c_str())) {
			this->LogMessage("Failed to process flightplan, cannot set cleaned route", cs);
			return res;
		}

		if (!fpd.AmendFlightPlan()) {
			this->LogMessage("Failed to process flightplan, cannot amend flightplan after setting cleaned route", cs);
			return res;
		}

		std::string targetSide = sideOverride;
		if (!targetSide.empty()) {
			to_upper(targetSide);
		}
		if (targetSide.empty() && dep == "LFPG" && this->lfpgCas && !sid.north_south.empty()) {
			targetSide = sid.north_south;
		}
		const bool useCas = (dep == "LFPG") && !targetSide.empty() && !ap.rwy_sides.empty();

		std::map<std::string, sidinfo>::iterator assignedSidIt{};
		std::string assignedRwy = fpd.GetDepartureRwy();
		to_upper(assignedRwy);
		const bool hasCustomRwyConfig = !this->customRunwayConfig.empty();
		bool casNeedsOverride = false;

		if (useCas) {
			if (assignedRwy.empty()) {
				casNeedsOverride = true;
			}
			else {
				auto sideIt = ap.rwy_sides.find(assignedRwy);
				auto activeIt = ap.rwys.find(assignedRwy);
				if (sideIt == ap.rwy_sides.end() || sideIt->second != targetSide || activeIt == ap.rwys.end() || !activeIt->second) {
					casNeedsOverride = true;
					assignedRwy.clear();
				}
			}
		}

		if (assignedRwy.empty() || hasCustomRwyConfig || casNeedsOverride) {
			this->LogDebugMessage("No runway assigned, or override active, attempting to pick first active runway for SID", cs);

			// SIDs can have a priority assigned per runway, allowing for "hierarchy" depending on runway config (as currently possible in ES sectorfiles).
			// If no priority is assigned, the default of 0 will be used and the first active runway will be picked.
			int prio = -1;
			for (const auto& [rwy, isRwyActive] : ap.rwys) {
				if (!isRwyActive)
					continue; // Runway is not active
				this->LogDebugMessage("Checking active runway " + rwy, cs);

				if (useCas) {
					auto sideIt = ap.rwy_sides.find(rwy);
					if (sideIt == ap.rwy_sides.end() || sideIt->second != targetSide) {
						continue; // Runway is on wrong side for CAS
					}
				}

				auto sidIt = sid.rwys.find(rwy);
				if (sidIt == sid.rwys.end())
					continue; // No SID entry for the given runway

				if (hasCustomRwyConfig) {
					this->LogDebugMessage("Custom config " + this->customRunwayConfig + " active, check for custom config of SID wp: " + sid.wp, cs);
					const auto& rwyConfig = this->runwayConfigs[this->customRunwayConfig];

					if (auto customSidIt = rwyConfig.sids.find(sid.wp); customSidIt != rwyConfig.sids.end()) {
						this->LogDebugMessage("Found SID wp " + customSidIt->first, cs);

						const auto& rwyPrioMap = customSidIt->second.rwyPrio;
						if (auto customRwyIt = rwyPrioMap.find(rwy); customRwyIt != rwyPrioMap.end()) {
							this->LogDebugMessage("Found SID rwy " + customRwyIt->first, cs);

							if (customRwyIt->second > prio) {
								this->LogDebugMessage("Found and applied custom RWY priority override for config " + this->customRunwayConfig, cs);
								assignedRwy = rwy;
								prio = customRwyIt->second;
							}
						}
					}

					// Default runway fallback for the custom config
					if (rwyConfig.def == rwy && 1 > prio) {
						this->LogDebugMessage("Found and applied custom default-RWY priority override for config " + this->customRunwayConfig, cs);
						assignedRwy = rwy;
						prio = 1;
					}
				}
				else if (assignedRwy.empty() && sidIt->second.prio > prio) {
					assignedRwy = rwy;
					assignedSidIt = sidIt;
					prio = assignedSidIt->second.prio;
				}
			}

			if (assignedRwy.empty()) {
				if (useCas) {
					this->LogMessage("Failed to process flightplan, no active runway for required LFPG north/south side", cs);
				}
				else {
					this->LogMessage("Failed to process flightplan, no runway assigned", cs);
				}

				res.valid = false;
				res.tag = "RWY";
				res.color = TAG_COLOR_RED;

				return res;
			}

			// TODO display warning once "valid" tag override below is fixed
			/*res.tag = "SID";
			res.color = TAG_COLOR_GREEN;*/
		}

		assignedSidIt = sid.rwys.find(assignedRwy);

		if (assignedSidIt == sid.rwys.end()) {
			this->LogMessage("Invalid flightplan, no matching SID found for runway", cs);

			res.valid = false;
			res.tag = "SID";
			res.color = TAG_COLOR_RED;

			return res;
		}

		sidinfo sidinfo = assignedSidIt->second;
		auto pickByEngine = [&](const std::string& jet, const std::string& prop) -> std::string {
			if (isPropEngine) {
				if (!prop.empty()) {
					return prop;
				}
				if (!jet.empty()) {
					return jet;
				}
			}
			else {
				if (!jet.empty()) {
					return jet;
				}
				if (!prop.empty()) {
					return prop;
				}
			}
			return "";
		};

		const bool usedNap = nap && !sidinfo.nap.empty();
		std::string selectedSid;
		bool usedLinked = useLinked;
		if (usedNap) {
			selectedSid = sidinfo.nap;
		}
		else if (useLinked) {
			selectedSid = pickByEngine(sidinfo.jet_dep, sidinfo.prop_dep);
		}
		else {
			selectedSid = pickByEngine(sidinfo.unlink_jet_dep, sidinfo.unlink_prop_dep);
			if (selectedSid.empty()) {
				selectedSid = pickByEngine(sidinfo.jet_dep, sidinfo.prop_dep);
				usedLinked = true;
			}
		}

		if (selectedSid.empty()) {
			this->LogMessage("Invalid flightplan, no matching SID found for runway", cs);

			res.valid = false;
			res.tag = "SID";
			res.color = TAG_COLOR_RED;

			return res;
		}

		const char* engineLabel = isPropEngine ? "prop" : "jet";
		const char* linkLabel = usedNap ? "nap" : (usedLinked ? "linked" : "unlinked");
		this->LogDebugMessage(std::string("--> Assigned sid/rwy: ") + selectedSid + "/" + assignedRwy + " (" + linkLabel + ", " + engineLabel + ")", cs);

		std::ostringstream sssid;
		sssid << selectedSid << "/" << assignedRwy;


		route.insert(route.begin(), sssid.str());

		if (!fpd.SetRoute(join(route).c_str())) {
			this->LogMessage("Failed to process flightplan, cannot set route including SID", cs);
			return res;
		}

		if (!fpd.AmendFlightPlan()) {
			this->LogMessage("Failed to process flightplan, cannot amend flightplan after setting route including SID", cs);
			return res;
		}

		int sidCfl = isPropEngine ? sidinfo.cfl_prop : sidinfo.cfl;
		if (sidCfl == 0) {
			sidCfl = sid.cfl;
		}

		int cfl = sidCfl;
		if (fp.GetFinalAltitude() < sidCfl) {
			this->LogDebugMessage("Flightplan has RFL below initial CFL for SID, setting RFL", cs);

			cfl = fp.GetFinalAltitude();
		}

		if (!cad.SetClearedAltitude(cfl)) {
			this->LogMessage("Failed to process flightplan, cannot set cleared flightlevel", cs);
			return res;
		}

		std::string assignedSquawk = cad.GetSquawk();
		if (assignedSquawk.empty() || assignedSquawk == "2000") {
			if (this->radarScreen == nullptr) {
				this->LogDebugMessage("Radar screen not initialised, cannot trigger automatic squawk assignment via TopSky or CCAMS", cs);
			}
			else {
				if (this->preferTopSkySquawkAssignment && this->topSkyAvailable) {
					this->radarScreen->StartTagFunction(cs.c_str(), nullptr, 0, cs.c_str(), TOPSKY_PLUGIN_NAME, TOPSKY_TAG_FUNC_ASSIGN_SQUAWK, POINT(), RECT());
					this->LogDebugMessage("Triggered automatic squawk assignment via TopSky", cs);
				}
				else if (this->ccamsAvailable) {
					this->radarScreen->StartTagFunction(cs.c_str(), nullptr, 0, cs.c_str(), CCAMS_PLUGIN_NAME, CCAMS_TAG_FUNC_ASSIGN_SQUAWK_AUTO, POINT(), RECT());
					this->LogDebugMessage("Triggered automatic squawk assignment via CCAMS", cs);
				}
				else if (this->topSkyAvailable) {
					this->radarScreen->StartTagFunction(cs.c_str(), nullptr, 0, cs.c_str(), TOPSKY_PLUGIN_NAME, TOPSKY_TAG_FUNC_ASSIGN_SQUAWK, POINT(), RECT());
					this->LogDebugMessage("Triggered automatic squawk assignment via TopSky", cs);
				}
				else {
					this->LogDebugMessage("Neither TopSky nor CCAMS are loaded, cannot trigger automatic squawk assignment", cs);
				}
			}
		}

		this->LogDebugMessage("Successfully processed flightplan", cs);

		// Add to list of processed flightplans if not added by auto-processing already
		this->IsFlightPlanProcessed(fp);
	}


	if (ap.validroutes.size() != 0) {

		flightplan fpl = flightplan(fp.GetCallsign(), fp.GetExtractedRoute(), fpd.GetRoute()); // create fp for route validation

		bool routecheck = false;
		int count = 0;
		for (auto vait = ap.validroutes.begin(); vait != ap.validroutes.end(); ++vait) {

			routecheck = false;
			auto selsidit = vait->waypts.begin();

			if (*selsidit == sid.wp) {
				if (vait->waypts.size() > 1) {
					try {

						
						count = 0; //counter to disregard previous found waypoints in fpl
						for (auto wyprouit = vait->waypts.begin(); wyprouit != vait->waypts.end(); ++wyprouit) {
							for (auto wypfpl = fpl.route.begin() + count; wypfpl != fpl.route.end(); ++wypfpl) {

								if (wypfpl->airway && wypfpl->name.rfind(*wyprouit) == 0) { // check if waypoint name is part of the airway (e.g. SID)
									
									routecheck = true;
									++count;
									break;
								}
								if (*wyprouit == wypfpl->name) {
									routecheck = true;
									++count;
									break;
								}
								else {
									routecheck = false;
								}
								++count;
							}
							if (!routecheck) {
								break;
							}

						}
					}
					catch (std::exception e) {
						this->LogDebugMessage("Error, No Routing", cs);
					}
				}
				else {
					routecheck = true;
				}
				if (routecheck && vait->adest == arr) { //check specified destinations like LOWI, LOWS, etc.

					if (this->checkMinMaxRFL && ((cad.GetFinalAltitude() == 0 && fpd.GetFinalAltitude() > vait->maxlvl * 100) || cad.GetFinalAltitude() > vait->maxlvl * 100)) {

						res.valid = false;
						res.tag = "MAX";
						res.color = TAG_COLOR_ORANGE;

						if (!validateOnly) {
							std::ostringstream msg;
							msg << "Flights from " << dep << " to " << arr << " via " << sid.wp << " have a maximum FL of " << vait->maxlvl;

							if (this->logMinMaxRFL) {
								this->LogMessage(msg.str(), cs);
							}
							else {
								this->LogDebugMessage(msg.str(), cs);
							}
						}

						return res;
					}
					if (this->checkMinMaxRFL && ((cad.GetFinalAltitude() == 0 && fpd.GetFinalAltitude() < vait->minlvl * 100) || (cad.GetFinalAltitude() != 0 && cad.GetFinalAltitude() < vait->minlvl * 100))) {

						res.valid = false;
						res.tag = "MIN";
						res.color = TAG_COLOR_ORANGE;

						if (!validateOnly) {
							std::ostringstream msg;
							msg << "Flights from " << dep << " to " << arr << " via " << sid.wp << " have a minimum FL of " << vait->minlvl;

							if (this->logMinMaxRFL) {
								this->LogMessage(msg.str(), cs);
							}
							else {
								this->LogDebugMessage(msg.str(), cs);
							}
						}

						return res;
					}

					//case all correct
					res.valid = true;
					res.tag = "";
					res.color = TAG_COLOR_NONE;
					applyArrivalWarnings();

					return res;

				}
				else if (routecheck && vait->adest != arr && vait->adest == "") { // check for non specified destinations
					if (this->checkMinMaxRFL && ((cad.GetFinalAltitude() == 0 && fpd.GetFinalAltitude() > vait->maxlvl * 100) || cad.GetFinalAltitude() > vait->maxlvl * 100)) {

						res.valid = false;
						res.tag = "MAX";
						res.color = TAG_COLOR_ORANGE;

						if (!validateOnly) {
							std::ostringstream msg;
							msg << "Flights from " << dep << " via " << sid.wp << " have a maximum FL of " << vait->maxlvl;

							if (this->logMinMaxRFL) {
								this->LogMessage(msg.str(), cs);
							}
							else {
								this->LogDebugMessage(msg.str(), cs);
							}
						}

						break;
					}
					if (this->checkMinMaxRFL && ((cad.GetFinalAltitude() == 0 && fpd.GetFinalAltitude() < vait->minlvl * 100) || (cad.GetFinalAltitude() != 0 && cad.GetFinalAltitude() < vait->minlvl * 100))) {

						res.valid = false;
						res.tag = "MIN";
						res.color = TAG_COLOR_ORANGE;

						if (!validateOnly) {
							std::ostringstream msg;
							msg << "Flights from " << dep << " via " << sid.wp << " have a minimum FL of " << vait->minlvl;

							if (this->logMinMaxRFL) {
								this->LogMessage(msg.str(), cs);
							}
							else {
								this->LogDebugMessage(msg.str(), cs);
							}
						}

						break;
					}

					//case all correct
					res.valid = true;
					res.tag = "";
					res.color = TAG_COLOR_NONE;
					applyArrivalWarnings();

					return res;

				}
				else if (this->CheckFlightPlanProcessed(fp)) {
					res.valid = false;
					res.tag = "INV";
					res.color = TAG_COLOR_ORANGE;

					continue;
				}
				else {
					res.valid = false;
					res.tag = "";
					res.color = TAG_COLOR_NONE;
					continue;
				}
			}
		}
	}
	applyArrivalWarnings();
	return res;
}

bool CDelHel::CheckFlightPlanProcessed(const EuroScopePlugIn::CFlightPlan& fp)
{
	std::string cs = fp.GetCallsign();

	if (std::find(this->processed.begin(), this->processed.end(), cs) != this->processed.end()) {
		return true;
	}
	return false;
}

bool CDelHel::IsFlightPlanProcessed(const EuroScopePlugIn::CFlightPlan& fp)
{
	std::string cs = fp.GetCallsign();

	if (std::find(this->processed.begin(), this->processed.end(), cs) != this->processed.end()) {
		return true;
	}

	this->processed.push_back(cs);
	return false;
}

std::string CDelHel::GetRmSideOverride(const EuroScopePlugIn::CFlightPlan& fp)
{
	if (!this->lfpgRm) {
		return "";
	}

	EuroScopePlugIn::CFlightPlanData fpd = fp.GetFlightPlanData();
	std::string dep = fpd.GetOrigin();
	to_upper(dep);
	if (dep != "LFPG") {
		return "";
	}

	EuroScopePlugIn::CRadarTarget rt = fp.GetCorrelatedRadarTarget();
	if (!rt.IsValid()) {
		return "";
	}

	EuroScopePlugIn::CRadarTargetPositionData pos = rt.GetPosition();
	if (!pos.IsValid()) {
		return "";
	}

	EuroScopePlugIn::CPosition p = pos.GetPosition();
	geo_point gp{ p.m_Latitude, p.m_Longitude };
	if (this->debug) {
		std::ostringstream dbg;
		dbg << "RM manual pos lat=" << gp.lat << " lon=" << gp.lon;
		this->LogDebugMessage(dbg.str(), fp.GetCallsign());
	}

	if (PointInPolygon(gp, LFPG_RM_NORTH)) {
		this->LogDebugMessage("RM manual matched NORTH polygon", fp.GetCallsign());
		return "NORTH";
	}
	if (PointInPolygon(gp, LFPG_RM_SOUTH)) {
		this->LogDebugMessage("RM manual matched SOUTH polygon", fp.GetCallsign());
		return "SOUTH";
	}
	if (this->debug) {
		this->LogDebugMessage("RM manual matched no polygon", fp.GetCallsign());
	}
	return "";
}

void CDelHel::AutoProcessFlightPlans()
{
	for (EuroScopePlugIn::CRadarTarget rt = this->RadarTargetSelectFirst(); rt.IsValid(); rt = this->RadarTargetSelectNext(rt)) {
		EuroScopePlugIn::CRadarTargetPositionData pos = rt.GetPosition();
		// Skip auto-processing if aircraft is not on the ground (currently using flightlevel threshold)
		// TODO better option for finding aircraft on ground
		if (!pos.IsValid() || pos.GetFlightLevel() > AUTO_ASSIGN_MIN_FL) {
			continue;
		}

		EuroScopePlugIn::CFlightPlan fp = rt.GetCorrelatedFlightPlan();
		// Skip auto-processing if aircraft is tracked (with exception of aircraft tracked by current controller)
		if (!fp.IsValid() || (strcmp(fp.GetTrackingControllerId(), "") != 0 && !fp.GetTrackingControllerIsMe())) {
			continue;
		}

		std::string dep = fp.GetFlightPlanData().GetOrigin();
		to_upper(dep);

		std::string arr = fp.GetFlightPlanData().GetDestination();
		to_upper(arr);

		// Skip auto-processing for aircraft without a valid flightplan (no departure/destination airport)
		if (dep == "" || arr == "") {
			continue;
		}

		auto ait = this->airports.find(dep);
		// Skip auto-processing of departures not available in the airport config
		if (ait == this->airports.end()) {
			continue;
		}
		// Skip auto-processing of airports currently not set as active in EuroScope
		if (!ait->second.active) {
			continue;
		}

		if (this->IsFlightPlanProcessed(fp)) {
			continue;
		}

		std::string rmSideOverride;
		if (this->lfpgRm && dep == "LFPG" && this->lfpgRmEnabledAt.has_value()) {
			const auto now = std::chrono::steady_clock::now();
			auto [it, inserted] = this->firstSeenTargets.emplace(fp.GetCallsign(), now);
			if (inserted) {
				it->second = now;
			}

			if (it->second >= *this->lfpgRmEnabledAt) {
				const auto age = now - it->second;
				if (age <= std::chrono::seconds(10)) {
					EuroScopePlugIn::CPosition p = pos.GetPosition();
					geo_point gp{ p.m_Latitude, p.m_Longitude };
					if (this->debug) {
						std::ostringstream dbg;
						dbg << "RM pos lat=" << gp.lat << " lon=" << gp.lon << " age=" << std::chrono::duration_cast<std::chrono::seconds>(age).count() << "s";
						this->LogDebugMessage(dbg.str(), fp.GetCallsign());
					}
					if (PointInPolygon(gp, LFPG_RM_NORTH)) {
						rmSideOverride = "NORTH";
						this->LogDebugMessage("RM matched NORTH polygon", fp.GetCallsign());
					}
					else if (PointInPolygon(gp, LFPG_RM_SOUTH)) {
						rmSideOverride = "SOUTH";
						this->LogDebugMessage("RM matched SOUTH polygon", fp.GetCallsign());
					}
					else if (this->debug) {
						this->LogDebugMessage("RM matched no polygon", fp.GetCallsign());
					}
				}
			}
		}

		this->ProcessFlightPlan(fp, this->assignNap, false, rmSideOverride);
	}
}

void CDelHel::LogMessage(std::string message)
{
	this->DisplayUserMessage("Message", PLUGIN_NAME, message.c_str(), true, true, true, false, false);
}

void CDelHel::LogMessage(std::string message, std::string type)
{
	this->DisplayUserMessage(PLUGIN_NAME, type.c_str(), message.c_str(), true, true, true, this->flashOnMessage, false);
}

void CDelHel::LogDebugMessage(std::string message)
{
	if (this->debug) {
		this->LogMessage(message);
	}
}

void CDelHel::LogDebugMessage(std::string message, std::string type)
{
	if (this->debug) {
		this->LogMessage(message, type);
	}
}

void CDelHel::CheckForUpdate()
{
	try
	{
		semver::version latest{ this->latestVersion.get() };
		semver::version current{ PLUGIN_VERSION };

		if (latest > current) {
			std::ostringstream ss;
			ss << "A new version (" << latest << ") of " << PLUGIN_NAME << " is available, download it at " << PLUGIN_LATEST_DOWNLOAD_URL;

			this->LogMessage(ss.str(), "Update");
		}
	}
	catch (std::exception& e)
	{
		MessageBox(NULL, e.what(), PLUGIN_NAME, MB_OK | MB_ICONERROR);
	}

	this->latestVersion = std::future<std::string>();
}

void CDelHel::CheckLoadedPlugins()
{
	this->topSkyAvailable = false;
	this->ccamsAvailable = false;

	HMODULE hMods[1024];
	HANDLE hProcess;
	DWORD cbNeeded;
	unsigned int i;

	hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, GetCurrentProcessId());
	if (hProcess == NULL) {
		this->LogDebugMessage("Failed to check loaded plugins");
		return;
	}

	if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
		for (i = 0; i < (cbNeeded / sizeof(HMODULE)); i++) {
			TCHAR szModName[MAX_PATH];
			if (GetModuleFileNameEx(hProcess, hMods[i], szModName, sizeof(szModName) / sizeof(TCHAR))) {
				std::string moduleName = szModName;
				size_t pos = moduleName.find_last_of("\\");
				if (pos != std::string::npos) {
					moduleName = moduleName.substr(pos + 1);
				}

				if (moduleName == TOPSKY_DLL_NAME) {
					this->topSkyAvailable = true;
					this->LogDebugMessage("Found TopSky plugin", "Config");
				}
				else if (moduleName == CCAMS_DLL_NAME) {
					this->ccamsAvailable = true;
					this->LogDebugMessage("Found CCAMS plugin", "Config");
				}
			}
		}
	}

	CloseHandle(hProcess);
}

void __declspec (dllexport) EuroScopePlugInInit(EuroScopePlugIn::CPlugIn** ppPlugInInstance)
{
	*ppPlugInInstance = pPlugin = new CDelHel();
}

void __declspec (dllexport) EuroScopePlugInExit(void)
{
	delete pPlugin;
}
