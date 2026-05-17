#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <sstream>
#include <future>
#include <chrono>
#include <optional>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <psapi.h>

#include "EuroScope/EuroScopePlugIn.h"
#include "semver/semver.hpp"
#include "nlohmann/json.hpp"

#include "constants.h"
#include "helpers.h"
#include "airport.h"
#include "validation.h"
#include "flightplan.h"
#include "sid.h"
#include "rwy_config.h"
#include "RadarScreen.h"

using json = nlohmann::json;
using namespace std::chrono_literals;

class CDelHel : public EuroScopePlugIn::CPlugIn
{
public:
	CDelHel();
	virtual ~CDelHel();

	bool OnCompileCommand(const char* sCommandLine);
	void OnGetTagItem(EuroScopePlugIn::CFlightPlan FlightPlan, EuroScopePlugIn::CRadarTarget RadarTarget, int ItemCode, int TagData, char sItemString[16], int* pColorCode, COLORREF* pRGB, double* pFontSize);
	void OnFunctionCall(int FunctionId, const char* sItemString, POINT Pt, RECT Area);
	void OnTimer(int Counter);
	void OnFlightPlanDisconnect(EuroScopePlugIn::CFlightPlan FlightPlan);
	void OnAirportRunwayActivityChanged();
	EuroScopePlugIn::CRadarScreen* OnRadarScreenCreated(const char* sDisplayName, bool NeedRadarContent, bool GeoReferenced, bool CanBeSaved, bool CanBeCreated);

private:
	bool debug;
	bool updateCheck;
	bool assignNap;
	bool autoProcess;
	bool warnRFLBelowCFL;
	bool logMinMaxRFL;
	bool checkMinMaxRFL;
	bool flashOnMessage;
	bool topSkyAvailable;
	bool ccamsAvailable;
	bool preferTopSkySquawkAssignment;
	bool lfpgLinked;
	bool lfpgCas;
	bool lfpgRm;
	bool customConfigActive;
	std::optional<std::chrono::steady_clock::time_point> lfpgRmEnabledAt;
	std::map<std::string, std::chrono::steady_clock::time_point> firstSeenTargets;
	std::set<std::string> unlinkedDepartureAirports;
	std::set<char> natArrivalIcaoPrefixes;
	std::set<std::string> arvlArrivalAirports;
	std::string customRunwayConfig;
	std::future<std::string> latestVersion;
	std::map<std::string, airport> airports;
	std::vector<std::string> processed;
	std::map<std::string, rwy_config> runwayConfigs;
	RadarScreen* radarScreen;
	bool IsAirportLinked(const std::string& icao) const;
	bool IsNatArrivalAirport(const std::string& icao) const;
	bool IsArvlArrivalAirport(const std::string& icao) const;
	void SetAirportLinked(const std::string& icao, bool linked);
	std::string SerializeUnlinkedDepartureAirports() const;
	void DeserializeUnlinkedDepartureAirports(const std::string& serialized);

	void LoadSettings();
	void SaveSettings();
	void ReadRoutingConfig();
	void ReadAirportConfig();
	void ReadCustomConfigs();
	void UpdateActiveAirports();

	validation ProcessFlightPlan(EuroScopePlugIn::CFlightPlan& fp, bool nap, bool validateOnly = false, const std::string& sideOverride = "");
	bool CDelHel::CheckFlightPlanProcessed(const EuroScopePlugIn::CFlightPlan& fp);
	bool IsFlightPlanProcessed(const EuroScopePlugIn::CFlightPlan& fp);
	void AutoProcessFlightPlans();
	std::string GetRmSideOverride(const EuroScopePlugIn::CFlightPlan& fp);

	void LogMessage(std::string message);
	void LogMessage(std::string message, std::string handler);
	void LogDebugMessage(std::string message);
	void LogDebugMessage(std::string message, std::string type);

	void CheckForUpdate();
	void CheckLoadedPlugins();
};

