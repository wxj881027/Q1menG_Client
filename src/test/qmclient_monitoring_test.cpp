#define CONF_TEST 1
#include <engine/client/gpu_upload_limiter.h>

#include <game/client/components/qmclient/monitoring/monitoring.h>
#include <game/client/components/qmclient/perf_logging.h>
#include <game/client/components/qmclient/settings_perf_windows.h>
#include <game/client/components/qmclient/settings_resource_preview.h>
#include <game/client/components/settings_resource_jobs.h>
#include <game/client/frame_scheduler.h>
#include <game/client/ui.h>

#include <gtest/gtest.h>
#include <test/test.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

namespace
{

	std::string ReadRepoFile(const char *pPath)
	{
		return ReadTestSourceFile(pPath);
	}

	bool ContainsAll(const std::string &Source, std::initializer_list<const char *> Needles)
	{
		for(const char *pNeedle : Needles)
		{
			if(Source.find(pNeedle) == std::string::npos)
				return false;
		}
		return true;
	}

	bool ContainsAny(const std::string &Source, std::initializer_list<const char *> Needles)
	{
		for(const char *pNeedle : Needles)
		{
			if(Source.find(pNeedle) != std::string::npos)
				return true;
		}
		return false;
	}

	std::string Trim(const std::string &Text)
	{
		const size_t Begin = Text.find_first_not_of(" \t\r\n");
		if(Begin == std::string::npos)
			return {};

		const size_t End = Text.find_last_not_of(" \t\r\n");
		return Text.substr(Begin, End - Begin + 1);
	}

	std::vector<std::string> SplitLines(const std::string &Source)
	{
		std::vector<std::string> vLines;
		std::stringstream Stream(Source);
		std::string Line;
		while(std::getline(Stream, Line))
			vLines.push_back(Line);
		return vLines;
	}

	void AppendUniqueCandidate(std::vector<std::string> &vCandidates, const std::string &Candidate)
	{
		if(Candidate.empty())
			return;

		for(const std::string &Existing : vCandidates)
		{
			if(Existing == Candidate)
				return;
		}
		vCandidates.push_back(Candidate);
	}

	std::vector<std::string> ExtractQuotedCandidates(const std::string &Line)
	{
		std::vector<std::string> vFound;
		size_t Pos = 0;
		while((Pos = Line.find('"', Pos)) != std::string::npos)
		{
			const size_t End = Line.find('"', Pos + 1);
			if(End == std::string::npos)
				break;
			AppendUniqueCandidate(vFound, Line.substr(Pos + 1, End - Pos - 1));
			Pos = End + 1;
		}
		return vFound;
	}

	std::string ExtractVariableAssignment(const std::string &Line)
	{
		for(const char *pVar : {"pTitle", "pText", "pLabel"})
		{
			const size_t VarPos = Line.find(pVar);
			if(VarPos == std::string::npos)
				continue;

			const size_t EqualPos = Line.find('=', VarPos + 1);
			if(EqualPos == std::string::npos)
				continue;

			return Trim(Line.substr(EqualPos + 1));
		}
		return {};
	}

	[[maybe_unused]] std::vector<std::string> CollectStableTextCandidates(const std::string &Source)
	{
		const std::vector<std::string> vLines = SplitLines(Source);
		std::vector<std::string> vCandidates;
		for(size_t LineIndex = 0; LineIndex < vLines.size(); ++LineIndex)
		{
			const std::string &Line = vLines[LineIndex];
			if(!ContainsAny(Line,
				   {
					   "Ui()->DoLabel(",
					   "DoButton_Menu(",
					   "DoButton_CheckBox",
					   "DoButton_CheckBoxAutoVMarginAndSet",
					   "Ui()->DoScrollbarOption",
					   "DoSettingsLabel",
					   "DoSettingsMenuLabel",
					   "DoSettingsButton_",
					   "DoSettingsScrollbarOption",
					   "SettingsTextElement",
					   "QmNewFeatureLabel",
					   "RainbowColor",
					   "DoModuleHeadline",
					   "pTitle",
					   "pText",
					   "pLabel",
				   }))
			{
				continue;
			}

			for(const std::string &Quoted : ExtractQuotedCandidates(Line))
				AppendUniqueCandidate(vCandidates, Quoted);

			const size_t BlockBegin = LineIndex > 2 ? LineIndex - 2 : 0;
			const size_t BlockEnd = std::min(LineIndex + 3, vLines.size());
			for(size_t NearbyLine = BlockBegin; NearbyLine < BlockEnd; ++NearbyLine)
			{
				for(const std::string &Quoted : ExtractQuotedCandidates(vLines[NearbyLine]))
					AppendUniqueCandidate(vCandidates, Quoted);

				const std::string Assignment = ExtractVariableAssignment(vLines[NearbyLine]);
				if(!Assignment.empty())
					AppendUniqueCandidate(vCandidates, Assignment);
			}

			if(Line.find("QmNewFeatureLabel") != std::string::npos)
				AppendUniqueCandidate(vCandidates, "QmNewFeatureLabel");
			if(Line.find("RainbowColor") != std::string::npos)
				AppendUniqueCandidate(vCandidates, "RainbowColor");
			if(Line.find("DoModuleHeadline") != std::string::npos)
				AppendUniqueCandidate(vCandidates, "DoModuleHeadline");
		}
		return vCandidates;
	}

	struct SStableTextCandidate
	{
		int m_Line = 0;
		std::string m_Text;
		std::string m_SourceLine;
	};

	struct SStableTextRawAllow
	{
		const char *m_pFile;
		int m_Line;
		const char *m_pReason;
	};

	bool IsStableTextAllowedReason(const char *pReason)
	{
		for(const char *pAllowed : {
			    "dynamic-value",
			    "user-generated",
			    "localized-list-data",
			    "stateful-new-label",
			    "animated-style",
			    "icon-only",
			    "status-message",
			    "search-result",
			    "input-text",
		    })
		{
			if(str_comp(pReason, pAllowed) == 0)
				return true;
		}
		return false;
	}

	bool IsDynamicStableTextCandidateLine(const std::string &Line)
	{
		return ContainsAny(Line, {
						 "aBuf",
						 "pSkinContainer->Name()",
						 "Language.m_Name",
						 "m_aStatusMessage",
						 "FONT_ICON_",
						 "pValue",
						 "DoEditBox",
						 "Input",
						 "Search",
						 "m_aName",
						 "m_aClan",
						 "Profile.",
						 "Client.",
						 "pEntry->",
						 "pType->",
						 "pVar->",
						 "SourceName(",
						 "pText,",
						 "pLabel,",
						 "pTitle,",
					 });
	}

	bool IsStableTextCandidatePayloadIgnored(const std::string &Text)
	{
		return Text == "%" || Text == "ms" || Text == "ms (off)" || Text == "s" || Text == " min" || Text == " seconds" || Text == " seconds (never)" || Text == "X";
	}

	bool IsPooledStableTextLine(const std::string &Line)
	{
		return ContainsAny(Line, {
						 "DoSettingsMenuLabel(",
						 "DoSettingsButton_Menu(",
						 "DoSettingsButton_CheckBox(",
						 "DoSettingsButton_CheckBoxAutoVMarginAndSet(",
						 "DoButton_Menu(",
						 "DoButton_CheckBox(",
						 "DoButton_CheckBox_Common(",
						 "DoButton_CheckBoxAutoVMarginAndSet(",
						 "DoSettingsScrollbarOption(",
						 "SettingsTextElement(",
						 "DoSettingsLabel(",
						 "DoQmSettingsLabel(",
						 "DoQmSettingsCheckbox(",
						 "DoQmSettingsMenuButton(",
					 });
	}

	std::vector<SStableTextCandidate> CollectRawStableTextCandidatesWithLines(const std::string &Source)
	{
		const std::vector<std::string> vLines = SplitLines(Source);
		std::vector<SStableTextCandidate> vCandidates;
		for(size_t LineIndex = 0; LineIndex < vLines.size(); ++LineIndex)
		{
			const std::string TrimmedLine = Trim(vLines[LineIndex]);
			if(TrimmedLine.empty() || TrimmedLine.rfind("//", 0) == 0)
				continue;
			if(TrimmedLine.find("TextRender()->TextColor(") != std::string::npos)
				continue;
			if(IsPooledStableTextLine(TrimmedLine) || IsDynamicStableTextCandidateLine(TrimmedLine))
				continue;
			if(!ContainsAny(TrimmedLine, {
							     "Ui()->DoLabel(",
							     "DoButton_Menu(",
							     "DoButton_CheckBox",
							     "Ui()->DoScrollbarOption",
							     "QmNewFeatureLabel",
							     "RainbowColor",
							     "DoModuleHeadline",
						     }))
			{
				continue;
			}

			std::vector<std::string> vTexts = ExtractQuotedCandidates(TrimmedLine);
			if(TrimmedLine.find("QmNewFeatureLabel") != std::string::npos)
				AppendUniqueCandidate(vTexts, "QmNewFeatureLabel");
			if(TrimmedLine.find("RainbowColor") != std::string::npos)
				AppendUniqueCandidate(vTexts, "RainbowColor");
			if(TrimmedLine.find("DoModuleHeadline") != std::string::npos)
				AppendUniqueCandidate(vTexts, "DoModuleHeadline");
			if(vTexts.empty() && ContainsAny(TrimmedLine, {"pTitle", "pText", "pLabel"}))
				AppendUniqueCandidate(vTexts, TrimmedLine);

			for(const std::string &Text : vTexts)
			{
				if(Text.empty() || IsStableTextCandidatePayloadIgnored(Text))
					continue;
				vCandidates.push_back({(int)LineIndex + 1, Text, TrimmedLine});
			}
		}
		return vCandidates;
	}

	bool IsStableTextCandidateAllowed(const SStableTextCandidate &Candidate, const char *pFile, const std::vector<SStableTextRawAllow> &vAllowlist)
	{
		for(const SStableTextRawAllow &Allow : vAllowlist)
		{
			EXPECT_TRUE(IsStableTextAllowedReason(Allow.m_pReason)) << Allow.m_pReason;
			if(str_comp(Allow.m_pFile, pFile) == 0 && Allow.m_Line == Candidate.m_Line)
				return true;
		}
		return false;
	}

	std::vector<SStableTextCandidate> FilterCandidatesNotCoveredByMenuPoolOrAllowlist(const char *pFile, const std::vector<SStableTextCandidate> &vCandidates, const std::vector<SStableTextRawAllow> &vAllowlist)
	{
		std::vector<SStableTextCandidate> vUnexpected;
		for(const SStableTextCandidate &Candidate : vCandidates)
		{
			if(!IsStableTextCandidateAllowed(Candidate, pFile, vAllowlist))
				vUnexpected.push_back(Candidate);
		}
		return vUnexpected;
	}

	std::string JoinCandidates(const std::vector<SStableTextCandidate> &vCandidates)
	{
		std::ostringstream Out;
		for(const SStableTextCandidate &Candidate : vCandidates)
			Out << "\nline " << Candidate.m_Line << ": " << Candidate.m_Text << " :: " << Candidate.m_SourceLine;
		return Out.str();
	}

	std::string ExtractSourceFunctionBody(const std::string &Source, const char *pSignature)
	{
		const size_t SignaturePos = Source.find(pSignature);
		if(SignaturePos == std::string::npos)
			return {};

		const size_t BodyStart = Source.find('{', SignaturePos);
		if(BodyStart == std::string::npos)
			return {};

		int Depth = 0;
		for(size_t i = BodyStart; i < Source.size(); ++i)
		{
			if(Source[i] == '{')
				++Depth;
			else if(Source[i] == '}')
			{
				--Depth;
				if(Depth == 0)
					return Source.substr(BodyStart, i - BodyStart + 1);
			}
		}
		return {};
	}

	std::string ExtractSourceBlock(const std::string &Source, const char *pBeginMarker, const char *pEndMarker)
	{
		const size_t Begin = Source.find(pBeginMarker);
		if(Begin == std::string::npos)
			return {};
		const size_t End = Source.find(pEndMarker, Begin);
		if(End == std::string::npos)
			return Source.substr(Begin);
		return Source.substr(Begin, End - Begin);
	}

	size_t CountSubstring(const std::string &Haystack, const std::string &Needle)
	{
		if(Needle.empty())
			return 0;

		size_t Count = 0;
		size_t Pos = 0;
		while((Pos = Haystack.find(Needle, Pos)) != std::string::npos)
		{
			++Count;
			Pos += Needle.size();
		}
		return Count;
	}

} // namespace

TEST(QmMonitoringHelpers, ConnectionGradeTracksDisconnectedState)
{
	SQmNetworkMetrics Net;
	Net.m_Connected = false;
	EXPECT_EQ(QmDetermineConnectionGrade(Net), EQmConnectionGrade::DISCONNECTED);
}

TEST(QmMonitoringHelpers, ConnectionGradeUsesThresholdTable)
{
	SQmNetworkMetrics Net;
	Net.m_Connected = true;
	Net.m_SnapshotLatencyMs = 40.0f;
	Net.m_PredictionLatencyMs = 50.0f;
	Net.m_JitterMs = 5.0f;
	Net.m_PacketLossPct = 0.0f;
	EXPECT_EQ(QmDetermineConnectionGrade(Net), EQmConnectionGrade::NORMAL);

	Net.m_PredictionLatencyMs = 110.0f;
	EXPECT_EQ(QmDetermineConnectionGrade(Net), EQmConnectionGrade::ELEVATED);

	Net.m_PredictionLatencyMs = 210.0f;
	EXPECT_EQ(QmDetermineConnectionGrade(Net), EQmConnectionGrade::SEVERE);
}

TEST(QmMonitoringHelpers, PrimaryCausePrefersDominantMetric)
{
	SQmNetworkMetrics Net;
	SQmPerformanceMetrics Perf;

	Net.m_Connected = false;
	EXPECT_EQ(QmDeterminePrimaryCause(Net, Perf, EQmConnectionGrade::DISCONNECTED), EQmDiagnosticCause::NONE);

	Net.m_Connected = true;
	Net.m_SnapshotLatencyMs = 120.0f;
	Net.m_PredictionLatencyMs = 40.0f;
	EXPECT_EQ(QmDeterminePrimaryCause(Net, Perf, EQmConnectionGrade::ELEVATED), EQmDiagnosticCause::DOWNSTREAM);

	Net.m_SnapshotLatencyMs = 20.0f;
	Net.m_PredictionLatencyMs = 95.0f;
	EXPECT_EQ(QmDeterminePrimaryCause(Net, Perf, EQmConnectionGrade::ELEVATED), EQmDiagnosticCause::UPSTREAM);

	Net.m_PredictionLatencyMs = 30.0f;
	Net.m_JitterMs = 28.0f;
	EXPECT_EQ(QmDeterminePrimaryCause(Net, Perf, EQmConnectionGrade::SEVERE), EQmDiagnosticCause::JITTER);

	Net.m_JitterMs = 6.0f;
	Net.m_PacketLossPct = 8.0f;
	EXPECT_EQ(QmDeterminePrimaryCause(Net, Perf, EQmConnectionGrade::SEVERE), EQmDiagnosticCause::PACKET_LOSS);

	Net.m_PacketLossPct = 0.0f;
	Net.m_ConnectionProblems = true;
	EXPECT_EQ(QmDeterminePrimaryCause(Net, Perf, EQmConnectionGrade::SEVERE), EQmDiagnosticCause::DOWNSTREAM);

	Net.m_PacketLossPct = 2.0f;
	Net.m_ConnectionProblems = true;
	Net.m_SnapshotLatencyMs = 130.0f;
	Net.m_PredictionLatencyMs = 20.0f;
	EXPECT_EQ(QmDeterminePrimaryCause(Net, Perf, EQmConnectionGrade::ELEVATED), EQmDiagnosticCause::PACKET_LOSS);
}

TEST(QmMonitoringHelpers, DiagnosticLossRateUsesSendDeltaAndResends)
{
	EXPECT_NEAR(QmComputeDiagnosticPacketLossPct(60, 20), 33.3333f, 0.001f);
	EXPECT_FLOAT_EQ(QmComputeDiagnosticPacketLossPct(60, 20), 33.333332f);
	EXPECT_FLOAT_EQ(QmComputeDiagnosticPacketLossPct(0, 0), 0.0f);
	EXPECT_FLOAT_EQ(QmComputeDiagnosticPacketLossPct(0, 3), 100.0f);
}

TEST(QmMonitoringHelpers, RollbackAmountUsesNegativeGameTimeMargin)
{
	EXPECT_FLOAT_EQ(QmComputeRollbackMs(-18.0f), 18.0f);
	EXPECT_FLOAT_EQ(QmComputeRollbackMs(6.0f), 0.0f);
}

TEST(QmMonitoringHelpers, PeakSelectionPrefersLatestMatchingPeak)
{
	std::array<float, 8> aHistory = {41.0f, 39.0f, 41.0f, 24.0f, 24.0f, 18.0f, 24.0f, 17.0f};
	EXPECT_EQ(QmFindLatestPeakIndex(aHistory, 0, 8), 2);
	EXPECT_EQ(QmFindLatestAbsolutePeakIndex(aHistory, 0, 8), 2);
}

TEST(QmMonitoringHelpers, SignedPeakSelectionUsesLatestAbsolutePeak)
{
	std::array<float, 8> aHistory = {-7.0f, 5.0f, -9.0f, 3.0f, 9.0f, 4.0f, 8.0f, 2.0f};
	EXPECT_EQ(QmFindLatestAbsolutePeakIndex(aHistory, 0, 8), 4);
}

TEST(QmMonitoringHelpers, UiScaleGrowsOnHighResolutionScreens)
{
	EXPECT_FLOAT_EQ(QmComputeMonitoringUiScale(800.0f, 600.0f), 0.65f);
	const float Expected1600x900 = std::sqrt((1600.0f / 1920.0f) * (900.0f / 1080.0f));
	EXPECT_FLOAT_EQ(QmComputeMonitoringUiScale(1600.0f, 900.0f), Expected1600x900);
	EXPECT_FLOAT_EQ(QmComputeMonitoringUiScale(3840.0f, 2160.0f), 1.8f);
}

TEST(QmMonitoringHelpers, PanelOpacityClampsPercentToUnitRange)
{
	EXPECT_FLOAT_EQ(QmComputeMonitoringPanelOpacity(-20), 0.0f);
	EXPECT_FLOAT_EQ(QmComputeMonitoringPanelOpacity(35), 0.35f);
	EXPECT_FLOAT_EQ(QmComputeMonitoringPanelOpacity(140), 1.0f);
}

TEST(QmMonitoringHelpers, ProcessCpuUsageNormalizesAcrossLogicalCpus)
{
	EXPECT_FLOAT_EQ(QmNormalizeProcessCpuUsagePct(-1.0f, 8), -1.0f);
	EXPECT_FLOAT_EQ(QmNormalizeProcessCpuUsagePct(114.0f, 8), 14.25f);
	EXPECT_FLOAT_EQ(QmNormalizeProcessCpuUsagePct(1600.0f, 16), 100.0f);
	EXPECT_FLOAT_EQ(QmNormalizeProcessCpuUsagePct(114.0f, 0), 100.0f);
}

TEST(QmMonitoringHelpers, TotalCpuUsageComputesBusyDelta)
{
	EXPECT_FLOAT_EQ(QmComputeTotalCpuUsagePct(100, 1000, 125, 1100), 75.0f);
	EXPECT_FLOAT_EQ(QmComputeTotalCpuUsagePct(100, 1000, 200, 1100), 0.0f);
	EXPECT_FLOAT_EQ(QmComputeTotalCpuUsagePct(100, 1000, 100, 1100), 100.0f);
	EXPECT_FLOAT_EQ(QmComputeTotalCpuUsagePct(100, 0, 125, 1100), -1.0f);
	EXPECT_FLOAT_EQ(QmComputeTotalCpuUsagePct(100, 1000, 90, 1100), -1.0f);
	EXPECT_FLOAT_EQ(QmComputeTotalCpuUsagePct(100, 1000, 125, 990), -1.0f);
}

TEST(QmMonitoringHelpers, CpuRatioValueShowsProcessAndTotalCpu)
{
	char aBuf[32];
	FormatCpuRatioValue(aBuf, sizeof(aBuf), -1.0f, 35.0f);
	EXPECT_STREQ(aBuf, "--");
	FormatCpuRatioValue(aBuf, sizeof(aBuf), 12.4f, -1.0f);
	EXPECT_STREQ(aBuf, "12%");
	FormatCpuRatioValue(aBuf, sizeof(aBuf), 12.4f, 35.6f);
	EXPECT_STREQ(aBuf, "12%/36%");
}

TEST(QmMonitoringHelpers, TrafficStatsMatchOfficialDebugMath)
{
	const auto Stats = QmComputeTrafficStats(10, 1000, 14, 1320);
	EXPECT_EQ(Stats.m_Packets, 4u);
	EXPECT_EQ(Stats.m_PayloadBytes, 320u);
	EXPECT_EQ(Stats.m_OverheadBytes, 168u);
	EXPECT_EQ(Stats.m_TotalBytes, 488u);
	EXPECT_EQ(Stats.m_AveragePayloadBytes, 80u);
	EXPECT_FLOAT_EQ(Stats.m_RateKibPerSec, 3.8125f);
}

TEST(QmMonitoringHelpers, HudLayoutPlacesPanelLeftOfGraphColumn)
{
	const SQmMonitoringHudLayout Layout = QmComputeMonitoringHudLayout(1600.0f, 900.0f, 1184.0f, 16.0f);
	EXPECT_FLOAT_EQ(Layout.m_PanelRect.w, 768.0f);
	EXPECT_FLOAT_EQ(Layout.m_PanelRect.h, 594.0f);
	EXPECT_FLOAT_EQ(Layout.m_PanelRect.x, 400.0f);
	EXPECT_FLOAT_EQ(Layout.m_PanelRect.y, 32.0f);
	EXPECT_FLOAT_EQ(Layout.m_ContentRect.x, 410.0f);
	EXPECT_FLOAT_EQ(Layout.m_ContentRect.y, 42.0f);
}

TEST(QmMonitoringHelpers, BodyLayoutPreservesMetricsBudgetOnCompactPanels)
{
	const SQmMonitoringBodyLayout Layout = QmComputeMonitoringBodyLayout(260.0f, 1.0f);
	EXPECT_NEAR(Layout.m_MainGraphHeight, 63.1f, 0.1f);
	EXPECT_NEAR(Layout.m_FpsGraphHeight, 39.8f, 0.1f);
	EXPECT_NEAR(Layout.m_PrimaryCardsHeight, 37.2f, 0.1f);
	EXPECT_NEAR(Layout.m_MetricsExtraHeight, 31.9f, 0.1f);
	EXPECT_GT(Layout.m_PrimaryCardsHeight, 30.0f);
}

TEST(QmMonitoringHelpers, HudLayoutUsesLargerPanelOn4kScreens)
{
	const SQmMonitoringHudLayout Layout = QmComputeMonitoringHudLayout(3840.0f, 2160.0f, 2842.0f, 38.0f);
	EXPECT_FLOAT_EQ(Layout.m_PanelRect.w, 1843.0f);
	EXPECT_FLOAT_EQ(Layout.m_PanelRect.h, 1405.0f);
	EXPECT_FLOAT_EQ(Layout.m_ContentRect.x, 983.0f);
	EXPECT_FLOAT_EQ(Layout.m_ContentRect.y, 98.0f);
}

TEST(QmMonitoringHelpers, HudLayoutClampsPanelInsideScreenBounds)
{
	const SQmMonitoringHudLayout Layout = QmComputeMonitoringHudLayout(360.0f, 240.0f, 120.0f, 16.0f);
	EXPECT_FLOAT_EQ(Layout.m_PanelRect.x, 0.0f);
	EXPECT_FLOAT_EQ(Layout.m_PanelRect.y, 0.0f);
	EXPECT_LE(Layout.m_PanelRect.x + Layout.m_PanelRect.w, 360.0f);
	EXPECT_LE(Layout.m_PanelRect.y + Layout.m_PanelRect.h, 240.0f);
}

TEST(QmMonitoringHelpers, DeviceMetricsDefaultToUnavailable)
{
	SQmPerformanceMetrics Perf;
	EXPECT_FALSE(Perf.m_DeviceSampleAvailable);
	EXPECT_FLOAT_EQ(Perf.m_GpuUtilPct, -1.0f);
	EXPECT_FLOAT_EQ(Perf.m_GpuDedicatedVramMb, -1.0f);
	EXPECT_FLOAT_EQ(Perf.m_GpuDedicatedVramBudgetMb, -1.0f);
	EXPECT_FLOAT_EQ(Perf.m_GpuSharedVramMb, -1.0f);
	EXPECT_FLOAT_EQ(Perf.m_DiskReadMbPerSec, -1.0f);
}

TEST(QmMonitoringHelpers, TeeSkinListFrameTelemetryExposesRowsFields)
{
	std::ifstream File(TestSourcePath("src/game/client/components/menus_settings.cpp"));
	ASSERT_TRUE(File.good());
	std::stringstream Buffer;
	Buffer << File.rdbuf();
	const std::string Source = Buffer.str();
	EXPECT_NE(Source.find("event=list_frame page=settings:tee"), std::string::npos);
	EXPECT_NE(Source.find("rows_total=%d rows_visible=%d rows_rendered=%d rows_iterated=%d rows_skipped=%d"), std::string::npos);
	EXPECT_NE(Source.find("first_visible_index=%d last_visible_index=%d"), std::string::npos);
}

TEST(QmMonitoringHelpers, JumpHintLegacyConfigMigratesAllFieldsToQmConfig)
{
	std::ifstream TClientFile(TestSourcePath("src/engine/shared/config_variables_tclient.h"));
	ASSERT_TRUE(TClientFile.good());
	std::stringstream TClientBuffer;
	TClientBuffer << TClientFile.rdbuf();
	const std::string TClientSource = TClientBuffer.str();

	std::ifstream QmFile(TestSourcePath("src/engine/shared/config_variables_qmclient.h"));
	ASSERT_TRUE(QmFile.good());
	std::stringstream QmBuffer;
	QmBuffer << QmFile.rdbuf();
	const std::string QmSource = QmBuffer.str();

	std::ifstream GameClientFile(TestSourcePath("src/game/client/gameclient.cpp"));
	ASSERT_TRUE(GameClientFile.good());
	std::stringstream GameClientBuffer;
	GameClientBuffer << GameClientFile.rdbuf();
	const std::string GameClientSource = GameClientBuffer.str();

	// The jump-hint UI moved from tc_jump_hint* to qm_jump_hint*. This test
	// guards the migration shape so we do not silently drop old users' enabled
	// state, color, position, or font size when only the text key is migrated.
	EXPECT_NE(TClientSource.find("MACRO_CONFIG_INT(TcJumpHintLegacy, tc_jump_hint,"), std::string::npos);
	EXPECT_NE(TClientSource.find("MACRO_CONFIG_STR(TcJumpHintTextLegacy, tc_jump_hint_text,"), std::string::npos);
	EXPECT_NE(TClientSource.find("MACRO_CONFIG_COL(TcJumpHintColorLegacy, tc_jump_hint_color,"), std::string::npos);
	EXPECT_NE(TClientSource.find("MACRO_CONFIG_INT(TcJumpHintXLegacy, tc_jump_hint_x,"), std::string::npos);
	EXPECT_NE(TClientSource.find("MACRO_CONFIG_INT(TcJumpHintYLegacy, tc_jump_hint_y,"), std::string::npos);
	EXPECT_NE(TClientSource.find("MACRO_CONFIG_INT(TcJumpHintSizeLegacy, tc_jump_hint_size,"), std::string::npos);
	// Legacy keys are read-only migration inputs. Keeping CFGFLAG_SAVE on them
	// would re-save old tc_jump_hint* values and let them overwrite qm_jump_hint*
	// again whenever the user resets the new value back to its default.
	EXPECT_EQ(TClientSource.find("TcJumpHintLegacy, tc_jump_hint, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE"), std::string::npos);
	EXPECT_EQ(TClientSource.find("TcJumpHintTextLegacy, tc_jump_hint_text, 512, \"\", CFGFLAG_CLIENT | CFGFLAG_SAVE"), std::string::npos);
	EXPECT_EQ(TClientSource.find("TcJumpHintColorLegacy, tc_jump_hint_color, 255, CFGFLAG_CLIENT | CFGFLAG_SAVE"), std::string::npos);
	EXPECT_EQ(TClientSource.find("TcJumpHintXLegacy, tc_jump_hint_x, 20, 0, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE"), std::string::npos);
	EXPECT_EQ(TClientSource.find("TcJumpHintYLegacy, tc_jump_hint_y, 5, 0, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE"), std::string::npos);
	EXPECT_EQ(TClientSource.find("TcJumpHintSizeLegacy, tc_jump_hint_size, 10, 0, 50, CFGFLAG_CLIENT | CFGFLAG_SAVE"), std::string::npos);
	EXPECT_NE(QmSource.find("MACRO_CONFIG_INT(QmJumpHint, qm_jump_hint,"), std::string::npos);
	EXPECT_NE(QmSource.find("MACRO_CONFIG_STR(QmJumpHintText, qm_jump_hint_text,"), std::string::npos);
	EXPECT_NE(QmSource.find("MACRO_CONFIG_COL(QmJumpHintColor, qm_jump_hint_color,"), std::string::npos);
	EXPECT_NE(QmSource.find("MACRO_CONFIG_INT(QmJumpHintX, qm_jump_hint_x,"), std::string::npos);
	EXPECT_NE(QmSource.find("MACRO_CONFIG_INT(QmJumpHintY, qm_jump_hint_y,"), std::string::npos);
	EXPECT_NE(QmSource.find("MACRO_CONFIG_INT(QmJumpHintSize, qm_jump_hint_size,"), std::string::npos);
	EXPECT_NE(GameClientSource.find("MigrateInt(g_Config.m_QmJumpHint, g_Config.m_TcJumpHintLegacy"), std::string::npos);
	EXPECT_NE(GameClientSource.find("MigrateStr(g_Config.m_QmJumpHintText, sizeof(g_Config.m_QmJumpHintText), g_Config.m_TcJumpHintTextLegacy"), std::string::npos);
	EXPECT_NE(GameClientSource.find("MigrateCol(g_Config.m_QmJumpHintColor, g_Config.m_TcJumpHintColorLegacy"), std::string::npos);
	EXPECT_NE(GameClientSource.find("MigrateInt(g_Config.m_QmJumpHintX, g_Config.m_TcJumpHintXLegacy"), std::string::npos);
	EXPECT_NE(GameClientSource.find("MigrateInt(g_Config.m_QmJumpHintY, g_Config.m_TcJumpHintYLegacy"), std::string::npos);
	EXPECT_NE(GameClientSource.find("MigrateInt(g_Config.m_QmJumpHintSize, g_Config.m_TcJumpHintSizeLegacy"), std::string::npos);
}

TEST(QmMonitoringHelpers, JumpHintSettingsCardExposesAllQmFields)
{
	const std::string Source = ReadRepoFile("src/game/client/components/qmclient/menus_qmclient.cpp");
	const size_t ModuleMarker = Source.find("// ========== 模块: 位置跳跃提示 ==========");
	ASSERT_NE(ModuleMarker, std::string::npos);
	const size_t ModuleEnd = Source.find("case EQmModuleId::WeaponTrajectory:", ModuleMarker);
	ASSERT_NE(ModuleEnd, std::string::npos);
	const std::string Body = Source.substr(ModuleMarker, ModuleEnd - ModuleMarker);

	// Jump hint migrated from tc_jump_hint* to qm_jump_hint*. The settings page
	// must expose the whole qm_* surface, not just the enable toggle, otherwise
	// users can render the HUD element but cannot adjust its color, position, or
	// size without using console commands.
	EXPECT_NE(Body.find("DoQmSettingsCheckboxAuto(&g_Config.m_QmJumpHint"), std::string::npos);
	EXPECT_NE(Body.find("&g_Config.m_QmJumpHintColor"), std::string::npos);
	EXPECT_NE(Body.find("&g_Config.m_QmJumpHintX"), std::string::npos);
	EXPECT_NE(Body.find("&g_Config.m_QmJumpHintY"), std::string::npos);
	EXPECT_NE(Body.find("&g_Config.m_QmJumpHintSize"), std::string::npos);
	EXPECT_NE(Body.find("RenderSliderWithValueInput(&s_QmJumpHintXInputId"), std::string::npos);
	EXPECT_NE(Body.find("RenderSliderWithValueInput(&s_QmJumpHintYInputId"), std::string::npos);
	EXPECT_NE(Body.find("RenderSliderWithValueInput(&s_QmJumpHintSizeInputId"), std::string::npos);
	EXPECT_EQ(Body.find("m_TcJumpHint"), std::string::npos);
}

TEST(QmMonitoringHelpers, QmUiRuntimeTelemetryExposesSettingsContext)
{
	std::ifstream HeaderFile(TestSourcePath("src/game/client/QmUi/QmRt.h"));
	ASSERT_TRUE(HeaderFile.good());
	std::stringstream HeaderBuffer;
	HeaderBuffer << HeaderFile.rdbuf();
	const std::string Header = HeaderBuffer.str();

	std::ifstream SourceFile(TestSourcePath("src/game/client/QmUi/QmRt.cpp"));
	ASSERT_TRUE(SourceFile.good());
	std::stringstream SourceBuffer;
	SourceBuffer << SourceFile.rdbuf();
	const std::string Source = SourceBuffer.str();

	EXPECT_NE(Header.find("void SetPerfContext(const char *pPage, const char *pOperation);"), std::string::npos);
	EXPECT_EQ(Header.find("float m_LayoutMs = 0.0f;"), std::string::npos);
	EXPECT_NE(Header.find("int m_ActiveAnimCount = 0;"), std::string::npos);
	EXPECT_NE(Header.find("int m_QueuedAnimCount = 0;"), std::string::npos);
	EXPECT_NE(Source.find("active_anims=%d queued_anims=%d"), std::string::npos);
	EXPECT_EQ(Source.find("layout_ms=%.3f"), std::string::npos);
	EXPECT_NE(Source.find("QmPerfLogPayload(\"perf/ui_runtime\""), std::string::npos);
	EXPECT_NE(Source.find("QmPerfLogStage(\"perf/ui_runtime\", pStage, DurationMs, Force, pClient, pPage, nullptr, pExtra);"), std::string::npos);
	EXPECT_NE(Source.find("LogPerfStage(m_pGameClient->Client(), m_aPerfPage[0] != '\\0' ? m_aPerfPage : nullptr, \"ui_runtime_total\", RenderTimer.ElapsedMs(), false, aExtra);"), std::string::npos);
}

TEST(QmMonitoringHelpers, QmClientPerfTelemetryUsesLiveClientContext)
{
	{
		std::ifstream File(TestSourcePath("src/game/client/components/qmclient/menus_qmclient.cpp"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();

		EXPECT_NE(Source.find("void LogQmPerfStage(IClient *pClient, const char *pStage, double DurationMs, bool Force = false, const char *pExtra = nullptr)"), std::string::npos);
		EXPECT_NE(Source.find("QmPerfLogStage(\"perf/qmclient\", pStage, DurationMs, Force, pClient, nullptr, nullptr, pExtra);"), std::string::npos);
		EXPECT_EQ(CountSubstring(Source, "LogQmPerfStage("), CountSubstring(Source, "LogQmPerfStage(Client(),") + 1);
		EXPECT_NE(Source.find("QmPerfLogPayload(\"perf/qmclient\", aPayload, Client(), CurrentQmUiPerfPage());"), std::string::npos);
		EXPECT_EQ(Source.find("QmPerfLogPayload(\"perf/qmclient\", aPayload);"), std::string::npos);
	}
	{
		std::ifstream File(TestSourcePath("src/game/client/gameclient.cpp"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();

		EXPECT_NE(Source.find("m_UiRuntimeV2.SetPerfContext(pPerfPage, m_Menus.CurrentQmUiPerfOperation());"), std::string::npos);
	}
}

TEST(QmMonitoringHelpers, SectionQuadBatchingDoesNotCrossTextOrClipBoundaries)
{
	{
		std::ifstream File(TestSourcePath("src/game/client/ui.h"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();

		EXPECT_NE(Source.find("class CUiScopedQuadBatch"), std::string::npos);
		EXPECT_NE(Source.find("void BeginQuadBatch() const;"), std::string::npos);
		EXPECT_NE(Source.find("void FlushQuadBatch() const;"), std::string::npos);
		EXPECT_NE(Source.find("void EndQuadBatch() const;"), std::string::npos);
	}
	{
		std::ifstream File(TestSourcePath("src/game/client/ui.cpp"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();

		EXPECT_NE(Source.find("CUiScopedQuadBatch::CUiScopedQuadBatch"), std::string::npos);
		EXPECT_NE(Source.find("CUiScopedQuadBatch::~CUiScopedQuadBatch"), std::string::npos);
		EXPECT_NE(Source.find("void CUi::FlushQuadBatch() const"), std::string::npos);
		EXPECT_EQ(Source.find("RenderQuadContainerAsSpriteMultiple(m_QuadBatchContainerIndex"), std::string::npos);
		EXPECT_NE(Source.find("TextRender()->RenderTextContainer"), std::string::npos);
		EXPECT_NE(Source.find("void CUi::RenderLabelTextContainerAligned"), std::string::npos);
		EXPECT_NE(Source.find("FlushQuadBatch();\n\tTextRender()->RenderTextContainer"), std::string::npos);
		EXPECT_NE(Source.find("void CUi::ClipEnable(const CUIRect *pRect)\n{\n\tFlushQuadBatch();"), std::string::npos);
		EXPECT_NE(Source.find("void CUi::ClipDisable()\n{\n\tFlushQuadBatch();"), std::string::npos);
	}
	{
		std::ifstream File(TestSourcePath("src/game/client/components/tclient/menus_tclient.cpp"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();

		EXPECT_NE(Source.find("CUiScopedQuadBatch QuadBatchScope(Ui());"), std::string::npos);
	}
}

TEST(UiQuadBatch, PureColorFlushUsesUntexturedQuadContainerPath)
{
	std::ifstream File(TestSourcePath("src/game/client/ui.cpp"));
	ASSERT_TRUE(File.good());
	std::stringstream Buffer;
	Buffer << File.rdbuf();
	const std::string Source = Buffer.str();
	const std::string Body = ExtractSourceFunctionBody(Source, "void CUi::FlushQuadBatch() const");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("UiQuadBatchHasPendingSubmission"), std::string::npos);
	EXPECT_NE(Body.find("Graphics()->TextureClear();"), std::string::npos);
	EXPECT_NE(Body.find("Graphics()->SetColor(m_QuadBatchColor);"), std::string::npos);
	EXPECT_EQ(Body.find("RenderQuadContainerAsSpriteMultiple"), std::string::npos);
	EXPECT_NE(Body.find("m_vQuadBatchSprites"), std::string::npos);
	EXPECT_NE(Body.find("RenderQuadContainerEx(m_QuadBatchContainerIndex, 0, -1"), std::string::npos);
	EXPECT_NE(Body.find("Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);"), std::string::npos);
	EXPECT_NE(Body.find("m_vQuadBatchSprites.clear();"), std::string::npos);
	EXPECT_NE(Body.find("m_QuadBatchContainerIndex = -1;"), std::string::npos);
}

TEST(QmMonitoringHelpers, UiQuadBatchFlushGuardsInvalidRectsAndKeepsSubmissionPlan)
{
	EXPECT_FALSE(UiBatchableRectHasPositiveSize(0.0f, 10.0f));
	EXPECT_FALSE(UiBatchableRectHasPositiveSize(10.0f, 0.0f));
	EXPECT_FALSE(UiBatchableRectHasPositiveSize(-1.0f, 10.0f));
	EXPECT_FALSE(UiBatchableRectHasPositiveSize(10.0f, -1.0f));
	EXPECT_TRUE(UiBatchableRectHasPositiveSize(1.0f, 1.0f));
	EXPECT_TRUE(UiBatchableRectHasPositiveSize(200.0f, 24.0f));

	EXPECT_FALSE(UiQuadBatchHasPendingSubmission(-1, 0));
	EXPECT_FALSE(UiQuadBatchHasPendingSubmission(-1, 3));
	EXPECT_FALSE(UiQuadBatchHasPendingSubmission(2, 0));
	EXPECT_TRUE(UiQuadBatchHasPendingSubmission(0, 1));
	EXPECT_TRUE(UiQuadBatchHasPendingSubmission(5, 4));

	const ColorRGBA Red(1.0f, 0.0f, 0.0f, 1.0f);
	const ColorRGBA Blue(0.0f, 0.0f, 1.0f, 1.0f);

	{
		const SUiQuadBatchSubmissionPlan Plan = UiPlanQuadBatchSubmission(false, -1, Red, -1, Blue);
		EXPECT_TRUE(Plan.m_LeavesBatchUntouched);
		EXPECT_FALSE(Plan.m_RenderImmediately);
		EXPECT_FALSE(Plan.m_FlushBeforeQueue);
		EXPECT_FALSE(Plan.m_QueueSprite);
	}

	{
		const SUiQuadBatchSubmissionPlan Plan = UiPlanQuadBatchSubmission(false, -1, Red, 7, Blue);
		EXPECT_FALSE(Plan.m_LeavesBatchUntouched);
		EXPECT_TRUE(Plan.m_RenderImmediately);
		EXPECT_FALSE(Plan.m_FlushBeforeQueue);
		EXPECT_FALSE(Plan.m_QueueSprite);
	}

	{
		const SUiQuadBatchSubmissionPlan Plan = UiPlanQuadBatchSubmission(true, -1, Red, 7, Blue);
		EXPECT_FALSE(Plan.m_LeavesBatchUntouched);
		EXPECT_FALSE(Plan.m_RenderImmediately);
		EXPECT_FALSE(Plan.m_FlushBeforeQueue);
		EXPECT_TRUE(Plan.m_QueueSprite);
	}

	{
		const SUiQuadBatchSubmissionPlan Plan = UiPlanQuadBatchSubmission(true, 7, Red, 7, Red);
		EXPECT_FALSE(Plan.m_LeavesBatchUntouched);
		EXPECT_FALSE(Plan.m_RenderImmediately);
		EXPECT_FALSE(Plan.m_FlushBeforeQueue);
		EXPECT_TRUE(Plan.m_QueueSprite);
	}

	{
		const SUiQuadBatchSubmissionPlan Plan = UiPlanQuadBatchSubmission(true, 7, Red, 8, Red);
		EXPECT_FALSE(Plan.m_LeavesBatchUntouched);
		EXPECT_FALSE(Plan.m_RenderImmediately);
		EXPECT_TRUE(Plan.m_FlushBeforeQueue);
		EXPECT_TRUE(Plan.m_QueueSprite);
	}

	{
		const SUiQuadBatchSubmissionPlan Plan = UiPlanQuadBatchSubmission(true, 7, Red, 7, Blue);
		EXPECT_FALSE(Plan.m_LeavesBatchUntouched);
		EXPECT_FALSE(Plan.m_RenderImmediately);
		EXPECT_TRUE(Plan.m_FlushBeforeQueue);
		EXPECT_TRUE(Plan.m_QueueSprite);
	}
}

TEST(QmMonitoringHelpers, SettingsPerfWindowAccumulatesFpsAndFrameStats)
{
	CQmSettingsPerfWindowTracker Tracker;
	Tracker.StartFixedFrameWindow("settings_open", "online", "settings:tee", "none", 30, false);

	Tracker.RecordFrame(0.010f, 12.0, false);
	Tracker.RecordFrame(0.020f, 30.0, false);
	Tracker.RecordFrame(0.0f, 99.0, false);
	Tracker.RecordFrame(-1.0f, 99.0, false);

	EXPECT_TRUE(Tracker.HasActiveWindow());
	const SQmSettingsPerfWindowSummary Summary = Tracker.FinishActiveWindow();

	EXPECT_STREQ(Summary.m_aOperation, "settings_open");
	EXPECT_STREQ(Summary.m_aContext, "online");
	EXPECT_STREQ(Summary.m_aPage, "settings:tee");
	EXPECT_EQ(Summary.m_SampleFrames, 2);
	EXPECT_NEAR(Summary.m_SampleSeconds, 0.030f, 0.0001f);
	EXPECT_NEAR(Summary.m_FpsAvg, 66.666f, 0.01f);
	EXPECT_NEAR(Summary.m_FpsMin, 50.0f, 0.01f);
	EXPECT_NEAR(Summary.m_FpsMax, 100.0f, 0.01f);
	EXPECT_NEAR(Summary.m_FpsOnePctLow, 50.0f, 0.01f);
	EXPECT_NEAR(Summary.m_FrameMsAvg, 15.0f, 0.01f);
	EXPECT_NEAR(Summary.m_FrameMsP95, 20.0f, 0.01f);
	EXPECT_NEAR(Summary.m_FrameMsP99, 20.0f, 0.01f);
	EXPECT_NEAR(Summary.m_FrameMsMax, 20.0f, 0.01f);
	EXPECT_NEAR(Summary.m_MenuMsMax, 30.0f, 0.01f);
	EXPECT_FALSE(Summary.m_CapLimited);
	EXPECT_FALSE(Tracker.HasActiveWindow());
}

TEST(QmMonitoringHelpers, SettingsPerfWindowLogsOnePercentLowFps)
{
	const std::string Header = ReadRepoFile("src/game/client/components/qmclient/settings_perf_windows.h");
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");

	EXPECT_NE(Header.find("m_FpsOnePctLow"), std::string::npos);
	EXPECT_EQ(Header.find("m_FpsOnePctLow = m_Summary.m_FrameMsP99 > 0.0f ? 1000.0f / m_Summary.m_FrameMsP99 : 0.0f;"), std::string::npos);
	EXPECT_NE(Header.find("OnePercentLowFpsFromFrameMs"), std::string::npos);
	EXPECT_NE(Menus.find("fps_1pct_low=%.3f"), std::string::npos);
	EXPECT_NE(Menus.find("fps_1pct_source=real_sampled"), std::string::npos);
	EXPECT_NE(Menus.find("window_start_frame=%"), std::string::npos);
	EXPECT_NE(Menus.find("window_end_frame=%"), std::string::npos);
	EXPECT_NE(Menus.find("Summary.m_FpsOnePctLow"), std::string::npos);
}

// Intent: catch regression of the stable-text key mismatch root cause.
// Bug: plan-collection replay uses MenuTextSettingsContentView(Screen), visible render uses real MainView;
// sub-pixel Rect.w differences × 0.1-pixel bucket granularity (round_to_int(width * 10)) produced 8424
// key mismatches in the 2026-06-19 fresh log (qm_perf_2026-06-19_21-51-10_summary.json).
// Fix: MaxWidthBucket uses 4-pixel granularity (round_to_int(width / 4.0f) * 4) to tolerate sub-pixel drift;
// FontSize and UiScaleBucket keep 0.1 granularity (those values are stable across plan vs visible paths).
TEST(QmMonitoringHelpers, SettingsTextStyleKeyUsesFourPixelMaxWidthBucket)
{
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");

	const size_t AssignPos = Menus.find("m_MaxWidthBucket =");
	ASSERT_NE(AssignPos, std::string::npos) << "BuildMenuTextStyleKey must assign m_MaxWidthBucket";
	const std::string Tail = Menus.substr(AssignPos, 200);

	EXPECT_NE(Tail.find("round_to_int(MaxWidth / 4.0f) * 4"), std::string::npos)
		<< "MaxWidthBucket must use 4-pixel granularity";
	EXPECT_EQ(Tail.find("MenuTextBucket(MaxWidth)"), std::string::npos)
		<< "MaxWidthBucket must not use 0.1-pixel MenuTextBucket anymore";
}

// Intent: catch regression of the stable-text double-bookkeeping bug.
// Bug: CScopedMenuTextVisibleGuard constructor unconditionally cleared per-frame counters; outer shell guard
// (menus.cpp RenderMenuShell) and inner content guard (menus_settings.cpp RenderSettings) each constructed
// one. Inner constructor wiped outer's accumulation; both destructors logged when candidates > 0, producing
// 2x duplicate settings_text_usage payloads (6/18 hit-rate audit found 2:269 duplication histogram).
// Fix: guard uses stack semantics — only stack bottom (m_Previous == false) clears counters on construct
// and flushes log on destruct; nested guards inherit parent's counters and add their own on top.
TEST(QmMonitoringHelpers, SettingsTextVisibleGuardUsesStackSemantics)
{
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");

	const size_t CtorPos = Menus.find("CScopedMenuTextVisibleGuard::CScopedMenuTextVisibleGuard");
	ASSERT_NE(CtorPos, std::string::npos);
	const std::string CtorTail = Menus.substr(CtorPos, 1500);

	EXPECT_NE(CtorTail.find("if(!m_Previous)"), std::string::npos)
		<< "Constructor must gate counter reset on stack-bottom (m_Previous == false)";

	const size_t DtorPos = Menus.find("CScopedMenuTextVisibleGuard::~CScopedMenuTextVisibleGuard");
	ASSERT_NE(DtorPos, std::string::npos);
	const std::string DtorTail = Menus.substr(DtorPos, 800);

	EXPECT_NE(DtorTail.find("if(!m_Previous && m_pMenus->m_MenuTextStableCandidatesThisFrame > 0)"), std::string::npos)
		<< "Destructor must gate log on stack-bottom (m_Previous == false)";
}

TEST(QmMonitoringHelpers, RealOnePctLowUsesFrameSamples)
{
	CQmSettingsPerfWindowTracker Tracker;
	Tracker.StartFixedFrameWindow("settings_tab_switch", "offline", "settings:assets", "entity_bg", 100, false, 1000);
	SQmSettingsPerfWindowFrameResult Result;
	for(int i = 0; i < 99; ++i)
		Result = Tracker.RecordFrame(0.001f, 1.0, false, 1001 + i);
	Result = Tracker.RecordFrame(0.100f, 100.0, false, 1100);
	ASSERT_TRUE(Result.m_ShouldFlush);
	const SQmSettingsPerfWindowSummary Summary = Result.m_Summary;

	EXPECT_EQ(Summary.m_WindowStartFrame, 1000);
	EXPECT_EQ(Summary.m_WindowEndFrame, 1100);
	EXPECT_NEAR(Summary.m_FrameMsP99, 1.0f, 0.01f);
	EXPECT_NEAR(Summary.m_FpsOnePctLow, 10.0f, 0.01f);
	EXPECT_NE(Summary.m_FpsOnePctLow, Summary.m_FrameMsP99 > 0.0f ? 1000.0f / Summary.m_FrameMsP99 : 0.0f);
}

TEST(QmMonitoringHelpers, SettingsPerfWindowEndsAfterFixedFrameBudget)
{
	CQmSettingsPerfWindowTracker Tracker;
	Tracker.StartFixedFrameWindow("settings_tab_switch", "offline", "settings:tclient", "0", 3, true);

	EXPECT_FALSE(Tracker.RecordFrame(0.010f, 2.0, false).m_ShouldFlush);
	EXPECT_FALSE(Tracker.RecordFrame(0.010f, 3.0, false).m_ShouldFlush);
	const SQmSettingsPerfWindowFrameResult Result = Tracker.RecordFrame(0.010f, 4.0, false);

	ASSERT_TRUE(Result.m_ShouldFlush);
	EXPECT_EQ(Result.m_Summary.m_SampleFrames, 3);
	EXPECT_STREQ(Result.m_Summary.m_aOperation, "settings_tab_switch");
	EXPECT_STREQ(Result.m_Summary.m_aContext, "offline");
	EXPECT_STREQ(Result.m_Summary.m_aPage, "settings:tclient");
	EXPECT_STREQ(Result.m_Summary.m_aTab, "0");
	EXPECT_TRUE(Result.m_Summary.m_CapLimited);
	EXPECT_FALSE(Tracker.HasActiveWindow());
}

TEST(QmMonitoringHelpers, SettingsPerfScrollWindowEndsAfterIdleTimeout)
{
	CQmSettingsPerfWindowTracker Tracker;
	Tracker.StartScrollWindow("settings_tee_scroll", "offline", "settings:tee", "none", 0.250f, false);

	EXPECT_FALSE(Tracker.RecordFrame(0.016f, 5.0, true).m_ShouldFlush);
	EXPECT_FALSE(Tracker.RecordFrame(0.100f, 6.0, false).m_ShouldFlush);
	EXPECT_FALSE(Tracker.RecordFrame(0.149f, 7.0, false).m_ShouldFlush);
	const SQmSettingsPerfWindowFrameResult Result = Tracker.RecordFrame(0.001f, 8.0, false);

	ASSERT_TRUE(Result.m_ShouldFlush);
	EXPECT_EQ(Result.m_Summary.m_SampleFrames, 4);
	EXPECT_STREQ(Result.m_Summary.m_aOperation, "settings_tee_scroll");
	EXPECT_NEAR(Result.m_Summary.m_MenuMsMax, 8.0f, 0.01f);
	EXPECT_FALSE(Tracker.HasActiveWindow());
}

TEST(QmMonitoringHelpers, SettingsPerfWindowStartFlushesInterruptedWindow)
{
	CQmSettingsPerfWindowTracker Tracker;
	Tracker.StartFixedFrameWindow("settings_open", "offline", "settings:tee", "none", 30, false);
	Tracker.RecordFrame(0.016f, 7.0, false);
	Tracker.RecordFrame(0.017f, 8.0, false);

	const SQmSettingsPerfWindowFrameResult Interrupted = Tracker.StartScrollWindow("settings_tee_scroll", "offline", "settings:tee", "none", 0.250f, false);

	ASSERT_TRUE(Interrupted.m_ShouldFlush);
	EXPECT_STREQ(Interrupted.m_Summary.m_aOperation, "settings_open");
	EXPECT_EQ(Interrupted.m_Summary.m_SampleFrames, 2);
	EXPECT_TRUE(Tracker.HasActiveWindow());
	EXPECT_STREQ(Tracker.ActiveOperation(), "settings_tee_scroll");
}

TEST(QmMonitoringHelpers, SettingsOpenWindowIsProtectedFromStalePreviousSettingsState)
{
	const std::string MenusSource = ReadTestSourceFile("src/game/client/components/menus.cpp");
	const std::string SettingsSource = ReadTestSourceFile("src/game/client/components/menus_settings.cpp");

	EXPECT_NE(MenusSource.find("OldPage != NewPage && NewPage == PAGE_SETTINGS"), std::string::npos);
	EXPECT_NE(MenusSource.find("m_SettingsPerfLastPage = -1;"), std::string::npos);
	EXPECT_NE(SettingsSource.find("if(m_SettingsPerfLastPage != -1)"), std::string::npos);
	EXPECT_NE(SettingsSource.find("StartSettingsPerfFixedWindow(\"settings_tab_switch\""), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsPerfWindowFlushesActiveSamplesOnShutdown)
{
	const std::string MenusSource = ReadTestSourceFile("src/game/client/components/menus.cpp");

	EXPECT_NE(MenusSource.find("void CMenus::OnShutdown()"), std::string::npos);
	EXPECT_NE(MenusSource.find("if(m_SettingsPerfWindowTracker.HasActiveWindow())"), std::string::npos);
	EXPECT_NE(MenusSource.find("const SQmSettingsPerfWindowSummary Summary = m_SettingsPerfWindowTracker.FinishActiveWindow();"), std::string::npos);
	EXPECT_NE(MenusSource.find("LogSettingsPerfWindowSummary(Summary);"), std::string::npos);
}

namespace
{

	template<typename TPredicate>
	bool WaitUntil(TPredicate Predicate, std::chrono::milliseconds Timeout = std::chrono::milliseconds(200))
	{
		const auto Deadline = std::chrono::steady_clock::now() + Timeout;
		while(std::chrono::steady_clock::now() < Deadline)
		{
			if(Predicate())
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return Predicate();
	}

} // namespace

TEST(QmMonitoringHelpers, DevicePerfSnapshotCacheReturnsConsistentVersionedSnapshot)
{
	CQmDevicePerfSnapshotCache Cache;

	SQmDevicePerfSample First;
	First.m_GpuUtilPct = 11.0f;
	First.m_GpuDedicatedVramMb = 101.0f;
	First.m_GpuDedicatedVramBudgetMb = 2048.0f;
	First.m_Available = true;
	const SQmDevicePerfSnapshot FirstSnapshot = Cache.Publish(First);

	SQmDevicePerfSample Second;
	Second.m_GpuUtilPct = 27.5f;
	Second.m_GpuDedicatedVramMb = 205.0f;
	Second.m_GpuDedicatedVramBudgetMb = 4096.0f;
	Second.m_GpuSharedVramMb = 17.0f;
	Second.m_DiskReadMbPerSec = 3.5f;
	Second.m_Available = true;
	const SQmDevicePerfSnapshot Published = Cache.Publish(Second);

	const SQmDevicePerfSnapshot Read = Cache.Snapshot();
	EXPECT_GT(Published.m_Version, FirstSnapshot.m_Version);
	EXPECT_EQ(Read.m_Version, Published.m_Version);
	EXPECT_FLOAT_EQ(Read.m_Sample.m_GpuUtilPct, Second.m_GpuUtilPct);
	EXPECT_FLOAT_EQ(Read.m_Sample.m_GpuDedicatedVramMb, Second.m_GpuDedicatedVramMb);
	EXPECT_FLOAT_EQ(Read.m_Sample.m_GpuDedicatedVramBudgetMb, Second.m_GpuDedicatedVramBudgetMb);
	EXPECT_FLOAT_EQ(Read.m_Sample.m_GpuSharedVramMb, Second.m_GpuSharedVramMb);
	EXPECT_FLOAT_EQ(Read.m_Sample.m_DiskReadMbPerSec, Second.m_DiskReadMbPerSec);
	EXPECT_EQ(Read.m_Sample.m_Available, Second.m_Available);
}

TEST(QmMonitoringHelpers, DevicePerfSnapshotCacheKeepsSampleAndVersionConsistentAcrossThreads)
{
	CQmDevicePerfSnapshotCache Cache;
	std::atomic<bool> Stop{false};
	std::atomic<int> MismatchCount{0};

	std::thread Writer([&]() {
		for(uint64_t Version = 1; Version <= 2000; ++Version)
		{
			SQmDevicePerfSample Sample;
			Sample.m_GpuUtilPct = (float)Version;
			Sample.m_GpuDedicatedVramMb = (float)Version * 2.0f;
			Sample.m_GpuDedicatedVramBudgetMb = (float)Version * 4.0f;
			Sample.m_Available = true;
			Cache.Publish(Sample);
		}
		Stop.store(true, std::memory_order_release);
	});

	std::thread Reader([&]() {
		while(!Stop.load(std::memory_order_acquire))
		{
			const SQmDevicePerfSnapshot Snapshot = Cache.Snapshot();
			if(Snapshot.m_Version == 0)
				continue;
			if(Snapshot.m_Sample.m_GpuUtilPct != (float)Snapshot.m_Version ||
				Snapshot.m_Sample.m_GpuDedicatedVramMb != (float)Snapshot.m_Version * 2.0f ||
				Snapshot.m_Sample.m_GpuDedicatedVramBudgetMb != (float)Snapshot.m_Version * 4.0f)
			{
				MismatchCount.fetch_add(1, std::memory_order_relaxed);
			}
		}
	});

	Writer.join();
	Reader.join();
	EXPECT_EQ(MismatchCount.load(std::memory_order_relaxed), 0);
}

TEST(QmMonitoringHelpers, DevicePerfSamplerStateStopsWorkerOnDisableAndCanRestart)
{
	std::atomic<int> SampleCalls{0};
	auto SampleFn = [&SampleCalls]() {
		SQmDevicePerfSample Sample;
		Sample.m_GpuUtilPct = (float)SampleCalls.fetch_add(1, std::memory_order_relaxed) + 1.0f;
		Sample.m_Available = true;
		return Sample;
	};

	CQmAsyncDevicePerfSampler Sampler(SampleFn, std::chrono::milliseconds(5));
	QmUpdateDevicePerfSamplerState(Sampler, false);
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	EXPECT_EQ(SampleCalls.load(std::memory_order_relaxed), 0);

	QmUpdateDevicePerfSamplerState(Sampler, true);
	ASSERT_TRUE(WaitUntil([&]() { return SampleCalls.load(std::memory_order_relaxed) >= 2; }));

	QmUpdateDevicePerfSamplerState(Sampler, false);
	const int CallsAfterDisable = SampleCalls.load(std::memory_order_relaxed);
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	EXPECT_EQ(SampleCalls.load(std::memory_order_relaxed), CallsAfterDisable);
	const SQmDevicePerfSnapshot ClearedSnapshot = Sampler.Snapshot();
	EXPECT_EQ(ClearedSnapshot.m_Version, 0u);
	EXPECT_FALSE(ClearedSnapshot.m_Sample.m_Available);
	EXPECT_FLOAT_EQ(ClearedSnapshot.m_Sample.m_GpuUtilPct, -1.0f);

	QmUpdateDevicePerfSamplerState(Sampler, true);
	ASSERT_TRUE(WaitUntil([&]() { return SampleCalls.load(std::memory_order_relaxed) > CallsAfterDisable; }));
	Sampler.Stop();
}

TEST(QmMonitoringHelpers, DiskReadRateUsesMegabytesPerSecond)
{
	EXPECT_FLOAT_EQ(QmComputeDiskReadMbPerSec(0, 0, 1024 * 1024, 1000000000ull), -1.0f);
	EXPECT_FLOAT_EQ(QmComputeDiskReadMbPerSec(0, 1000000000ull, 1024 * 1024, 2000000000ull), 1.0f);
	EXPECT_FLOAT_EQ(QmComputeDiskReadMbPerSec(1024, 2000000000ull, 1024, 3000000000ull), 0.0f);
	EXPECT_FLOAT_EQ(QmComputeDiskReadMbPerSec(2048, 5000000000ull, 1024, 4000000000ull), -1.0f);
	EXPECT_FLOAT_EQ(QmComputeDiskReadMbPerSec(1024, 1000000000ull, 2048, 1000000000ull), -1.0f);
}

TEST(QmMonitoringHelpers, PerfConfigDefaultsUseLowThresholdWithoutJsonToggle)
{
	std::ifstream File(TestSourcePath("src/engine/shared/config_variables_qmclient.h"));
	ASSERT_TRUE(File.good());
	std::stringstream Buffer;
	Buffer << File.rdbuf();
	const std::string Source = Buffer.str();

	EXPECT_NE(Source.find("MACRO_CONFIG_INT(QmPerfDebugThresholdMs, qm_perf_debug_threshold_ms, 4, 1, 1000"), std::string::npos);
	EXPECT_EQ(Source.find("MACRO_CONFIG_INT(QmPerfJson, qm_perf_json, 0, 0, 1"), std::string::npos);
}

TEST(QmMonitoringHelpers, PerfDurationGateUsesConfiguredThreshold)
{
	const int OldThreshold = g_Config.m_QmPerfDebugThresholdMs;
	g_Config.m_QmPerfDebugThresholdMs = 4;

	EXPECT_FALSE(QmPerfShouldLogDuration(3.999));
	EXPECT_TRUE(QmPerfShouldLogDuration(4.0));
	EXPECT_TRUE(QmPerfShouldLogDuration(0.0, true));

	g_Config.m_QmPerfDebugThresholdMs = OldThreshold;
}

TEST(QmMonitoringHelpers, ProcessHighPriorityConfigExistsAndDefaultsOff)
{
	std::ifstream File(TestSourcePath("src/engine/shared/config_variables_qmclient.h"));
	ASSERT_TRUE(File.good());
	std::stringstream Buffer;
	Buffer << File.rdbuf();
	const std::string Source = Buffer.str();

	EXPECT_NE(Source.find("MACRO_CONFIG_INT(QmProcessHighPriority, qm_process_high_priority, 0, 0, 1"), std::string::npos);
	EXPECT_NE(Source.find("MACRO_CONFIG_INT(QmAssetsPreviewBudgetMbOverride, qm_assets_preview_budget_mb_override, 0, 0, 16384"), std::string::npos);
	EXPECT_NE(Source.find("MACRO_CONFIG_INT(QmAssetsPreviewBudgetPercent, qm_assets_preview_budget_percent, 8, 0, 100"), std::string::npos);
}

TEST(QmMonitoringHelpers, WindowsStartupPriorityHookIsOptionalAndGuarded)
{
	std::ifstream File(TestSourcePath("src/engine/client/client.cpp"));
	ASSERT_TRUE(File.good());
	std::stringstream Buffer;
	Buffer << File.rdbuf();
	const std::string Source = Buffer.str();

	EXPECT_NE(Source.find("#if defined(CONF_FAMILY_WINDOWS)"), std::string::npos);
	EXPECT_NE(Source.find("if(g_Config.m_QmProcessHighPriority)"), std::string::npos);
	EXPECT_NE(Source.find("SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)"), std::string::npos);
}

TEST(QmMonitoringHelpers, PerfLoggingAlwaysEmitsJsonPayload)
{
	std::ifstream File(TestSourcePath("src/game/client/components/qmclient/perf_logging.h"));
	ASSERT_TRUE(File.good());
	std::stringstream Buffer;
	Buffer << File.rdbuf();
	const std::string Source = Buffer.str();

	EXPECT_EQ(Source.find("if(g_Config.m_QmPerfJson == 0)"), std::string::npos);
	EXPECT_NE(Source.find("str_copy(aJson, \"{\", sizeof(aJson));"), std::string::npos);
	EXPECT_NE(Source.find("dbg_msg(pSystem, \"%s\", aJson);"), std::string::npos);
	EXPECT_NE(Source.find("if(!QmPerfShouldLogDuration(DurationMs, Force))"), std::string::npos);
}

TEST(QmMonitoringHelpers, PerfPayloadJsonFieldsPreserveSpaceContainingValues)
{
	char aJson[1024];
	bool First = true;
	str_copy(aJson, "{", sizeof(aJson));
	QmPerfAppendPayloadJsonFields(aJson, sizeof(aJson), First, "event=source_request skin=My Skin Name priority=visible first_visible_skin=Another Skin");
	str_append(aJson, "}", sizeof(aJson));

	EXPECT_NE(str_find(aJson, "\"skin\":\"My Skin Name\""), nullptr);
	EXPECT_NE(str_find(aJson, "\"priority\":\"visible\""), nullptr);
	EXPECT_NE(str_find(aJson, "\"first_visible_skin\":\"Another Skin\""), nullptr);
}

TEST(QmMonitoringHelpers, RuntimePerfCallsitesUseSharedLoggingHelpers)
{
	for(const char *pPath : {
		    "src/game/client/components/countryflags.cpp",
		    "src/game/client/components/menus.cpp",
		    "src/game/client/components/menus_settings_assets.cpp",
		    "src/game/client/components/section_loader.cpp",
		    "src/game/client/components/qmclient/menus_qmclient.cpp",
		    "src/game/client/components/tclient/menus_tclient.cpp",
	    })
	{
		std::ifstream File(TestSourcePath(pPath));
		ASSERT_TRUE(File.good()) << pPath;
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();
		EXPECT_EQ(Source.find("dbg_msg(\"perf/"), std::string::npos) << pPath;
	}
}

TEST(QmMonitoringHelpers, QmClientModuleDragGhostUsesPressAnchor)
{
	std::ifstream File(TestSourcePath("src/game/client/components/qmclient/menus_qmclient.cpp"));
	ASSERT_TRUE(File.good());
	std::stringstream Buffer;
	Buffer << File.rdbuf();
	const std::string Source = Buffer.str();

	const size_t PressBlockPos = Source.find("if(!InteractionBlocked && Ui()->MouseButtonClicked(0) && OverHeader)");
	ASSERT_NE(PressBlockPos, std::string::npos);
	const size_t HoldBlockPos = Source.find("if(!InteractionBlocked && s_DragState.m_pPressed == pModule && Ui()->MouseButton(0) && s_DragState.m_pDragging == nullptr)", PressBlockPos);
	ASSERT_NE(HoldBlockPos, std::string::npos);
	const std::string PressBlock = Source.substr(PressBlockPos, HoldBlockPos - PressBlockPos);

	const size_t AfterHoldPos = Source.find("if(s_DragState.m_pDragging != pModule)", HoldBlockPos);
	ASSERT_NE(AfterHoldPos, std::string::npos);
	const std::string HoldBlock = Source.substr(HoldBlockPos, AfterHoldPos - HoldBlockPos);

	EXPECT_NE(PressBlock.find("s_DragState.m_GrabOffset = vec2(Ui()->MouseX() - CardRect.x, Ui()->MouseY() - CardRect.y);"), std::string::npos);
	EXPECT_NE(PressBlock.find("s_DragState.m_DraggedWidth = CardRect.w;"), std::string::npos);
	EXPECT_NE(PressBlock.find("s_DragState.m_DraggedHeight = CardRect.h;"), std::string::npos);
	EXPECT_NE(PressBlock.find("s_DragState.m_HasDragAnchor = true;"), std::string::npos);
	EXPECT_NE(HoldBlock.find("s_DragState.m_pDragging = pModule;"), std::string::npos);
	EXPECT_EQ(HoldBlock.find("s_DragState.m_GrabOffset = vec2(Ui()->MouseX() - CardRect.x, Ui()->MouseY() - CardRect.y);"), std::string::npos);
	EXPECT_NE(Source.find("if(s_DragState.m_pDragging == nullptr || !s_DragState.m_HasDragAnchor)"), std::string::npos);
}

TEST(QmMonitoringHelpers, MenuTextPoolReplacesSettingsOnlyBoundary)
{
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Source = ReadRepoFile("src/game/client/components/menus.cpp");
	EXPECT_TRUE(ContainsAll(Header, {"m_MenuTextPool", "SMenuTextPoolEntry", "SMenuTextStyleKey", "MenuTextElement(", "DoMenuLabelStreamed(", "PrebuildSettingsMenuTextPool("}));
	EXPECT_TRUE(ContainsAll(Source, {"SettingsTextElement", "MenuTextElement", "MENU_TEXT_SCOPE_SETTINGS"}));
	EXPECT_EQ(Header.find("std::unordered_map<std::string, SSettingsTextPoolEntry> m_SettingsTextPool"), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsStableTextMissAndStaleBlockVisibleBuild)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus.cpp");
	EXPECT_TRUE(ContainsAll(Source, {"event=settings_text_miss", "event=settings_text_stale", "m_MenuTextPoolVisibleGuard", "StableTextMiss", "StableTextStale"}));
	EXPECT_TRUE(ContainsAll(Source, {"event=settings_text_usage", "m_MenuTextStableCandidatesThisFrame", "m_MenuTextStableHitsThisFrame", "m_MenuTextStableReusedThisFrame"}));
	EXPECT_TRUE(ContainsAll(Source, {"MenuTextPoolSizeForTesting", "CScopedMenuTextVisibleGuard"}));
	EXPECT_EQ(Source.find("context-checkbox-common-"), std::string::npos);
	EXPECT_TRUE(ContainsAll(Source, {
						"scope=%s page=%s tab=%d subtab=%d key=%s reason=%s plan_status=%s operation=%s frame=%",
						"%s:%d:%d:%d:%s:fs%d:al%d:mw%d:us%d:hd%d:cm%d",
						"StyleKey.m_Align",
						"StyleKey.m_MaxWidthBucket",
						"StyleKey.m_HiDpiScaleBucket",
						"StyleKey.m_CompactMode",
					}));
	EXPECT_TRUE(ContainsAll(Source, {
						"const char *CMenus::SettingsPerfStableTextScope(int Page) const",
						"str_comp(SettingsPerfActiveOperation(), \"ingame_esc_open\") == 0",
						"(void)Page;",
						"return str_comp(pActivePage, aPage) == 0 ? \"target_settings\" : \"settings\";",
						"SettingsPerfStableTextScope(Page)",
					}));
	EXPECT_TRUE(ContainsAll(Source, {"LogSettingsTextPoolCoverageGap(Client(), \"settings_text_miss\", Scope, SettingsPerfStableTextScope(Page), Page, Tab, Subtab", "LogSettingsTextPoolCoverageGap(Client(), \"settings_text_stale\", Scope, SettingsPerfStableTextScope(Page), Page, Tab, Subtab"}));
	EXPECT_TRUE(ContainsAll(Source, {"case ESettingsInvalidationReason::DPI_CHANGED: return \"dpi\";", "case ESettingsInvalidationReason::UI_SCALE_CHANGED: return \"ui_scale\";"}));

	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	EXPECT_TRUE(ContainsAll(Header, {
						"void DoSettingsMenuLabel(int Page, int Tab, int Subtab",
						"int DoSettingsButton_Menu(int Page, int Tab, int Subtab, CButtonContainer *pBC",
						"int DoSettingsButton_CheckBox(int Page, int Tab, int Subtab",
						"bool DoSettingsScrollbarOption(int Page, int Tab, int Subtab",
					}));
	EXPECT_NE(Source.find("CUIElement &TextElement = MenuTextElement(MENU_TEXT_SCOPE_SETTINGS, Page, Tab, Subtab, pTextId, StyleKey);"), std::string::npos);
	EXPECT_NE(Source.find("dbg_assert(pBC != nullptr, \"settings menu button requires a stable button container\")"), std::string::npos);
	EXPECT_EQ(Header.find("CButtonContainer *pBC = nullptr"), std::string::npos);
	EXPECT_EQ(Source.find("s_FallbackButton"), std::string::npos);
	EXPECT_NE(Source.find("DoButton_Menu(pBC, pText, Checked, pRect, Flags, nullptr, Corners, Rounding, FontFactor, Color, &TextElement)"), std::string::npos);
	EXPECT_EQ(Source.find("reason=%s\", pReason != nullptr ? pReason : \"unknown\""), std::string::npos);

	const std::string TClient = ReadRepoFile("src/game/client/components/tclient/menus_tclient.cpp");
	EXPECT_TRUE(ContainsAll(TClient, {
						 "DoSettingsButton_Menu(SETTINGS_TCLIENT, TCLIENT_TAB_BINDWHEEL, TCLIENT_TAB_BINDWHEEL, &s_OverrideButton",
						 "DoSettingsButton_Menu(SETTINGS_TCLIENT, TCLIENT_TAB_BINDWHEEL, TCLIENT_TAB_BINDWHEEL, &s_AddButton",
						 "DoSettingsButton_Menu(SETTINGS_TCLIENT, TCLIENT_TAB_BINDWHEEL, TCLIENT_TAB_BINDWHEEL, &s_RemoveButton",
					 }));
}

TEST(QmMonitoringHelpers, MenuTextPoolStaleRefreshDoesNotReinitRegisteredElement)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus.cpp");
	const size_t StaleBranch = Source.find("else if(It->second.m_Generation != m_MenuTextPoolGeneration)");
	ASSERT_NE(StaleBranch, std::string::npos);
	const size_t ReturnElement = Source.find("return It->second.m_Element;", StaleBranch);
	ASSERT_NE(ReturnElement, std::string::npos);
	const std::string Body = Source.substr(StaleBranch, ReturnElement - StaleBranch);

	EXPECT_NE(Body.find("Ui()->ResetUIElement(It->second.m_Element);"), std::string::npos);
	EXPECT_EQ(Body.find("It->second.m_Element.Init(Ui(), 1);"), std::string::npos);
}

TEST(QmMonitoringHelpers, StableSettingsHelpersRequireExplicitTextIds)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus.cpp");
	EXPECT_EQ(Source.find("ctx-scrollbar-"), std::string::npos);
	EXPECT_EQ(Source.find("ctx-checkbox-"), std::string::npos);
	EXPECT_EQ(Source.find("ctx-menu-label-"), std::string::npos);
	EXPECT_EQ(Source.find("ctx-menu-button-"), std::string::npos);
	EXPECT_EQ(Source.find("ctx-label-"), std::string::npos);
}

TEST(QmMonitoringHelpers, TClientStableTextCandidateAuditIsEmptyExceptAllowlist)
{
	const char *pFile = "src/game/client/components/tclient/menus_tclient.cpp";
	const std::string Source = ReadRepoFile(pFile);
	const std::vector<SStableTextCandidate> vCandidates = CollectRawStableTextCandidatesWithLines(Source);
	const std::vector<SStableTextRawAllow> vAllowlist = {
		{pFile, 3464, "user-generated"},
		{pFile, 3803, "dynamic-value"},
		{pFile, 3805, "localized-list-data"},
		{pFile, 3933, "localized-list-data"},
		{pFile, 3994, "localized-list-data"},
		{pFile, 4106, "user-generated"},
		{pFile, 4118, "user-generated"},
		{pFile, 4180, "localized-list-data"},
		{pFile, 4190, "dynamic-value"},
		{pFile, 4222, "localized-list-data"},
		{pFile, 4229, "dynamic-value"},
		{pFile, 4283, "localized-list-data"},
		{pFile, 4744, "dynamic-value"},
		{pFile, 4747, "dynamic-value"},
		{pFile, 4750, "dynamic-value"},
		{pFile, 4755, "user-generated"},
		{pFile, 4757, "user-generated"},
		{pFile, 5126, "dynamic-value"},
		{pFile, 5459, "localized-list-data"},
		{pFile, 5496, "localized-list-data"},
		{pFile, 5512, "localized-list-data"},
	};
	const std::vector<SStableTextCandidate> vUnexpected = FilterCandidatesNotCoveredByMenuPoolOrAllowlist(pFile, vCandidates, vAllowlist);
	EXPECT_TRUE(vUnexpected.empty()) << JoinCandidates(vUnexpected);
}

TEST(QmMonitoringHelpers, QmClientStableTextCandidateAuditIsEmptyExceptAllowlist)
{
	const char *pFile = "src/game/client/components/qmclient/menus_qmclient.cpp";
	const std::string Source = ReadRepoFile(pFile);
	const std::vector<SStableTextCandidate> vCandidates = CollectRawStableTextCandidatesWithLines(Source);
	const std::vector<SStableTextRawAllow> vAllowlist = {
		{pFile, 763, "stateful-new-label"},
		{pFile, 755, "stateful-new-label"},
		{pFile, 940, "animated-style"},
		{pFile, 941, "animated-style"},
		{pFile, 948, "animated-style"},
		{pFile, 949, "animated-style"},
		{pFile, 952, "animated-style"},
		{pFile, 956, "animated-style"},
		{pFile, 957, "animated-style"},
		{pFile, 959, "animated-style"},
		{pFile, 960, "animated-style"},
		{pFile, 961, "animated-style"},
		{pFile, 964, "animated-style"},
		{pFile, 965, "animated-style"},
		{pFile, 966, "animated-style"},
		{pFile, 967, "animated-style"},
		{pFile, 968, "animated-style"},
		{pFile, 974, "animated-style"},
		{pFile, 983, "animated-style"},
		{pFile, 991, "animated-style"},
		{pFile, 1987, "dynamic-value"},
		{pFile, 2153, "icon-only"},
		{pFile, 2480, "animated-style"},
		{pFile, 2481, "status-message"},
		{pFile, 2488, "animated-style"},
		{pFile, 2489, "status-message"},
		{pFile, 3459, "localized-list-data"},
		{pFile, 4313, "localized-list-data"},
		{pFile, 4675, "status-message"},
		{pFile, 4679, "user-generated"},
		{pFile, 4687, "user-generated"},
		{pFile, 4952, "status-message"},
		{pFile, 5183, "stateful-new-label"},
		{pFile, 5179, "stateful-new-label"},
		{pFile, 5191, "stateful-new-label"},
		{pFile, 5769, "status-message"},
		{pFile, 5765, "status-message"},
		{pFile, 5777, "status-message"},
		{pFile, 6257, "status-message"},
		{pFile, 6258, "status-message"},
		{pFile, 950, "animated-style"},
		{pFile, 951, "animated-style"},
		{pFile, 958, "animated-style"},
		{pFile, 962, "animated-style"},
		{pFile, 969, "animated-style"},
		{pFile, 970, "animated-style"},
		{pFile, 972, "animated-style"},
		{pFile, 976, "animated-style"},
		{pFile, 979, "animated-style"},
		{pFile, 980, "animated-style"},
		{pFile, 993, "animated-style"},
		{pFile, 2499, "animated-style"},
		{pFile, 2500, "animated-style"},
		{pFile, 4698, "localized-list-data"},
		{pFile, 5202, "stateful-new-label"},
		{pFile, 5788, "status-message"},
		{pFile, 947, "animated-style"},
		{pFile, 955, "animated-style"},
		{pFile, 963, "animated-style"},
		{pFile, 973, "animated-style"},
		{pFile, 975, "animated-style"},
		{pFile, 977, "animated-style"},
		{pFile, 978, "animated-style"},
		{pFile, 990, "animated-style"},
		{pFile, 2609, "animated-style"},
		{pFile, 2610, "user-generated"},
		{pFile, 7202, "stateful-new-label"},
	};
	const std::vector<SStableTextCandidate> vUnexpected = FilterCandidatesNotCoveredByMenuPoolOrAllowlist(pFile, vCandidates, vAllowlist);
	EXPECT_TRUE(vUnexpected.empty()) << JoinCandidates(vUnexpected);
	EXPECT_FALSE(IsPooledStableTextLine("Ui()->DoLabel(&TitleRect, QmNewFeatureLabel(pTitle, pNewFeatureId, aTitle, sizeof(aTitle)), LgHeadlineSizeNew, TEXTALIGN_ML);"));
	EXPECT_FALSE(IsPooledStableTextLine("Ui()->DoLabel(&Label, RainbowColor(), LgBodySize, TEXTALIGN_ML);"));
	EXPECT_FALSE(IsPooledStableTextLine("RenderQmModuleHeadline(View, pTitle, pTip, true);"));
}

TEST(QmMonitoringHelpers, BaseSettingsStableTextCandidateAuditIsEmptyExceptAllowlist)
{
	const char *pFile = "src/game/client/components/menus_settings.cpp";
	const std::string Source = ReadRepoFile(pFile);
	const std::vector<SStableTextCandidate> vCandidates = CollectRawStableTextCandidatesWithLines(Source);
	const std::vector<SStableTextRawAllow> vAllowlist = {
		{pFile, 472, "animated-style"},
		{pFile, 1446, "input-text"},
		{pFile, 1759, "dynamic-value"},
		{pFile, 1790, "localized-list-data"},
		{pFile, 1935, "localized-list-data"},
		{pFile, 1971, "localized-list-data"},
		{pFile, 1980, "localized-list-data"},
		{pFile, 1989, "localized-list-data"},
		{pFile, 2680, "search-result"},
		{pFile, 3590, "localized-list-data"},
		{pFile, 3644, "input-text"},
		{pFile, 3648, "input-text"},
		{pFile, 3838, "input-text"},
		{pFile, 4135, "localized-list-data"},
		{pFile, 4804, "status-message"},
		{pFile, 4809, "status-message"},
		{pFile, 6427, "input-text"},
		{pFile, 474, "localized-list-data"},
		{pFile, 1761, "dynamic-value"},
		{pFile, 1973, "localized-list-data"},
		{pFile, 1982, "localized-list-data"},
		{pFile, 1991, "localized-list-data"},
		{pFile, 4890, "status-message"},
		{pFile, 4895, "status-message"},
		{pFile, 6513, "input-text"},
	};
	const std::vector<SStableTextCandidate> vUnexpected = FilterCandidatesNotCoveredByMenuPoolOrAllowlist(pFile, vCandidates, vAllowlist);
	EXPECT_TRUE(vUnexpected.empty()) << JoinCandidates(vUnexpected);
}

TEST(QmMonitoringHelpers, SettingsStaticLabelsUseTextElementCache)
{
	{
		std::ifstream File(TestSourcePath("src/game/client/components/menus_settings.cpp"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();

		EXPECT_NE(Source.find("SettingsTextElement(SETTINGS_TEE, -1, \"tee-name-label\")"), std::string::npos);
		EXPECT_NE(Source.find("SettingsTextElement(SETTINGS_TEE, -1, \"tee-clan-label\")"), std::string::npos);
		EXPECT_NE(Source.find("SettingsTextElement(SETTINGS_DDNET, -1, \"ddnet-demo-title\")"), std::string::npos);
		EXPECT_NE(Source.find("SettingsTextElement(SETTINGS_DDNET, -1, \"ddnet-ghost-title\")"), std::string::npos);
		EXPECT_NE(Source.find("SettingsTextElement(SETTINGS_DDNET, -1, \"ddnet-gameplay-title\")"), std::string::npos);
		EXPECT_NE(Source.find("SettingsTextElement(SETTINGS_DDNET, -1, \"ddnet-background-title\")"), std::string::npos);
		EXPECT_NE(Source.find("SettingsTextElement(SETTINGS_DDNET, -1, \"ddnet-miscellaneous-title\")"), std::string::npos);
		EXPECT_NE(Source.find("SettingsTextElement(SETTINGS_DDNET, -1, \"ddnet-run-on-join-label\")"), std::string::npos);
		EXPECT_NE(Source.find("DoSettingsButton_CheckBox(SETTINGS_DDNET, -1, &s_UseCurrentMapId, \"Use current map as background\""), std::string::npos);
		EXPECT_NE(Source.find("DoSettingsButton_CheckBox(SETTINGS_DDNET, -1, &g_Config.m_ClBackgroundShowTilesLayers, \"Show tiles layers from BG map\""), std::string::npos);
	}
	{
		std::ifstream File(TestSourcePath("src/game/client/components/tclient/menus_tclient.cpp"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();

		EXPECT_NE(Source.find("SettingsTextElement(SETTINGS_TCLIENT, m_TClientSettingsTab, pTitle)"), std::string::npos);
		EXPECT_NE(Source.find("SettingsTextElement(SETTINGS_TCLIENT, m_TClientSettingsTab, \"tclient-visual-font-cursor-title\")"), std::string::npos);
		EXPECT_NE(Source.find("SettingsTextElement(SETTINGS_TCLIENT, m_TClientSettingsTab, \"tclient-custom-font-label\")"), std::string::npos);
	}
	{
		std::ifstream File(TestSourcePath("src/game/client/components/qmclient/menus_qmclient.cpp"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();

		EXPECT_NE(Source.find("SettingsTextElement(SETTINGS_QMCLIENT, m_QmClientSettingsTab, pTitle)"), std::string::npos);
		EXPECT_NE(Source.find("SettingsTextElement(SETTINGS_QMCLIENT, QMCLIENT_SETTINGS_TAB_VISUAL, pText)"), std::string::npos);
		EXPECT_EQ(Source.find("SettingsTextElement(SETTINGS_QMCLIENT, m_QmClientSettingsTab, pValue)"), std::string::npos);
	}
}

TEST(QmMonitoringHelpers, TClientSettingsCardsUseSharedBoxAndAlignedFirstSection)
{
	const std::string Source = ReadRepoFile("src/game/client/components/tclient/menus_tclient.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsTClientSettings(CUIRect MainView, bool PrewarmOnly)");
	ASSERT_FALSE(Body.empty());

	EXPECT_EQ(Body.find("auto DrawSectionBox = "), std::string::npos);
	EXPECT_EQ(Body.find("Ui()->RenderBatchableRect(&Section, ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f), IGraphics::CORNER_ALL, 10.0f);"), std::string::npos);
	EXPECT_NE(Body.find("DrawTClientCacheSectionBox(BoxRect);"), std::string::npos);
	EXPECT_NE(Body.find("CUIRect BoxRect = {Col.x, Col.y + TopMargin, Col.w, Height - TopMargin};"), std::string::npos);
	EXPECT_EQ(Body.find("CUIRect MeasuredColumn = Col;\n\t\tconst float Height = MeasureSection(MeasuredColumn);"), std::string::npos);
	EXPECT_EQ(Body.find("CUIRect MeasuredColumn = Col;\n\t\t\tconst float Height = MeasureSection(MeasuredColumn);"), std::string::npos);
	EXPECT_NE(Body.find("InsetTClientCacheSectionContent(MeasuredColumn);"), std::string::npos);
	EXPECT_NE(Body.find("CUIRect BoxRect = LayoutSection(MeasuredColumn, false);"), std::string::npos);
	EXPECT_NE(Body.find("BoxRect.x = Col.x;"), std::string::npos);
	EXPECT_NE(Body.find("BoxRect.w = Col.w;"), std::string::npos);
	EXPECT_NE(Body.find("InsetTClientCacheSectionContent(ContentColumn);"), std::string::npos);
	EXPECT_NE(Body.find("Col.y = ContentColumn.y;"), std::string::npos);

	const size_t ThemeSection = Source.find("SSettingsSection CMenus::BuildTClientThemeCacheSection()");
	ASSERT_NE(ThemeSection, std::string::npos);
	const size_t ThemeSectionEnd = Source.find("SSettingsSection CMenus::BuildTClientAutoReplyCacheSection()", ThemeSection);
	ASSERT_NE(ThemeSectionEnd, std::string::npos);
	const std::string ThemeBody = Source.substr(ThemeSection, ThemeSectionEnd - ThemeSection);
	EXPECT_NE(ThemeBody.find("ConfigureSplitCachedStaticLayer(S, \"Visual: Font & Cursor\""), std::string::npos);
	EXPECT_NE(ThemeBody.find("RenderTClientThemeInteractiveLayer(Col); }, Margin);"), std::string::npos);
	EXPECT_EQ(ThemeBody.find("RenderTClientThemeInteractiveLayer(Col); }, MarginBetweenSections);"), std::string::npos);

	const size_t ThemeLayout = Source.find("float CMenus::LayoutTClientThemeCacheSection(CUIRect &CurrentColumn, bool Render)");
	ASSERT_NE(ThemeLayout, std::string::npos);
	const size_t ThemeLayoutEnd = Source.find("float CMenus::RenderTClientThemeInteractiveLayer", ThemeLayout);
	ASSERT_NE(ThemeLayoutEnd, std::string::npos);
	const std::string ThemeLayoutBody = Source.substr(ThemeLayout, ThemeLayoutEnd - ThemeLayout);
	EXPECT_NE(ThemeLayoutBody.find("CurrentColumn.HSplitTop(Margin, nullptr, &CurrentColumn);"), std::string::npos);
	EXPECT_EQ(ThemeLayoutBody.find("CurrentColumn.HSplitTop(MarginBetweenSections, nullptr, &CurrentColumn);"), std::string::npos);

	const size_t AutoReplyLayout = Source.find("float CMenus::LayoutTClientAutoReplyCacheSection(CUIRect &CurrentColumn, bool Render)");
	ASSERT_NE(AutoReplyLayout, std::string::npos);
	const size_t AutoReplyLayoutEnd = Source.find("float CMenus::RenderTClientAutoReplyInteractiveLayer", AutoReplyLayout);
	ASSERT_NE(AutoReplyLayoutEnd, std::string::npos);
	const std::string AutoReplyLayoutBody = Source.substr(AutoReplyLayout, AutoReplyLayoutEnd - AutoReplyLayout);
	EXPECT_NE(AutoReplyLayoutBody.find("const float SavedY = CurrentColumn.y;"), std::string::npos);
	EXPECT_NE(AutoReplyLayoutBody.find("return CurrentColumn.y - SavedY;"), std::string::npos);
	EXPECT_EQ(AutoReplyLayoutBody.find("return CurrentColumn.y - BoxRect.y;"), std::string::npos);

	const size_t PetLayout = Source.find("float CMenus::LayoutTClientPetCacheSection(CUIRect &CurrentColumn, bool Render)");
	ASSERT_NE(PetLayout, std::string::npos);
	const size_t PetLayoutEnd = Source.find("float CMenus::RenderTClientPetInteractiveLayer", PetLayout);
	ASSERT_NE(PetLayoutEnd, std::string::npos);
	const std::string PetLayoutBody = Source.substr(PetLayout, PetLayoutEnd - PetLayout);
	EXPECT_NE(PetLayoutBody.find("const float SavedY = CurrentColumn.y;"), std::string::npos);
	EXPECT_NE(PetLayoutBody.find("return CurrentColumn.y - SavedY;"), std::string::npos);
	EXPECT_EQ(PetLayoutBody.find("return CurrentColumn.y - BoxRect.y;"), std::string::npos);

	const size_t HudLayout = Source.find("float CMenus::LayoutTClientHudCacheSection(CUIRect &CurrentColumn, bool Render)");
	ASSERT_NE(HudLayout, std::string::npos);
	const size_t HudLayoutEnd = Source.find("float CMenus::RenderTClientHudInteractiveLayer", HudLayout);
	ASSERT_NE(HudLayoutEnd, std::string::npos);
	const std::string HudLayoutBody = Source.substr(HudLayout, HudLayoutEnd - HudLayout);
	EXPECT_NE(HudLayoutBody.find("const float SavedY = CurrentColumn.y;"), std::string::npos);
	EXPECT_NE(HudLayoutBody.find("return CurrentColumn.y - SavedY;"), std::string::npos);
	EXPECT_EQ(HudLayoutBody.find("return CurrentColumn.y - BoxRect.y;"), std::string::npos);

	const size_t DrawBoxRect = Source.find("CUIRect CMenus::TClientCacheSectionBoxRect(CUIRect BoxRect) const");
	ASSERT_NE(DrawBoxRect, std::string::npos);
	const size_t DrawBoxRectEnd = Source.find("void CMenus::InsetTClientCacheSectionContent", DrawBoxRect);
	ASSERT_NE(DrawBoxRectEnd, std::string::npos);
	const std::string DrawBoxRectBody = Source.substr(DrawBoxRect, DrawBoxRectEnd - DrawBoxRect);
	EXPECT_NE(DrawBoxRectBody.find("BoxRect.h += Padding;"), std::string::npos);
	EXPECT_NE(DrawBoxRectBody.find("BoxRect.y -= Padding * 0.5f;"), std::string::npos);
	EXPECT_EQ(DrawBoxRectBody.find("BoxRect.w += Padding;"), std::string::npos);
	EXPECT_EQ(DrawBoxRectBody.find("BoxRect.x -= Padding * 0.5f;"), std::string::npos);

	const size_t DrawBox = Source.find("void CMenus::DrawTClientCacheSectionBox(CUIRect BoxRect)");
	ASSERT_NE(DrawBox, std::string::npos);
	const size_t DrawBoxEnd = Source.find("float CMenus::RenderTClientCacheSectionFallback", DrawBox);
	ASSERT_NE(DrawBoxEnd, std::string::npos);
	const std::string DrawBoxBody = Source.substr(DrawBox, DrawBoxEnd - DrawBox);
	EXPECT_EQ(DrawBoxBody.find("CUi::ms_DarkButtonColorFunction.GetColor(false, false)"), std::string::npos);
	EXPECT_EQ(Source.find("ColorRGBA TClientCacheSectionBackgroundColor()"), std::string::npos);
	EXPECT_EQ(Source.find("return ColorRGBA(0.08f, 0.085f, 0.09f, 0.92f);"), std::string::npos);
	EXPECT_NE(DrawBoxBody.find("BoxRect.Draw(Ui()->ScaleBackgroundAlpha(MenuPanelColor(0.92f)), IGraphics::CORNER_ALL, 10.0f);"), std::string::npos);
	EXPECT_EQ(DrawBoxBody.find("Ui()->RenderBatchableRect(&BoxRect"), std::string::npos);
	EXPECT_EQ(DrawBoxBody.find("BoxRect.w += Padding;"), std::string::npos);
	EXPECT_EQ(DrawBoxBody.find("BoxRect.x -= Padding * 0.5f;"), std::string::npos);
	EXPECT_NE(DrawBoxBody.find("BoxRect = TClientCacheSectionBoxRect(BoxRect);"), std::string::npos);

	const size_t InsetHelper = Source.find("void CMenus::InsetTClientCacheSectionContent(CUIRect &ContentRect) const");
	ASSERT_NE(InsetHelper, std::string::npos);
	const size_t InsetHelperEnd = Source.find("void CMenus::DrawTClientCacheSectionBox", InsetHelper);
	ASSERT_NE(InsetHelperEnd, std::string::npos);
	const std::string InsetHelperBody = Source.substr(InsetHelper, InsetHelperEnd - InsetHelper);
	EXPECT_NE(InsetHelperBody.find("ContentRect.VSplitLeft(Margin, nullptr, &ContentRect);"), std::string::npos);
	EXPECT_NE(InsetHelperBody.find("ContentRect.VSplitRight(Margin, &ContentRect, nullptr);"), std::string::npos);

	const std::string BindChatBody = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsTClientChatBinds(CUIRect MainView)");
	ASSERT_FALSE(BindChatBody.empty());
	EXPECT_EQ(BindChatBody.find("Background.w += Padding;"), std::string::npos);
	EXPECT_EQ(BindChatBody.find("Background.x -= Padding * 0.5f;"), std::string::npos);
	EXPECT_EQ(BindChatBody.find("Background.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f), IGraphics::CORNER_ALL, 10.0f);"), std::string::npos);
	EXPECT_NE(BindChatBody.find("DrawTClientCacheSectionBox(Section);"), std::string::npos);
	EXPECT_NE(BindChatBody.find("s_ScrollRegion.AddRect(TClientCacheSectionBoxRect(Section))"), std::string::npos);
	EXPECT_NE(BindChatBody.find("InsetTClientCacheSectionContent(ContentColumn);"), std::string::npos);
}

TEST(QmMonitoringHelpers, QmClientFocusModeSectionLabelsUseDisplayTextNotTranslationKeys)
{
	const std::string Source = ReadRepoFile("src/game/client/components/qmclient/menus_qmclient.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsQmClient(CUIRect MainView, bool ContributorsPage, bool PrewarmOnly)");
	ASSERT_FALSE(Body.empty());
	const size_t FocusCardAnchor = Body.find("RenderQmModuleHeadline(CardContent, 2, Localize(\"Zen Mode\")");
	ASSERT_NE(FocusCardAnchor, std::string::npos);
	const size_t FocusModeCase = Body.rfind("case EQmModuleId::FocusMode:", FocusCardAnchor);
	ASSERT_NE(FocusModeCase, std::string::npos);
	const size_t FocusModeEnd = Body.find("case EQmModuleId::WeaponAnimation:", FocusModeCase);
	ASSERT_NE(FocusModeEnd, std::string::npos);
	const std::string FocusModeBody = Body.substr(FocusModeCase, FocusModeEnd - FocusModeCase);

	EXPECT_NE(FocusModeBody.find("auto DoFocusSectionLabel = [&](CUIRect &Target, const char *pTextId, const char *pLabel)"), std::string::npos);
	EXPECT_EQ(FocusModeBody.find("Localize(pTextId)"), std::string::npos);
	EXPECT_NE(FocusModeBody.find("DoQmSettingsLabel(pTextId, &Row, Localize(pLabel), LgBodySize * 0.82f);"), std::string::npos);
	EXPECT_NE(FocusModeBody.find("DoFocusSectionLabel(LeftColumn, \"qmclient-focus-section-interface\", \"Interface\");"), std::string::npos);
	EXPECT_NE(FocusModeBody.find("DoFocusSectionLabel(LeftColumn, \"qmclient-focus-section-players\", \"Players\");"), std::string::npos);
	EXPECT_NE(FocusModeBody.find("DoFocusSectionLabel(LeftColumn, \"qmclient-focus-section-visuals\", \"Visuals\");"), std::string::npos);
	EXPECT_NE(FocusModeBody.find("DoFocusSectionLabel(RightColumn, \"qmclient-focus-section-audio\", \"Audio\");"), std::string::npos);
	EXPECT_NE(FocusModeBody.find("DoFocusSectionLabel(RightColumn, \"qmclient-focus-section-chat\", \"Chat\");"), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsTextColdStartAvoidsGlobalLanguageCacheAndCachesCheckboxLabels)
{
	{
		std::ifstream File(TestSourcePath("src/game/client/components/menus_settings.cpp"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();

		EXPECT_EQ(Source.find("PrepareLanguagePageCache(MainView.w);"), std::string::npos);
		EXPECT_EQ(Source.find("if(g_Config.m_UiSettingsPage == SETTINGS_LANGUAGE)"), std::string::npos);
		EXPECT_NE(Source.find("PrepareLanguagePageCache(List.w, true);"), std::string::npos);
		EXPECT_EQ(Source.find("PrepareLanguagePageCache(MainView.w, false)"), std::string::npos);
		EXPECT_NE(Source.find("PrepareLanguagePageCache(Right.w, false);"), std::string::npos);
		EXPECT_NE(Source.find("SettingsWarmupConsumeBudget(m_SettingsFrameBudget, ESettingsWarmupCost::TEXT_CONTAINER)"), std::string::npos);
		EXPECT_NE(Source.find("const bool TextChanged = RectEl.m_Text != Language.m_Name.c_str();"), std::string::npos);
		EXPECT_NE(Source.find("const bool SizeChanged = RectEl.m_Width != Label.w || RectEl.m_Height != Label.h;"), std::string::npos);
		EXPECT_NE(Source.find("const bool NeedsTextContainer = !RectEl.m_UITextContainer.Valid() || ColorChanged || TextChanged || SizeChanged;"), std::string::npos);
		EXPECT_NE(Source.find("DoSettingsButton_CheckBox(SETTINGS_GENERAL"), std::string::npos);
		EXPECT_NE(Source.find("DoSettingsButton_CheckBox(SETTINGS_GRAPHICS"), std::string::npos);
		EXPECT_NE(Source.find("DoSettingsButton_CheckBox(SETTINGS_SOUND"), std::string::npos);
		EXPECT_NE(Source.find("DoSettingsButton_CheckBox(SETTINGS_DDNET"), std::string::npos);
	}
	{
		std::ifstream File(TestSourcePath("src/game/client/components/menus.cpp"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();

		EXPECT_NE(Source.find("int CMenus::DoSettingsButton_CheckBox(int Page, int Tab, const void *pId, const char *pTextId, const char *pText, int Checked, const CUIRect *pRect)"), std::string::npos);
		EXPECT_NE(Source.find("int CMenus::DoSettingsButton_CheckBox(int Page, int Tab, int Subtab, const void *pId, const char *pTextId, const char *pText, int Checked, const CUIRect *pRect)"), std::string::npos);
		EXPECT_NE(Source.find("const SMenuTextStyleKey StyleKey = BuildMenuTextStyleKey(&Label, Box.h * CUi::ms_FontmodHeight, TEXTALIGN_ML, Props);"), std::string::npos);
		EXPECT_NE(Source.find("CUIElement &LabelElement = MenuTextElement(MENU_TEXT_SCOPE_SETTINGS, Page, Tab, Subtab, pTextId, StyleKey);"), std::string::npos);
		EXPECT_NE(Source.find("DoButton_CheckBox_Common_WithLabelElement(pId, pText, Checked ? \"X\" : \"\", pRect, BUTTONFLAG_LEFT, &LabelElement);"), std::string::npos);
		EXPECT_NE(Source.find("DoSettingsLabelStreamed(*pLabelElement, &Label, pText, FontSize, TEXTALIGN_ML, Props);"), std::string::npos);
		EXPECT_NE(Source.find("int CMenus::DoButton_CheckBox_Common(const void *pId, const char *pText, const char *pBoxText, const CUIRect *pRect, const unsigned Flags)"), std::string::npos);
		EXPECT_EQ(Source.find("context-checkbox-common-"), std::string::npos);
		EXPECT_EQ(Source.find("m_SettingsTextContextPage >= 0 && pText != nullptr && pText[0] != '\\0'"), std::string::npos);
		EXPECT_NE(Source.find("void CMenus::DoMenuLabelStreamed(EMenuTextScope Scope, CUIElement &Element, const CUIRect *pRect, const char *pText"), std::string::npos);
		EXPECT_NE(Source.find("if(pText == nullptr)"), std::string::npos);
		EXPECT_NE(Source.find("Ui()->DoLabel(&Label, pText, FontSize, TEXTALIGN_ML, Props);"), std::string::npos);
	}
}

TEST(QmMonitoringHelpers, SettingsTextPlanPrebuildSeparatesInvisibleWarmupFromVisibleRender)
{
	{
		std::ifstream File(TestSourcePath("src/game/client/components/menus.h"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();

		EXPECT_NE(Source.find("struct SMenuTextPlanItem"), std::string::npos);
		EXPECT_NE(Source.find("std::string m_TextId;"), std::string::npos);
		EXPECT_NE(Source.find("std::string m_Text;"), std::string::npos);
		EXPECT_NE(Source.find("enum EMenuTextStyleMode"), std::string::npos);
		EXPECT_NE(Source.find("MENU_TEXT_STYLE_DEFAULT"), std::string::npos);
		EXPECT_NE(Source.find("MENU_TEXT_STYLE_RECT"), std::string::npos);
		EXPECT_NE(Source.find("MENU_TEXT_STYLE_EXACT"), std::string::npos);
		EXPECT_NE(Source.find("MENU_TEXT_STYLE_ALLOWLIST_DYNAMIC"), std::string::npos);
		EXPECT_NE(Source.find("EMenuTextStyleMode m_StyleMode"), std::string::npos);
		EXPECT_NE(Source.find("std::string m_AllowlistReason;"), std::string::npos);
		EXPECT_NE(Source.find("std::string m_SourceTag;"), std::string::npos);
		EXPECT_EQ(Source.find("bool m_UseExplicitStyleKey = false;"), std::string::npos);
		EXPECT_NE(Source.find("MENU_TEXT_SCOPE_INGAME"), std::string::npos);
		EXPECT_NE(Source.find("SMenuTextPlanItem AddStableTextDefault("), std::string::npos);
		EXPECT_NE(Source.find("SMenuTextPlanItem AddStableTextLabel("), std::string::npos);
		EXPECT_NE(Source.find("SMenuTextPlanItem AddStableTextCheckbox("), std::string::npos);
		EXPECT_NE(Source.find("SMenuTextPlanItem AddStableTextScrollbar("), std::string::npos);
		EXPECT_NE(Source.find("SMenuTextPlanItem AddStableTextButton("), std::string::npos);
		EXPECT_NE(Source.find("void BuildTClientSettingsMenuTextPlan(std::vector<SMenuTextPlanItem> &vItems"), std::string::npos);
		EXPECT_NE(Source.find("void BuildQmClientSettingsMenuTextPlan(std::vector<SMenuTextPlanItem> &vItems"), std::string::npos);
		EXPECT_NE(Source.find("void BuildIngameMenuTextPlan(std::vector<SMenuTextPlanItem> &vItems, CUIRect MainView);"), std::string::npos);
		EXPECT_NE(Source.find("void BuildBaseSettingsMenuTextPlan(std::vector<SMenuTextPlanItem> &vItems, CUIRect MainView);"), std::string::npos);
		EXPECT_NE(Source.find("void BuildSettingsMenuTextPlan(std::vector<SMenuTextPlanItem> &vItems, CUIRect MainView);"), std::string::npos);
		EXPECT_NE(Source.find("bool PrebuildSettingsTextPlanItem(const SMenuTextPlanItem &Item, int &RemainingBudget);"), std::string::npos);
		EXPECT_NE(Source.find("int CountMissingSettingsMenuTextPlanItems() const;"), std::string::npos);
		EXPECT_NE(Source.find("int PrebuildSettingsTextPoolForLoading(int Budget, const char *pOperationOverride = nullptr);"), std::string::npos);
		EXPECT_EQ(Source.find("void PrebuildVisibleSettingsTextPool(const CUIRect &MainView, int Budget);"), std::string::npos);
		EXPECT_TRUE(ContainsAll(Source, {
							"std::vector<SMenuTextPlanItem> m_vSettingsMenuTextPrebuildPlan;",
							"std::unordered_set<std::string> m_SettingsMenuTextPlannedKeys;",
							"size_t m_SettingsMenuTextPlanCursor",
							"uint64_t m_SettingsMenuTextPlanGeneration",
							"SSettingsMenuTextPrebuildStats m_SettingsMenuTextLastPrebuildStats",
							"m_MenuTextStablePlannedThisFrame",
							"m_MenuTextStableUnplannedThisFrame",
							"std::unordered_set<std::string> m_SettingsMenuTextPlannedDescriptors;",
						}));
		EXPECT_NE(Source.find("bool DoSettingsScrollbarOption(int Page, int Tab, const char *pTextId, const void *pId, int *pOption, const CUIRect *pRect, const char *pStr, int Min, int Max, const IScrollbarScale *pScale = &CUi::ms_LinearScrollbarScale, unsigned Flags = 0u, const char *pSuffix = \"\", const char *pMaxText = nullptr);"), std::string::npos);
	}
	{
		std::ifstream File(TestSourcePath("src/game/client/components/menus.cpp"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();
		const std::string LoadingBody = ExtractSourceFunctionBody(Source, "int CMenus::PrebuildSettingsTextPoolForLoading(int Budget, const char *pOperationOverride)");

		EXPECT_NE(Source.find("bool CMenus::PrebuildSettingsTextPlanItem(const SMenuTextPlanItem &Item, int &RemainingBudget)"), std::string::npos);
		const std::string PrebuildItemBody = ExtractSourceFunctionBody(Source, "bool CMenus::PrebuildSettingsTextPlanItem(const SMenuTextPlanItem &Item, int &RemainingBudget)");
		EXPECT_NE(Source.find("void CMenus::BuildBaseSettingsMenuTextPlan(std::vector<SMenuTextPlanItem> &vItems, CUIRect MainView)"), std::string::npos);
		EXPECT_EQ(Source.find("AddDefaultStyleItem("), std::string::npos);
		EXPECT_NE(Source.find("static constexpr int s_aBaseSettingsPages[]"), std::string::npos);
		EXPECT_NE(Source.find("RenderSettings(MainView);"), std::string::npos);
		EXPECT_NE(Source.find("SETTINGS_GENERAL"), std::string::npos);
		EXPECT_NE(Source.find("SETTINGS_DDNET"), std::string::npos);
		EXPECT_EQ(Source.find("AddGeneralItem(\""), std::string::npos);
		EXPECT_EQ(Source.find("AddGeneralCheckbox(\""), std::string::npos);
		EXPECT_EQ(Source.find("AddStableTextDefault(SETTINGS_TEE"), std::string::npos);
		EXPECT_NE(Source.find("void CMenus::BuildIngameMenuTextPlan(std::vector<SMenuTextPlanItem> &vItems, CUIRect MainView)"), std::string::npos);
		EXPECT_NE(Source.find("\"ingame-tab-server-info\""), std::string::npos);
		EXPECT_NE(Source.find("RenderMenubar(TabBar, IClient::STATE_ONLINE);"), std::string::npos);
		EXPECT_NE(Source.find("RenderServerInfo(ContentView);"), std::string::npos);
		EXPECT_NE(Source.find("plan_status=%s"), std::string::npos);
		EXPECT_NE(Source.find("planned=%d unplanned=%d"), std::string::npos);
		EXPECT_NE(Source.find("MenuTextDescriptorKey("), std::string::npos);
		EXPECT_NE(Source.find("const bool HasDescriptor = m_SettingsMenuTextPlannedDescriptors.find(MenuTextDescriptorKey("), std::string::npos);
		EXPECT_NE(Source.find("HasDescriptor ? (KeyPlanned ? \"not_built\" : \"key_mismatch\") : \"missing_descriptor\""), std::string::npos);
		EXPECT_NE(Source.find("Box.Margin(2.0f, &Box);\n\tSLabelProperties Props;"), std::string::npos);
		EXPECT_NE(Source.find("void CMenus::BuildSettingsMenuTextPlan(std::vector<SMenuTextPlanItem> &vItems, CUIRect MainView)"), std::string::npos);
		EXPECT_NE(Source.find("CUIRect SettingsMainView = MenuTextSettingsContentView(Screen);"), std::string::npos);
		EXPECT_NE(Source.find("BuildVisibleSettingsMenuTextPlan(vVisibleItems, SettingsMainView);"), std::string::npos);
		EXPECT_NE(Source.find("BuildIngameMenuTextPlan(vItems, Screen);"), std::string::npos);
		EXPECT_NE(Source.find("int CMenus::CountMissingSettingsMenuTextPlanItems()"), std::string::npos);
		EXPECT_NE(Source.find("It->second.m_Generation != m_MenuTextPoolGeneration"), std::string::npos);
		EXPECT_NE(Source.find("int CMenus::PrebuildSettingsTextPoolForLoading(int Budget, const char *pOperationOverride)"), std::string::npos);
		EXPECT_EQ(Source.find("void CMenus::PrebuildVisibleSettingsTextPool(const CUIRect &MainView, int Budget)"), std::string::npos);
		ASSERT_FALSE(PrebuildItemBody.empty());
		EXPECT_NE(PrebuildItemBody.find("pRect->m_UITextContainer.Valid()"), std::string::npos);
		EXPECT_NE(PrebuildItemBody.find("Entry.m_Built = true;"), std::string::npos);
		EXPECT_NE(PrebuildItemBody.find("Entry.m_Generation = m_MenuTextPoolGeneration;"), std::string::npos);
		EXPECT_NE(Source.find("bool CMenus::DoSettingsScrollbarOption(int Page, int Tab, const char *pTextId, const void *pId, int *pOption, const CUIRect *pRect, const char *pStr, int Min, int Max, const IScrollbarScale *pScale, unsigned Flags, const char *pSuffix, const char *pMaxText)"), std::string::npos);
		EXPECT_EQ(Source.find("RenderSettingsTClient(ContentView, true);"), std::string::npos);
		EXPECT_EQ(Source.find("RenderSettingsQmClient(ContentView, false, true);"), std::string::npos);
		EXPECT_EQ(Source.find("PrebuildVisibleSettingsTextPool(ContentView"), std::string::npos);
		ASSERT_FALSE(LoadingBody.empty());
		EXPECT_NE(LoadingBody.find("m_vSettingsMenuTextPrebuildPlan"), std::string::npos);
		EXPECT_NE(LoadingBody.find("m_SettingsMenuTextPlanCursor"), std::string::npos);
		EXPECT_EQ(LoadingBody.find("BuildSettingsMenuTextPlan(vItems);"), std::string::npos);
		EXPECT_EQ(LoadingBody.find("PrebuildSettingsTClientTextPool("), std::string::npos);
		EXPECT_EQ(LoadingBody.find("PrebuildSettingsQmClientTextPool("), std::string::npos);
		EXPECT_EQ(LoadingBody.find("const int LastPage = SettingsCanonicalPage(m_SettingsRuntimeMetadata.m_LastPage);"), std::string::npos);
	}
	{
		const std::string TClient = ReadRepoFile("src/game/client/components/tclient/menus_tclient.cpp");
		const std::string QmClient = ReadRepoFile("src/game/client/components/qmclient/menus_qmclient.cpp");
		const std::string Settings = ReadRepoFile("src/game/client/components/menus_settings.cpp");
		const std::string ScrollRegionHeader = ReadRepoFile("src/game/client/ui_scrollregion.h");
		const std::string TClientPlanBody = ExtractSourceFunctionBody(TClient, "void CMenus::BuildTClientSettingsMenuTextPlan(std::vector<SMenuTextPlanItem> &vItems, CUIRect MainView, int Tab)");
		const std::string QmClientPlanBody = ExtractSourceFunctionBody(QmClient, "void CMenus::BuildQmClientSettingsMenuTextPlan(std::vector<SMenuTextPlanItem> &vItems, CUIRect MainView, int Tab)");
		const std::string TClientSettingsBody = ExtractSourceFunctionBody(TClient, "void CMenus::RenderSettingsTClientSettings(CUIRect MainView, bool PrewarmOnly)");
		EXPECT_NE(TClient.find("void CMenus::BuildTClientSettingsMenuTextPlan(std::vector<SMenuTextPlanItem> &vItems"), std::string::npos);
		EXPECT_NE(QmClient.find("void CMenus::BuildQmClientSettingsMenuTextPlan(std::vector<SMenuTextPlanItem> &vItems"), std::string::npos);
		ASSERT_FALSE(TClientPlanBody.empty());
		ASSERT_FALSE(QmClientPlanBody.empty());
		EXPECT_NE(TClientPlanBody.find("g_Config.m_UiSettingsPage = SETTINGS_TCLIENT;"), std::string::npos);
		EXPECT_NE(QmClientPlanBody.find("g_Config.m_UiSettingsPage = SETTINGS_QMCLIENT;"), std::string::npos);
		EXPECT_NE(TClientPlanBody.find("RenderSettings(MainView);"), std::string::npos);
		EXPECT_NE(QmClientPlanBody.find("RenderSettings(MainView);"), std::string::npos);
		EXPECT_EQ(TClientPlanBody.find("RenderSettingsTClient(MainView, true);"), std::string::npos);
		EXPECT_EQ(QmClientPlanBody.find("RenderSettingsQmClient(MainView, false, true);"), std::string::npos);
		EXPECT_NE(TClient.find("m_MenuTextPlanCollecting = true;"), std::string::npos);
		EXPECT_NE(QmClient.find("m_MenuTextPlanCollecting = true;"), std::string::npos);
		EXPECT_NE(TClient.find("m_pMenuTextPlanCollection = &vItems;"), std::string::npos);
		EXPECT_NE(QmClient.find("m_pMenuTextPlanCollection = &vItems;"), std::string::npos);
		EXPECT_NE(TClient.find("m_MenuTextPlanCollecting = PreviousCollecting;"), std::string::npos);
		EXPECT_NE(QmClient.find("m_MenuTextPlanCollecting = PreviousCollecting;"), std::string::npos);
		ASSERT_FALSE(TClientSettingsBody.empty());
		EXPECT_NE(ScrollRegionHeader.find("void SetContentHeightForNextFrame(float ContentHeight);"), std::string::npos);
		EXPECT_EQ(TClientSettingsBody.find("LogSettingsStage(\"tclient_settings_right_prewarm\", RightColumnTimer);\n\t\t\treturn;"), std::string::npos);
		EXPECT_NE(TClientSettingsBody.find("s_ScrollRegion.SetContentHeightForNextFrame("), std::string::npos);
		EXPECT_NE(TClientSettingsBody.find("FinishSettingsScrollRegion(s_ScrollRegion, ScrollFrame, &ScrollRegion, SETTINGS_TCLIENT);"), std::string::npos);
		EXPECT_LT(TClientSettingsBody.find("s_ScrollRegion.SetContentHeightForNextFrame("), TClientSettingsBody.find("FinishSettingsScrollRegion(s_ScrollRegion, ScrollFrame, &ScrollRegion, SETTINGS_TCLIENT);"));
		EXPECT_EQ(Settings.find("DoButton_CheckBox(&g_Config.m_ClAutoDemoRecord"), std::string::npos);
		EXPECT_EQ(Settings.find("DoButton_CheckBox(&g_Config.m_ClAutoScreenshot"), std::string::npos);
		EXPECT_EQ(Settings.find("DoButton_CheckBox(&g_Config.m_ClAutoStatboardScreenshot"), std::string::npos);
		EXPECT_EQ(Settings.find("DoButton_CheckBox(&g_Config.m_ClAutoCSV"), std::string::npos);
		EXPECT_NE(Settings.find("const bool CollectingMenuTextPlan = m_MenuTextPlanCollecting;"), std::string::npos);
		EXPECT_NE(Settings.find("const bool SettingsPerfEnabled = PerfDebugEnabled() && !CollectingMenuTextPlan;"), std::string::npos);
		EXPECT_NE(Settings.find("if(!CollectingMenuTextPlan && g_Config.m_UiSettingsPage != SETTINGS_ASSETS"), std::string::npos);
		EXPECT_NE(Settings.find("if(!CollectingMenuTextPlan)"), std::string::npos);
		EXPECT_NE(Settings.find("if(!s_SettingsTransitionInitialized)"), std::string::npos);
		EXPECT_NE(Settings.find("if(!CollectingMenuTextPlan && m_SettingsPerfLastPage == g_Config.m_UiSettingsPage)"), std::string::npos);
		EXPECT_NE(Settings.find("m_SettingsPerfLastPage = g_Config.m_UiSettingsPage;"), std::string::npos);
		EXPECT_NE(Settings.find("\"general-auto-demo-record\""), std::string::npos);
		EXPECT_NE(Settings.find("\"general-auto-screenshot\""), std::string::npos);
		EXPECT_NE(Settings.find("\"general-auto-statboard-screenshot\""), std::string::npos);
		EXPECT_NE(Settings.find("\"general-auto-csv\""), std::string::npos);
	}
	{
		std::ifstream File(TestSourcePath("src/game/client/gameclient.cpp"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();
		const std::string Body = ExtractSourceFunctionBody(Source, "void CGameClient::PrewarmSettingsRuntimeCachesDuringLoading(const char *pLoadingCaption, const char *pLoadingMessage)");

		ASSERT_FALSE(Body.empty());
		EXPECT_EQ(Body.find("return;"), std::string::npos);
		EXPECT_NE(Body.find("m_Menus.PrewarmSettingsPages();"), std::string::npos);
		EXPECT_NE(Body.find("m_Menus.PrewarmSettingsTextPoolForLoading("), std::string::npos);
		EXPECT_NE(Body.find("SettingsLoadingPrewarmAdvance("), std::string::npos);
		EXPECT_EQ(Body.find("while(SettingsLoadingPrewarmShouldKeepPumping"), std::string::npos);
	}
}

TEST(QmMonitoringHelpers, SettingsTextPlanCoversHighValueTClientAndQmClientStaticLabels)
{
	{
		const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
		const std::string Settings = ReadRepoFile("src/game/client/components/menus_settings.cpp");

		EXPECT_NE(Settings.find("DoSettingsMenuLabel(SETTINGS_GENERAL, -1, -1, \"game-title\""), std::string::npos);
		EXPECT_NE(Settings.find("DoSettingsMenuLabel(SETTINGS_GENERAL, -1, -1, \"language-title\""), std::string::npos);
		EXPECT_NE(Settings.find("DoSettingsMenuLabel(SETTINGS_GENERAL, -1, -1, \"client-title\""), std::string::npos);
		EXPECT_NE(Settings.find("DoSettingsButton_CheckBox(SETTINGS_GENERAL, -1, &g_Config.m_ClDyncam, \"general-dynamic-camera\""), std::string::npos);
		EXPECT_NE(Settings.find("DoSettingsButton_CheckBox(SETTINGS_GENERAL, -1, &g_Config.m_ClAutoswitchWeapons, \"general-switch-weapon-pickup\""), std::string::npos);
		EXPECT_NE(Settings.find("DoSettingsButton_CheckBox(SETTINGS_GENERAL, -1, &g_Config.m_ClAutoswitchWeaponsOutOfAmmo, \"general-switch-weapon-out-of-ammo\""), std::string::npos);
		EXPECT_NE(Settings.find("DoSettingsButton_CheckBox(SETTINGS_GENERAL, -1, &g_Config.m_ClSkipStartMenu, \"general-skip-main-menu\""), std::string::npos);
		EXPECT_NE(Settings.find("DoSettingsScrollbarOption(SETTINGS_GENERAL, -1, \"general-refresh-rate\""), std::string::npos);
		EXPECT_NE(Settings.find("DoSettingsButton_CheckBox(SETTINGS_GENERAL, -1, &s_LowerRefreshRate, \"general-lower-refresh-rate\""), std::string::npos);
		EXPECT_NE(Settings.find("DoSettingsButton_Menu(SETTINGS_GENERAL, -1, -1, &s_SettingsButtonId, \"general-settings-file\""), std::string::npos);

		EXPECT_NE(Menus.find("static constexpr int s_aBaseSettingsPages[]"), std::string::npos);
		EXPECT_NE(Menus.find("RenderSettings(MainView);"), std::string::npos);
		EXPECT_EQ(Menus.find("AddGeneralItem(\""), std::string::npos);
		EXPECT_EQ(Menus.find("AddGeneralCheckbox(\""), std::string::npos);
		EXPECT_EQ(Menus.find("AddGeneralScrollbar(\""), std::string::npos);
		EXPECT_EQ(Menus.find("AddGeneralMenuButton(\""), std::string::npos);
	}
	{
		std::ifstream File(TestSourcePath("src/game/client/components/tclient/menus_tclient.cpp"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();

		EXPECT_EQ(Source.find("DoSettingsScrollbarOption(SETTINGS_TCLIENT, m_TClientSettingsTab, m_TClientSettingsTab, nullptr"), std::string::npos);
		EXPECT_NE(Source.find("DoSettingsLabel(SETTINGS_TCLIENT, m_TClientSettingsTab, \"tclient-hammer-mode\""), std::string::npos);
		EXPECT_NE(Source.find("DoSettingsScrollbarOption(SETTINGS_TCLIENT, m_TClientSettingsTab, \"tclient-cursor-scale\""), std::string::npos);
		EXPECT_NE(Source.find("DoSettingsScrollbarOption(SETTINGS_TCLIENT, m_TClientSettingsTab, m_TClientSettingsTab, \"tclient-prediction-margin\""), std::string::npos);
		EXPECT_NE(Source.find("DoSettingsScrollbarOption(SETTINGS_TCLIENT, m_TClientSettingsTab, \"tclient-pet-size\""), std::string::npos);
		const std::string PlanBody = ExtractSourceFunctionBody(Source, "void CMenus::BuildTClientSettingsMenuTextPlan(std::vector<SMenuTextPlanItem> &vItems, CUIRect MainView, int Tab)");
		ASSERT_FALSE(PlanBody.empty());
		EXPECT_NE(PlanBody.find("RenderSettings(MainView);"), std::string::npos);
		EXPECT_EQ(PlanBody.find("RenderSettingsTClient(MainView, true);"), std::string::npos);
		EXPECT_EQ(Source.find("AddItem(\"tclient-wheel-animate-ms\""), std::string::npos);
		EXPECT_EQ(Source.find("AddItem(\"tclient-pet-alpha\""), std::string::npos);
		EXPECT_EQ(Source.find("AddItem(\"tclient-indicator-offset\""), std::string::npos);
		EXPECT_EQ(Source.find("AddItem(\"tclient-outline-width\""), std::string::npos);
		EXPECT_EQ(Source.find("AddItem(\"tclient-bg-draw-fade-time\""), std::string::npos);
		EXPECT_NE(Source.find("DoSettingsMenuLabel(SETTINGS_TCLIENT, m_TClientSettingsTab, m_TClientSettingsTab, \"tclient-auto-reply-title\""), std::string::npos);
		EXPECT_NE(Source.find("DoSettingsMenuLabel(SETTINGS_TCLIENT, m_TClientSettingsTab, m_TClientSettingsTab, \"tclient-player-indicator-title\""), std::string::npos);
		EXPECT_NE(Source.find("SettingsTextElement(SETTINGS_TCLIENT, m_TClientSettingsTab, \"tclient-tee-status-bar-title\")"), std::string::npos);
		EXPECT_EQ(Source.find("else if(Tab == TCLIENT_TAB_BINDCHAT)"), std::string::npos);
		EXPECT_EQ(Source.find("for(const auto &[pTitle, vBindDefaults] : CBindChat::BIND_DEFAULTS)"), std::string::npos);
		EXPECT_EQ(Source.find("AddItem(BindDefault.m_pTitle, Localize(BindDefault.m_pTitle), 210.0f, LineSize, FontSize);"), std::string::npos);
	}
	{
		std::ifstream File(TestSourcePath("src/game/client/components/qmclient/menus_qmclient.cpp"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();

		EXPECT_NE(Source.find("SettingsTextElement(SETTINGS_QMCLIENT, m_QmClientSettingsTab, \"qmclient-chat-bubble-duration\")"), std::string::npos);
		EXPECT_NE(Source.find("SettingsTextElement(SETTINGS_QMCLIENT, m_QmClientSettingsTab, \"qmclient-chat-bubble-opacity\")"), std::string::npos);
		EXPECT_NE(Source.find("SettingsTextElement(SETTINGS_QMCLIENT, m_QmClientSettingsTab, \"qmclient-display-mode\")"), std::string::npos);
		EXPECT_NE(Source.find("SettingsTextElement(SETTINGS_QMCLIENT, m_QmClientSettingsTab, \"qmclient-translation-service\")"), std::string::npos);
		EXPECT_NE(Source.find("SettingsTextElement(SETTINGS_QMCLIENT, m_QmClientSettingsTab, \"qmclient-target-language\")"), std::string::npos);
		EXPECT_NE(Source.find("SettingsTextElement(SETTINGS_QMCLIENT, m_QmClientSettingsTab, \"qmclient-llm-provider\")"), std::string::npos);
		const std::string RenderBody = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsQmClient(CUIRect MainView, bool ContributorsPage, bool PrewarmOnly)");
		ASSERT_FALSE(RenderBody.empty());
		EXPECT_NE(RenderBody.find("auto DoQmSettingsCheckboxAuto ="), std::string::npos);
		EXPECT_NE(RenderBody.find("const int OriginalValue = *pValue;"), std::string::npos);
		EXPECT_NE(RenderBody.find("if(PrewarmOnly || Ui()->RenderOnly())"), std::string::npos);
		EXPECT_NE(RenderBody.find("*pValue = OriginalValue;"), std::string::npos);
		EXPECT_NE(RenderBody.find("const char *pTextId"), std::string::npos);
		EXPECT_EQ(RenderBody.find("return DoQmSettingsCheckbox(pId, pText, pText"), std::string::npos);
		EXPECT_EQ(RenderBody.find("DoQmSettingsCheckboxAuto(&g_Config.m_QmFootParticles, Localize(\"Local particle effects\")"), std::string::npos);
		EXPECT_EQ(RenderBody.find("DoButton_CheckBoxAutoVMarginAndSet("), std::string::npos);
		const std::string PlanBody = ExtractSourceFunctionBody(Source, "void CMenus::BuildQmClientSettingsMenuTextPlan(std::vector<SMenuTextPlanItem> &vItems, CUIRect MainView, int Tab)");
		ASSERT_FALSE(PlanBody.empty());
		EXPECT_NE(PlanBody.find("RenderSettings(MainView);"), std::string::npos);
		EXPECT_EQ(PlanBody.find("RenderSettingsQmClient(MainView, false, true);"), std::string::npos);
	}
}

TEST(QmMonitoringHelpers, SettingsStableTextRegistryCoversVisibleWrappers)
{
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Settings = ReadRepoFile("src/game/client/components/menus_settings.cpp");
	const std::string TClient = ReadRepoFile("src/game/client/components/tclient/menus_tclient.cpp");
	const std::string QmClient = ReadRepoFile("src/game/client/components/qmclient/menus_qmclient.cpp");
	const std::string Ingame = ReadRepoFile("src/game/client/components/menus_ingame.cpp");

	EXPECT_NE(Menus.find("AddStableTextLabel("), std::string::npos);
	EXPECT_NE(Menus.find("AddStableTextCheckbox("), std::string::npos);
	EXPECT_NE(Menus.find("AddStableTextScrollbar("), std::string::npos);
	EXPECT_NE(Menus.find("AddStableTextButton("), std::string::npos);
	EXPECT_NE(Menus.find("CollectMenuTextPlanItem("), std::string::npos);
	EXPECT_NE(Menus.find("m_MenuTextPlanCollecting"), std::string::npos);
	const std::string TClientPlanBody = ExtractSourceFunctionBody(TClient, "void CMenus::BuildTClientSettingsMenuTextPlan(std::vector<SMenuTextPlanItem> &vItems, CUIRect MainView, int Tab)");
	const std::string QmClientPlanBody = ExtractSourceFunctionBody(QmClient, "void CMenus::BuildQmClientSettingsMenuTextPlan(std::vector<SMenuTextPlanItem> &vItems, CUIRect MainView, int Tab)");
	ASSERT_FALSE(TClientPlanBody.empty());
	ASSERT_FALSE(QmClientPlanBody.empty());
	EXPECT_NE(TClientPlanBody.find("RenderSettings(MainView);"), std::string::npos);
	EXPECT_NE(QmClientPlanBody.find("RenderSettings(MainView);"), std::string::npos);
	EXPECT_EQ(TClientPlanBody.find("RenderSettingsTClient(MainView, true);"), std::string::npos);
	EXPECT_EQ(QmClientPlanBody.find("RenderSettingsQmClient(MainView, false, true);"), std::string::npos);
	EXPECT_NE(TClient.find("const bool PreviousCollecting = m_MenuTextPlanCollecting;"), std::string::npos);
	EXPECT_NE(QmClient.find("const bool PreviousCollecting = m_MenuTextPlanCollecting;"), std::string::npos);
	EXPECT_NE(TClient.find("m_MenuTextPlanCollecting = true;"), std::string::npos);
	EXPECT_NE(QmClient.find("m_MenuTextPlanCollecting = true;"), std::string::npos);
	EXPECT_NE(TClient.find("m_MenuTextPlanCollecting = PreviousCollecting;"), std::string::npos);
	EXPECT_NE(QmClient.find("m_MenuTextPlanCollecting = PreviousCollecting;"), std::string::npos);
	EXPECT_EQ(TClient.find("auto AddItem = [&]"), std::string::npos);
	EXPECT_EQ(QmClient.find("auto AddItem = [&]"), std::string::npos);
	EXPECT_EQ(Menus.find("Item.m_UseExplicitStyleKey"), std::string::npos);
	EXPECT_NE(Menus.find("CMenus::SMenuTextStyleKey CMenus::SettingsMenuTextPlanStyleKey(const SMenuTextPlanItem &Item) const"), std::string::npos);
	EXPECT_NE(Menus.find("switch(Item.m_StyleMode)"), std::string::npos);

	const std::vector<std::string> vRequiredBaseIds = {
		"\"game-title\"",
		"\"language-title\"",
		"\"client-title\"",
		"\"tee-name-label\"",
		"\"tee-clan-label\"",
		"\"ddnet-demo-title\"",
		"\"ddnet-ghost-title\"",
		"\"ddnet-gameplay-title\"",
		"\"ddnet-background-title\"",
		"\"ddnet-miscellaneous-title\"",
		"\"ddnet-run-on-join-label\"",
		"\"Save the best demo of each race\"",
		"\"Enable replays\"",
		"\"Enable ghost\"",
		"\"Show text entities\"",
		"\"Show others\"",
		"\"Show others (own team only)\"",
		"\"Show background quads\"",
		"\"Predict events (experimental)\"",
		"\"AntiPing (latency compensation)\"",
		"\"Use current map as background\"",
		"\"Show tiles layers from BG map\"",
	};
	for(const std::string &Id : vRequiredBaseIds)
	{
		EXPECT_NE(Settings.find(Id), std::string::npos) << Id;
	}
	EXPECT_NE(Menus.find("static constexpr int s_aBaseSettingsPages[]"), std::string::npos);
	EXPECT_NE(Menus.find("RenderSettings(MainView);"), std::string::npos);
	EXPECT_EQ(Menus.find("AddStableTextDefault(SETTINGS_TEE"), std::string::npos);
	EXPECT_EQ(Menus.find("AddGeneralCheckbox(\""), std::string::npos);

	const std::vector<std::string> vRequiredTClientIds = {
		"\"tclient-outline-width\"",
		"\"tclient-statusbar-main-title\"",
		"\"tclient-statusbar-codes-title\"",
		"\"tclient-statusbar-local-time-title\"",
		"\"tclient-statusbar-colors-title\"",
		"\"tclient-statusbar-empty-preview\"",
		"\"tclient-statusbar-scheme-label\"",
		"\"tclient-statusbar-show\"",
		"\"tclient-statusbar-show-labels\"",
		"\"tclient-statusbar-height\"",
		"\"tclient-statusbar-12-hour-clock\"",
		"\"tclient-statusbar-seconds\"",
		"\"tclient-statusbar-alpha\"",
		"\"tclient-statusbar-text-alpha\"",
		"\"tclient-statusbar-apply-scheme\"",
		"\"tclient-statusbar-add-item\"",
		"\"tclient-statusbar-remove-item\"",
	};
	for(const std::string &Id : vRequiredTClientIds)
	{
		EXPECT_NE(TClient.find(Id), std::string::npos) << Id;
	}
	EXPECT_EQ(TClient.find("AddStableTextScrollbar(SETTINGS_TCLIENT"), std::string::npos);
	EXPECT_EQ(TClient.find("AddStableTextCheckbox(SETTINGS_TCLIENT"), std::string::npos);
	EXPECT_EQ(TClient.find("AddStableTextButton(SETTINGS_TCLIENT"), std::string::npos);

	const std::vector<std::string> vRequiredQmClientIds = {
		"\"qmclient-chat-bubble-duration\"",
		"\"qmclient-chat-bubble-opacity\"",
		"\"qmclient-display-mode\"",
		"\"qmclient-translation-service\"",
		"\"qmclient-target-language\"",
		"\"qmclient-llm-provider\"",
	};
	for(const std::string &Id : vRequiredQmClientIds)
	{
		EXPECT_NE(QmClient.find(Id), std::string::npos) << Id;
	}
	EXPECT_EQ(QmClient.find("AddStableText"), std::string::npos);

	const std::vector<std::string> vRequiredIngameIds = {
		"\"ingame-tab-game\"",
		"\"ingame-tab-players\"",
		"\"ingame-tab-server-info\"",
		"\"ingame-tab-browser\"",
		"\"ingame-tab-ghost\"",
		"\"ingame-tab-call-vote\"",
		"\"ingame-server-info-title\"",
		"\"ingame-server-info-address-label\"",
		"\"ingame-server-info-ping-label\"",
		"\"ingame-server-info-version-label\"",
		"\"ingame-server-info-password-label\"",
		"\"ingame-server-info-community-label\"",
		"\"ingame-game-info-title\"",
		"\"ingame-game-info-type-label\"",
		"\"ingame-game-info-map-label\"",
		"\"ingame-game-info-players-label\"",
		"\"ingame-server-info-motd-title\"",
	};
	for(const std::string &Id : vRequiredIngameIds)
	{
		EXPECT_TRUE(Menus.find(Id) != std::string::npos || Ingame.find(Id) != std::string::npos) << Id;
	}
	EXPECT_NE(Menus.find("DoIngameMenuTab("), std::string::npos);
	EXPECT_NE(Menus.find("BuildIngameMenuTextPlan(vItems, Screen);"), std::string::npos);
	EXPECT_NE(Menus.find("RenderMenubar(TabBar, IClient::STATE_ONLINE);"), std::string::npos);
	EXPECT_NE(Menus.find("RenderServerInfo(ContentView);"), std::string::npos);
	EXPECT_EQ(Menus.find("AddIngameTab("), std::string::npos);
	EXPECT_NE(Menus.find("return DoMenuTabV2(pButtonContainer, pText, Checked != 0, pRect, Corners, nullptr, nullptr, nullptr, nullptr, &TextElement);"), std::string::npos);
	EXPECT_EQ(Menus.find("DoMenuTabV2(&s_ServerInfoButton, Localize(\"Server info\")"), std::string::npos);
	EXPECT_NE(Ingame.find("DoIngameMenuTitleLabel(PAGE_SERVER_INFO, \"ingame-server-info-title\""), std::string::npos);
	EXPECT_NE(Ingame.find("DoIngameMenuTitleLabel(PAGE_SERVER_INFO, \"ingame-game-info-title\""), std::string::npos);
	EXPECT_NE(Ingame.find("DoIngameMenuTitleLabel(PAGE_SERVER_INFO, \"ingame-server-info-motd-title\""), std::string::npos);
	EXPECT_EQ(Ingame.find("DoIngameMenuLabel(PAGE_SERVER_INFO, \"ingame-server-info-name\""), std::string::npos);
	EXPECT_EQ(Ingame.find("DoIngameMenuLabel(PAGE_SERVER_INFO, \"ingame-game-info-map\", &Label, aBuf"), std::string::npos);
	const size_t OnlineBranch = Menus.find("case IClient::STATE_ONLINE:");
	ASSERT_NE(OnlineBranch, std::string::npos);
	const size_t IngameGuard = Menus.find("TextVisibleGuard.emplace(this);", OnlineBranch);
	const size_t IngameContent = Menus.find("if(m_GamePage == PAGE_GAME)", OnlineBranch);
	const size_t IngameSettings = Menus.find("RenderSettings(MainView);", IngameContent);
	const size_t IngameMenubar = Menus.find("RenderMenubar(TabBar, ClientState);", OnlineBranch);
	ASSERT_NE(IngameGuard, std::string::npos);
	ASSERT_NE(IngameContent, std::string::npos);
	ASSERT_NE(IngameSettings, std::string::npos);
	ASSERT_NE(IngameMenubar, std::string::npos);
	EXPECT_LT(IngameGuard, IngameContent);
	EXPECT_LT(IngameGuard, IngameSettings);
	EXPECT_LT(IngameGuard, IngameMenubar);
	const size_t OfflineBranch = Menus.find("case IClient::STATE_OFFLINE:");
	ASSERT_NE(OfflineBranch, std::string::npos);
	const size_t OfflineGuard = Menus.find("std::optional<CScopedMenuTextVisibleGuard> TextVisibleGuard;", OfflineBranch);
	ASSERT_NE(OfflineGuard, std::string::npos);
	const size_t OfflineContent = Menus.find("CPerfTimer ContentTimer;", OfflineGuard);
	ASSERT_NE(OfflineContent, std::string::npos);
	const std::string OfflineGuardBody = Menus.substr(OfflineGuard, OfflineContent - OfflineGuard);
	EXPECT_NE(OfflineGuardBody.find("if(m_MenuPage == PAGE_SETTINGS)"), std::string::npos);
	EXPECT_EQ(OfflineGuardBody.find("if(m_GamePage != PAGE_SETTINGS)"), std::string::npos);
	EXPECT_EQ(Menus.find("Props.m_MaxWidth = Width;"), std::string::npos);
	EXPECT_EQ(Menus.find("CUIRect{0.0f, 0.0f, Width, Height - 4.0f}"), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsStableTextPlanKeysMatchVisibleWrappers)
{
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Settings = ReadRepoFile("src/game/client/components/menus_settings.cpp");

	const std::string ScrollbarBody = ExtractSourceFunctionBody(Menus, "CMenus::SMenuTextPlanItem CMenus::AddStableTextScrollbar(int Page, int Tab, int Subtab, const char *pTextId, const char *pText, const CUIRect &Rect, unsigned Flags, const char *pSourceTag) const");
	ASSERT_FALSE(ScrollbarBody.empty());
	EXPECT_NE(ScrollbarBody.find("BuildSettingsScrollbarTextStyle("), std::string::npos);

	const std::string ScrollbarOptionBody = ExtractSourceFunctionBody(Menus, "bool CMenus::DoSettingsScrollbarOption(int Page, int Tab, int Subtab, const char *pTextId, const void *pId, int *pOption, const CUIRect *pRect, const char *pStr, int Min, int Max, const IScrollbarScale *pScale, unsigned Flags, const char *pSuffix, const char *pMaxText)");
	ASSERT_FALSE(ScrollbarOptionBody.empty());
	EXPECT_NE(ScrollbarOptionBody.find("BuildSettingsScrollbarTextStyle("), std::string::npos);
	EXPECT_EQ(ScrollbarOptionBody.find("DoSettingsMenuLabel(Page, Tab, Subtab, pTextId, &Label, pStr, FontSize, TEXTALIGN_ML, {}, (int)Label.w);"), std::string::npos);

	EXPECT_NE(Header.find("SMenuTextStyleKey BuildSettingsScrollbarTextStyle(const CUIRect &Rect, unsigned Flags, CUIRect *pOutLabel = nullptr) const;"), std::string::npos);
	EXPECT_NE(Settings.find("DoSettingsScrollbarOption(SETTINGS_APPEARANCE, APPEARANCE_TAB_HUD, \"appearance-freeze-bars-alpha-inside-freeze\""), std::string::npos);
	EXPECT_NE(Header.find("SMenuTextStyleKey BuildSettingsShellTitleTextStyle(const CUIRect &Rect, CUIRect *pOutLabel = nullptr) const;"), std::string::npos);
	EXPECT_NE(Menus.find("BuildSettingsShellTitleTextStyle("), std::string::npos);
	EXPECT_NE(Menus.find("settings-shell-title"), std::string::npos);
	EXPECT_NE(Settings.find("DoSettingsScrollbarOption(SETTINGS_DDNET, -1, \"ddnet-default-zoom\""), std::string::npos);
	EXPECT_NE(Settings.find("DoSettingsButton_CheckBox(SETTINGS_DDNET, -1, &g_Config.m_ClRaceGhost, \"Enable ghost\""), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsStableTextPrebuildCompletesTargetPlan)
{
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string PrebuildBody = ExtractSourceFunctionBody(Menus, "int CMenus::PrebuildSettingsTextPoolForLoading(int Budget, const char *pOperationOverride)");
	const std::string CountBody = ExtractSourceFunctionBody(Menus, "int CMenus::CountMissingSettingsMenuTextPlanItems()");
	const std::string LogBody = ExtractSourceFunctionBody(Menus, "void CMenus::PrebuildSettingsMenuTextPool(int Budget, const char *pScopeOverride, const char *pOperationOverride)");

	ASSERT_FALSE(PrebuildBody.empty());
	ASSERT_FALSE(CountBody.empty());
	ASSERT_FALSE(LogBody.empty());
	EXPECT_NE(Header.find("int CountMissingSettingsMenuTextPlanItems() const;"), std::string::npos);
	EXPECT_NE(CountBody.find("m_vSettingsMenuTextPrebuildPlan"), std::string::npos);
	EXPECT_EQ(CountBody.find("BuildSettingsMenuTextPlan(vItems);"), std::string::npos);
	EXPECT_NE(PrebuildBody.find("m_SettingsMenuTextLastPrebuildStats.m_Remaining = CountMissingSettingsMenuTextPlanItems();"), std::string::npos);
	EXPECT_NE(LogBody.find("const int RemainingMissing = m_SettingsMenuTextLastPrebuildStats.m_Remaining;"), std::string::npos);
	EXPECT_EQ(LogBody.find("const int RemainingMissing = CountMissingSettingsMenuTextPlanItems();"), std::string::npos);
	EXPECT_EQ(CountBody.find("for(const SMenuTextPlanItem &Item : m_vSettingsMenuTextPrebuildPlan)"), std::string::npos);
	EXPECT_NE(CountBody.find("m_SettingsMenuTextPlanCollectionComplete"), std::string::npos);
	EXPECT_NE(CountBody.find("m_vSettingsMenuTextPrebuildPlan.size() - (int)m_SettingsMenuTextPlanCursor"), std::string::npos);
}

TEST(QmMonitoringHelpers, IngameEscStableTextRegistryCoversMenubar)
{
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string PlanBody = ExtractSourceFunctionBody(Menus, "void CMenus::BuildIngameMenuTextPlan(std::vector<SMenuTextPlanItem> &vItems, CUIRect MainView)");
	const std::string PrebuildBody = ExtractSourceFunctionBody(Menus, "int CMenus::PrebuildSettingsTextPoolForLoading(int Budget, const char *pOperationOverride)");
	const std::string CollectionBody = ExtractSourceFunctionBody(Menus, "void CMenus::PrepareSettingsMenuTextPlanCollectionUnits(const char *pOperationOverride)");
	ASSERT_FALSE(PlanBody.empty());
	ASSERT_FALSE(PrebuildBody.empty());
	ASSERT_FALSE(CollectionBody.empty());

	EXPECT_NE(Header.find("void PrebuildIngameEscTextPoolBeforeOpen(int Budget);"), std::string::npos);
	EXPECT_NE(Menus.find("PrebuildIngameEscTextPoolBeforeOpen("), std::string::npos);
	EXPECT_EQ(Menus.find("PrebuildIngameEscTextPoolBeforeOpen(96);"), std::string::npos);
	EXPECT_EQ(CollectionBody.find("str_comp(pOperation, \"ingame_esc_open\") == 0"), std::string::npos);
	EXPECT_NE(Menus.find("BuildIngameMenuTextPlan(m_vSettingsMenuTextPrebuildPlan, Screen);"), std::string::npos);
	EXPECT_NE(PrebuildBody.find("AdvanceSettingsMenuTextPlanCollection("), std::string::npos);
	for(const char *pId : {
		    "\"ingame-tab-game\"",
		    "\"ingame-tab-players\"",
		    "\"ingame-tab-server-info\"",
		    "\"ingame-tab-browser\"",
		    "\"ingame-tab-ghost\"",
		    "\"ingame-tab-call-vote\"",
	    })
	{
		EXPECT_NE(PlanBody.find(pId), std::string::npos) << pId;
	}
	EXPECT_EQ(Menus.find("plan_status\":\"missing_descriptor\""), std::string::npos);
}

TEST(QmMonitoringHelpers, StartupTextPrewarmCollectsIngamePlanIncrementally)
{
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string PrebuildBody = ExtractSourceFunctionBody(Menus, "int CMenus::PrebuildSettingsTextPoolForLoading(int Budget, const char *pOperationOverride)");
	const std::string CollectionBody = ExtractSourceFunctionBody(Menus, "void CMenus::PrepareSettingsMenuTextPlanCollectionUnits(const char *pOperationOverride)");
	ASSERT_FALSE(PrebuildBody.empty());
	ASSERT_FALSE(CollectionBody.empty());

	EXPECT_EQ(CollectionBody.find("const bool IngameEscOperation = str_comp(pOperation, \"ingame_esc_open\") == 0;"), std::string::npos);
	EXPECT_EQ(CollectionBody.find("if(IngameEscOperation)"), std::string::npos);
	EXPECT_NE(CollectionBody.find("MENU_TEXT_PLAN_UNIT_INGAME_ESC"), std::string::npos);
	EXPECT_NE(PrebuildBody.find("AdvanceSettingsMenuTextPlanCollection("), std::string::npos);
	EXPECT_EQ(PrebuildBody.find("BuildSettingsMenuTextPlan(vItems, SettingsMainView);"), std::string::npos);
	EXPECT_EQ(PrebuildBody.find("str_comp(pOperation, \"ingame_esc_open\") != 0"), std::string::npos);
}

TEST(QmMonitoringHelpers, LoadingAndEscPrebuildDoNotSynchronouslyBuildFullSettingsPlan)
{
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string PrebuildBody = ExtractSourceFunctionBody(Menus, "int CMenus::PrebuildSettingsTextPoolForLoading(int Budget, const char *pOperationOverride)");
	const std::string EscBody = ExtractSourceFunctionBody(Menus, "void CMenus::PrebuildIngameEscTextPoolBeforeOpen(int Budget)");
	const std::string EnsureBody = ExtractSourceFunctionBody(Menus, "void CMenus::EnsureSettingsMenuTextPlanReadyForVisible()");
	ASSERT_FALSE(PrebuildBody.empty());
	ASSERT_FALSE(EscBody.empty());
	ASSERT_FALSE(EnsureBody.empty());

	EXPECT_NE(Header.find("void BuildVisibleSettingsMenuTextPlan(std::vector<SMenuTextPlanItem> &vItems, CUIRect MainView);"), std::string::npos);
	EXPECT_NE(PrebuildBody.find("AdvanceSettingsMenuTextPlanCollection("), std::string::npos);
	EXPECT_EQ(PrebuildBody.find("BuildSettingsMenuTextPlan(vItems, SettingsMainView);"), std::string::npos);
	EXPECT_EQ(EscBody.find("BuildSettingsMenuTextPlan(m_vSettingsMenuTextPrebuildPlan, SettingsMainView);"), std::string::npos);
	EXPECT_NE(EscBody.find("PrebuildSettingsMenuTextPool(minimum(Budget, maximum(1, AdaptiveBudget.m_TextPrebuildTokens)), \"target_settings\", \"ingame_esc_open\");"), std::string::npos);
	EXPECT_NE(EnsureBody.find("BuildVisibleSettingsMenuTextPlan(vVisibleItems, SettingsMainView);"), std::string::npos);
	EXPECT_EQ(EnsureBody.find("BuildSettingsMenuTextPlan(m_vSettingsMenuTextPrebuildPlan);"), std::string::npos);
}

TEST(QmMonitoringHelpers, IngameEscPrewarmsStableTextBeforeVisibleFrame)
{
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string OnRenderBody = ExtractSourceFunctionBody(Menus, "void CMenus::OnRender()");
	const std::string CollectionBody = ExtractSourceFunctionBody(Menus, "void CMenus::PrepareSettingsMenuTextPlanCollectionUnits(const char *pOperationOverride)");
	const std::string CollectBody = ExtractSourceFunctionBody(Menus, "void CMenus::CollectSettingsMenuTextPlanUnit(const SSettingsMenuTextPlanCollectionUnit &Unit, CUIRect Screen, CUIRect SettingsMainView)");
	ASSERT_FALSE(OnRenderBody.empty());
	ASSERT_FALSE(CollectionBody.empty());
	ASSERT_FALSE(CollectBody.empty());

	const size_t StartWindowPos = OnRenderBody.find("StartSettingsPerfFixedWindow(\"ingame_esc_open\"");
	const size_t SetActivePos = OnRenderBody.find("SetActive(true);", StartWindowPos);
	const size_t RenderPos = OnRenderBody.find("Render();");
	ASSERT_NE(StartWindowPos, std::string::npos);
	ASSERT_NE(SetActivePos, std::string::npos);
	ASSERT_NE(RenderPos, std::string::npos);
	EXPECT_LT(StartWindowPos, SetActivePos);
	EXPECT_LT(SetActivePos, RenderPos);

	EXPECT_NE(Header.find("uint64_t m_IngameEscOpenFrame"), std::string::npos);
	EXPECT_NE(Header.find("bool m_IngameServerInfoBackgroundPrepareRequested"), std::string::npos);
	EXPECT_EQ(OnRenderBody.find("PrebuildIngameEscTextPoolBeforeOpen("), std::string::npos);
	EXPECT_EQ(OnRenderBody.find("PrebuildSettingsMenuTextPool("), std::string::npos);
	EXPECT_NE(OnRenderBody.find("Client()->PerfFrame() > m_IngameEscOpenFrame"), std::string::npos);
	EXPECT_NE(OnRenderBody.find("PrepareIngameServerInfoTextRuntime();"), std::string::npos);
	EXPECT_NE(CollectionBody.find("m_vSettingsMenuTextPlanCollectionUnits.push_back({MENU_TEXT_PLAN_UNIT_INGAME_ESC, -1, -1});"), std::string::npos);
	EXPECT_NE(CollectBody.find("case MENU_TEXT_PLAN_UNIT_INGAME_ESC:"), std::string::npos);
	EXPECT_NE(CollectBody.find("BuildIngameMenuTextPlan(m_vSettingsMenuTextPrebuildPlan, Screen);"), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsMenuTextPlanCollectionUsesIncrementalCursor)
{
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string PrebuildBody = ExtractSourceFunctionBody(Menus, "int CMenus::PrebuildSettingsTextPoolForLoading(int Budget, const char *pOperationOverride)");
	const std::string EscBody = ExtractSourceFunctionBody(Menus, "void CMenus::PrebuildIngameEscTextPoolBeforeOpen(int Budget)");
	ASSERT_FALSE(PrebuildBody.empty());
	ASSERT_FALSE(EscBody.empty());

	EXPECT_NE(Header.find("struct SSettingsMenuTextPlanCollectionUnit"), std::string::npos);
	EXPECT_NE(Header.find("struct SSettingsMenuTextPlanCollectionStats"), std::string::npos);
	EXPECT_NE(Header.find("std::vector<SSettingsMenuTextPlanCollectionUnit> m_vSettingsMenuTextPlanCollectionUnits;"), std::string::npos);
	EXPECT_NE(Header.find("size_t m_SettingsMenuTextPlanCollectionCursor"), std::string::npos);
	EXPECT_NE(Header.find("uint64_t m_SettingsMenuTextPlanCollectionGeneration"), std::string::npos);
	EXPECT_NE(Header.find("bool m_SettingsMenuTextPlanCollectionDirty"), std::string::npos);
	EXPECT_NE(Header.find("bool m_SettingsMenuTextPlanCollectionComplete"), std::string::npos);
	EXPECT_NE(Header.find("SSettingsMenuTextPlanCollectionStats m_SettingsMenuTextLastCollectionStats"), std::string::npos);
	EXPECT_NE(Header.find("int SettingsTextPlanCollectionRemaining() const"), std::string::npos);

	EXPECT_NE(Menus.find("void CMenus::PrepareSettingsMenuTextPlanCollectionUnits(const char *pOperationOverride)"), std::string::npos);
	EXPECT_NE(Menus.find("bool CMenus::AdvanceSettingsMenuTextPlanCollection(int Budget, const char *pOperationOverride)"), std::string::npos);
	EXPECT_NE(Menus.find("void CMenus::CollectSettingsMenuTextPlanUnit(const SSettingsMenuTextPlanCollectionUnit &Unit, CUIRect Screen, CUIRect SettingsMainView)"), std::string::npos);
	EXPECT_NE(Menus.find("event=settings_text_plan_collection"), std::string::npos);
	EXPECT_NE(Menus.find("units_done=%d units_total=%d remaining=%d budget=%d complete=%d dirty=%d phase=%s scope=%s operation=%s"), std::string::npos);

	EXPECT_NE(PrebuildBody.find("AdvanceSettingsMenuTextPlanCollection("), std::string::npos);
	EXPECT_LT(PrebuildBody.find("AdvanceSettingsMenuTextPlanCollection("), PrebuildBody.find("while(m_SettingsMenuTextPlanCursor < m_vSettingsMenuTextPrebuildPlan.size())"));
	EXPECT_EQ(PrebuildBody.find("BuildSettingsMenuTextPlan(vItems, SettingsMainView);"), std::string::npos);
	EXPECT_EQ(PrebuildBody.find("BuildSettingsMenuTextPlan(vItems);"), std::string::npos);
	EXPECT_EQ(PrebuildBody.find("BuildBaseSettingsMenuTextPlan(vItems, SettingsMainView);"), std::string::npos);
	EXPECT_EQ(PrebuildBody.find("for(int Tab = 0; Tab < NumTClientTextPlanTabs; ++Tab)"), std::string::npos);
	EXPECT_EQ(PrebuildBody.find("for(int Tab = 0; Tab < NUMBER_OF_QMCLIENT_SETTINGS_TABS; ++Tab)"), std::string::npos);

	EXPECT_EQ(EscBody.find("BuildSettingsMenuTextPlan("), std::string::npos);
	EXPECT_EQ(EscBody.find("BuildSettingsMenuTextPlan("), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsPlanCollectionDoesNotSynchronouslyPumpStartupOrEsc)
{
	const std::string GameClient = ReadRepoFile("src/game/client/gameclient.cpp");
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string PrewarmBody = ExtractSourceFunctionBody(GameClient, "void CGameClient::PrewarmSettingsRuntimeCachesDuringLoading(const char *pLoadingCaption, const char *pLoadingMessage)");
	const std::string RenderBody = ExtractSourceFunctionBody(Menus, "void CMenus::OnRender()");
	const std::string EscBody = ExtractSourceFunctionBody(Menus, "void CMenus::PrebuildIngameEscTextPoolBeforeOpen(int Budget)");
	ASSERT_FALSE(PrewarmBody.empty());
	ASSERT_FALSE(RenderBody.empty());
	ASSERT_FALSE(EscBody.empty());

	EXPECT_NE(PrewarmBody.find("constexpr int TEXT_PREWARM_BUDGET_PER_STEP = 8;"), std::string::npos);
	EXPECT_NE(PrewarmBody.find("m_Menus.PrewarmSettingsTextPoolForLoading(TEXT_PREWARM_BUDGET_PER_STEP);"), std::string::npos);
	EXPECT_EQ(PrewarmBody.find("while(SettingsLoadingPrewarmShouldKeepPumping"), std::string::npos);
	EXPECT_EQ(PrewarmBody.find("MAX_PREWARM_ATTEMPTS = 128"), std::string::npos);

	EXPECT_EQ(RenderBody.find("PrebuildIngameEscTextPoolBeforeOpen(96);"), std::string::npos);
	EXPECT_EQ(RenderBody.find("PrebuildIngameEscTextPoolBeforeOpen(4);"), std::string::npos);
	EXPECT_EQ(RenderBody.find("PrebuildIngameEscTextPoolBeforeOpen(3);"), std::string::npos);
	EXPECT_NE(RenderBody.find("StartSettingsPerfFixedWindow(\"ingame_esc_open\""), std::string::npos);
	EXPECT_EQ(EscBody.find("BuildSettingsMenuTextPlan("), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsPlanCollectionDoesNotEnterVisibleGuard)
{
	const std::string Settings = ReadRepoFile("src/game/client/components/menus_settings.cpp");
	const std::string RenderSettingsBody = ExtractSourceFunctionBody(Settings, "void CMenus::RenderSettings(CUIRect MainView)");
	ASSERT_FALSE(RenderSettingsBody.empty());

	EXPECT_NE(RenderSettingsBody.find("std::optional<CScopedMenuTextVisibleGuard> TextVisibleGuard;"), std::string::npos);
	const size_t GuardCondition = RenderSettingsBody.find("if(!CollectingMenuTextPlan)");
	const size_t GuardEmplace = RenderSettingsBody.find("TextVisibleGuard.emplace(this);");
	ASSERT_NE(GuardCondition, std::string::npos);
	ASSERT_NE(GuardEmplace, std::string::npos);
	EXPECT_LT(GuardCondition, GuardEmplace);
	EXPECT_EQ(RenderSettingsBody.find("const CScopedMenuTextVisibleGuard TextVisibleGuard(this);"), std::string::npos);
}

TEST(QmMonitoringHelpers, VisibleGuardKeepsPlanMetadataAfterInvalidation)
{
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string InvalidateBody = ExtractSourceFunctionBody(Menus, "void CMenus::InvalidateMenuTextPool(const char *pReason)");
	const std::string EnsureBody = ExtractSourceFunctionBody(Menus, "void CMenus::EnsureSettingsMenuTextPlanReadyForVisible()");
	ASSERT_FALSE(InvalidateBody.empty());
	ASSERT_FALSE(EnsureBody.empty());

	EXPECT_NE(Header.find("void EnsureSettingsMenuTextPlanReadyForVisible();"), std::string::npos);
	EXPECT_NE(InvalidateBody.find("if(!m_MenuTextPoolVisibleGuard)"), std::string::npos);
	EXPECT_NE(InvalidateBody.find("m_SettingsMenuTextPlanMetadataDirty = true;"), std::string::npos);
	EXPECT_NE(EnsureBody.find("std::vector<SMenuTextPlanItem> vVisibleItems;"), std::string::npos);
	EXPECT_NE(EnsureBody.find("BuildVisibleSettingsMenuTextPlan(vVisibleItems, SettingsMainView);"), std::string::npos);
	EXPECT_NE(EnsureBody.find("m_SettingsMenuTextPlannedDescriptors.insert"), std::string::npos);
	EXPECT_NE(EnsureBody.find("m_SettingsMenuTextPlannedKeys.insert"), std::string::npos);
	EXPECT_NE(EnsureBody.find("m_vSettingsMenuTextPrebuildPlan.clear();"), std::string::npos);
	EXPECT_EQ(EnsureBody.find("m_vSettingsMenuTextPlanCollectionUnits.clear();"), std::string::npos);
	EXPECT_EQ(EnsureBody.find("m_SettingsMenuTextPlanCollectionDirty = true;"), std::string::npos);
	EXPECT_NE(Menus.find("EnsureSettingsMenuTextPlanReadyForVisible();\n\tm_pMenus->m_MenuTextPoolVisibleGuard = true;"), std::string::npos);
}

TEST(QmMonitoringHelpers, DemoBrowserStartupDoesNotSynchronouslyFetchAllHeaders)
{
	const std::string MenusDemo = ReadRepoFile("src/game/client/components/menus_demo.cpp");
	const std::string RenderListBody = ExtractSourceFunctionBody(MenusDemo, "void CMenus::RenderDemoBrowserList(CUIRect ListView, bool &WasListboxItemActivated)");
	const std::string PopulateBody = ExtractSourceFunctionBody(MenusDemo, "void CMenus::DemolistPopulate()");
	ASSERT_FALSE(RenderListBody.empty());
	ASSERT_FALSE(PopulateBody.empty());

	EXPECT_NE(RenderListBody.find("DemolistPopulate();"), std::string::npos);
	EXPECT_NE(RenderListBody.find("DemolistOnUpdate(true);"), std::string::npos);
	EXPECT_EQ(PopulateBody.find("FetchAllHeaders();"), std::string::npos);
	EXPECT_EQ(PopulateBody.find("EnsureAllDemoDates();"), std::string::npos);
	EXPECT_NE(MenusDemo.find("AdvanceDemoBrowserMetadata("), std::string::npos);
	EXPECT_NE(MenusDemo.find("event=demo_browser_startup"), std::string::npos);
}

TEST(QmMonitoringHelpers, DemoBrowserFetchInfoUsesBoundedProgress)
{
	const std::string MenusDemo = ReadRepoFile("src/game/client/components/menus_demo.cpp");
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string ButtonsBody = ExtractSourceFunctionBody(MenusDemo, "void CMenus::RenderDemoBrowserButtons(CUIRect ButtonsView, bool WasListboxItemActivated)");
	const std::string SortListBody = ExtractSourceFunctionBody(MenusDemo, "void CMenus::RenderDemoBrowserList(CUIRect ListView, bool &WasListboxItemActivated)");
	const std::string DetailsBody = ExtractSourceFunctionBody(MenusDemo, "void CMenus::RenderDemoBrowserDetails(CUIRect DetailsView)");
	const std::string FetchAllBody = ExtractSourceFunctionBody(MenusDemo, "void CMenus::FetchAllHeaders()");
	const std::string EnsureDatesBody = ExtractSourceFunctionBody(MenusDemo, "void CMenus::EnsureAllDemoDates()");
	ASSERT_FALSE(ButtonsBody.empty());
	ASSERT_FALSE(SortListBody.empty());
	ASSERT_FALSE(DetailsBody.empty());
	ASSERT_FALSE(FetchAllBody.empty());
	ASSERT_FALSE(EnsureDatesBody.empty());

	EXPECT_NE(ButtonsBody.find("g_Config.m_BrDemoFetchInfo"), std::string::npos);
	EXPECT_EQ(ButtonsBody.find("FetchAllHeaders();"), std::string::npos);
	EXPECT_EQ(SortListBody.find("EnsureAllDemoDates();"), std::string::npos);
	EXPECT_EQ(FetchAllBody.find("for(auto &Item : m_vDemos)"), std::string::npos);
	EXPECT_EQ(EnsureDatesBody.find("for(auto &Item : m_vDemos)"), std::string::npos);
	EXPECT_NE(FetchAllBody.find("AdvanceDemoBrowserMetadata(2, g_Config.m_BrDemoSort == SORT_DATE ? 4 : 0, \"fetch_info\");"), std::string::npos);
	EXPECT_NE(EnsureDatesBody.find("AdvanceDemoBrowserMetadata(0, maximum(1, AdaptiveBudget.m_DemoMetadataTokens), \"ensure_dates\");"), std::string::npos);
	EXPECT_EQ(SortListBody.find("if(EnsureDemoDate(*pItem))"), std::string::npos);
	EXPECT_NE(SortListBody.find("if(pItem->m_DateLoaded && pItem->m_DateValid)"), std::string::npos);
	EXPECT_EQ(DetailsBody.find("!FetchHeader(*pItem)"), std::string::npos);
	EXPECT_NE(DetailsBody.find("!pItem->m_InfosLoaded"), std::string::npos);
	EXPECT_NE(Header.find("size_t m_DemoHeaderFetchCursor"), std::string::npos);
	EXPECT_NE(Header.find("size_t m_DemoDateFetchCursor"), std::string::npos);
	EXPECT_NE(Header.find("bool m_DemoHeaderFetchComplete"), std::string::npos);
	EXPECT_NE(Header.find("bool m_DemoDateFetchComplete"), std::string::npos);
	EXPECT_NE(MenusDemo.find("event=demo_browser_header_fetch"), std::string::npos);
	EXPECT_NE(MenusDemo.find("event=demo_browser_date_fetch"), std::string::npos);
	EXPECT_NE(MenusDemo.find("event=demo_browser_preview_load"), std::string::npos);
	EXPECT_NE(MenusDemo.find("metadata_remaining=%d"), std::string::npos);
}

TEST(QmMonitoringHelpers, DemoBrowserMetadataPrioritizesVisibleWindow)
{
	const std::string MenusDemo = ReadRepoFile("src/game/client/components/menus_demo.cpp");
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string AdvanceBody = ExtractSourceFunctionBody(MenusDemo, "void CMenus::AdvanceDemoBrowserMetadata(int HeaderBudget, int DateBudget, const char *pTrigger, int VisibleFirst, int VisibleEnd)");
	const std::string RenderListBody = ExtractSourceFunctionBody(MenusDemo, "void CMenus::RenderDemoBrowserList(CUIRect ListView, bool &WasListboxItemActivated)");
	ASSERT_FALSE(AdvanceBody.empty());
	ASSERT_FALSE(RenderListBody.empty());

	EXPECT_NE(Header.find("void AdvanceDemoBrowserMetadata(int HeaderBudget, int DateBudget, const char *pTrigger, int VisibleFirst = -1, int VisibleEnd = -1);"), std::string::npos);
	EXPECT_NE(RenderListBody.find("int FirstVisibleIndex = -1;"), std::string::npos);
	EXPECT_NE(RenderListBody.find("EndVisibleIndex = ItemIndex + 1;"), std::string::npos);
	EXPECT_NE(RenderListBody.find("\"list_frame\",\n\t\tFirstVisibleIndex,\n\t\tEndVisibleIndex);"), std::string::npos);
	EXPECT_LT(AdvanceBody.find("for(int i = VisibleFirst; i < VisibleEnd && RemainingBudget > 0; ++i)"), AdvanceBody.find("while(m_DemoHeaderFetchCursor < m_vDemos.size() && RemainingBudget > 0)"));
	EXPECT_LT(AdvanceBody.find("for(int i = VisibleFirst; i < VisibleEnd && RemainingBudget > 0; ++i)", AdvanceBody.find("if(!m_DemoDateFetchComplete")), AdvanceBody.find("while(m_DemoDateFetchCursor < m_vDemos.size() && RemainingBudget > 0)"));
	EXPECT_NE(AdvanceBody.find("std::stable_sort(m_vDemos.begin(), m_vDemos.end());\n\t\t\tDemolistOnUpdate(false);"), std::string::npos);
	EXPECT_NE(AdvanceBody.find("visible_scanned=%d"), std::string::npos);
	EXPECT_NE(AdvanceBody.find("background_scanned=%d"), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsAssetsPreviewAdmissionPrefersCombinedVisibleWindow)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Source.find("struct SSettingsAssetsVisibleAdmission"), std::string::npos);
	EXPECT_NE(Body.find("SSettingsAssetsVisibleAdmission CombinedVisibleAdmission"), std::string::npos);
	EXPECT_NE(Body.find("visible_first=1"), std::string::npos);
	EXPECT_NE(Body.find("visible_starts=%d prefetch_starts=%d background_starts=%d"), std::string::npos);
	EXPECT_LT(Body.find("SSettingsAssetsVisibleAdmission CombinedVisibleAdmission"), Body.find("StartWorkshopThumb("));
	EXPECT_LT(Body.find("SSettingsAssetsVisibleAdmission CombinedVisibleAdmission"), Body.find("SchedulePreviewRange("));
	EXPECT_EQ(Body.find("StartWorkshopThumb(Asset, SettingsWorkshopThumbShouldStartHighPriority(VisibleDownloadableIndex, FirstVisibleDownloadableIndex, LastVisibleDownloadableIndex));"), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsAssetsVisibleReadyPreflightPrecedesDraw)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Source.find("enum class EAssetsVisiblePreflightState"), std::string::npos);
	EXPECT_NE(Source.find("struct SSettingsAssetsVisiblePreflight"), std::string::npos);
	EXPECT_NE(Body.find("PrepareAssetsVisibleContentBudgeted("), std::string::npos);
	EXPECT_EQ(Body.find("RenderAssetsVisibleReadySkeleton("), std::string::npos);
	EXPECT_NE(Body.find("stage=assets_visible_preflight"), std::string::npos);
	EXPECT_NE(Body.find("stage=assets_card_geometry"), std::string::npos);
	EXPECT_NE(Body.find("visible_ready=%d geometry_stable=%d thumb_starts_before_visible=%d thumb_starts_during_draw=%d"), std::string::npos);
	EXPECT_NE(Body.find("MaxWorkshopThumbJumpStartsPerFrame"), std::string::npos);
	EXPECT_NE(Body.find("const int WorkshopThumbStartLimitThisFrame = WorkshopListJumpScrollActive ? MaxWorkshopThumbJumpStartsPerFrame : MaxWorkshopThumbStartsPerFrameAdaptive;"), std::string::npos);
	EXPECT_LT(Body.find("PrepareAssetsVisibleContentBudgeted("), Body.find("for(size_t ListIndex = WorkshopVisibleRange.m_FirstItem"));
	const size_t DrawLoop = Body.find("for(size_t ListIndex = WorkshopVisibleRange.m_FirstItem");
	ASSERT_NE(DrawLoop, std::string::npos);
	const size_t DrawLog = Body.find("assets_preview_draw_workshop_cards", DrawLoop);
	ASSERT_NE(DrawLog, std::string::npos);
	const std::string DrawLoopBody = Body.substr(DrawLoop, DrawLog - DrawLoop);
	EXPECT_EQ(DrawLoopBody.find("StartWorkshopThumb("), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsUsesAdaptiveBudgetForPreviewAndThumbWork)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("BeginSettingsUiFrameScheduler("), std::string::npos);
	EXPECT_NE(Body.find("AdaptiveBudget.m_VisibleTokens"), std::string::npos);
	EXPECT_NE(Body.find("AdaptiveBudget.m_PrefetchTokens"), std::string::npos);
	EXPECT_NE(Body.find("AdaptiveBudget.m_GpuUploadTokens"), std::string::npos);
	EXPECT_NE(Body.find("const bool AssetsScrollPressure = ResourceFrameContext.m_ScrollActive || ResourceFrameContext.m_JumpScrollActive;"), std::string::npos);
	EXPECT_NE(Body.find("const int AdaptivePrefetchTokens = AssetsScrollPressure ? 0 : AdaptiveBudget.m_PrefetchTokens;"), std::string::npos);
	EXPECT_NE(Body.find("const int AdaptiveBackgroundTokens = AssetsScrollPressure ? 0 : AdaptiveBudget.m_BackgroundTokens;"), std::string::npos);
	EXPECT_NE(Body.find("WorkshopVisibleRange.m_EndItem + (AssetsScrollPressure ? 0 : Columns)"), std::string::npos);
	EXPECT_NE(Menus.find("event=settings_adaptive_budget"), std::string::npos);
	EXPECT_EQ(Body.find("constexpr int MaxWorkshopThumbStartsPerFrame = 16;"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsPageSwitchDefersPreviewGpuUploads)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("const bool AssetsPageSwitchActive = m_SettingsPageSwitchActive;"), std::string::npos);
	EXPECT_NE(Body.find("const bool AssetsShellOnlyFrame = AssetsTabSwitchFirstFrame || AssetsPageSwitchActive;"), std::string::npos);
	EXPECT_NE(Body.find("const bool AssetsContentWarmupBlocked = AssetsShellOnlyFrame || AssetsScrollPressure;"), std::string::npos);
	EXPECT_NE(Body.find("AssetsUploadBlocked = AssetsContentWarmupBlocked || AssetsDirectScrollUploadBlocked"), std::string::npos);
	EXPECT_NE(Body.find("MaxPreviewUploadsPerFrame = AssetsUploadBlocked ? 0 : AdaptiveBudget.m_GpuUploadTokens;"), std::string::npos);
	EXPECT_NE(Body.find("MaxWorkshopThumbUploadsPerFrame = AssetsUploadBlocked ? 0 : AdaptiveBudget.m_GpuUploadTokens;"), std::string::npos);
	EXPECT_EQ(Body.find("const int MaxPreviewUploadsPerFrame = maximum(1, AdaptiveBudget.m_GpuUploadTokens);"), std::string::npos);
	EXPECT_EQ(Body.find("const int MaxWorkshopThumbUploadsPerFrame = maximum(1, AdaptiveBudget.m_GpuUploadTokens);"), std::string::npos);
	EXPECT_EQ(Body.find("m_SettingsFrameBudget.m_MaxGpuUploads = maximum(m_SettingsFrameBudget.m_MaxGpuUploads, AdaptiveBudget.m_GpuUploadTokens);"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsDirectScrollDefersPreviewGpuUploads)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("constexpr int AssetsScrollUploadCooldownFrames = 6;"), std::string::npos);
	EXPECT_NE(Body.find("AssetsDirectScrollUploadBlocked"), std::string::npos);
	EXPECT_NE(Body.find("AssetsUploadBlocked = AssetsContentWarmupBlocked || AssetsDirectScrollUploadBlocked"), std::string::npos);
	EXPECT_NE(Body.find("MaxPreviewUploadsPerFrame = AssetsUploadBlocked ? 0 : AdaptiveBudget.m_GpuUploadTokens;"), std::string::npos);
	EXPECT_NE(Body.find("const char *pAssetsUploadBlockFrameContext = AssetsDirectScrollUploadBlocked ? \"scroll_cooldown\""), std::string::npos);
	EXPECT_NE(Body.find("pAssetsUploadBlockFrameContext"), std::string::npos);
	EXPECT_NE(Body.find("scroll_upload_cooldown=%d frame_context=%s upload_block=%s"), std::string::npos);
	EXPECT_NE(Body.find("RefreshAssetsScrollUploadCooldownForOffset"), std::string::npos);
	EXPECT_NE(Body.find("RefreshAssetsScrollUploadCooldownForOffset(ListScrollActive, s_ListBox.ScrollOffsetY()"), std::string::npos);
	EXPECT_NE(Body.find("RefreshAssetsScrollUploadCooldownForOffset(WorkshopListScrollActive, s_WorkshopAssetsListBox.ScrollOffsetY()"), std::string::npos);
	EXPECT_NE(Body.find("RefreshAssetsUploadBudget();"), std::string::npos);
	EXPECT_EQ(Body.find("const int MaxPreviewUploadsPerFrame = AssetsPageSwitchActive ? 0 : AdaptiveBudget.m_GpuUploadTokens;"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsDirectScrollDefersWorkshopFinalize)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("MaxWorkshopThumbDecodeFinalizesThisFrame = AssetsUploadBlocked ? 0 : MaxWorkshopThumbDecodeFinalizesPerFrame;"), std::string::npos);
	EXPECT_NE(Body.find("MaxWorkshopThumbUploadsPerFrame = AssetsUploadBlocked ? 0 : AdaptiveBudget.m_GpuUploadTokens;"), std::string::npos);
	EXPECT_NE(Body.find("SettingsResourceFrameStageBudget(FinalizeFrameContext, FinalizePriority, MaxWorkshopThumbDecodeFinalizesThisFrame, 0)"), std::string::npos);
	EXPECT_NE(Body.find("scroll_upload_cooldown=%d"), std::string::npos);
	EXPECT_EQ(Body.find("const int MaxWorkshopThumbUploadsPerFrame = AssetsPageSwitchActive ? 0 : AdaptiveBudget.m_GpuUploadTokens;"), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsUiBudgetTelemetryExists)
{
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string TClient = ReadRepoFile("src/game/client/components/tclient/menus_tclient.cpp");
	const std::string Assets = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");

	EXPECT_NE(Header.find("struct SSettingsUiBudgetFrame"), std::string::npos);
	EXPECT_NE(Header.find("LogSettingsUiBudget("), std::string::npos);
	EXPECT_NE(Menus.find("event=settings_ui_budget"), std::string::npos);
	EXPECT_NE(Menus.find("layout_ms=%.3f text_ms=%.3f text_new=%d text_reused=%d draw_calls=%d vertices=%d indices=%d heap_allocs=%d visible_widgets=%d"), std::string::npos);
	EXPECT_NE(TClient.find("LogSettingsUiBudget(\"settings:tclient\""), std::string::npos);
	EXPECT_NE(Assets.find("LogSettingsUiBudget(\"settings:assets\""), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsInternalTabSwitchStartsFpsWindow)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("StartSettingsPerfFixedWindow(\"settings_assets_tab_switch\""), std::string::npos);
	EXPECT_NE(Body.find("CurrentQmUiPerfPage()"), std::string::npos);
	EXPECT_NE(Body.find("str_format(aAssetsPerfTab"), std::string::npos);
	EXPECT_NE(Body.find("s_AssetsTabSwitchFirstFrame = 1;"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsPerfStagesUseRealFrameId)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");

	EXPECT_NE(Source.find("auto LogAssetsFramePerfStage"), std::string::npos);
	EXPECT_NE(Source.find("LogAssetsPerfStageForClient(Client(), pStage, DurationMs, Force, pExtra);"), std::string::npos);
	EXPECT_NE(Source.find("LogAssetsFramePerfStage(\"assets_preview_draw_workshop_cards\""), std::string::npos);
	EXPECT_NE(Source.find("LogAssetsFramePerfStage(\"assets_card_geometry\""), std::string::npos);
	EXPECT_NE(Source.find("LogAssetsPerfStageForClient(Client(), \"assets_window_focus\""), std::string::npos);
	EXPECT_NE(Source.find("LogAssetsPerfStageForClient(Client(), \"assets_preview_upload_queue_push\""), std::string::npos);
	EXPECT_NE(Source.find("LogAssetsPerfStageForClient(Client(), \"assets_workshop_thumb_start_local\""), std::string::npos);
	EXPECT_NE(Source.find("LogAssetsPerfStageForClient(Client(), \"assets_workshop_thumb_start_remote\""), std::string::npos);
	EXPECT_EQ(Source.find("QmPerfLogStage(\"perf/assets\", pStage, DurationMs, Force, nullptr"), std::string::npos);
}

TEST(QmMonitoringHelpers, PerfAnalyzerReportsUiBudgetFields)
{
	const std::string Stats = ReadRepoFile("qmclient_scripts/perf/lib/stats.ts");
	const std::string Quality = ReadRepoFile("qmclient_scripts/perf/lib/quality.ts");
	const std::string Report = ReadRepoFile("qmclient_scripts/perf/lib/report.ts");
	const std::string Tests = ReadRepoFile("qmclient_scripts/perf/test.ts");

	EXPECT_NE(Stats.find("export interface SettingsUiBudgetSummary"), std::string::npos);
	EXPECT_NE(Stats.find("settingsUiBudgetSummary(entries"), std::string::npos);
	EXPECT_NE(Stats.find("layout_ms"), std::string::npos);
	EXPECT_NE(Stats.find("text_ms"), std::string::npos);
	EXPECT_NE(Stats.find("draw_calls"), std::string::npos);
	EXPECT_NE(Stats.find("heap_allocs"), std::string::npos);
	EXPECT_NE(Quality.find("settingsUiBudget"), std::string::npos);
	EXPECT_NE(Report.find("Settings UI Budget"), std::string::npos);
	EXPECT_EQ(Report.find("APPROXIMATE"), std::string::npos);
	EXPECT_EQ(Report.find("REPORT ONLY"), std::string::npos);
	EXPECT_EQ(Report.find("Draw Calls Est."), std::string::npos);
	EXPECT_EQ(Report.find("Vertices Est."), std::string::npos);
	EXPECT_EQ(Report.find("Indices Est."), std::string::npos);
	EXPECT_EQ(Report.find("占位观测"), std::string::npos);
	EXPECT_EQ(Report.find("不代表本轮已做通用文本渲染优化"), std::string::npos);
	EXPECT_NE(Tests.find("testSettingsUiBudgetFieldsAppearInSummaryAndReport"), std::string::npos);
}

TEST(QmMonitoringHelpers, PerfTelemetryOverheadIsAttributedByFrameWindow)
{
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Stats = ReadRepoFile("qmclient_scripts/perf/lib/stats.ts");
	const std::string Report = ReadRepoFile("qmclient_scripts/perf/lib/report.ts");
	const std::string Tests = ReadRepoFile("qmclient_scripts/perf/test.ts");
	const std::string Body = ExtractSourceFunctionBody(Menus, "void CMenus::OnRender()");
	ASSERT_FALSE(Body.empty());

	// Text/glyph telemetry is useful only if its own frame-tail flush can be
	// blamed when it becomes the problem. This prevents low-FPS windows from
	// falling back to attribution=none or hiding profiler overhead inside UI total.
	EXPECT_NE(Body.find("TextRender()->FlushQmTextRuntimeBudgetLog();"), std::string::npos);
	EXPECT_NE(Body.find("LogPerfStage(Client(), \"telemetry_flush\", StageTimer.ElapsedMs());"), std::string::npos);
	EXPECT_NE(Stats.find("telemetry_overhead"), std::string::npos);
	EXPECT_NE(Stats.find("telemetry_flush"), std::string::npos);
	EXPECT_NE(Report.find("Telemetry Flush"), std::string::npos);
	EXPECT_NE(Tests.find("testPerfOverheadIsReportedAsCulprit"), std::string::npos);
}

TEST(QmMonitoringHelpers, TClientSettingsTab0StableTextKeysMatchPlan)
{
	const std::string Source = ReadRepoFile("src/game/client/components/tclient/menus_tclient.cpp");
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");

	EXPECT_NE(Menus.find("BuildTClientSettingsMenuTextPlan(vItems, MainView, m_TClientSettingsTab);"), std::string::npos);
	EXPECT_NE(Source.find("DoSettingsScrollbarOption(SETTINGS_TCLIENT, m_TClientSettingsTab, m_TClientSettingsTab, \"tclient-prediction-margin\""), std::string::npos);
	EXPECT_NE(Source.find("DoSettingsScrollbarOption(SETTINGS_TCLIENT, m_TClientSettingsTab, m_TClientSettingsTab, \"tclient-outline-opacity\""), std::string::npos);
	EXPECT_NE(Source.find("DoSettingsScrollbarOption(SETTINGS_TCLIENT, m_TClientSettingsTab, m_TClientSettingsTab, \"tclient-outline-solid-opacity\""), std::string::npos);
	EXPECT_NE(Source.find("DoSettingsScrollbarOption(SETTINGS_TCLIENT, m_TClientSettingsTab, m_TClientSettingsTab, \"tclient-outline-width\""), std::string::npos);
	EXPECT_NE(Source.find("DoSettingsMenuLabel(SETTINGS_TCLIENT, m_TClientSettingsTab, m_TClientSettingsTab, \"tclient-player-indicator-title\""), std::string::npos);

	EXPECT_EQ(Source.find("DoSettingsScrollbarOption(SETTINGS_TCLIENT, m_TClientSettingsTab, \"tclient-prediction-margin\""), std::string::npos);
	EXPECT_EQ(Source.find("SettingsTextElement(SETTINGS_TCLIENT, m_TClientSettingsTab, \"tclient-player-indicator-title\")"), std::string::npos);
}

TEST(QmMonitoringHelpers, TClientTab0StableTextKeysMatchLatestLogSamples)
{
	const std::string Source = ReadRepoFile("src/game/client/components/tclient/menus_tclient.cpp");

	EXPECT_NE(Source.find("DoSettingsScrollbarOption(SETTINGS_TCLIENT, m_TClientSettingsTab, m_TClientSettingsTab, \"tclient-antiping-uncertainty-scale\""), std::string::npos);
	EXPECT_NE(Source.find("DoSettingsScrollbarOption(SETTINGS_TCLIENT, m_TClientSettingsTab, m_TClientSettingsTab, \"tclient-auto-vote-minimum-time\""), std::string::npos);
	EXPECT_NE(Source.find("DoSettingsMenuLabel(SETTINGS_TCLIENT, m_TClientSettingsTab, m_TClientSettingsTab, \"tclient-auto-reply-title\""), std::string::npos);
	EXPECT_NE(Source.find("\"tclient-antiping-uncertainty-scale\""), std::string::npos);
	EXPECT_NE(Source.find("\"tclient-auto-vote-minimum-time\""), std::string::npos);
	EXPECT_NE(Source.find("\"tclient-auto-reply-title\""), std::string::npos);
	EXPECT_EQ(Source.find("SettingsTextElement(SETTINGS_TCLIENT, m_TClientSettingsTab, \"tclient-auto-reply-title\")"), std::string::npos);
}

TEST(QmMonitoringHelpers, TClientPrewarmDoesNotRunUnboundedInVisibleTargetFrame)
{
	const std::string Source = ReadRepoFile("src/game/client/components/tclient/menus_tclient.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsTClientSettings(CUIRect MainView, bool PrewarmOnly)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("const bool TClientVisibleTargetFrame = !PrewarmOnly"), std::string::npos);
	EXPECT_NE(Body.find("SetProgressiveEnabled(TClientVisibleTargetFrame)"), std::string::npos);
	EXPECT_NE(Body.find("SetMaxSectionsPerFrame(TClientVisibleTargetFrame ?"), std::string::npos);
	EXPECT_NE(Body.find("tclient_settings_left_prewarm_budgeted"), std::string::npos);
	EXPECT_NE(Body.find("tclient_settings_right_prewarm_budgeted"), std::string::npos);
	EXPECT_EQ(Body.find("s_VisualFontLoader.SetProgressiveEnabled(false);"), std::string::npos);
	EXPECT_EQ(Body.find("s_RightSectionLoader.SetProgressiveEnabled(false);"), std::string::npos);
}

TEST(QmMonitoringHelpers, TClientSectionMeasuredHeightMatchesRenderedHeight)
{
	const std::string Source = ReadRepoFile("src/game/client/components/tclient/menus_tclient.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsTClientSettings(CUIRect MainView, bool PrewarmOnly)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("LogTClientSectionHeightConsistency"), std::string::npos);
	EXPECT_NE(Body.find("section_height_measured=%.3f section_height_rendered=%.3f height_delta=%.3f"), std::string::npos);
	EXPECT_NE(Body.find("MeasuredHeight = "), std::string::npos);
	EXPECT_NE(Body.find("RenderedHeight = "), std::string::npos);
	EXPECT_NE(Body.find("absolute(HeightDelta) <= 0.01f"), std::string::npos);
	EXPECT_NE(Body.find("RenderBoxedFullSection"), std::string::npos);
	EXPECT_NE(Body.find("FillCachedStaticLayer"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsWorkshopCardDrawHasSubstageTelemetry)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("assets_preview_draw_workshop_cards_layout_text"), std::string::npos);
	EXPECT_NE(Body.find("assets_preview_draw_workshop_cards_preview_draw"), std::string::npos);
	EXPECT_NE(Body.find("assets_preview_draw_workshop_cards_thumb_scheduling"), std::string::npos);
	EXPECT_NE(Body.find("layout_text_ms=%.3f preview_draw_ms=%.3f thumb_scheduling_ms=%.3f"), std::string::npos);
	EXPECT_NE(Body.find("char aExtra[512];"), std::string::npos);
	EXPECT_NE(Body.find("CardLayoutTextTimer"), std::string::npos);
	EXPECT_NE(Body.find("CardPreviewDrawTimer"), std::string::npos);
	EXPECT_NE(Body.find("ThumbSchedulingTimer"), std::string::npos);
	EXPECT_NE(Body.find("WorkshopCardLayoutTextMs += CardLayoutTextTimer.ElapsedMs();"), std::string::npos);
	EXPECT_NE(Body.find("WorkshopCardPreviewDrawMs += CardPreviewDrawTimer.ElapsedMs();"), std::string::npos);
	EXPECT_NE(Body.find("const double PreflightMs = ThumbSchedulingTimer.ElapsedMs();"), std::string::npos);
	EXPECT_NE(Body.find("WorkshopCardThumbSchedulingMs += PreflightMs;"), std::string::npos);
	const size_t PreparePos = Body.find("auto PrepareAssetsVisibleContentBudgeted =");
	const size_t DrawLoopPos = Body.find("s_WorkshopAssetsListBox.SkipItems(WorkshopVisibleRange.m_FirstItem)");
	ASSERT_NE(PreparePos, std::string::npos);
	ASSERT_NE(DrawLoopPos, std::string::npos);
	EXPECT_LT(PreparePos, DrawLoopPos);
	EXPECT_NE(Body.find("UiBudget.m_LayoutMs = WorkshopCardsTimer.ElapsedMs();"), std::string::npos);
	EXPECT_NE(Body.find("UiBudget.m_TextReused = 0;"), std::string::npos);
	EXPECT_EQ(Body.find("WorkshopCardLoopMs * 0.5"), std::string::npos);
	EXPECT_EQ(Body.find("WorkshopCardLoopMs - WorkshopCardLayoutTextMs"), std::string::npos);
	EXPECT_EQ(Body.find("UiBudget.m_LayoutMs = AssetsUiBudgetTimer.ElapsedMs();"), std::string::npos);
	EXPECT_EQ(Body.find("UiBudget.m_TextReused = WorkshopVisibleRange.m_RenderedItems;"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsTabSwitchLimitsFirstFrameVisibleThumbStarts)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("constexpr int MaxAssetsTabSwitchVisibleThumbStartsFirstFrame = 2;"), std::string::npos);
	EXPECT_NE(Body.find("AssetsTabSwitchFirstFrame"), std::string::npos);
	EXPECT_NE(Body.find("MaxAssetsTabSwitchVisibleThumbStartsFirstFrame"), std::string::npos);
	EXPECT_NE(Body.find("tab_switch_first_frame=%d"), std::string::npos);
	EXPECT_NE(Body.find("visible_thumb_start_limit=%d"), std::string::npos);
	EXPECT_NE(Body.find("CombinedVisibleAdmission.m_VisibleStarts >= VisibleThumbStartLimitThisFrame"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsTabSwitchKeepsVisibleThumbStartsCappedForCooldown)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Source.find("constexpr int AssetsTabSwitchCooldownFrames"), std::string::npos);
	EXPECT_NE(Body.find("s_AssetsTabSwitchCooldownFrames = AssetsTabSwitchCooldownFrames;"), std::string::npos);
	EXPECT_NE(Body.find("const bool AssetsTabSwitchCooldownActive = s_AssetsTabSwitchCooldownFrames > 0;"), std::string::npos);
	EXPECT_NE(Body.find("AssetsTabSwitchCooldownActive ? MaxAssetsTabSwitchVisibleThumbStartsPerFrame : MaxWorkshopThumbStartsPerFrameAdaptive"), std::string::npos);
	EXPECT_NE(Body.find("tab_switch_cooldown_frames=%d"), std::string::npos);
	EXPECT_NE(Body.find("--s_AssetsTabSwitchCooldownFrames;"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsEntityBgInstalledThumbsRespectTabSwitchVisibleCap)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Source.find("int &VisibleStartsThisFrame"), std::string::npos);
	EXPECT_NE(Source.find("int MaxVisibleStartsPerFrame"), std::string::npos);
	EXPECT_NE(Source.find("if(HighPriority && VisibleStartsThisFrame >= MaxVisibleStartsPerFrame)"), std::string::npos);
	EXPECT_NE(Source.find("++VisibleStartsThisFrame;"), std::string::npos);
	EXPECT_NE(Body.find("CombinedVisibleAdmission.m_VisibleStarts"), std::string::npos);
	EXPECT_NE(Body.find("VisibleThumbStartLimitThisFrame"), std::string::npos);
	EXPECT_NE(Body.find("CombinedVisibleAdmission.m_VisibleStarts,\n\t\t\t\t\t   VisibleThumbStartLimitThisFrame"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsTabSwitchDoesNotTransformCardGeometry)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("TriggerUiSwitchAnimation(AssetsTabSwitchNode"), std::string::npos);
	EXPECT_NE(Body.find("AssetsTabSwitchFirstFrame"), std::string::npos);
	EXPECT_EQ(Body.find("ApplyUiSwitchOffset(MainView, TransitionStrength, s_AssetsTransitionDirection"), std::string::npos);
	EXPECT_EQ(Body.find("ApplyUiSwitchOffset(MainView"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsCardShellFirstRenderingExists)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Source.find("enum class ESettingsAssetsCardHydrationLayer"), std::string::npos);
	EXPECT_NE(Source.find("SSettingsAssetsCardShell"), std::string::npos);
	EXPECT_NE(Source.find("SSettingsAssetsCardMetadataCacheEntry"), std::string::npos);
	EXPECT_NE(Source.find("SSettingsAssetsCardPreviewState"), std::string::npos);
	EXPECT_NE(Body.find("RenderAssetsCardShell("), std::string::npos);
	EXPECT_NE(Body.find("RenderAssetsCardMetadataCached("), std::string::npos);
	EXPECT_NE(Body.find("RenderAssetsCardPreview("), std::string::npos);
	EXPECT_LT(Body.find("RenderAssetsCardShell("), Body.find("RenderAssetsCardMetadataCached("));
	EXPECT_LT(Body.find("RenderAssetsCardShell("), Body.find("RenderAssetsCardPreview("));
}

TEST(QmMonitoringHelpers, AssetsCardMetadataUsesDedicatedCache)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Source.find("struct SSettingsAssetsCardCacheKey"), std::string::npos);
	EXPECT_NE(Source.find("m_AssetId"), std::string::npos);
	EXPECT_NE(Source.find("m_Tab"), std::string::npos);
	EXPECT_NE(Source.find("m_LocaleHash"), std::string::npos);
	EXPECT_NE(Source.find("m_UiScale"), std::string::npos);
	EXPECT_NE(Source.find("m_CardWidth"), std::string::npos);
	EXPECT_NE(Source.find("m_StatusHash"), std::string::npos);
	EXPECT_NE(Source.find("m_Installed"), std::string::npos);
	EXPECT_NE(Source.find("m_DownloadFailed"), std::string::npos);
	EXPECT_NE(Source.find("m_LocalOnly"), std::string::npos);
	EXPECT_NE(Source.find("SETTINGS_ASSETS_CARD_METADATA_CACHE_MAX_ENTRIES"), std::string::npos);
	EXPECT_NE(Source.find("TrimAssetsCardMetadataCacheForInsert("), std::string::npos);
	EXPECT_NE(Source.find("static std::unordered_map<SSettingsAssetsCardCacheKey, SSettingsAssetsCardMetadataCacheEntry"), std::string::npos);
	EXPECT_NE(Source.find("BuildAssetsCardCacheKey("), std::string::npos);
	EXPECT_NE(Source.find("Key.m_StatusHash = str_quickhash"), std::string::npos);
	EXPECT_NE(Source.find("FindAssetsCardMetadata("), std::string::npos);
	EXPECT_NE(Source.find("HydrateAssetsCardMetadata("), std::string::npos);
	EXPECT_NE(Source.find("HydrateAssetsCardMetadataTimed("), std::string::npos);
	EXPECT_EQ(Source.find("RefreshAssetsCardMetadata("), std::string::npos);
	EXPECT_NE(Body.find("SSettingsAssetsCardMetadataCacheEntry *pMetadata = FindAssetsCardMetadata("), std::string::npos);
	EXPECT_NE(Body.find("RequestAssetsCardMetadataHydration("), std::string::npos);
	EXPECT_EQ(Body.find("pMetadata = HydrateAssetsCardMetadataTimed("), std::string::npos);
	EXPECT_EQ(Body.find("RefreshAssetsCardMetadata(*pMetadata"), std::string::npos);
	EXPECT_EQ(Source.find("RegisterMenuTextPlanItem(\"assets-card"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsMetadataRequestDoesNotHydrateInRenderPath)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string RequestBody = ExtractSourceFunctionBody(Source, "static void RequestAssetsCardMetadataHydration");
	ASSERT_FALSE(RequestBody.empty());

	EXPECT_NE(Source.find("struct SSettingsAssetsCardMetadataRequest"), std::string::npos);
	EXPECT_NE(Source.find("QueueAssetsCardMetadataHydration("), std::string::npos);
	EXPECT_NE(RequestBody.find("QueueAssetsCardMetadataHydration("), std::string::npos);
	EXPECT_EQ(RequestBody.find("HydrateAssetsCardMetadataTimed("), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsMetadataHydratesOnlyThroughBudgetDrain)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	const std::string DrainBody = ExtractSourceFunctionBody(Source, "static int DrainAssetsCardMetadataHydrationRequests");
	ASSERT_FALSE(Body.empty());
	ASSERT_FALSE(DrainBody.empty());

	EXPECT_NE(Body.find("const int AssetsInitialMetadataLayoutTokens = maximum(1, minimum(AdaptiveBudget.m_VisibleTokens, 4));"), std::string::npos);
	EXPECT_NE(Body.find("const int AssetsMetadataLayoutTokensThisFrame = AssetsShellOnlyFrame ? AssetsInitialMetadataLayoutTokens"), std::string::npos);
	EXPECT_NE(Body.find("DrainAssetsCardMetadataHydrationRequests(AssetsMetadataLayoutTokensThisFrame"), std::string::npos);
	EXPECT_NE(DrainBody.find("while(MetadataLayoutTokens > 0"), std::string::npos);
	EXPECT_NE(DrainBody.find("HydrateAssetsCardMetadataTimed("), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsScrollPressureBlocksContentHydrationWork)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	// Fast scroll/jump scroll drops frames when preview/decode/upload work is
	// allowed in the same render frame. Metadata gets a tiny visible-only budget
	// so labels do not disappear while holding the scrollbar.
	EXPECT_NE(Body.find("const bool AssetsContentWarmupBlocked = AssetsShellOnlyFrame || AssetsScrollPressure;"), std::string::npos);
	EXPECT_NE(Body.find("const int AssetsMetadataLayoutTokensThisFrame = AssetsShellOnlyFrame ? AssetsInitialMetadataLayoutTokens"), std::string::npos);
	EXPECT_NE(Body.find("const int AssetsPreviewArtifactTokensThisFrame = AssetsContentWarmupBlocked ? 0 : AdaptivePreviewArtifactTokens;"), std::string::npos);
	EXPECT_NE(Body.find("const int AssetsTextureUploadTokensThisFrame = AssetsContentWarmupBlocked ? 0 : AdaptiveTextureUploadTokens;"), std::string::npos);
	EXPECT_NE(Body.find("const int MaxPreviewDecodeStartsPerFrame = AssetsContentWarmupBlocked ? 0 : maximum(1, AdaptiveBudget.m_VisibleTokens + AdaptivePrefetchTokens + AdaptiveBackgroundTokens);"), std::string::npos);
	EXPECT_NE(Body.find("const int MaxWorkshopThumbStartsPerFrameAdaptive = AssetsContentWarmupBlocked ? 0 : maximum(1, AdaptiveBudget.m_VisibleTokens + AdaptivePrefetchTokens + AdaptiveBackgroundTokens);"), std::string::npos);
	EXPECT_NE(Body.find("const int VisibleThumbStartLimitThisFrame = AssetsContentWarmupBlocked ? 0 :"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsScrollPressureStillRendersMetadataFallback)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	// Regression guard: holding the scrollbar must not hide card titles. The
	// immediate fallback uses fixed card geometry, so it remains readable without
	// waiting for streamed text containers to hydrate.
	EXPECT_NE(Body.find("const bool AssetsRenderCardMetadataFallback = !AssetsShellOnlyFrame;"), std::string::npos);
	EXPECT_NE(Body.find("else if(AssetsRenderCardMetadataFallback)"), std::string::npos);
	EXPECT_NE(Body.find("pMetadata != nullptr && AssetsContentWarmupBlocked"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsScrollPressureSkipsPreviewSchedulingAndUploads)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	// Scroll pressure is the path the user stress-tested: scheduling/decode/upload
	// work must not run in the same frame as a fast list movement.
	EXPECT_NE(Body.find("if(!AssetsContentWarmupBlocked)"), std::string::npos);
	EXPECT_NE(Body.find("PrepareAssetsLocalVisibleContentBudgeted();"), std::string::npos);
	EXPECT_NE(Body.find("PrepareAssetsVisibleContentBudgeted();"), std::string::npos);
	EXPECT_NE(Body.find("ProcessAssetsResourcePreviewJobs(Graphics(), ResourcePreviewTelemetry, ResourcePreviewUploadBudget);"), std::string::npos);
	EXPECT_NE(Body.find("if(!AssetsContentWarmupBlocked && FirstVisibleIndex >= 0)"), std::string::npos);
	EXPECT_LT(Body.find("if(!AssetsContentWarmupBlocked)"), Body.find("ProcessAssetsResourcePreviewJobs(Graphics(), ResourcePreviewTelemetry, ResourcePreviewUploadBudget);"));
}

TEST(QmMonitoringHelpers, AssetsTabSwitchFirstFrameDoesNotStartThumbsOrUploads)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("const bool AssetsShellOnlyFrame = AssetsTabSwitchFirstFrame || AssetsPageSwitchActive;"), std::string::npos);
	EXPECT_NE(Body.find("const int AssetsTextureUploadTokensThisFrame = AssetsContentWarmupBlocked ? 0 : AdaptiveTextureUploadTokens;"), std::string::npos);
	EXPECT_NE(Body.find("if(!AssetsContentWarmupBlocked)\n\t\t\t\t\t\t\tSchedulePreviewRange("), std::string::npos);
	EXPECT_NE(Body.find("if(!AssetsContentWarmupBlocked && StartWorkshopThumb(Asset, Visible))"), std::string::npos);
	EXPECT_NE(Body.find("if(!AssetsContentWarmupBlocked)"), std::string::npos);
	EXPECT_NE(Body.find("PrepareAssetsVisibleContentBudgeted();"), std::string::npos);
	EXPECT_NE(Body.find("ProcessAssetsResourcePreviewJobs(Graphics(), ResourcePreviewTelemetry, ResourcePreviewUploadBudget);"), std::string::npos);
	EXPECT_LT(Body.find("if(!AssetsContentWarmupBlocked)"), Body.find("ProcessAssetsResourcePreviewJobs(Graphics(), ResourcePreviewTelemetry, ResourcePreviewUploadBudget);"));
	EXPECT_NE(Body.find("metadata_hydrated=%d"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsDrawLoopOnlyUsesReadyContent)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());
	const size_t DrawLoopStart = Body.find("s_WorkshopAssetsListBox.SkipItems(WorkshopVisibleRange.m_FirstItem)");
	ASSERT_NE(DrawLoopStart, std::string::npos);
	const size_t DrawLoopEnd = Body.find("s_WorkshopAssetsListBox.SkipItems((int)CombinedCount - WorkshopVisibleRange.m_EndItem)", DrawLoopStart);
	ASSERT_NE(DrawLoopEnd, std::string::npos);
	const std::string DrawLoopBody = Body.substr(DrawLoopStart, DrawLoopEnd - DrawLoopStart);

	EXPECT_NE(DrawLoopBody.find("FindAssetsCardMetadata("), std::string::npos);
	EXPECT_NE(DrawLoopBody.find("RenderAssetsCardMetadataCached("), std::string::npos);
	EXPECT_NE(DrawLoopBody.find("RenderAssetsCardPreview("), std::string::npos);
	EXPECT_EQ(DrawLoopBody.find("RequestAssetsCardMetadataHydration("), std::string::npos);
	EXPECT_EQ(DrawLoopBody.find("HydrateAssetsCardMetadataTimed("), std::string::npos);
	EXPECT_EQ(DrawLoopBody.find("StartWorkshopThumb("), std::string::npos);
	EXPECT_EQ(DrawLoopBody.find("SchedulePreviewRange("), std::string::npos);
	EXPECT_EQ(DrawLoopBody.find("ProcessAssetsResourcePreviewJobs("), std::string::npos);
	EXPECT_EQ(DrawLoopBody.find("DrainSharedResourcePreviewUploadQueue("), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsVisiblePreflightRunsOutsideCardDrawLoop)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());
	const size_t PreparePos = Body.find("PrepareAssetsVisibleContentBudgeted(");
	const size_t DrawLoopStart = Body.find("s_WorkshopAssetsListBox.SkipItems(WorkshopVisibleRange.m_FirstItem)");
	ASSERT_NE(PreparePos, std::string::npos);
	ASSERT_NE(DrawLoopStart, std::string::npos);

	// The preflight is the only place that may schedule visible/near-visible
	// content. If this work drifts back into the card loop, tab switches still
	// stall even though the shell-first layer exists.
	EXPECT_LT(PreparePos, DrawLoopStart);
	EXPECT_EQ(Body.find("RunAssetsVisibleReadyPreflight();"), std::string::npos);
	EXPECT_NE(Body.find("LogAssetsFramePerfStage(\"assets_visible_preflight\""), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsCardDrawLoopDoesNotStartThumbsPreviewJobsOrUploads)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());
	const size_t DrawLoopStart = Body.find("s_WorkshopAssetsListBox.SkipItems(WorkshopVisibleRange.m_FirstItem)");
	ASSERT_NE(DrawLoopStart, std::string::npos);
	const size_t DrawLoopEnd = Body.find("s_WorkshopAssetsListBox.SkipItems((int)CombinedCount - WorkshopVisibleRange.m_EndItem)", DrawLoopStart);
	ASSERT_NE(DrawLoopEnd, std::string::npos);
	const std::string DrawLoopBody = Body.substr(DrawLoopStart, DrawLoopEnd - DrawLoopStart);

	// Regression guard for the broken previews/default tee issue: the draw loop
	// may only consume ready state. It must not start jobs, uploads, or thumbs.
	EXPECT_EQ(DrawLoopBody.find("StartWorkshopThumb("), std::string::npos);
	EXPECT_EQ(DrawLoopBody.find("SchedulePreviewRange("), std::string::npos);
	EXPECT_EQ(DrawLoopBody.find("StartAssetsEntityBgPreviewArtifactJob("), std::string::npos);
	EXPECT_EQ(DrawLoopBody.find("ProcessAssetsResourcePreviewJobs("), std::string::npos);
	EXPECT_EQ(DrawLoopBody.find("SettingsResourcePreviewConsumeUploadBudget("), std::string::npos);
}

TEST(QmMonitoringHelpers, EntityBgPreviewArtifactBackfillsReadyTexture)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string PreviewSource = ReadRepoFile("src/game/client/components/qmclient/settings_resource_preview.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	// Entity BG needs its own tile/artifact preview, not a permanent placeholder.
	// The artifact job must mark CPU artifact ready and the upload drain must
	// backfill a texture that the card renderer can draw in later frames.
	EXPECT_NE(Source.find("StartAssetsEntityBgPreviewArtifactJob("), std::string::npos);
	EXPECT_NE(Source.find("gs_SettingsAssetsResourcePreviewCache.MarkArtifactReady("), std::string::npos);
	EXPECT_NE(PreviewSource.find("m_TextureReady = true"), std::string::npos);
	EXPECT_NE(Body.find("if(pPipelineState != nullptr && pPipelineState->m_Texture.IsValid())"), std::string::npos);
	const size_t FirstPrepare = std::min(Body.find("auto PrepareAssetsLocalVisibleContentBudgeted ="), Body.find("auto PrepareAssetsVisibleContentBudgeted ="));
	ASSERT_NE(FirstPrepare, std::string::npos);
	EXPECT_LT(FirstPrepare, Body.find("StartAssetsEntityBgPreviewArtifactJob("));
	EXPECT_EQ(Body.substr(Body.find("s_WorkshopAssetsListBox.SkipItems(WorkshopVisibleRange.m_FirstItem)"), Body.find("s_WorkshopAssetsListBox.SkipItems((int)CombinedCount - WorkshopVisibleRange.m_EndItem)") - Body.find("s_WorkshopAssetsListBox.SkipItems(WorkshopVisibleRange.m_FirstItem)")).find("StartAssetsEntityBgPreviewArtifactJob("), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsEntityBgPreviewJobUsesStableKeyNotItemPointer)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string StartBody = ExtractSourceFunctionBody(Source, "static bool StartAssetsEntityBgPreviewArtifactJob");
	ASSERT_FALSE(StartBody.empty());

	// Preview jobs may finish after a fast scroll, list rebuild, or tab switch.
	// They must be keyed by stable resource identity rather than retaining a
	// pointer to a card/list item whose storage can move or disappear.
	EXPECT_EQ(Source.find("StartAssetsEntityBgPreviewArtifactJob(const SResourcePreviewKey &PreviewKey, CMenus::SCustomItem *pItem"), std::string::npos);
	EXPECT_NE(Source.find("StartAssetsEntityBgPreviewArtifactJob(const SResourcePreviewKey &PreviewKey, const char *pAssetName"), std::string::npos);
	EXPECT_EQ(StartBody.find("pItem->"), std::string::npos);
	EXPECT_NE(StartBody.find("ResolveEntityBgPreviewArtifactSource(pStorage, pAssetName"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsCardHydrationSchedulerDefersContentAfterTabSwitch)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Source.find("struct SSettingsAssetsCardHydrationScheduler"), std::string::npos);
	EXPECT_NE(Source.find("BeginAssetsCardHydrationFrame("), std::string::npos);
	EXPECT_NE(Source.find("CanHydrateMetadata("), std::string::npos);
	EXPECT_NE(Source.find("CanRenderMetadata("), std::string::npos);
	EXPECT_NE(Source.find("CanHydratePreview("), std::string::npos);
	EXPECT_NE(Source.find("CanRenderPreview("), std::string::npos);
	EXPECT_NE(Source.find("m_TabSwitchShellOnlyFrame"), std::string::npos);
	EXPECT_NE(Body.find("SSettingsAssetsCardHydrationScheduler CardHydrationScheduler = BeginAssetsCardHydrationFrame("), std::string::npos);
	EXPECT_NE(Body.find("RequestAssetsCardMetadataHydration("), std::string::npos);
	EXPECT_NE(Body.find("CardHydrationScheduler.CanRenderPreview("), std::string::npos);
	EXPECT_EQ(Body.find("RenderAssetsCardPreview(Shell, PreviewState, true, CardHydrationScheduler.CanHydratePreview("), std::string::npos);
	EXPECT_EQ(Body.find("const bool RenderMetadata = CardHydrationScheduler.CanRenderMetadata(CombinedVisible, MetadataCached) &&"), std::string::npos);
	EXPECT_EQ(Body.find("(MetadataCached || PreviewPipelineScheduler.CanHydrateMetadata(ESettingsResourcePreviewPriority::VISIBLE));"), std::string::npos);
	EXPECT_NE(Body.find("tab_switch_shell_only=%d"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsCardPreviewDrawDoesNotStarveAfterBudget)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Source.find("bool CanRenderPreview(bool Visible, bool HasPreviewContent, bool HeavyPreviewDeferred = false)"), std::string::npos);
	EXPECT_NE(Source.find("if(!HasPreviewContent)"), std::string::npos);
	EXPECT_NE(Body.find("CardHydrationScheduler.CanRenderPreview(CombinedVisible, PreviewReady, PreviewState.m_EntityBgHeavyPreviewDeferred)"), std::string::npos);
	EXPECT_NE(Body.find("CardHydrationScheduler.CanRenderPreview(CombinedVisible, PreviewReady)"), std::string::npos);
	EXPECT_EQ(Body.find("CardHydrationScheduler.CanHydratePreview(CombinedVisible, PreviewState.m_Texture.IsValid() || PreviewState.m_DrawEntityTileArtifact)"), std::string::npos);
	EXPECT_EQ(Body.find("CardHydrationScheduler.CanHydratePreview(CombinedVisible, PreviewReady)"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsCardPreviewHeavyPathLeavesDrawLoop)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Source.find("EnsureAssetsCardPreviewArtifact("), std::string::npos);
	EXPECT_NE(Source.find("RenderAssetsEntityTilePreviewArtifact("), std::string::npos);
	EXPECT_NE(Body.find("EnsureAssetsCardPreviewArtifact("), std::string::npos);
	EXPECT_NE(Body.find("RenderAssetsCardPreview("), std::string::npos);
	EXPECT_NE(Source.find("auto RenderAssetsCardShell = [&](const SSettingsAssetsCardShell &Shell)"), std::string::npos);
	EXPECT_NE(Source.find("ShellRect.Draw(ColorRGBA(0.03f, 0.04f, 0.06f, 0.16f), IGraphics::CORNER_ALL, 10.0f);"), std::string::npos);
	const size_t ShellStart = Source.find("auto RenderAssetsCardShell = [&](const SSettingsAssetsCardShell &Shell)");
	ASSERT_NE(ShellStart, std::string::npos);
	const size_t ShellEnd = Source.find("};", ShellStart);
	ASSERT_NE(ShellEnd, std::string::npos);
	const std::string ShellBody = Source.substr(ShellStart, ShellEnd - ShellStart);
	EXPECT_EQ(ShellBody.find("DrawPreviewFrame(Shell.m_TextureRect);"), std::string::npos);
	EXPECT_EQ(Body.find("static const int COLS = 7, ROWS = 7;"), std::string::npos);
	EXPECT_EQ(Body.find("for(int r = 0; r < ROWS; r++)"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsEntityBgPreviewHeavyPathDeferredDuringTabSwitchCooldown)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Source.find("m_EntityBgHeavyPreviewDeferred"), std::string::npos);
	EXPECT_NE(Body.find("const bool DeferEntityBgHeavyPreview = s_CurCustomTab == ASSETS_TAB_ENTITY_BG && AssetsTabSwitchCooldownActive"), std::string::npos);
	EXPECT_NE(Body.find("PreviewState.m_EntityBgHeavyPreviewDeferred = DeferEntityBgHeavyPreview"), std::string::npos);
	EXPECT_NE(Body.find("++EntityBgHeavyPreviewDeferredCount;"), std::string::npos);
	EXPECT_NE(Body.find("CardHydrationScheduler.CanRenderPreview(CombinedVisible, PreviewReady, PreviewState.m_EntityBgHeavyPreviewDeferred)"), std::string::npos);
	EXPECT_NE(Body.find("entity_bg_preview_deferred=%d"), std::string::npos);
	EXPECT_NE(Body.find("entity_bg_preview_deferred_count=%d"), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsResourcePreviewPipelineExists)
{
	const std::string Header = ReadRepoFile("src/game/client/components/qmclient/settings_resource_preview.h");
	const std::string Source = ReadRepoFile("src/game/client/components/qmclient/settings_resource_preview.cpp");
	const std::string CMake = ReadRepoFile("CMakeLists.txt");

	EXPECT_NE(Header.find("struct SResourcePreviewKey"), std::string::npos);
	EXPECT_NE(Header.find("struct SResourcePreviewState"), std::string::npos);
	EXPECT_NE(Header.find("class CSettingsResourcePreviewCache"), std::string::npos);
	EXPECT_NE(Header.find("class CSettingsResourcePreviewScheduler"), std::string::npos);
	EXPECT_NE(Header.find("class CSettingsResourcePreviewJob"), std::string::npos);
	EXPECT_NE(Header.find("enum class ESettingsResourcePreviewPriority"), std::string::npos);
	EXPECT_NE(Header.find("VISIBLE"), std::string::npos);
	EXPECT_NE(Header.find("NEAR_VISIBLE"), std::string::npos);
	EXPECT_NE(Header.find("BACKGROUND"), std::string::npos);
	EXPECT_NE(Source.find("CSettingsResourcePreviewScheduler::BeginFrame"), std::string::npos);
	const std::string BeginFrameBody = ExtractSourceFunctionBody(Source, "void CSettingsResourcePreviewScheduler::BeginFrame(int VisibleBudget, int NearVisibleBudget, int BackgroundBudget, int UploadBudget)");
	ASSERT_FALSE(BeginFrameBody.empty());
	EXPECT_EQ(BeginFrameBody.find("CanStartPreviewJob("), std::string::npos);
	EXPECT_NE(CMake.find("components/qmclient/settings_resource_preview.cpp"), std::string::npos);
	EXPECT_NE(CMake.find("src/game/client/components/qmclient/settings_resource_preview.cpp"), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsResourcePreviewPipelineHasRealArtifactAndUploadScheduler)
{
	const std::string Header = ReadRepoFile("src/game/client/components/qmclient/settings_resource_preview.h");
	const std::string Source = ReadRepoFile("src/game/client/components/qmclient/settings_resource_preview.cpp");

	EXPECT_NE(Header.find("struct SResourcePreviewArtifact"), std::string::npos);
	EXPECT_NE(Header.find("class CSettingsResourcePreviewUploadScheduler"), std::string::npos);
	EXPECT_NE(Header.find("EnqueueUpload(const SResourcePreviewKey &Key, CImageInfo &&Image"), std::string::npos);
	EXPECT_NE(Header.find("Drain(SResourcePreviewUploadBudget &Budget, SResourcePreviewTelemetry &Telemetry"), std::string::npos);
	EXPECT_NE(Header.find("m_vUploadQueue"), std::string::npos);
	EXPECT_NE(Header.find("m_ArtifactReady"), std::string::npos);
	EXPECT_NE(Header.find("m_UploadQueueDepth"), std::string::npos);
	EXPECT_NE(Header.find("m_UploadBudgetExhausted"), std::string::npos);

	EXPECT_NE(Source.find("BuildPreviewArtifact("), std::string::npos);
	EXPECT_NE(Source.find("CSettingsResourcePreviewJob::CSettingsResourcePreviewJob(std::string Name, CImageInfo &&Image, int TargetSize)"), std::string::npos);
	EXPECT_NE(Source.find("Result.m_Artifact = BuildPreviewArtifact(std::move(m_InputImage), m_TargetSize);"), std::string::npos);
	EXPECT_NE(Source.find("CSettingsResourcePreviewUploadScheduler::Drain"), std::string::npos);
	EXPECT_NE(Source.find("SettingsResourcePreviewConsumeUploadBudget(Budget"), std::string::npos);
	EXPECT_NE(Source.find("SettingsResourcePreviewCommitUploadBudget(Budget"), std::string::npos);
	EXPECT_NE(Source.find("if(pGraphics == nullptr)"), std::string::npos);
	EXPECT_NE(Source.find("Telemetry.m_UploadQueueDepth = (int)m_vUploadQueue.size();"), std::string::npos);
	EXPECT_NE(Source.find("if(Texture.IsValid())"), std::string::npos);
	EXPECT_NE(Source.find("Cache.MarkUploadFailed(Item.m_Key);"), std::string::npos);
	EXPECT_EQ(Source.find("Result.m_Image = std::move(m_InputImage);"), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsResourcePreviewSchedulerZeroBudgetDoesNotAdmitWork)
{
	CSettingsResourcePreviewScheduler Scheduler;
	Scheduler.BeginFrame(0, 0, 0, 0);

	EXPECT_FALSE(Scheduler.CanHydrateMetadata(ESettingsResourcePreviewPriority::VISIBLE));
	EXPECT_FALSE(Scheduler.CanHydrateMetadata(ESettingsResourcePreviewPriority::NEAR_VISIBLE));
	EXPECT_FALSE(Scheduler.CanHydrateMetadata(ESettingsResourcePreviewPriority::BACKGROUND));
	EXPECT_FALSE(Scheduler.CanStartPreviewJob(ESettingsResourcePreviewPriority::VISIBLE));
	EXPECT_FALSE(Scheduler.CanStartPreviewJob(ESettingsResourcePreviewPriority::NEAR_VISIBLE));
	EXPECT_FALSE(Scheduler.CanStartPreviewJob(ESettingsResourcePreviewPriority::BACKGROUND));
	EXPECT_FALSE(Scheduler.CanUploadPreview());
}

TEST(QmMonitoringHelpers, SettingsResourcePreviewShellOnlyBlocksPreviewJobsAndUploads)
{
	CSettingsResourcePreviewScheduler Scheduler;
	Scheduler.BeginFrame(2, 2, 2, 2);
	Scheduler.SetShellOnlyFrame(true);

	EXPECT_TRUE(Scheduler.CanHydrateMetadata(ESettingsResourcePreviewPriority::VISIBLE));
	EXPECT_FALSE(Scheduler.CanHydrateMetadata(ESettingsResourcePreviewPriority::NEAR_VISIBLE));
	EXPECT_FALSE(Scheduler.CanHydrateMetadata(ESettingsResourcePreviewPriority::BACKGROUND));
	EXPECT_FALSE(Scheduler.CanStartPreviewJob(ESettingsResourcePreviewPriority::VISIBLE));
	EXPECT_FALSE(Scheduler.CanStartPreviewJob(ESettingsResourcePreviewPriority::NEAR_VISIBLE));
	EXPECT_FALSE(Scheduler.CanStartPreviewJob(ESettingsResourcePreviewPriority::BACKGROUND));
	EXPECT_FALSE(Scheduler.CanUploadPreview());
}

TEST(QmMonitoringHelpers, SettingsResourcePreviewUploadBudgetUsesSharedLimiterBeforeLocalBudget)
{
	CGpuUploadLimiter Limiter;
	Limiter.OnFrameStart(0);
	SSettingsResourceMergeBudget MergeBudget;
	MergeBudget.m_MaxGpuUploads = 1;
	SResourcePreviewUploadBudget PreviewBudget;
	PreviewBudget.m_MaxUploads = 1;
	PreviewBudget.m_pMergeBudget = &MergeBudget;
	PreviewBudget.m_pGpuUploadLimiter = &Limiter;

	EXPECT_FALSE(SettingsResourcePreviewConsumeUploadBudget(PreviewBudget));
	EXPECT_EQ(PreviewBudget.m_UploadsUsed, 0);
	EXPECT_EQ(MergeBudget.m_MaxGpuUploads, 1);
	EXPECT_EQ(Limiter.UploadsThisFrame(), 0);

	Limiter.OnFrameStart(1);
	EXPECT_TRUE(SettingsResourcePreviewConsumeUploadBudget(PreviewBudget));
	EXPECT_EQ(PreviewBudget.m_UploadsUsed, 1);
	EXPECT_EQ(MergeBudget.m_MaxGpuUploads, 0);
	SettingsResourcePreviewCommitUploadBudget(PreviewBudget);
	EXPECT_EQ(Limiter.UploadsThisFrame(), 1);
}

TEST(QmMonitoringHelpers, SettingsResourcePreviewRejectsInvalidUploadImagesBeforeBudget)
{
	uint8_t aPixel[4] = {255, 255, 255, 255};

	CImageInfo MissingData;
	MissingData.m_Width = 1;
	MissingData.m_Height = 1;
	MissingData.m_Format = CImageInfo::FORMAT_RGBA;
	EXPECT_FALSE(SettingsResourcePreviewImageValidForUpload(MissingData));

	CImageInfo ZeroWidth;
	ZeroWidth.m_Width = 0;
	ZeroWidth.m_Height = 1;
	ZeroWidth.m_Format = CImageInfo::FORMAT_RGBA;
	ZeroWidth.m_pData = aPixel;
	EXPECT_FALSE(SettingsResourcePreviewImageValidForUpload(ZeroWidth));

	CImageInfo ZeroHeight;
	ZeroHeight.m_Width = 1;
	ZeroHeight.m_Height = 0;
	ZeroHeight.m_Format = CImageInfo::FORMAT_RGBA;
	ZeroHeight.m_pData = aPixel;
	EXPECT_FALSE(SettingsResourcePreviewImageValidForUpload(ZeroHeight));

	CImageInfo ValidImage;
	ValidImage.m_Width = 1;
	ValidImage.m_Height = 1;
	ValidImage.m_Format = CImageInfo::FORMAT_RGBA;
	ValidImage.m_pData = aPixel;
	EXPECT_TRUE(SettingsResourcePreviewImageValidForUpload(ValidImage));
}

TEST(QmMonitoringHelpers, SettingsResourcePreviewDrainRejectsInvalidImagesBeforeConsumingBudget)
{
	const std::string Source = ReadRepoFile("src/game/client/components/qmclient/settings_resource_preview.cpp");
	const std::string DrainBody = ExtractSourceFunctionBody(Source, "int CSettingsResourcePreviewUploadScheduler::Drain(SResourcePreviewUploadBudget &Budget, SResourcePreviewTelemetry &Telemetry, CSettingsResourcePreviewCache &Cache, IGraphics *pGraphics)");
	const std::string DrainOneBody = ExtractSourceFunctionBody(Source, "bool CSettingsResourcePreviewUploadScheduler::DrainOne(SResourcePreviewUploadBudget &Budget, SResourcePreviewTelemetry &Telemetry, CSettingsResourcePreviewCache &Cache, IGraphics *pGraphics)");
	ASSERT_FALSE(DrainBody.empty());
	ASSERT_FALSE(DrainOneBody.empty());

	EXPECT_NE(DrainBody.find("DrainOne(Budget, Telemetry, Cache, pGraphics)"), std::string::npos);
	const size_t ValidatePos = DrainOneBody.find("!SettingsResourcePreviewImageValidForUpload(Item.m_Image)");
	const size_t ConsumePos = DrainOneBody.find("SettingsResourcePreviewConsumeUploadBudget(Budget");
	ASSERT_NE(ValidatePos, std::string::npos);
	ASSERT_NE(ConsumePos, std::string::npos);
	EXPECT_LT(ValidatePos, ConsumePos);
	EXPECT_NE(DrainOneBody.find("Cache.MarkUploadFailed(Item.m_Key);"), std::string::npos);
	EXPECT_NE(DrainOneBody.find("Item.m_Finalize(false, IGraphics::CTextureHandle())"), std::string::npos);
	EXPECT_NE(DrainOneBody.find("m_vUploadQueue.push_front(std::move(Item));"), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsResourcePreviewUploadBudgetExhaustionRequeuesWithoutFailure)
{
	const std::string Source = ReadRepoFile("src/game/client/components/qmclient/settings_resource_preview.cpp");
	const std::string DrainOneBody = ExtractSourceFunctionBody(Source, "bool CSettingsResourcePreviewUploadScheduler::DrainOne(SResourcePreviewUploadBudget &Budget, SResourcePreviewTelemetry &Telemetry, CSettingsResourcePreviewCache &Cache, IGraphics *pGraphics)");
	ASSERT_FALSE(DrainOneBody.empty());

	const size_t BudgetExhaustedPos = DrainOneBody.find("!SettingsResourcePreviewConsumeUploadBudget(Budget)");
	const size_t UploadPos = DrainOneBody.find("pGraphics->LoadTextureRawMove");
	ASSERT_NE(BudgetExhaustedPos, std::string::npos);
	ASSERT_NE(UploadPos, std::string::npos);
	ASSERT_LT(BudgetExhaustedPos, UploadPos);
	const std::string BudgetExhaustedBody = DrainOneBody.substr(BudgetExhaustedPos, UploadPos - BudgetExhaustedPos);
	EXPECT_NE(BudgetExhaustedBody.find("m_vUploadQueue.push_front(std::move(Item));"), std::string::npos);
	EXPECT_EQ(BudgetExhaustedBody.find("Item.m_Image.Free()"), std::string::npos);
	EXPECT_EQ(BudgetExhaustedBody.find("Item.m_Finalize(false)"), std::string::npos);
	EXPECT_EQ(BudgetExhaustedBody.find("Cache.MarkPreviewJobDone(Item.m_Key, false)"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsSharedUploadSchedulerDoesNotRequeueMovedPreviewImages)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	EXPECT_NE(Source.find("DrainSharedResourcePreviewUploadQueue"), std::string::npos);
	EXPECT_NE(Source.find("while(gs_SettingsAssetsResourcePreviewUploadScheduler.QueueDepth() == 0 && !vReadyQueue.empty()"), std::string::npos);
	EXPECT_NE(Source.find("while(gs_SettingsAssetsResourcePreviewUploadScheduler.QueueDepth() == 0 && !WorkshopState.m_vReadyThumbQueue.empty()"), std::string::npos);
	const size_t LocalEnqueue = Source.find("gs_SettingsAssetsResourcePreviewUploadScheduler.EnqueueUploadToTarget(\n\t\t\t\tPreviewKey");
	ASSERT_NE(LocalEnqueue, std::string::npos);
	const size_t LocalFailure = Source.find("if(!gs_SettingsAssetsResourcePreviewUploadScheduler.DrainOne(PreviewUploadBudget", LocalEnqueue);
	ASSERT_NE(LocalFailure, std::string::npos);
	const size_t LocalEnd = Source.find("char aFinalizeUploadExtra", LocalFailure);
	ASSERT_NE(LocalEnd, std::string::npos);
	const std::string LocalFailureBody = Source.substr(LocalFailure, LocalEnd - LocalFailure);
	EXPECT_EQ(LocalFailureBody.find("vReadyQueue.push_front(Handle)"), std::string::npos);
	EXPECT_EQ(LocalFailureBody.find("ResetCustomItemPreviewState(*pItem)"), std::string::npos);
	EXPECT_NE(LocalFailureBody.find("GPU_UPLOAD_BUDGET"), std::string::npos);

	const size_t WorkshopEnqueue = Source.find("gs_SettingsAssetsResourcePreviewUploadScheduler.EnqueueUploadToTarget(\n\t\t\t\t\tPreviewKey", LocalEnd);
	ASSERT_NE(WorkshopEnqueue, std::string::npos);
	const size_t WorkshopFailure = Source.find("if(!gs_SettingsAssetsResourcePreviewUploadScheduler.DrainOne(WorkshopPreviewUploadBudget", WorkshopEnqueue);
	ASSERT_NE(WorkshopFailure, std::string::npos);
	const size_t WorkshopEnd = Source.find("WorkshopThumbUploadedBytesThisFrame", WorkshopFailure);
	ASSERT_NE(WorkshopEnd, std::string::npos);
	const std::string WorkshopFailureBody = Source.substr(WorkshopFailure, WorkshopEnd - WorkshopFailure);
	EXPECT_EQ(WorkshopFailureBody.find("WorkshopState.m_vReadyThumbQueue.push_front"), std::string::npos);
	EXPECT_EQ(WorkshopFailureBody.find("ResetWorkshopThumbReadyState(*pAsset)"), std::string::npos);
	EXPECT_NE(WorkshopFailureBody.find("GPU_UPLOAD_BUDGET"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsLocalEntityBgStartsPreviewPipelineAndKeepsFallbackVisible)
{
	// Entity Background Image cards are separate from the Entities tab. The artifact
	// pipeline may be pending, but the old map/video fallback must stay visible.
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());
	const size_t LocalBranch = Body.find("if(!UsesCombinedAssetList(pCurrentCategory))");
	ASSERT_NE(LocalBranch, std::string::npos);
	const size_t LocalBranchEnd = Body.find("if(const SAssetResourceCategory *pCategory = AssetResourceCategoryByTab(s_CurCustomTab); UsesCombinedAssetList(pCategory)", LocalBranch);
	ASSERT_NE(LocalBranchEnd, std::string::npos);
	const std::string LocalBody = Body.substr(LocalBranch, LocalBranchEnd - LocalBranch);

	EXPECT_NE(LocalBody.find("BuildAssetsResourcePreviewKey("), std::string::npos);
	EXPECT_NE(LocalBody.find("SettingsResourcePreviewDrawResult("), std::string::npos);
	EXPECT_NE(LocalBody.find("StartAssetsEntityBgPreviewArtifactJob("), std::string::npos);
	EXPECT_NE(LocalBody.find("RenderAssetsCardPreview(Shell, PreviewState"), std::string::npos);
	EXPECT_NE(LocalBody.find("if(s_CurCustomTab == ASSETS_TAB_ENTITY_BG && !PreviewReady && !AssetsContentWarmupBlocked)"), std::string::npos);
	EXPECT_NE(LocalBody.find("RenderEntityBgFallback(Shell.m_TextureRect)"), std::string::npos);
	EXPECT_NE(LocalBody.find("RenderEntityBgVideoFallback(Shell.m_TextureRect)"), std::string::npos);
}

TEST(QmMonitoringHelpers, TeeListDoesNotExposePartialSkinPreviewUploads)
{
	// The tee list must not render a CSkin while only some sprites have uploaded.
	// Partially uploaded skins looked like broken/default tees in the settings list.
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsTee(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("const CSkin *pSkin = State == CSkins::CSkinContainer::EState::LOADED ? pSkinContainer->Skin().get() : pDefaultSkin;"), std::string::npos);
	EXPECT_EQ(Body.find("const CSkin *pSkin = pSkinContainer->Skin() != nullptr ? pSkinContainer->Skin().get() : pDefaultSkin;"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsEntitiesPreviewUsesTileArtifactRenderer)
{
	// This is the Entities asset tab, not Entity Background Image. Entities need
	// the tile-layout preview renderer; drawing the source texture as one quad
	// makes the card preview look missing or wrong.
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Source.find("bool m_DrawEntityTileArtifact = false;"), std::string::npos);
	EXPECT_NE(Body.find("auto RenderAssetsEntityTilePreviewArtifact = "), std::string::npos);
	EXPECT_NE(Body.find("PreviewState.m_DrawEntityTileArtifact = PreviewState.m_Texture.IsValid();"), std::string::npos);
	EXPECT_NE(Body.find("PreviewState.m_DrawEntityTileArtifact = s_CurCustomTab == ASSETS_TAB_ENTITIES && gs_SettingsAssetsEntityGamePreview && Asset.m_ThumbTexture.IsValid();"), std::string::npos);
	EXPECT_NE(Body.find("RenderAssetsEntityTilePreviewArtifact(PreviewFrameRect, PreviewState.m_Texture);"), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsFrameSchedulerExposesResourceAndTextBudgets)
{
	const std::string Header = ReadRepoFile("src/game/client/components/settings_resource_jobs.h");
	const std::string Source = ReadRepoFile("src/game/client/components/settings_resource_jobs.cpp");
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");

	EXPECT_NE(Header.find("m_FrameId"), std::string::npos);
	EXPECT_NE(Header.find("m_aOperation"), std::string::npos);
	EXPECT_NE(Header.find("m_aPage"), std::string::npos);
	EXPECT_NE(Header.find("m_aTab"), std::string::npos);
	EXPECT_NE(Header.find("m_Subtab"), std::string::npos);
	EXPECT_NE(Header.find("m_aContext"), std::string::npos);
	EXPECT_NE(Header.find("m_TabSwitchFirstFrame"), std::string::npos);
	EXPECT_NE(Header.find("m_FramePressure"), std::string::npos);
	EXPECT_NE(Header.find("m_ResourceUploadTokens"), std::string::npos);
	EXPECT_NE(Header.find("m_TextContainerTokens"), std::string::npos);
	EXPECT_NE(Header.find("m_GlyphRasterizeTokens"), std::string::npos);
	EXPECT_NE(Header.find("m_GlyphUploadTokens"), std::string::npos);
	EXPECT_NE(Header.find("m_ParagraphLayoutTokens"), std::string::npos);
	EXPECT_NE(Header.find("m_MetadataLayoutTokens"), std::string::npos);
	EXPECT_NE(Header.find("m_PreviewArtifactTokens"), std::string::npos);
	EXPECT_NE(Header.find("m_TextureUploadTokens"), std::string::npos);
	EXPECT_NE(Header.find("m_CardDrawTokens"), std::string::npos);
	EXPECT_NE(Source.find("Output.m_ResourceUploadTokens = Output.m_GpuUploadTokens;"), std::string::npos);
	EXPECT_NE(Source.find("Output.m_TextContainerTokens = Output.m_TextPrebuildTokens;"), std::string::npos);
	EXPECT_NE(Source.find("Output.m_GlyphRasterizeTokens"), std::string::npos);
	EXPECT_NE(Source.find("Output.m_GlyphUploadTokens"), std::string::npos);
	EXPECT_NE(Source.find("Output.m_ParagraphLayoutTokens"), std::string::npos);
	EXPECT_NE(Source.find("Output.m_MetadataLayoutTokens"), std::string::npos);
	EXPECT_NE(Source.find("Output.m_PreviewArtifactTokens"), std::string::npos);
	EXPECT_NE(Source.find("Output.m_TextureUploadTokens"), std::string::npos);
	EXPECT_NE(Source.find("Output.m_CardDrawTokens"), std::string::npos);
	EXPECT_NE(Menus.find("resource_upload_tokens=%d"), std::string::npos);
	EXPECT_NE(Menus.find("text_container_tokens=%d"), std::string::npos);
	EXPECT_NE(Menus.find("glyph_rasterize_tokens=%d"), std::string::npos);
	EXPECT_NE(Menus.find("glyph_upload_tokens=%d"), std::string::npos);
	EXPECT_NE(Menus.find("paragraph_layout_tokens=%d"), std::string::npos);
	EXPECT_NE(Menus.find("metadata_layout_tokens=%d"), std::string::npos);
	EXPECT_NE(Menus.find("preview_artifact_tokens=%d"), std::string::npos);
	EXPECT_NE(Menus.find("texture_upload_tokens=%d"), std::string::npos);
	EXPECT_NE(Menus.find("card_draw_tokens=%d"), std::string::npos);

	SSettingsAdaptiveBudgetState State;
	SSettingsAdaptiveBudgetInput Input;
	Input.m_FrameMsAverage = 2.0f;
	Input.m_FrameMsP95 = 3.0f;
	Input.m_BackgroundBacklog = 4;
	const SSettingsAdaptiveBudgetOutput Output = SettingsAdaptiveBudgetStep(Input, State);
	EXPECT_EQ(Output.m_ResourceUploadTokens, Output.m_GpuUploadTokens);
	EXPECT_EQ(Output.m_TextContainerTokens, Output.m_TextPrebuildTokens);
	EXPECT_GE(Output.m_GlyphRasterizeTokens, 1);
	EXPECT_GE(Output.m_GlyphUploadTokens, 1);
	EXPECT_GE(Output.m_ParagraphLayoutTokens, 1);
	EXPECT_GE(Output.m_MetadataLayoutTokens, 1);
	EXPECT_GE(Output.m_PreviewArtifactTokens, 1);
	EXPECT_GE(Output.m_TextureUploadTokens, 1);
	EXPECT_GE(Output.m_CardDrawTokens, 1);

	SSettingsAdaptiveBudgetInput PressureInput;
	PressureInput.m_FrameMsAverage = 20.0f;
	PressureInput.m_FrameMsP95 = 20.0f;
	PressureInput.m_TargetFrameMs = 8.333f;
	PressureInput.m_BackgroundBacklog = 4;
	const SSettingsAdaptiveBudgetOutput PressureOutput = SettingsAdaptiveBudgetStep(PressureInput, State);
	EXPECT_EQ(PressureOutput.m_ParagraphLayoutTokens, 0);
	EXPECT_LE(PressureOutput.m_PreviewArtifactTokens, 1);
}

TEST(QmMonitoringHelpers, SettingsSchedulerFeedsTextRuntimeCostIntoAdaptiveBudget)
{
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Source = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string PrepareBody = ExtractSourceFunctionBody(Source, "void CMenus::PrepareSettingsAdaptiveBudgetInput(SSettingsAdaptiveBudgetInput &Input)");
	const std::string LogBody = ExtractSourceFunctionBody(Source, "void CMenus::LogSettingsAdaptiveBudget(const char *pSource, const SSettingsAdaptiveBudgetInput &Input, const SSettingsAdaptiveBudgetOutput &Output) const");
	ASSERT_FALSE(PrepareBody.empty());
	ASSERT_FALSE(LogBody.empty());

	EXPECT_NE(Header.find("m_TextContainerCreateMsEwma"), std::string::npos);
	EXPECT_NE(PrepareBody.find("TextRender()->QmTextRuntimeBudgetSnapshot()"), std::string::npos);
	EXPECT_NE(PrepareBody.find("Input.m_TextContainerCreateMsEwma"), std::string::npos);
	EXPECT_NE(LogBody.find("text_create_ewma_ms=%.3f"), std::string::npos);
	EXPECT_NE(LogBody.find("text_scroll_cap=%d"), std::string::npos);
}

TEST(QmMonitoringHelpers, UiFrameSchedulerOwnsTextAndResourceBudgets)
{
	const std::string Assets = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Ingame = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string Skins = ReadRepoFile("src/game/client/components/skins.cpp");

	EXPECT_NE(Menus.find("BeginSettingsUiFrameScheduler"), std::string::npos);
	EXPECT_NE(Menus.find("CurrentSettingsUiFrameBudget"), std::string::npos);
	EXPECT_NE(Assets.find("CurrentSettingsUiFrameBudget()"), std::string::npos);
	EXPECT_NE(Assets.find("AdaptiveBudget.m_MetadataLayoutTokens"), std::string::npos);
	EXPECT_NE(Assets.find("AdaptiveBudget.m_PreviewArtifactTokens"), std::string::npos);
	EXPECT_NE(Assets.find("AdaptiveBudget.m_TextureUploadTokens"), std::string::npos);
	EXPECT_NE(Ingame.find("GameClient()->FrameScheduler()->ComputeBudget(EFrameSchedulerConsumer::IngameServerInfo, TextBudgetInput)"), std::string::npos);
	EXPECT_NE(Ingame.find("m_IngameTextFrameBudget.m_TextContainerTokens"), std::string::npos);
	EXPECT_NE(Ingame.find("m_IngameTextFrameBudget.m_ParagraphLayoutTokens"), std::string::npos);
	EXPECT_NE(Skins.find("SettingsGpuUploadFrameBudgetForFrame()"), std::string::npos);
	EXPECT_NE(Skins.find("SettingsResourcePreviewConsumeUploadBudget(SkinPreviewUploadBudget"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsAndSkinsUseSharedTextureUploadDrain)
{
	const std::string Assets = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Skins = ReadRepoFile("src/game/client/components/skins.cpp");
	const std::string Preview = ReadRepoFile("src/game/client/components/qmclient/settings_resource_preview.h");
	const std::string AssetsBody = ExtractSourceFunctionBody(Assets, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	const std::string SkinProcessBody = ExtractSourceFunctionBody(Skins, "CSkins::ESkinProcessResult CSkins::ProcessSkinContainer(CSkinContainer *pSkinContainer, CSkinLoadingStats &Stats,\n\tint &SkinsProcessedThisFrame, std::chrono::nanoseconds StartTime,\n\tstd::chrono::nanoseconds MaxTime)");
	const std::string SkinDrainBody = ExtractSourceFunctionBody(Skins, "CSkins::ESkinProcessResult CSkins::DrainSettingsSkinPreviewUpload(CSkinContainer *pSkinContainer, CSkinLoadingStats &Stats,\n\tint &SkinsProcessedThisFrame, std::chrono::nanoseconds StartTime,\n\tstd::chrono::nanoseconds MaxTime)");
	ASSERT_FALSE(AssetsBody.empty());
	ASSERT_FALSE(SkinProcessBody.empty());
	ASSERT_FALSE(SkinDrainBody.empty());

	EXPECT_NE(Preview.find("CSettingsResourcePreviewUploadScheduler"), std::string::npos);
	EXPECT_NE(Preview.find("SettingsResourcePreviewImageValidForUpload"), std::string::npos);
	EXPECT_NE(Preview.find("EnqueueUploadToTarget"), std::string::npos);
	EXPECT_NE(Assets.find("gs_SettingsAssetsResourcePreviewUploadScheduler.Drain("), std::string::npos);
	EXPECT_NE(AssetsBody.find("EnqueueUploadToTarget("), std::string::npos);
	EXPECT_EQ(AssetsBody.find("ReplaceCustomItemPreviewTexture("), std::string::npos);
	EXPECT_EQ(AssetsBody.find("ReplaceWorkshopThumbTexture("), std::string::npos);
	EXPECT_NE(Assets.find("ResourcePreviewUploadBudget.m_MaxUploads = ResourcePreviewUploadMergeBudget.m_MaxGpuUploads;"), std::string::npos);
	EXPECT_NE(Assets.find("ResourcePreviewUploadBudget.m_pFrameBudget = SettingsFrameBudget();"), std::string::npos);
	EXPECT_NE(Skins.find("SResourcePreviewUploadBudget SkinPreviewUploadBudget"), std::string::npos);
	EXPECT_NE(SkinDrainBody.find("LoadSkinFinish(pSkinContainer"), std::string::npos);
	EXPECT_NE(SkinDrainBody.find("SettingsResourcePreviewConsumeUploadBudget(SkinPreviewUploadBudget, SETTINGS_SKIN_SOURCE_TEXTURE_UPLOADS)"), std::string::npos);
	EXPECT_EQ(SkinDrainBody.find("BeginSkinPreviewUpload(pSkinContainer"), std::string::npos);
	EXPECT_EQ(SkinDrainBody.find("UploadNextSkinPreviewSprite(pSkinContainer, SkinPreviewUploadBudget)"), std::string::npos);
	EXPECT_EQ(SkinDrainBody.find("FinishSkinPreviewUpload(pSkinContainer)"), std::string::npos);
	EXPECT_NE(Skins.find("preview_uploads"), std::string::npos);
}

TEST(QmMonitoringHelpers, IngameServerInfoUsesStableTextAndSnapshotCache)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderServerInfo(CUIRect MainView)");
	const std::string SetGamePageBody = ExtractSourceFunctionBody(Menus, "void CMenus::SetGamePage(int NewPage)");
	const std::string RenderBody = ExtractSourceFunctionBody(Menus, "void CMenus::Render()");
	const std::string ValueBody = ExtractSourceFunctionBody(Source, "void CMenus::RenderIngameServerInfoValueCached(const char *pTextId, unsigned &TextHash, const CUIRect *pRect, const char *pText, float Size, int Align, const SLabelProperties &LabelProps)");
	const std::string SnapshotDrainBody = ExtractSourceFunctionBody(Source, "void CMenus::DrainSnapshotTextContainers()");
	ASSERT_FALSE(Body.empty());
	ASSERT_FALSE(SetGamePageBody.empty());
	ASSERT_FALSE(RenderBody.empty());
	ASSERT_FALSE(ValueBody.empty());
	ASSERT_FALSE(SnapshotDrainBody.empty());

	EXPECT_NE(Header.find("struct SIngameServerInfoTextSnapshot"), std::string::npos);
	EXPECT_NE(Header.find("struct SMenuSnapshotTextKey"), std::string::npos);
	EXPECT_NE(Header.find("m_SnapshotTextCache"), std::string::npos);
	EXPECT_NE(Header.find("m_SnapshotTextPending"), std::string::npos);
	EXPECT_NE(Source.find("RenderIngameServerInfoValueCached("), std::string::npos);
	EXPECT_NE(Source.find("RequestSnapshotTextContainer("), std::string::npos);
	EXPECT_NE(Source.find("DrainSnapshotTextContainers()"), std::string::npos);
	EXPECT_NE(Body.find("DoServerInfoField(\"ingame-server-info-address-label\""), std::string::npos);
	EXPECT_NE(Body.find("RenderIngameServerInfoValueCached(\"ingame-server-info-name-value\""), std::string::npos);
	EXPECT_NE(Body.find("RenderIngameServerInfoValueCached(pValueTextId, ValueHash"), std::string::npos);
	EXPECT_NE(Body.find("\"ingame-server-info-address-value\""), std::string::npos);
	EXPECT_NE(Body.find("\"ingame-server-info-ping-value\""), std::string::npos);
	EXPECT_NE(Body.find("\"ingame-server-info-version-value\""), std::string::npos);
	EXPECT_NE(Body.find("\"ingame-server-info-password-value\""), std::string::npos);
	EXPECT_NE(Body.find("\"ingame-game-info-type-value\""), std::string::npos);
	EXPECT_NE(Body.find("\"ingame-game-info-map-value\""), std::string::npos);
	EXPECT_NE(Body.find("\"ingame-game-info-players-value\""), std::string::npos);
	EXPECT_EQ(Body.find("Ui()->DoLabel(&Label, CurrentServerInfo.m_aName"), std::string::npos);
	EXPECT_EQ(Body.find("Ui()->DoLabel(&ValueRect"), std::string::npos);
	EXPECT_NE(Body.find("m_IngameServerInfoTextSnapshot"), std::string::npos);
	EXPECT_NE(SetGamePageBody.find("NewPage == PAGE_SERVER_INFO"), std::string::npos);
	EXPECT_NE(SetGamePageBody.find("StartSettingsPerfFixedWindow(\"ingame_server_info\", \"online\", \"game\", \"server_info\", 30)"), std::string::npos);
	EXPECT_NE(RenderBody.find("if(m_GamePage == PAGE_SETTINGS)\n\t\t\t\tTextVisibleGuard.emplace(this);"), std::string::npos);
	EXPECT_NE(ValueBody.find("RenderSnapshotTextContainer(*pReadyElement"), std::string::npos);
	EXPECT_EQ(ValueBody.find("DoMenuLabelStreamed(MENU_TEXT_SCOPE_INGAME, *pReadyElement"), std::string::npos);
	EXPECT_NE(SnapshotDrainBody.find("m_IngameTextFrameBudget.m_TextContainerTokens"), std::string::npos);
	EXPECT_EQ(SnapshotDrainBody.find("m_CurrentSettingsUiFrameBudget.m_TextContainerTokens"), std::string::npos);
}

TEST(QmMonitoringHelpers, IngameEscOpenHasConcreteSectionTelemetry)
{
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Ingame = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string RenderBody = ExtractSourceFunctionBody(Menus, "void CMenus::Render()");
	const std::string GameBody = ExtractSourceFunctionBody(Ingame, "void CMenus::RenderGame(CUIRect MainView)");
	ASSERT_FALSE(RenderBody.empty());
	ASSERT_FALSE(GameBody.empty());

	EXPECT_EQ(Menus.find("ingame_esc_button_column"), std::string::npos);
	EXPECT_NE(GameBody.find("ingame_esc_button_column"), std::string::npos);
	EXPECT_NE(GameBody.find("MainView.HSplitTop(45.0f + (HasSecondaryButtonBar ? 35.0f : 0.0f), &ButtonBars, &MainView);"), std::string::npos);
	EXPECT_LT(GameBody.find("MainView.HSplitTop(45.0f + (HasSecondaryButtonBar ? 35.0f : 0.0f), &ButtonBars, &MainView);"), GameBody.find("ingame_esc_button_column"));
	const std::string ButtonColumnLog = "LogIngamePerfStage(Client(), \"ingame_esc_button_column\", ButtonColumnTimer.ElapsedMs(), false, aButtonColumnPerfExtra);";
	const size_t ButtonColumnTimerPos = GameBody.find("CPerfTimer ButtonColumnTimer;");
	const size_t ButtonColumnLogPos = GameBody.find(ButtonColumnLog);
	const size_t LastButtonControlPos = GameBody.find("Console()->ExecuteLine(\"toggle_local_console\");");
	const size_t TouchEditingBranchPos = GameBody.find("if(GameClient()->m_TouchControls.IsEditingActive())", LastButtonControlPos);
	const size_t NormalButtonColumnFlushPos = GameBody.find("\n\tLogButtonColumnPerf();", LastButtonControlPos);
	ASSERT_NE(ButtonColumnTimerPos, std::string::npos);
	ASSERT_NE(ButtonColumnLogPos, std::string::npos);
	ASSERT_NE(LastButtonControlPos, std::string::npos);
	ASSERT_NE(TouchEditingBranchPos, std::string::npos);
	ASSERT_NE(NormalButtonColumnFlushPos, std::string::npos);
	EXPECT_EQ(CountSubstring(GameBody, ButtonColumnLog), 1u);
	EXPECT_LT(ButtonColumnTimerPos, ButtonColumnLogPos);
	EXPECT_LT(LastButtonControlPos, NormalButtonColumnFlushPos);
	EXPECT_LT(NormalButtonColumnFlushPos, TouchEditingBranchPos);
	EXPECT_LT(NormalButtonColumnFlushPos, GameBody.find("RenderTouchControlsEditor"));
	EXPECT_LT(NormalButtonColumnFlushPos, GameBody.find("RenderTouchButtonEditor"));
	EXPECT_LT(NormalButtonColumnFlushPos, GameBody.find("RenderConfigSettings"));
	EXPECT_LT(NormalButtonColumnFlushPos, GameBody.find("RenderPreviewSettings"));

	const std::string ButtonColumnRegion = GameBody.substr(ButtonColumnTimerPos, TouchEditingBranchPos - ButtonColumnTimerPos);
	std::istringstream RegionStream(ButtonColumnRegion);
	std::string Line;
	std::string PreviousNonEmptyLine;
	while(std::getline(RegionStream, Line))
	{
		const std::string TrimmedLine = Trim(Line);
		if(TrimmedLine.empty())
			continue;
		if(TrimmedLine.find("return;") != std::string::npos)
		{
			if(PreviousNonEmptyLine != "if(ButtonColumnPerfLogged)")
				EXPECT_EQ(PreviousNonEmptyLine, "LogButtonColumnPerf();");
		}
		PreviousNonEmptyLine = TrimmedLine;
	}

	EXPECT_NE(RenderBody.find("ingame_esc_menu_shell"), std::string::npos);
	EXPECT_NE(RenderBody.find("ingame_esc_tab_content"), std::string::npos);
	EXPECT_NE(RenderBody.find("ingame_server_info_layout"), std::string::npos);
	EXPECT_NE(RenderBody.find("const char *pOperationName = SettingsPerfActiveOperation();"), std::string::npos);
	EXPECT_NE(RenderBody.find("operation=%s context=online page=%s tab=none frame=%"), std::string::npos);
	EXPECT_NE(RenderBody.find("context=online"), std::string::npos);
	EXPECT_NE(RenderBody.find("tab=none"), std::string::npos);
	EXPECT_NE(RenderBody.find("frame=%\" PRIu64"), std::string::npos);
	EXPECT_NE(RenderBody.find("LogPerfStage(Client(), \"ingame_esc_menu_shell\", ShellTimer.ElapsedMs(), false, aEscPerfExtra);"), std::string::npos);
	EXPECT_NE(RenderBody.find("LogPerfStage(Client(), \"ingame_esc_tab_content\", StageTimer.ElapsedMs(), TransitionActive, aEscPerfExtra);"), std::string::npos);
	EXPECT_NE(RenderBody.find("LogPerfStage(Client(), \"ingame_server_info_layout\", StageDurationMs, TransitionActive, aEscPerfExtra);"), std::string::npos);
}

TEST(QmMonitoringHelpers, ServerInfoTextPreparedOnlyWhenOpeningServerInfoPage)
{
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Ingame = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string OnRenderBody = ExtractSourceFunctionBody(Menus, "void CMenus::OnRender()");
	const std::string SetGamePageBody = ExtractSourceFunctionBody(Menus, "void CMenus::SetGamePage(int NewPage)");
	const std::string RenderBody = ExtractSourceFunctionBody(Ingame, "void CMenus::RenderServerInfo(CUIRect MainView)");
	ASSERT_FALSE(OnRenderBody.empty());
	ASSERT_FALSE(SetGamePageBody.empty());
	ASSERT_FALSE(RenderBody.empty());

	// Esc-opening PAGE_GAME and switching to PAGE_SERVER_INFO must not do the
	// dynamic server-info snapshot/MOTD prepare synchronously in the input path.
	// The prepare is allowed only from the OnRender frame-end background path.
	EXPECT_NE(Header.find("void PrepareIngameServerInfoTextRuntime("), std::string::npos);
	EXPECT_NE(Ingame.find("void CMenus::PrepareIngameServerInfoTextRuntime("), std::string::npos);
	EXPECT_NE(OnRenderBody.find("PrepareIngameServerInfoTextRuntime("), std::string::npos);
	EXPECT_EQ(SetGamePageBody.find("PrepareIngameServerInfoTextRuntime("), std::string::npos);
	EXPECT_NE(SetGamePageBody.find("NewPage == PAGE_SERVER_INFO"), std::string::npos);
	EXPECT_NE(SetGamePageBody.find("m_IngameServerInfoBackgroundPrepareRequested = false;"), std::string::npos);
	EXPECT_EQ(RenderBody.find("PrepareIngameServerInfoTextRuntime("), std::string::npos);
	EXPECT_NE(Ingame.find("event=server_info_text_prepare"), std::string::npos);
}

TEST(QmMonitoringHelpers, IngameServerInfoBackgroundPrepareDoesNotDrainMotdParagraph)
{
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Ingame = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string OnRenderBody = ExtractSourceFunctionBody(Menus, "void CMenus::OnRender()");
	const std::string PrepareBody = ExtractSourceFunctionBody(Ingame, "void CMenus::PrepareIngameServerInfoTextRuntime(const CUIRect *pMainView)");
	ASSERT_FALSE(OnRenderBody.empty());
	ASSERT_FALSE(PrepareBody.empty());

	// Esc-open background prewarm may prepare the small server-info value
	// snapshots and enqueue the MOTD paragraph, but it must not synchronously
	// drain the paragraph. Visible server-info frames must not run paragraph
	// drain either; that work is only allowed through the non-visible background
	// scheduler path.
	EXPECT_NE(OnRenderBody.find("PrepareIngameServerInfoTextRuntime();"), std::string::npos);
	EXPECT_NE(OnRenderBody.find("DrainIngameUiSnapshotTextRuntime();"), std::string::npos);
	EXPECT_NE(OnRenderBody.find("m_IngameServerInfoBackgroundPrepareRequested = !m_SnapshotTextPending.empty() || m_IngameMotdParagraphCache.m_Pending;"), std::string::npos);
	EXPECT_NE(PrepareBody.find("RequestIngameMotdParagraphCache("), std::string::npos);
	EXPECT_EQ(PrepareBody.find("DrainIngameUiTextRuntime("), std::string::npos);
	EXPECT_NE(PrepareBody.find("RequestSnapshotTextContainer("), std::string::npos);
}

TEST(QmMonitoringHelpers, ServerInfoDoesNotShowVisiblePlaceholderOnCacheMiss)
{
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Source = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string ValueBody = ExtractSourceFunctionBody(Source, "void CMenus::RenderIngameServerInfoValueCached(const char *pTextId, unsigned &TextHash, const CUIRect *pRect, const char *pText, float Size, int Align, const SLabelProperties &LabelProps)");
	const std::string SnapshotBody = ExtractSourceFunctionBody(Source, "bool CMenus::RequestSnapshotTextContainer(const char *pScope, const CUIRect *pRect, const char *pText, float Size, int Align, const SLabelProperties &LabelProps, CUIElement **ppReadyElement)");
	ASSERT_FALSE(ValueBody.empty());
	ASSERT_FALSE(SnapshotBody.empty());

	// Dynamic values may be budgeted, but the visible server-info card should
	// reuse the previous ready snapshot instead of showing a user-visible
	// placeholder/loading state on a cache miss.
	EXPECT_NE(Header.find("CUIElement *m_pLastReadyElement"), std::string::npos);
	EXPECT_NE(SnapshotBody.find("m_pLastReadyElement"), std::string::npos);
	EXPECT_NE(ValueBody.find("RenderSnapshotTextContainer(*pReadyElement"), std::string::npos);
	EXPECT_NE(ValueBody.find("RenderSnapshotTextContainer(*pLastReadyElement"), std::string::npos);
	EXPECT_EQ(ValueBody.find("Loading"), std::string::npos);
	// Dynamic values are content, not fixed UI chrome. A miss may enqueue a
	// snapshot text request, but must not synchronously draw through Ui()->DoLabel
	// because that recreates text containers in the visible server-info frame.
	EXPECT_EQ(ValueBody.find("Ui()->DoLabel"), std::string::npos);
}

TEST(QmMonitoringHelpers, MotdParagraphMissDoesNotRecreateInRenderPath)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderServerInfoMotd(CUIRect Motd)");
	ASSERT_FALSE(Body.empty());
	const std::string RequestBody = ExtractSourceFunctionBody(Source, "bool CMenus::RequestIngameMotdParagraphCache(CUIRect Motd, float FontSize)");
	ASSERT_FALSE(RequestBody.empty());

	EXPECT_NE(Header.find("struct SIngameMotdParagraphCache"), std::string::npos);
	EXPECT_NE(Header.find("m_IngameMotdParagraphCache"), std::string::npos);
	EXPECT_NE(Source.find("RequestIngameMotdParagraphCache("), std::string::npos);
	EXPECT_NE(Body.find("RequestIngameMotdParagraphCache(Motd"), std::string::npos);
	EXPECT_EQ(Source.find("EnsureIngameMotdParagraphCache("), std::string::npos);
	EXPECT_NE(RequestBody.find("if(!m_IngameMotdParagraphCache.m_Pending ||"), std::string::npos);
	EXPECT_EQ(RequestBody.find("SettingsAdaptiveBudgetStep("), std::string::npos);
	EXPECT_EQ(RequestBody.find("TextRender()->RecreateTextContainer("), std::string::npos);
	EXPECT_EQ(Body.find("TextRender()->RecreateTextContainer("), std::string::npos);
}

TEST(QmMonitoringHelpers, MotdParagraphHydratesOnlyThroughBudgetDrain)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderServerInfoMotd(CUIRect Motd)");
	const std::string DrainBody = ExtractSourceFunctionBody(Source, "void CMenus::DrainIngameMotdParagraphCache(CUIRect Motd, float FontSize, bool AllowCurrentFrame)");
	ASSERT_FALSE(Body.empty());
	ASSERT_FALSE(DrainBody.empty());

	EXPECT_NE(Header.find("m_Pending"), std::string::npos);
	EXPECT_NE(Header.find("m_PendingFrame"), std::string::npos);
	EXPECT_NE(Source.find("GameClient()->FrameScheduler()->ComputeBudget(EFrameSchedulerConsumer::IngameServerInfo, TextBudgetInput)"), std::string::npos);
	EXPECT_EQ(Source.find("BeginSettingsUiFrameScheduler(\"ingame_server_info_snapshot_text\""), std::string::npos);
	EXPECT_EQ(DrainBody.find("BeginSettingsUiFrameScheduler("), std::string::npos);
	EXPECT_NE(DrainBody.find("ParagraphLayoutTokens <= 0"), std::string::npos);
	EXPECT_NE(DrainBody.find("--m_IngameTextFrameBudget.m_ParagraphLayoutTokens"), std::string::npos);
	EXPECT_NE(Source.find("paragraph_budget_blocked"), std::string::npos);
	EXPECT_NE(Source.find("paragraph_cache_hit"), std::string::npos);
	EXPECT_NE(Source.find("paragraph_layout_ms"), std::string::npos);
	EXPECT_NE(DrainBody.find("TextRender()->CreateOrAppendTextContainer(m_IngameMotdParagraphCache.m_BuildTextContainerIndex"), std::string::npos);
	EXPECT_EQ(DrainBody.find("TextRender()->RecreateTextContainer(m_MotdTextContainerIndex"), std::string::npos);
	EXPECT_NE(DrainBody.find("m_IngameMotdParagraphCache.m_PendingFrame"), std::string::npos);
	EXPECT_NE(Body.find("RequestIngameMotdParagraphCache(Motd"), std::string::npos);
	EXPECT_EQ(Body.find("DrainIngameMotdParagraphCache(Motd"), std::string::npos);
	EXPECT_NE(Source.find("void CMenus::DrainIngameUiTextRuntime(bool AllowCurrentFrame)"), std::string::npos);
	EXPECT_NE(Header.find("m_PendingRect"), std::string::npos);
	EXPECT_NE(Header.find("m_BuildByteOffset"), std::string::npos);
	EXPECT_NE(Header.find("m_BuildTextContainerIndex"), std::string::npos);
	EXPECT_EQ(Header.find("m_IngameTextAdaptiveBudgetState"), std::string::npos);
	EXPECT_NE(Header.find("SSettingsAdaptiveBudgetOutput m_IngameTextFrameBudget"), std::string::npos);
	EXPECT_NE(Source.find("DrainIngameMotdParagraphCache(m_IngameMotdParagraphCache.m_PendingRect, m_IngameMotdParagraphCache.m_PendingFontSize, AllowCurrentFrame);"), std::string::npos);
	EXPECT_EQ(Source.find("m_IngameTextAdaptiveBudgetState"), std::string::npos);
	EXPECT_EQ(Source.find("m_IngameTextFrameBudget.m_ParagraphLayoutTokens = maximum(1, m_IngameTextFrameBudget.m_ParagraphLayoutTokens)"), std::string::npos);
	EXPECT_EQ(Source.find("m_CurrentSettingsUiFrameBudget.m_ParagraphLayoutTokens"), std::string::npos);
	EXPECT_NE(DrainBody.find("m_IngameTextFrameBudget.m_ParagraphLayoutTokens"), std::string::npos);
	EXPECT_EQ(Body.find("TextRender()->RecreateTextContainer(m_MotdTextContainerIndex"), std::string::npos);
}

TEST(QmMonitoringHelpers, MotdParagraphDrainBuildsInChunks)
{
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Source = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string DrainBody = ExtractSourceFunctionBody(Source, "void CMenus::DrainIngameMotdParagraphCache(CUIRect Motd, float FontSize, bool AllowCurrentFrame)");
	ASSERT_FALSE(DrainBody.empty());

	EXPECT_NE(Header.find("static constexpr int INGAME_MOTD_PARAGRAPH_CHUNK_BYTES"), std::string::npos);
	EXPECT_NE(Header.find("INGAME_MOTD_PARAGRAPH_CHUNK_BYTES = 24"), std::string::npos);
	EXPECT_NE(Header.find("CTextCursor m_BuildCursor"), std::string::npos);
	EXPECT_NE(Header.find("int m_BuildByteOffset"), std::string::npos);
	EXPECT_NE(DrainBody.find("INGAME_MOTD_PARAGRAPH_CHUNK_BYTES"), std::string::npos);
	EXPECT_NE(DrainBody.find("str_utf8_isstart"), std::string::npos);
	EXPECT_NE(DrainBody.find("TextRender()->CreateOrAppendTextContainer(m_IngameMotdParagraphCache.m_BuildTextContainerIndex"), std::string::npos);
	EXPECT_NE(DrainBody.find("m_IngameMotdParagraphCache.m_BuildByteOffset += ChunkLength"), std::string::npos);
	EXPECT_NE(DrainBody.find("m_MotdTextContainerIndex = m_IngameMotdParagraphCache.m_BuildTextContainerIndex"), std::string::npos);
	EXPECT_EQ(DrainBody.find("TextRender()->RecreateTextContainer(m_IngameMotdParagraphCache.m_PreviousTextContainerIndex"), std::string::npos);
	EXPECT_EQ(DrainBody.find("TextRender()->RecreateTextContainer(m_MotdTextContainerIndex"), std::string::npos);
}

TEST(QmMonitoringHelpers, MotdUsesReadyOrPreviousParagraphOnly)
{
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Source = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderServerInfoMotd(CUIRect Motd)");
	ASSERT_FALSE(Body.empty());

	// MOTD can be prepared asynchronously, but visible rendering must not show a
	// loading placeholder or blank range. It should render the current ready
	// paragraph, or a previous same-size paragraph while the new one hydrates.
	EXPECT_NE(Header.find("m_PreviousTextHash"), std::string::npos);
	EXPECT_NE(Header.find("m_PreviousTextContainerIndex"), std::string::npos);
	EXPECT_NE(Source.find("RenderIngameMotdPreviousParagraphCache("), std::string::npos);
	EXPECT_NE(Body.find("const bool RenderedMotdParagraph ="), std::string::npos);
	EXPECT_NE(Body.find("RenderIngameMotdPreviousParagraphCache("), std::string::npos);
	EXPECT_NE(Body.find("RenderIngameMotdFallbackText("), std::string::npos);
	EXPECT_NE(Body.find("server_info_not_ready=1"), std::string::npos);
	EXPECT_EQ(Body.find("Localize(\"Loading"), std::string::npos);
}

TEST(QmMonitoringHelpers, ValueSelectorDisplayUsesSingleLineShrink)
{
	const std::string Header = ReadRepoFile("src/game/client/ui.h");
	const std::string Source = ReadRepoFile("src/game/client/ui.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "SEditResult<int64_t> CUi::DoValueSelectorWithState(const void *pId, const CUIRect *pRect, const char *pLabel, int64_t Current, int64_t Min, int64_t Max, const SValueSelectorProperties &Props)");
	const std::string FlagsBody = ExtractSourceFunctionBody(Source, "static int GetFlagsForLabelProperties(const SLabelProperties &LabelProps, const CTextCursor *pReadCursor)");
	ASSERT_FALSE(Body.empty());
	ASSERT_FALSE(FlagsBody.empty());

	EXPECT_NE(Header.find("struct SLabelProperties"), std::string::npos);
	EXPECT_NE(Header.find("bool m_DisallowNewline"), std::string::npos);
	EXPECT_NE(FlagsBody.find("LabelProps.m_DisallowNewline ? TEXTFLAG_DISALLOW_NEWLINE : 0"), std::string::npos);
	EXPECT_NE(Body.find("SLabelProperties ValueLabelProps"), std::string::npos);
	EXPECT_NE(Body.find("ValueLabelProps.m_MaxWidth = pRect->w"), std::string::npos);
	EXPECT_NE(Body.find("ValueLabelProps.m_DisallowNewline = true"), std::string::npos);
	EXPECT_NE(Body.find("ValueLabelProps.m_StopAtEnd = true"), std::string::npos);
	EXPECT_NE(Body.find("ValueLabelProps.m_MinimumFontSize"), std::string::npos);
	EXPECT_NE(Body.find("const char *pDisplayText = m_ActiveValueSelectorState.m_pLastTextId == pId ? m_ActiveValueSelectorState.m_NumberInput.GetDisplayedString() : aBuf;"), std::string::npos);
	EXPECT_NE(Body.find("DoLabel(pRect, pDisplayText, 10.0f, TEXTALIGN_MC, ValueLabelProps);"), std::string::npos);
	EXPECT_NE(Body.find("auto RenderValueSelectorDisplay = [&]()"), std::string::npos);
	EXPECT_NE(Body.find("RenderValueSelectorDisplay();"), std::string::npos);
	EXPECT_LT(Body.find("RenderValueSelectorDisplay();"), Body.find("if(Inside && !MouseButton(0) && !MouseButton(1))"));
	EXPECT_EQ(Body.find("DoEditBox(&m_ActiveValueSelectorState.m_NumberInput"), std::string::npos);
	EXPECT_EQ(Body.find("DoLabel(pRect, aBuf, 10.0f, TEXTALIGN_MC);\n"), std::string::npos);
}

TEST(QmMonitoringHelpers, QmClientSliderValueInputReservesReadableValueWidth)
{
	const std::string Source = ReadRepoFile("src/game/client/components/qmclient/menus_qmclient.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsQmClient(CUIRect MainView, bool ContributorsPage, bool PrewarmOnly)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("auto RenderSliderWithValueInput = "), std::string::npos);
	EXPECT_NE(Body.find("const float InputWidth = std::clamp(72.0f * UiScale, 56.0f, 72.0f);"), std::string::npos);
	EXPECT_NE(Body.find("const float SuffixWidth = pSuffix[0] != '\\0' ? std::clamp(22.0f * UiScale, 18.0f, 24.0f) : 0.0f;"), std::string::npos);
	EXPECT_NE(Body.find("const float MinSliderWidth = std::clamp(54.0f * UiScale, 42.0f, 54.0f);"), std::string::npos);
}

TEST(QmMonitoringHelpers, SkinTransitionDurationLabelUsesSingleLineShrink)
{
	const std::string Source = ReadRepoFile("src/game/client/components/qmclient/menus_qmclient.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsQmClient(CUIRect MainView, bool ContributorsPage, bool PrewarmOnly)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("auto DoQmSettingsLabel = [this](const char *pTextId, CUIRect *pRect, const char *pText, float FontSize, int TextAlign = TEXTALIGN_ML, const SLabelProperties &LabelProps = {})"), std::string::npos);
	EXPECT_NE(Body.find("DoSettingsMenuLabel(SETTINGS_QMCLIENT, m_QmClientSettingsTab, m_QmClientSettingsTab, pTextId, pRect, pText, FontSize, TextAlign, LabelProps, (int)pRect->w);"), std::string::npos);
	EXPECT_NE(Body.find("SLabelProperties SkinTransitionDurationLabelProps"), std::string::npos);
	EXPECT_NE(Body.find("SkinTransitionDurationLabelProps.m_DisallowNewline = true"), std::string::npos);
	EXPECT_NE(Body.find("SkinTransitionDurationLabelProps.m_StopAtEnd = true"), std::string::npos);
	EXPECT_NE(Body.find("SkinTransitionDurationLabelProps.m_MinimumFontSize = 6.0f"), std::string::npos);
	EXPECT_NE(Body.find("DoQmSettingsLabel(\"qmclient-skin-transition-duration\", &LabelCol, Localize(\"Skin transition duration\"), LgBodySize, TEXTALIGN_ML, SkinTransitionDurationLabelProps);"), std::string::npos);
}

TEST(QmMonitoringHelpers, TeeSkinQueueOmitsCapacityAndUsesReadableIntervalInput)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsTee(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_EQ(Body.find("const bool CompactQueueCapacityControls"), std::string::npos);
	EXPECT_NE(Body.find("QueueSection.HSplitTop(20.0f, &QueueControls, &QueueSection);"), std::string::npos);
	EXPECT_NE(Body.find("QueueControls.VSplitLeft(QueueIntervalLabelWidth, &IntervalLabel, &IntervalControls);"), std::string::npos);
	EXPECT_NE(Body.find("IntervalControls.VSplitRight(QueueValueInputWidth + QueueValueUnitWidth, nullptr, &IntervalInputGroup);"), std::string::npos);
	EXPECT_NE(Body.find("IntervalInputGroup.VSplitRight(QueueValueUnitWidth, &IntervalInput, &IntervalUnit);"), std::string::npos);
	EXPECT_NE(Body.find("const float QueueValueInputWidth = 58.0f"), std::string::npos);
	EXPECT_NE(Body.find("const float QueueValueUnitWidth = 18.0f"), std::string::npos);
	EXPECT_NE(Body.find("const float QueueIntervalLabelWidth = 82.0f"), std::string::npos);
	EXPECT_NE(Body.find("SLabelProperties QueueControlLabelProps;"), std::string::npos);
	EXPECT_NE(Body.find("QueueControlLabelProps.m_DisallowNewline = true"), std::string::npos);
	EXPECT_NE(Body.find("QueueControlLabelProps.m_MinimumFontSize = 6.0f"), std::string::npos);
	EXPECT_NE(Body.find("DoSettingsMenuLabel(SETTINGS_TEE, -1, -1, \"tee-skin-queue-switch-interval\", &IntervalLabel, Localize(\"Switch interval\"), IntervalLabel.h * CUi::ms_FontmodHeight * 0.8f, TEXTALIGN_ML, QueueControlLabelProps, (int)IntervalLabel.w);"), std::string::npos);
	EXPECT_NE(Body.find("static CLineInputNumber s_aQueueIntervalInputs[NUM_DUMMIES];"), std::string::npos);
	EXPECT_NE(Body.find("CLineInputNumber &QueueIntervalInput = s_aQueueIntervalInputs[QueueDummy];"), std::string::npos);
	EXPECT_NE(Body.find("Ui()->DoEditBox(&QueueIntervalInput, &IntervalInput, 10.0f, IGraphics::CORNER_ALL, {}, TEXTALIGN_MC)"), std::string::npos);
	EXPECT_NE(Body.find("Ui()->DoLabel(&IntervalUnit, \"ms\""), std::string::npos);
	EXPECT_EQ(Body.find("DoValueSelectorWithState(&s_aQueueIntervalInputIds[QueueDummy]"), std::string::npos);
	EXPECT_EQ(Body.find("Ui()->DoScrollbarH(&QueueInterval"), std::string::npos);
	EXPECT_EQ(Body.find("IntervalScrollbar"), std::string::npos);
	EXPECT_EQ(Body.find("Localize(\"Queue capacity\")"), std::string::npos);
	EXPECT_EQ(Body.find("static char s_aQueueLengthInputIds[NUM_DUMMIES];"), std::string::npos);
	EXPECT_EQ(Body.find("Localize(\"Length\")"), std::string::npos);
	EXPECT_EQ(Body.find("Localize(\"Queue limit\")"), std::string::npos);
	EXPECT_EQ(Body.find("DoSettingsScrollbarOption(SETTINGS_TEE, -1, -1, \"tee-skin-queue-length\""), std::string::npos);
}

TEST(QmMonitoringHelpers, TeeSkinQueueDragShowsDropLineAndGhostRow)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsTee(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("static vec2 s_QueueDragGrabOffset = vec2(0.0f, 0.0f);"), std::string::npos);
	EXPECT_NE(Body.find("static CUIRect s_QueueDraggedRect;"), std::string::npos);
	EXPECT_NE(Body.find("CUIRect QueueDropLine;"), std::string::npos);
	EXPECT_NE(Body.find("HasQueueDropLine = true;"), std::string::npos);
	EXPECT_NE(Body.find("QueueDropLine.Draw(ColorRGBA(0.45f, 0.7f, 1.0f, 0.9f), IGraphics::CORNER_ALL, 1.0f);"), std::string::npos);
	EXPECT_NE(Body.find("CUIRect QueueDragGhost = s_QueueDraggedRect;"), std::string::npos);
	EXPECT_NE(Body.find("QueueDragGhost.x = Ui()->MouseX() - s_QueueDragGrabOffset.x;"), std::string::npos);
	EXPECT_NE(Body.find("QueueDragGhost.Draw(ColorRGBA(0.18f, 0.2f, 0.24f, 0.92f), IGraphics::CORNER_ALL, 4.0f);"), std::string::npos);
	EXPECT_NE(Body.find("QueueDragGhostLabel"), std::string::npos);
	EXPECT_NE(Body.find("s_QueueDragGrabOffset = Ui()->MousePos() - vec2(Item.m_Rect.x, Item.m_Rect.y);"), std::string::npos);
}

TEST(QmMonitoringHelpers, IngameTextRuntimeSkipsIdleLogLines)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::DrainIngameMotdParagraphCache(CUIRect Motd, float FontSize, bool AllowCurrentFrame)");
	ASSERT_FALSE(Body.empty());

	// The perf log can run for millions of frames. Avoid 0-cost text runtime rows
	// that only inflate logs and reports; keep rows when miss/block/layout happens.
	EXPECT_NE(Body.find("if(CacheHit || CacheMiss || BudgetBlocked || ParagraphLayoutMs >= QmPerfThresholdMs())"), std::string::npos);
	EXPECT_NE(Body.find("QmPerfLogPayload(\"perf/text\", aPayload, Client(), \"game\");"), std::string::npos);
}

TEST(QmMonitoringHelpers, RenderPathsDoNotCreateTextContainersSynchronously)
{
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Ingame = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string Assets = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string LabelBody = ExtractSourceFunctionBody(Menus, "void CMenus::DoMenuLabelStreamed(EMenuTextScope Scope, CUIElement &Element, const CUIRect *pRect, const char *pText, float Size, int Align, const SLabelProperties &LabelProps, int StrLen, const CTextCursor *pReadCursor, bool Render)");
	const std::string DrainBody = ExtractSourceFunctionBody(Menus, "void CMenus::DrainMenuTextContainerBuildRequests()");
	const std::string MotdBody = ExtractSourceFunctionBody(Ingame, "void CMenus::RenderServerInfoMotd(CUIRect Motd)");
	const std::string AssetsBody = ExtractSourceFunctionBody(Assets, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(LabelBody.empty());
	ASSERT_FALSE(DrainBody.empty());
	ASSERT_FALSE(MotdBody.empty());
	ASSERT_FALSE(AssetsBody.empty());

	const size_t QueueBranch = LabelBody.find("if(NeedsBuild && m_pSettingsTextPrebuildBudget == nullptr)");
	const size_t PrebuildBranch = LabelBody.find("if(m_pSettingsTextPrebuildBudget != nullptr)", QueueBranch);
	ASSERT_NE(QueueBranch, std::string::npos);
	ASSERT_NE(PrebuildBranch, std::string::npos);
	const std::string RenderRequestBranch = LabelBody.substr(QueueBranch, PrebuildBranch - QueueBranch);

	EXPECT_NE(LabelBody.find("QueueMenuTextContainerBuild"), std::string::npos);
	EXPECT_EQ(RenderRequestBranch.find("DrainMenuTextContainerBuild("), std::string::npos);
	EXPECT_NE(Menus.find("if(m_pSettingsTextPrebuildBudget != nullptr)"), std::string::npos);
	EXPECT_NE(Menus.find("DrainMenuTextContainerBuild(Element, pRect, pText, Size, Align, LabelProps, StrLen, pReadCursor, Render, &TextContainerRecreated);"), std::string::npos);
	EXPECT_NE(DrainBody.find("m_CurrentSettingsUiFrameBudget.m_TextContainerTokens > 0"), std::string::npos);
	EXPECT_NE(DrainBody.find("--m_CurrentSettingsUiFrameBudget.m_TextContainerTokens"), std::string::npos);
	EXPECT_NE(DrainBody.find("DrainMenuTextContainerBuild("), std::string::npos);
	EXPECT_EQ(LabelBody.find("Ui()->DoLabelStreamed(*Element.Rect(0)"), std::string::npos);
	EXPECT_EQ(MotdBody.find("TextRender()->RecreateTextContainer("), std::string::npos);
	EXPECT_EQ(AssetsBody.find("TextRender()->RecreateTextContainer("), std::string::npos);
	EXPECT_NE(AssetsBody.find("RequestAssetsCardMetadataHydration"), std::string::npos);
}

TEST(QmMonitoringHelpers, IngameServerInfoCardTitlesHaveImmediateFallback)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderServerInfo(CUIRect MainView)");
	const std::string MotdBody = ExtractSourceFunctionBody(Source, "void CMenus::RenderServerInfoMotd(CUIRect Motd)");
	ASSERT_FALSE(Body.empty());
	ASSERT_FALSE(MotdBody.empty());

	// Screenshot regression: budgeted ingame stable labels can be queued on cache
	// miss, but server-info card headers must still be visible in the current
	// frame instead of disappearing until the text container drain catches up.
	EXPECT_NE(Header.find("void DoIngameMenuTitleLabel("), std::string::npos);
	EXPECT_NE(Menus.find("void CMenus::DoIngameMenuTitleLabel("), std::string::npos);
	EXPECT_NE(Body.find("DoIngameMenuTitleLabel(PAGE_SERVER_INFO, \"ingame-server-info-title\""), std::string::npos);
	EXPECT_NE(Body.find("DoIngameMenuTitleLabel(PAGE_SERVER_INFO, \"ingame-game-info-title\""), std::string::npos);
	EXPECT_NE(MotdBody.find("DoIngameMenuTitleLabel(PAGE_SERVER_INFO, \"ingame-server-info-motd-title\""), std::string::npos);
	EXPECT_EQ(Body.find("DoIngameMenuTitleLabel(PAGE_SERVER_INFO, \"ingame-server-info-address-label\""), std::string::npos);
}

TEST(QmMonitoringHelpers, IngameMenuTabsHaveImmediateTextFallback)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "int CMenus::DoMenuTabV2(CButtonContainer *pButtonContainer, const char *pText, bool Active, const CUIRect *pRect, int Corners, const ColorRGBA *pCustomDefault, const ColorRGBA *pCustomActive, const ColorRGBA *pCustomHover, const CCommunityIcon *pCommunityIcon, CUIElement *pTextUiElement)");
	ASSERT_FALSE(Body.empty());

	// Screenshot regression: the Ghost / Call vote ingame tabs are critical
	// navigation labels. A budgeted streamed-text cache miss may enqueue the
	// real container, but the current frame must still draw readable text.
	EXPECT_NE(Source.find("\"ingame-tab-ghost\""), std::string::npos);
	EXPECT_NE(Source.find("\"ingame-tab-call-vote\""), std::string::npos);
	EXPECT_NE(Body.find("CUIElement::SUIElementRect *pElementRect = pTextUiElement->Rect(0);"), std::string::npos);
	EXPECT_NE(Body.find("const bool HadReadyContainer = pElementRect->m_UITextContainer.Valid();"), std::string::npos);
	EXPECT_NE(Body.find("DoMenuLabelStreamed(MENU_TEXT_SCOPE_INGAME, *pTextUiElement, &Label, pText, LabelFontSize, TEXTALIGN_MC);"), std::string::npos);
	EXPECT_NE(Body.find("if(pTextUiElement != &m_MenuTextFallbackElement && !HadReadyContainer && !pElementRect->m_UITextContainer.Valid())"), std::string::npos);
	EXPECT_NE(Body.find("Ui()->DoLabel(&Label, pText, LabelFontSize, TEXTALIGN_MC);"), std::string::npos);
}

TEST(QmMonitoringHelpers, IngameStableLabelsHaveImmediateTextFallback)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Ingame = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::DoIngameMenuLabel(int Page, const char *pTextId, const CUIRect *pRect, const char *pText, float Size, int Align, const SLabelProperties &LabelProps)");
	ASSERT_FALSE(Body.empty());

	// Server-info fixed labels such as Address/Ping/Version are visible UI
	// chrome, not optional content. They still use the ingame text pool, but a
	// current-frame cache miss must not make the label disappear.
	EXPECT_NE(Ingame.find("DoIngameMenuLabel(PAGE_SERVER_INFO, pLabelTextId"), std::string::npos);
	EXPECT_NE(Body.find("CUIElement::SUIElementRect *pElementRect = Element.Rect(0);"), std::string::npos);
	EXPECT_NE(Body.find("const bool HadReadyContainer = pElementRect->m_UITextContainer.Valid();"), std::string::npos);
	EXPECT_NE(Body.find("DoMenuLabelStreamed(MENU_TEXT_SCOPE_INGAME, Element, pRect, pText, Size, Align, LabelProps);"), std::string::npos);
	EXPECT_NE(Body.find("if(&Element != &m_MenuTextFallbackElement && !HadReadyContainer && !pElementRect->m_UITextContainer.Valid() && pRect != nullptr)"), std::string::npos);
	EXPECT_NE(Body.find("Ui()->DoLabel(pRect, pText, Size, Align, LabelProps);"), std::string::npos);
}

TEST(QmMonitoringHelpers, IngameServerInfoButtonsUseBudgetedTextPipeline)
{
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Source = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Ingame = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string Body = ExtractSourceFunctionBody(Ingame, "void CMenus::RenderServerInfo(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	// Server-info chrome includes action buttons as well as labels. Fixed
	// buttons must use the ingame text pool so opening a text-heavy server-info
	// page does not create button text containers synchronously.
	EXPECT_NE(Header.find("DoIngameMenuButton("), std::string::npos);
	EXPECT_NE(Source.find("int CMenus::DoIngameMenuButton("), std::string::npos);
	EXPECT_NE(Body.find("DoIngameMenuButton(PAGE_SERVER_INFO, \"ingame-server-info-copy-button\""), std::string::npos);
	EXPECT_EQ(Body.find("DoButton_Menu(&s_CopyButton, Localize(\"Copy info\")"), std::string::npos);
}

TEST(QmMonitoringHelpers, IngameMenuButtonKeepsNativeCenteredButtonTextLayout)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "int CMenus::DoIngameMenuButton(int Page, const char *pTextId, CButtonContainer *pButtonContainer, const char *pText, int Checked, const CUIRect *pRect, int Flags, int Corners, float Rounding)");
	ASSERT_FALSE(Body.empty());

	// Regression guard for ingame ESC buttons: the budgeted text helper must
	// use the same text rect calculation as DoButton_Menu, while keeping the
	// ingame scope and current-frame fallback. A hand-written rect diverges from
	// native centered labels and made Chinese button text appear off-center.
	EXPECT_NE(Source.find("CUIRect MenuButtonTextRect("), std::string::npos);
	EXPECT_NE(Body.find("CUIRect Text = MenuButtonTextRect(pRect, 0.0f, 0.0f);"), std::string::npos);
	EXPECT_NE(Body.find("DoButton_Menu(pButtonContainer, \"\", Checked"), std::string::npos);
	EXPECT_NE(Body.find("DoMenuLabelStreamed(MENU_TEXT_SCOPE_INGAME, TextElement"), std::string::npos);
	EXPECT_NE(Body.find("Ui()->DoLabel(&Text, pText"), std::string::npos);
	EXPECT_EQ(Body.find("CUIRect Text = *pRect;"), std::string::npos);
	EXPECT_EQ(Body.find("Text.HMargin(pRect->h >= 20.0f ? 2.0f : 1.0f"), std::string::npos);
	EXPECT_EQ(Body.find("HoverLift"), std::string::npos);
}

TEST(QmMonitoringHelpers, IngameMenuButtonDoesNotDependOnUiRuntimeAnimation)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "int CMenus::DoIngameMenuButton(int Page, const char *pTextId, CButtonContainer *pButtonContainer, const char *pText, int Checked, const CUIRect *pRect, int Flags, int Corners, float Rounding)");
	ASSERT_FALSE(Body.empty());

	// Regression guard for Esc-open crashes: ingame buttons can render before
	// the QmUi animation runtime is in a stable menu-frame context. Their
	// cached text path should use the native static button text rect and avoid
	// per-button animation runtime lookups.
	EXPECT_EQ(Body.find("GameClient()->UiRuntimeV2()->AnimRuntime()"), std::string::npos);
	EXPECT_EQ(Body.find("ResolveUiAnimValue("), std::string::npos);
	EXPECT_EQ(Body.find("HoverLift"), std::string::npos);
	EXPECT_NE(Body.find("CUIRect Text = MenuButtonTextRect(pRect, 0.0f, 0.0f);"), std::string::npos);
	EXPECT_NE(Body.find("DoButton_Menu(pButtonContainer, \"\", Checked"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsCardRightControlsStayCompact)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	// Screenshot regression: resource cards need the title/description to keep
	// priority. The right-side status pill and action icon are secondary
	// controls, so their reserved widths should stay compact.
	EXPECT_NE(Source.find("constexpr float AssetsCardStatusTagFontSize = 6.0f;"), std::string::npos);
	EXPECT_NE(Source.find("constexpr float AssetsCardStatusTagMinWidth = 24.0f;"), std::string::npos);
	EXPECT_NE(Source.find("constexpr float AssetsCardStatusTagMaxWidth = 36.0f;"), std::string::npos);
	EXPECT_NE(Source.find("constexpr float AssetsCardStatusTagHorizontalPadding = 2.0f;"), std::string::npos);
	EXPECT_NE(Source.find("constexpr float AssetCardHeaderControlHeight = 18.0f;"), std::string::npos);
	EXPECT_NE(Body.find("TitleRect.VSplitRight(16.0f, &TitleRect, &Shell.m_ActionButtonRect);"), std::string::npos);
	EXPECT_EQ(Source.find("constexpr float AssetsCardStatusTagMaxWidth = 52.0f;"), std::string::npos);
	EXPECT_EQ(Source.find("constexpr float AssetsCardStatusTagMaxWidth = 46.0f;"), std::string::npos);
	EXPECT_EQ(Body.find("TitleRect.VSplitRight(24.0f, &TitleRect, &Shell.m_ActionButtonRect);"), std::string::npos);
	EXPECT_EQ(Body.find("TitleRect.VSplitRight(20.0f, &TitleRect, &Shell.m_ActionButtonRect);"), std::string::npos);
}

TEST(QmMonitoringHelpers, StreamedLabelCachesSingleLineMiddleAlignMetrics)
{
	const std::string Header = ReadRepoFile("src/game/client/ui.h");
	const std::string Source = ReadRepoFile("src/game/client/ui.cpp");
	const std::string CachedBody = ExtractSourceFunctionBody(Source, "void CUi::DoLabel(CUIElement::SUIElementRect &RectEl, const CUIRect *pRect, const char *pText, float Size, int Align, const SLabelProperties &LabelProps, int StrLen, const CTextCursor *pReadCursor) const");
	const std::string StreamedBody = ExtractSourceFunctionBody(Source, "void CUi::DoLabelStreamed(CUIElement::SUIElementRect &RectEl, const CUIRect *pRect, const char *pText, float Size, int Align, const SLabelProperties &LabelProps, int StrLen, const CTextCursor *pReadCursor, bool Render, bool *pTextContainerRecreated) const");
	const std::string HelperBody = ExtractSourceFunctionBody(Source, "void CUi::RenderLabelTextContainerAligned(const CUIElement::SUIElementRect &RectEl, const CUIRect *pRect, int Align) const");
	ASSERT_FALSE(CachedBody.empty());
	ASSERT_FALSE(StreamedBody.empty());
	ASSERT_FALSE(HelperBody.empty());

	// Streamed single-line labels still need the same vertical centering metrics
	// as the immediate DoLabel path. Otherwise centered settings buttons and
	// centered preview placeholders drift upward after the text-pool rewrite.
	EXPECT_NE(Header.find("float m_BiggestCharacterHeight;"), std::string::npos);
	EXPECT_NE(Header.find("int m_LineCount;"), std::string::npos);
	EXPECT_NE(CachedBody.find("TextBounds.m_LineCount == 1 ? &TextBounds.m_BiggestCharacterHeight : nullptr"), std::string::npos);
	EXPECT_NE(CachedBody.find("RectEl.m_BiggestCharacterHeight = TextBounds.m_BiggestCharacterHeight;"), std::string::npos);
	EXPECT_NE(CachedBody.find("RectEl.m_LineCount = TextBounds.m_LineCount;"), std::string::npos);
	EXPECT_NE(HelperBody.find("RectEl.m_LineCount == 1 ? &RectEl.m_BiggestCharacterHeight : nullptr"), std::string::npos);
	EXPECT_NE(StreamedBody.find("RectEl.m_FontSize != Size"), std::string::npos);
	EXPECT_NE(StreamedBody.find("RectEl.m_TextAlign != Align"), std::string::npos);
	EXPECT_NE(StreamedBody.find("RectEl.m_LabelMaxWidth != LabelProps.m_MaxWidth"), std::string::npos);
	EXPECT_NE(StreamedBody.find("RectEl.m_LabelFlags != Flags"), std::string::npos);
	EXPECT_EQ(StreamedBody.find("CalcAlignedCursorPos(pRect, vec2(RectEl.m_Cursor.m_LongestLineWidth, RectEl.m_Cursor.Height()), Align);"), std::string::npos);
}

TEST(QmMonitoringHelpers, StreamedLabelRenderUsesCanonicalAlignmentHelper)
{
	const std::string Source = ReadRepoFile("src/game/client/ui.cpp");
	const std::string Header = ReadRepoFile("src/game/client/ui.h");
	const std::string StreamedBody = ExtractSourceFunctionBody(Source, "void CUi::DoLabelStreamed(CUIElement::SUIElementRect &RectEl, const CUIRect *pRect, const char *pText, float Size, int Align, const SLabelProperties &LabelProps, int StrLen, const CTextCursor *pReadCursor, bool Render, bool *pTextContainerRecreated) const");
	ASSERT_FALSE(StreamedBody.empty());

	EXPECT_NE(Header.find("RenderLabelTextContainerAligned"), std::string::npos);
	EXPECT_NE(Source.find("void CUi::RenderLabelTextContainerAligned"), std::string::npos);
	EXPECT_NE(StreamedBody.find("RenderLabelTextContainerAligned(RectEl, pRect, Align)"), std::string::npos);
	EXPECT_EQ(StreamedBody.find("TextRender()->RenderTextContainer(RectEl.m_UITextContainer"), std::string::npos);
	EXPECT_EQ(StreamedBody.find("pRect->x, pRect->y"), std::string::npos);
}

TEST(QmMonitoringHelpers, MenuLabelStreamedDoesNotBypassCachedLabelAlignment)
{
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Body = ExtractSourceFunctionBody(Menus, "void CMenus::DoMenuLabelStreamed(EMenuTextScope Scope, CUIElement &Element, const CUIRect *pRect, const char *pText, float Size, int Align, const SLabelProperties &LabelProps, int StrLen, const CTextCursor *pReadCursor, bool Render)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("Ui()->RenderLabelTextContainerAligned(*pElementRect, pRect, Align);"), std::string::npos);
	EXPECT_EQ(Body.find("TextRender()->RenderTextContainer(pElementRect->m_UITextContainer"), std::string::npos);
	EXPECT_EQ(Body.find("pRect->x, pRect->y"), std::string::npos);
}

TEST(QmMonitoringHelpers, MenuTextStyleKeyIncludesScaleButIgnoresAnimatedColorState)
{
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Source = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string BuildBody = ExtractSourceFunctionBody(Source, "CMenus::SMenuTextStyleKey CMenus::BuildMenuTextStyleKey(const CUIRect *pRect, float FontSize, int Align, const SLabelProperties &LabelProps) const");
	const std::string CacheKeyBody = ExtractSourceFunctionBody(Source, "std::string MenuTextCacheKey(CMenus::EMenuTextScope Scope, int Page, int Tab, int Subtab, const char *pTextId, const CMenus::SMenuTextStyleKey &StyleKey)");
	const std::string MenuNeedsBuildBody = ExtractSourceFunctionBody(Source, "bool CMenus::MenuTextContainerNeedsBuild(CUIElement &Element, const CUIRect *pRect, const char *pText, int StrLen, const CTextCursor *pReadCursor)");
	const std::string MenuStreamedBody = ExtractSourceFunctionBody(Source, "void CMenus::DoMenuLabelStreamed(EMenuTextScope Scope, CUIElement &Element, const CUIRect *pRect, const char *pText, float Size, int Align, const SLabelProperties &LabelProps, int StrLen, const CTextCursor *pReadCursor, bool Render)");
	const std::string UiSource = ReadRepoFile("src/game/client/ui.cpp");
	const std::string StreamedBody = ExtractSourceFunctionBody(UiSource, "void CUi::DoLabelStreamed(CUIElement::SUIElementRect &RectEl, const CUIRect *pRect, const char *pText, float Size, int Align, const SLabelProperties &LabelProps, int StrLen, const CTextCursor *pReadCursor, bool Render, bool *pTextContainerRecreated) const");

	EXPECT_NE(Header.find("int m_HiDpiScaleBucket"), std::string::npos);
	EXPECT_EQ(Header.find("int m_TextColorHash"), std::string::npos);
	EXPECT_EQ(Header.find("int m_OutlineColorHash"), std::string::npos);
	EXPECT_NE(Header.find("SMenuTextStyleKey BuildMenuTextStyleKey"), std::string::npos);
	EXPECT_FALSE(BuildBody.empty());
	EXPECT_FALSE(CacheKeyBody.empty());
	EXPECT_FALSE(MenuNeedsBuildBody.empty());
	EXPECT_FALSE(MenuStreamedBody.empty());
	EXPECT_FALSE(StreamedBody.empty());
	EXPECT_NE(BuildBody.find("Graphics()->ScreenHiDPIScale()"), std::string::npos);
	EXPECT_EQ(BuildBody.find("TextRender()->GetTextColor()"), std::string::npos);
	EXPECT_EQ(BuildBody.find("TextRender()->GetTextOutlineColor()"), std::string::npos);
	EXPECT_EQ(CacheKeyBody.find(":tc"), std::string::npos);
	EXPECT_EQ(CacheKeyBody.find(":oc"), std::string::npos);
	EXPECT_EQ(MenuNeedsBuildBody.find("ColorChanged"), std::string::npos);
	EXPECT_NE(MenuStreamedBody.find("pElementRect->m_TextColor = TextRender()->GetTextColor();"), std::string::npos);
	EXPECT_NE(MenuStreamedBody.find("pElementRect->m_TextOutlineColor = TextRender()->GetTextOutlineColor();"), std::string::npos);
	EXPECT_NE(StreamedBody.find("if(ColorChanged)"), std::string::npos);
	EXPECT_NE(StreamedBody.find("RectEl.m_TextColor = TextRender()->GetTextColor();"), std::string::npos);
	EXPECT_EQ(StreamedBody.find("|| ColorChanged ||"), std::string::npos);
	EXPECT_EQ(Source.find("StyleKey.m_UiScaleBucket = 100"), std::string::npos);
	EXPECT_EQ(Source.find("str_quickhash(\"default-text-style\")"), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsTextUsageSeparatesPoolHitFromRenderReadyHit)
{
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Source = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string MenuTextElementBody = ExtractSourceFunctionBody(Source, "CUIElement &CMenus::MenuTextElement(EMenuTextScope Scope, int Page, int Tab, int Subtab, const char *pTextId, const SMenuTextStyleKey &StyleKey)");
	const std::string StreamedBody = ExtractSourceFunctionBody(Source, "void CMenus::DoMenuLabelStreamed(EMenuTextScope Scope, CUIElement &Element, const CUIRect *pRect, const char *pText, float Size, int Align, const SLabelProperties &LabelProps, int StrLen, const CTextCursor *pReadCursor, bool Render)");
	ASSERT_FALSE(MenuTextElementBody.empty());
	ASSERT_FALSE(StreamedBody.empty());

	EXPECT_NE(Header.find("m_MenuTextStablePoolHitsThisFrame"), std::string::npos);
	EXPECT_NE(Header.find("m_MenuTextStableRenderReadyHitsThisFrame"), std::string::npos);
	EXPECT_NE(Header.find("m_MenuTextStableBuildQueuedThisFrame"), std::string::npos);
	EXPECT_NE(Header.find("m_MenuTextStableFallbackImmediateThisFrame"), std::string::npos);
	EXPECT_NE(Source.find("pool_hit=%d render_ready_hit=%d"), std::string::npos);
	EXPECT_NE(Source.find("build_queued=%d fallback_immediate=%d"), std::string::npos);
	EXPECT_NE(MenuTextElementBody.find("++m_MenuTextStablePoolHitsThisFrame"), std::string::npos);
	EXPECT_NE(StreamedBody.find("++m_MenuTextStableRenderReadyHitsThisFrame"), std::string::npos);
	EXPECT_NE(StreamedBody.find("++m_MenuTextStableBuildQueuedThisFrame"), std::string::npos);
	EXPECT_NE(StreamedBody.find("++m_MenuTextStableFallbackImmediateThisFrame"), std::string::npos);
}

TEST(QmMonitoringHelpers, IngameImmediateTextFallbackIsCountedForSchedulerCoverage)
{
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Source = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string TabBody = ExtractSourceFunctionBody(Source, "int CMenus::DoMenuTabV2(CButtonContainer *pButtonContainer, const char *pText, bool Active, const CUIRect *pRect, int Corners, const ColorRGBA *pCustomDefault, const ColorRGBA *pCustomActive, const ColorRGBA *pCustomHover, const CCommunityIcon *pCommunityIcon, CUIElement *pTextUiElement)");
	const std::string ButtonBody = ExtractSourceFunctionBody(Source, "int CMenus::DoIngameMenuButton(int Page, const char *pTextId, CButtonContainer *pButtonContainer, const char *pText, int Checked, const CUIRect *pRect, int Flags, int Corners, float Rounding)");
	const std::string LabelBody = ExtractSourceFunctionBody(Source, "void CMenus::DoIngameMenuLabel(int Page, const char *pTextId, const CUIRect *pRect, const char *pText, float Size, int Align, const SLabelProperties &LabelProps)");
	const std::string TitleBody = ExtractSourceFunctionBody(Source, "void CMenus::DoIngameMenuTitleLabel(int Page, const char *pTextId, const CUIRect *pRect, const char *pText, float Size, int Align, const SLabelProperties &LabelProps)");
	ASSERT_FALSE(TabBody.empty());
	ASSERT_FALSE(ButtonBody.empty());
	ASSERT_FALSE(LabelBody.empty());
	ASSERT_FALSE(TitleBody.empty());

	EXPECT_NE(Header.find("CountMenuTextImmediateFallback()"), std::string::npos);
	EXPECT_NE(Source.find("void CMenus::CountMenuTextImmediateFallback()"), std::string::npos);
	EXPECT_NE(Source.find("scheduler_coverage=%s"), std::string::npos);
	EXPECT_NE(Source.find("FallbackImmediate > 0 ? \"uncovered\" : \"budgeted\""), std::string::npos);
	EXPECT_NE(TabBody.find("if(pTextUiElement != &m_MenuTextFallbackElement && !HadReadyContainer && !pElementRect->m_UITextContainer.Valid())"), std::string::npos);
	EXPECT_NE(ButtonBody.find("if(&TextElement != &m_MenuTextFallbackElement && !HadReadyContainer && !pElementRect->m_UITextContainer.Valid())"), std::string::npos);
	EXPECT_NE(LabelBody.find("if(&Element != &m_MenuTextFallbackElement && !HadReadyContainer && !pElementRect->m_UITextContainer.Valid() && pRect != nullptr)"), std::string::npos);
	EXPECT_NE(TitleBody.find("if(&Element != &m_MenuTextFallbackElement && !HadReadyContainer && !pElementRect->m_UITextContainer.Valid() && pRect != nullptr)"), std::string::npos);
}

TEST(QmMonitoringHelpers, TextRenderingStabilizationHasVisualChecklist)
{
	const std::string Checklist = ReadRepoFile("docs/superpowers/plans/2026-06-18-text-rendering-stabilization-observability-visual-checklist.md");
	EXPECT_NE(Checklist.find("Button text is centered"), std::string::npos);
	EXPECT_NE(Checklist.find("render-ready hit coverage"), std::string::npos);
	EXPECT_NE(Checklist.find("Small-card right tags/buttons keep fixed priority"), std::string::npos);
}

TEST(QmMonitoringHelpers, IngameFixedChromeUsesBudgetedTextPipeline)
{
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Source = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Ingame = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string GameBody = ExtractSourceFunctionBody(Ingame, "void CMenus::RenderGame(CUIRect MainView)");
	const std::string CallVoteBody = ExtractSourceFunctionBody(Ingame, "void CMenus::RenderServerControl(CUIRect MainView)");
	const std::string UnfinishedBody = ExtractSourceFunctionBody(Ingame, "void CMenus::RenderUnfinishedMaps(CUIRect MainView)");
	const std::string GhostBody = ExtractSourceFunctionBody(Ingame, "void CMenus::RenderGhost(CUIRect MainView)");
	ASSERT_FALSE(GameBody.empty());
	ASSERT_FALSE(CallVoteBody.empty());
	ASSERT_FALSE(UnfinishedBody.empty());
	ASSERT_FALSE(GhostBody.empty());

	// Fixed ingame chrome is visible immediately when opening ESC, so it needs
	// the ingame text pool and its current-frame fallback. Leaving these labels
	// on raw DoButton/DoLabel paths recreates containers during the first visible
	// frame and caused server-info/title regressions in earlier iterations.
	EXPECT_NE(Header.find("DoIngameMenuCheckBox("), std::string::npos);
	EXPECT_NE(Source.find("int CMenus::DoIngameMenuCheckBox("), std::string::npos);
	EXPECT_NE(GameBody.find("DoIngameMenuButton(PAGE_GAME, \"ingame-game-disconnect\""), std::string::npos);
	EXPECT_NE(GameBody.find("DoIngameMenuButton(PAGE_GAME, \"ingame-game-edit-hud\""), std::string::npos);
	EXPECT_NE(GameBody.find("DoIngameMenuCheckBox(PAGE_GAME, \"ingame-game-edit-touch-controls\""), std::string::npos);
	EXPECT_EQ(GameBody.find("DoButton_Menu(&s_DisconnectButton, Localize(\"Disconnect\")"), std::string::npos);
	EXPECT_EQ(GameBody.find("DoButton_CheckBox(&s_TouchControlsEditCheckbox, Localize(\"Edit touch controls\")"), std::string::npos);

	EXPECT_NE(CallVoteBody.find("DoIngameMenuButton(PAGE_CALLVOTE, \"ingame-call-vote-call\""), std::string::npos);
	EXPECT_NE(CallVoteBody.find("DoIngameMenuLabel(PAGE_CALLVOTE, \"ingame-call-vote-reason-label\""), std::string::npos);
	EXPECT_NE(CallVoteBody.find("DoIngameMenuButton(PAGE_CALLVOTE, \"ingame-call-vote-force\""), std::string::npos);
	EXPECT_EQ(CallVoteBody.find("DoButton_Menu(&s_CallVoteButton, Localize(\"Call vote\")"), std::string::npos);
	EXPECT_EQ(CallVoteBody.find("Ui()->DoLabel(&Reason, pLabel, 14.0f"), std::string::npos);

	EXPECT_NE(UnfinishedBody.find("DoIngameMenuTitleLabel(PAGE_CALLVOTE, \"ingame-unfinished-maps-title\""), std::string::npos);
	EXPECT_NE(UnfinishedBody.find("DoIngameMenuCheckBox(PAGE_CALLVOTE, \"ingame-unfinished-auto-start-vote\""), std::string::npos);
	EXPECT_NE(UnfinishedBody.find("DoIngameMenuButton(PAGE_CALLVOTE, \"ingame-unfinished-random-pick\""), std::string::npos);
	EXPECT_EQ(UnfinishedBody.find("DoButton_CheckBox(&s_UnfinishedMapAutoVote, Localize(\"Auto start vote\")"), std::string::npos);

	EXPECT_NE(GhostBody.find("DoIngameMenuButton(PAGE_GHOST, \"ingame-ghost-directory\""), std::string::npos);
	EXPECT_NE(GhostBody.find("DoIngameMenuButton(PAGE_GHOST, ActivateAll ? \"ingame-ghost-activate-all\" : \"ingame-ghost-deactivate-all\""), std::string::npos);
	EXPECT_NE(GhostBody.find("DoIngameMenuButton(PAGE_GHOST, \"ingame-ghost-delete\""), std::string::npos);
	EXPECT_EQ(GhostBody.find("DoButton_Menu(&s_DirectoryButton, Localize(\"Ghosts directory\")"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsToolbarAndPlaceholdersUseBudgetedTextPipeline)
{
	const std::string Assets = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Assets, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	// Assets tab switches should not spend the first visible frame creating
	// toolbar or per-card placeholder text containers. Toolbar text uses the
	// settings text pool, while card placeholders stay visual-only until preview
	// content is ready.
	EXPECT_NE(Body.find("DoSettingsButton_Menu(SETTINGS_ASSETS, s_CurCustomTab, s_CurCustomTab, &s_AssetsEditorButton"), std::string::npos);
	EXPECT_NE(Body.find("DoSettingsButton_Menu(SETTINGS_ASSETS, s_CurCustomTab, s_CurCustomTab, &s_ShowWorkshopAssetsId"), std::string::npos);
	EXPECT_NE(Body.find("DoSettingsButton_Menu(SETTINGS_ASSETS, s_CurCustomTab, s_CurCustomTab, &s_WorkshopSyncId"), std::string::npos);
	EXPECT_NE(Body.find("DoSettingsButton_Menu(SETTINGS_ASSETS, s_CurCustomTab, s_CurCustomTab, &s_AssetsDirId"), std::string::npos);
	EXPECT_EQ(Body.find("DoButton_Menu(&s_AssetsEditorButton, Localize(\"Assets editor\")"), std::string::npos);
	EXPECT_EQ(Body.find("DoButton_Menu(&s_ShowWorkshopAssetsId, Localize(\"Show Workshop Assets\")"), std::string::npos);
	EXPECT_EQ(Body.find("DoButton_Menu(&s_WorkshopSyncId, Localize(\"Sync Workshop Assets\")"), std::string::npos);
	EXPECT_EQ(Body.find("DoButton_Menu(&s_AssetsDirId, Localize(\"Assets directory\")"), std::string::npos);
	EXPECT_NE(Body.find("DoSettingsMenuLabel(SETTINGS_ASSETS, s_CurCustomTab, s_CurCustomTab, \"assets-loading-list\""), std::string::npos);
	EXPECT_NE(Body.find("DoSettingsMenuLabel(SETTINGS_ASSETS, s_CurCustomTab, s_CurCustomTab, \"assets-workshop-no-assets\""), std::string::npos);
	EXPECT_EQ(Body.find("Ui()->DoLabel(&LoadingRect, Localize(\"Loading"), std::string::npos);
}

TEST(QmMonitoringHelpers, IngameCriticalTextFallbacksAreLimited)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string StreamedBody = ExtractSourceFunctionBody(Source, "void CMenus::DoMenuLabelStreamed(EMenuTextScope Scope, CUIElement &Element, const CUIRect *pRect, const char *pText, float Size, int Align, const SLabelProperties &LabelProps, int StrLen, const CTextCursor *pReadCursor, bool Render)");
	const std::string TabBody = ExtractSourceFunctionBody(Source, "int CMenus::DoMenuTabV2(CButtonContainer *pButtonContainer, const char *pText, bool Active, const CUIRect *pRect, int Corners, const ColorRGBA *pCustomDefault, const ColorRGBA *pCustomActive, const ColorRGBA *pCustomHover, const CCommunityIcon *pCommunityIcon, CUIElement *pTextUiElement)");
	const std::string TitleBody = ExtractSourceFunctionBody(Source, "void CMenus::DoIngameMenuTitleLabel(int Page, const char *pTextId, const CUIRect *pRect, const char *pText, float Size, int Align, const SLabelProperties &LabelProps)");
	const std::string LabelBody = ExtractSourceFunctionBody(Source, "void CMenus::DoIngameMenuLabel(int Page, const char *pTextId, const CUIRect *pRect, const char *pText, float Size, int Align, const SLabelProperties &LabelProps)");
	ASSERT_FALSE(StreamedBody.empty());
	ASSERT_FALSE(TabBody.empty());
	ASSERT_FALSE(TitleBody.empty());
	ASSERT_FALSE(LabelBody.empty());

	// Keep immediate fallback scoped to ingame stable UI chrome. Expanding it
	// inside DoMenuLabelStreamed would make ordinary settings text misses
	// synchronous again and undo the text-budget work.
	EXPECT_EQ(StreamedBody.find("!HadReadyContainer"), std::string::npos);
	EXPECT_NE(TabBody.find("!HadReadyContainer"), std::string::npos);
	EXPECT_NE(TitleBody.find("!HadReadyContainer"), std::string::npos);
	EXPECT_NE(LabelBody.find("!HadReadyContainer"), std::string::npos);
}

TEST(QmMonitoringHelpers, StableTextUsageTelemetryIsClassifiedAsStaticStable)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void LogSettingsTextPoolUsage(IClient *pClient, CMenus::EMenuTextScope Scope, const char *pScopeName, int Page, int Tab, int Subtab, const char *pOperation, uint64_t Frame, int Candidates, int Hits, int Reused, int Misses, int Stales, int TextNew, int TextReused, int Planned, int Unplanned, int PoolHits, int RenderReadyHits, int BuildQueued, int FallbackImmediate)");
	ASSERT_FALSE(Body.empty());

	// Static stable text is the only class allowed in the stable-text
	// denominator. Dynamic snapshot values, paragraphs, and card metadata have
	// separate hit-rate counters so a bad server name or resource title cache
	// cannot make the stable descriptor plan look broken.
	EXPECT_NE(Body.find("text_class=static_stable"), std::string::npos);
	EXPECT_EQ(Body.find("dynamic_snapshot"), std::string::npos);
	EXPECT_EQ(Body.find("paragraph"), std::string::npos);
	EXPECT_EQ(Body.find("card_metadata"), std::string::npos);
}

TEST(QmMonitoringHelpers, IngameServerInfoDoesNotTouchUninitializedUiElementRects)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderIngameServerInfoValueCached(const char *pTextId, unsigned &TextHash, const CUIRect *pRect, const char *pText, float Size, int Align, const SLabelProperties &LabelProps)");
	ASSERT_FALSE(Body.empty());

	EXPECT_EQ(Header.find("CUIElement m_IngameServerNameTextElement"), std::string::npos);
	EXPECT_EQ(Body.find("Element.Rect(0)"), std::string::npos);
	EXPECT_EQ(Body.find("DeleteTextContainer"), std::string::npos);
	EXPECT_NE(Body.find("(void)TextHash;"), std::string::npos);
}

TEST(QmMonitoringHelpers, IngameTextRuntimeDrainsOutsideRenderFunctions)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string RenderBody = ExtractSourceFunctionBody(Source, "void CMenus::RenderServerInfo(CUIRect MainView)");
	const std::string MotdBody = ExtractSourceFunctionBody(Source, "void CMenus::RenderServerInfoMotd(CUIRect Motd)");
	const std::string SnapshotDrainBody = ExtractSourceFunctionBody(Source, "void CMenus::DrainIngameUiSnapshotTextRuntime()");
	const std::string DrainBody = ExtractSourceFunctionBody(Source, "void CMenus::DrainIngameUiTextRuntime(bool AllowCurrentFrame)");
	ASSERT_FALSE(RenderBody.empty());
	ASSERT_FALSE(MotdBody.empty());
	ASSERT_FALSE(SnapshotDrainBody.empty());
	ASSERT_FALSE(DrainBody.empty());

	EXPECT_NE(Header.find("void DrainIngameUiSnapshotTextRuntime();"), std::string::npos);
	EXPECT_NE(Header.find("void DrainIngameUiTextRuntime(bool AllowCurrentFrame = false);"), std::string::npos);
	EXPECT_EQ(RenderBody.find("DrainSnapshotTextContainers()"), std::string::npos);
	EXPECT_EQ(MotdBody.find("DrainIngameMotdParagraphCache("), std::string::npos);
	EXPECT_NE(SnapshotDrainBody.find("DrainSnapshotTextContainers()"), std::string::npos);
	EXPECT_NE(DrainBody.find("DrainIngameUiSnapshotTextRuntime()"), std::string::npos);
	EXPECT_NE(DrainBody.find("DrainIngameMotdParagraphCache("), std::string::npos);
}

TEST(QmMonitoringHelpers, IngameEscOpenDefersServerInfoRuntimeDrain)
{
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string OnRenderBody = ExtractSourceFunctionBody(Menus, "void CMenus::OnRender()");
	ASSERT_FALSE(OnRenderBody.empty());

	EXPECT_NE(OnRenderBody.find("StartSettingsPerfFixedWindow(\"ingame_esc_open\""), std::string::npos);
	EXPECT_EQ(OnRenderBody.find("PrebuildIngameEscTextPoolBeforeOpen("), std::string::npos);
	EXPECT_EQ(OnRenderBody.find("PrebuildSettingsMenuTextPool("), std::string::npos);
	EXPECT_NE(Header.find("uint64_t m_IngameEscOpenFrame"), std::string::npos);
	EXPECT_NE(Header.find("bool m_IngameServerInfoBackgroundPrepareRequested"), std::string::npos);
	EXPECT_NE(OnRenderBody.find("m_IngameEscOpenFrame = Client()->PerfFrame();"), std::string::npos);
	EXPECT_NE(OnRenderBody.find("m_IngameServerInfoBackgroundPrepareRequested = true;"), std::string::npos);
	EXPECT_NE(OnRenderBody.find("Client()->PerfFrame() > m_IngameEscOpenFrame"), std::string::npos);
	EXPECT_NE(OnRenderBody.find("PrepareIngameServerInfoTextRuntime();"), std::string::npos);
	EXPECT_NE(OnRenderBody.find("if(m_GamePage == PAGE_SERVER_INFO)"), std::string::npos);
	const size_t VisibleServerInfoBranch = OnRenderBody.find("if(m_GamePage == PAGE_SERVER_INFO)");
	const size_t BackgroundPrepareBranch = OnRenderBody.find("else if(m_IngameServerInfoBackgroundPrepareRequested", VisibleServerInfoBranch);
	ASSERT_NE(VisibleServerInfoBranch, std::string::npos);
	ASSERT_NE(BackgroundPrepareBranch, std::string::npos);
	const std::string VisibleBranch = OnRenderBody.substr(VisibleServerInfoBranch, BackgroundPrepareBranch - VisibleServerInfoBranch);
	EXPECT_NE(VisibleBranch.find("DrainIngameUiSnapshotTextRuntime();"), std::string::npos);
	EXPECT_EQ(VisibleBranch.find("DrainIngameUiTextRuntime(false);"), std::string::npos);
	EXPECT_NE(OnRenderBody.find("DrainIngameUiTextRuntime(false);"), std::string::npos);
	EXPECT_EQ(OnRenderBody.find("else if(GameClient()->m_Motd.ServerMotd()[0])"), std::string::npos);
	EXPECT_NE(OnRenderBody.find("DrainIngameUiSnapshotTextRuntime();"), std::string::npos);
}

TEST(QmMonitoringHelpers, ServerInfoLayoutAvoidsTextWidthInRenderPath)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderServerInfo(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_EQ(Body.find("TextRender()->TextWidth"), std::string::npos);
	EXPECT_NE(Body.find("const float ServerInfoLabelWidth"), std::string::npos);
	EXPECT_NE(Body.find("pRow->VSplitLeft(ServerInfoLabelWidth"), std::string::npos);
}

TEST(QmMonitoringHelpers, MotdRenderUsesOnlyCurrentMatchingParagraphCache)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderServerInfoMotd(CUIRect Motd)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Header.find("m_LastStableHeight"), std::string::npos);
	EXPECT_NE(Source.find("bool CMenus::IngameMotdParagraphCacheMatches"), std::string::npos);
	EXPECT_NE(Body.find("const bool CacheReady = IngameMotdParagraphCacheMatches(Motd, MotdFontSize);"), std::string::npos);
	EXPECT_EQ(Body.find("if(m_IngameMotdParagraphCache.m_Valid && m_MotdTextContainerIndex.Valid())"), std::string::npos);
	EXPECT_NE(Body.find("if(CacheReady && m_MotdTextContainerIndex.Valid())"), std::string::npos);
}

TEST(QmMonitoringHelpers, DynamicSnapshotTextUsesBudgetedDrain)
{
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Ingame = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string SnapshotDrainBody = ExtractSourceFunctionBody(Ingame, "void CMenus::DrainSnapshotTextContainers()");

	EXPECT_NE(Header.find("struct SMenuSnapshotTextKey"), std::string::npos);
	EXPECT_NE(Header.find("RequestSnapshotTextContainer"), std::string::npos);
	EXPECT_NE(Header.find("DrainSnapshotTextContainers"), std::string::npos);
	EXPECT_NE(Header.find("m_SnapshotTextPending"), std::string::npos);
	EXPECT_NE(Menus.find("m_TextContainerTokens"), std::string::npos);
	EXPECT_NE(Menus.find("m_SnapshotTextCache.clear()"), std::string::npos);
	EXPECT_NE(Menus.find("m_SnapshotTextPending.clear()"), std::string::npos);
	EXPECT_NE(Menus.find("PrebuildSettingsTextPlanItem"), std::string::npos);
	EXPECT_NE(Menus.find("DrainMenuTextContainerBuild(Element, &Item.m_Rect"), std::string::npos);
	EXPECT_NE(Ingame.find("while(m_IngameTextFrameBudget.m_TextContainerTokens > 0 && !m_SnapshotTextPending.empty())"), std::string::npos);
	EXPECT_NE(Ingame.find("--m_IngameTextFrameBudget.m_TextContainerTokens"), std::string::npos);
	EXPECT_EQ(Ingame.find("m_CurrentSettingsUiFrameBudget.m_TextContainerTokens > 0 && !m_SnapshotTextPending.empty()"), std::string::npos);
	EXPECT_NE(Ingame.find("RenderIngameServerInfoValueCached"), std::string::npos);
	EXPECT_NE(Ingame.find("RequestSnapshotTextContainer("), std::string::npos);
	ASSERT_FALSE(SnapshotDrainBody.empty());
	// Snapshot cache miss/container creation is reported by the drain as a
	// frame aggregate. It must not be inferred from paragraph misses, otherwise
	// MOTD misses pollute dynamic short-text hit rate.
	EXPECT_NE(SnapshotDrainBody.find("snapshot_cache_miss=%d"), std::string::npos);
	EXPECT_NE(SnapshotDrainBody.find("text_container_new=%d"), std::string::npos);
	EXPECT_NE(SnapshotDrainBody.find("text_container_uploads=%d"), std::string::npos);
}

TEST(QmMonitoringHelpers, MotdParagraphUsesBudgetedDrain)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string DrainBody = ExtractSourceFunctionBody(Source, "void CMenus::DrainIngameMotdParagraphCache(CUIRect Motd, float FontSize, bool AllowCurrentFrame)");
	ASSERT_FALSE(DrainBody.empty());

	EXPECT_NE(DrainBody.find("m_IngameTextFrameBudget.m_ParagraphLayoutTokens"), std::string::npos);
	EXPECT_NE(DrainBody.find("m_ParagraphLayoutTokens"), std::string::npos);
	EXPECT_NE(DrainBody.find("--m_IngameTextFrameBudget.m_ParagraphLayoutTokens"), std::string::npos);
	EXPECT_EQ(DrainBody.find("m_CurrentSettingsUiFrameBudget.m_ParagraphLayoutTokens"), std::string::npos);
	EXPECT_NE(DrainBody.find("TextRender()->CreateOrAppendTextContainer(m_IngameMotdParagraphCache.m_BuildTextContainerIndex"), std::string::npos);
	EXPECT_EQ(DrainBody.find("TextRender()->RecreateTextContainer(m_MotdTextContainerIndex"), std::string::npos);
	EXPECT_NE(DrainBody.find("paragraph_cache_hit=%d"), std::string::npos);
	EXPECT_NE(DrainBody.find("paragraph_cache_miss=%d"), std::string::npos);
	EXPECT_EQ(DrainBody.find("static_stable_hit="), std::string::npos);
	EXPECT_EQ(DrainBody.find("snapshot_cache_miss="), std::string::npos);
}

TEST(QmMonitoringHelpers, VisibleIngameRenderDrainsParagraphOnlyAfterPendingFrame)
{
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Ingame = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string OnRenderBody = ExtractSourceFunctionBody(Menus, "void CMenus::OnRender()");
	const std::string PrepareBody = ExtractSourceFunctionBody(Ingame, "void CMenus::PrepareIngameServerInfoTextRuntime(const CUIRect *pMainView)");
	ASSERT_FALSE(OnRenderBody.empty());
	ASSERT_FALSE(PrepareBody.empty());

	// Long MOTD paragraph layout can be enqueued by prepare, but the visible
	// frame must not use the same-frame force path or drain paragraph work.
	EXPECT_NE(OnRenderBody.find("DrainIngameUiTextRuntime(false);"), std::string::npos);
	EXPECT_EQ(OnRenderBody.find("DrainIngameUiTextRuntime(true);"), std::string::npos);
	EXPECT_EQ(PrepareBody.find("DrainIngameUiTextRuntime(true);"), std::string::npos);
	EXPECT_EQ(PrepareBody.find("DrainIngameUiTextRuntime(false);"), std::string::npos);
	EXPECT_NE(PrepareBody.find("RequestIngameMotdParagraphCache("), std::string::npos);
}

TEST(QmMonitoringHelpers, MotdPrepareHydratesParagraphWithoutSameFrameStarvation)
{
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Source = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string PrepareBody = ExtractSourceFunctionBody(Source, "void CMenus::PrepareIngameServerInfoTextRuntime(const CUIRect *pMainView)");
	const std::string DrainBody = ExtractSourceFunctionBody(Source, "void CMenus::DrainIngameMotdParagraphCache(CUIRect Motd, float FontSize, bool AllowCurrentFrame)");
	const std::string RuntimeDrainBody = ExtractSourceFunctionBody(Source, "void CMenus::DrainIngameUiTextRuntime(bool AllowCurrentFrame)");
	ASSERT_FALSE(PrepareBody.empty());
	ASSERT_FALSE(DrainBody.empty());
	ASSERT_FALSE(RuntimeDrainBody.empty());

	// Server-info preparation may enqueue work, but it must respect the normal
	// budgeted drain path. Same-frame force would move long paragraph layout
	// back into the page-open frame.
	EXPECT_NE(Header.find("void DrainIngameMotdParagraphCache(CUIRect Motd, float FontSize, bool AllowCurrentFrame = false);"), std::string::npos);
	EXPECT_NE(Header.find("void DrainIngameUiTextRuntime(bool AllowCurrentFrame = false);"), std::string::npos);
	EXPECT_EQ(PrepareBody.find("DrainIngameUiTextRuntime(true);"), std::string::npos);
	EXPECT_EQ(PrepareBody.find("DrainIngameUiTextRuntime(false);"), std::string::npos);
	EXPECT_NE(PrepareBody.find("RequestIngameMotdParagraphCache("), std::string::npos);
	EXPECT_NE(DrainBody.find("!AllowCurrentFrame && Frame <= m_IngameMotdParagraphCache.m_PendingFrame"), std::string::npos);
	EXPECT_NE(RuntimeDrainBody.find("DrainIngameMotdParagraphCache(m_IngameMotdParagraphCache.m_PendingRect, m_IngameMotdParagraphCache.m_PendingFontSize, AllowCurrentFrame);"), std::string::npos);
}

TEST(QmMonitoringHelpers, MotdParagraphPendingDrainsAfterVisibleRequest)
{
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string OnRenderBody = ExtractSourceFunctionBody(Menus, "void CMenus::OnRender()");
	ASSERT_FALSE(OnRenderBody.empty());

	// If MOTD changes while the server-info page is already open, render can
	// only enqueue a paragraph request. The frame-end drain must continue the
	// budgeted paragraph pipeline on later frames, otherwise the announcement
	// area stays blank until the page is reopened.
	EXPECT_NE(OnRenderBody.find("if(IsActive() && Client()->State() == IClient::STATE_ONLINE)"), std::string::npos);
	EXPECT_NE(OnRenderBody.find("if(m_GamePage == PAGE_SERVER_INFO)"), std::string::npos);
	EXPECT_NE(OnRenderBody.find("DrainIngameUiTextRuntime(false);"), std::string::npos);
	EXPECT_EQ(OnRenderBody.find("else if(GameClient()->m_Motd.ServerMotd()[0])"), std::string::npos);
	EXPECT_NE(OnRenderBody.find("Client()->PerfFrame() > m_IngameEscOpenFrame"), std::string::npos);
	EXPECT_NE(OnRenderBody.find("PrepareIngameServerInfoTextRuntime();"), std::string::npos);
}

TEST(QmMonitoringHelpers, IngameTextPlanCollectionDoesNotTouchMotdRuntimeCache)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderServerInfoMotd(CUIRect Motd)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("if(m_MenuTextPlanCollecting)"), std::string::npos);
	EXPECT_LT(Body.find("if(m_MenuTextPlanCollecting)"), Body.find("GameClient()->m_Motd.ServerMotd()"));
	EXPECT_LT(Body.find("if(m_MenuTextPlanCollecting)"), Body.find("CScrollRegion"));
	EXPECT_LT(Body.find("if(m_MenuTextPlanCollecting)"), Body.find("RequestIngameMotdParagraphCache(Motd"));
	EXPECT_EQ(Body.find("DrainIngameMotdParagraphCache(Motd"), std::string::npos);
	EXPECT_NE(Body.find("return;"), std::string::npos);
}

TEST(QmMonitoringHelpers, IngameDynamicTextPlanCollectionDoesNotTouchRuntimeElements)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_ingame.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderIngameServerInfoValueCached(const char *pTextId, unsigned &TextHash, const CUIRect *pRect, const char *pText, float Size, int Align, const SLabelProperties &LabelProps)");
	ASSERT_FALSE(Body.empty());

	const size_t CollectingCheck = Body.find("if(m_MenuTextPlanCollecting)");
	const size_t TextHashWrite = Body.find("TextHash = NewHash;");
	const size_t SnapshotRequest = Body.find("RequestSnapshotTextContainer(");
	ASSERT_NE(CollectingCheck, std::string::npos);
	ASSERT_NE(TextHashWrite, std::string::npos);
	ASSERT_NE(SnapshotRequest, std::string::npos);
	EXPECT_LT(CollectingCheck, TextHashWrite);
	EXPECT_LT(CollectingCheck, SnapshotRequest);
	EXPECT_NE(Body.find("CollectMenuTextPlanItem(MENU_TEXT_SCOPE_INGAME"), std::string::npos);
	EXPECT_EQ(Body.find("Element.Rect(0)"), std::string::npos);
}

TEST(QmMonitoringHelpers, BackgroundTextureRenderSkipsBeforeInterfacesAreReady)
{
	const std::string ComponentHeader = ReadRepoFile("src/game/client/component.h");
	const std::string BackgroundSource = ReadRepoFile("src/game/client/components/background.cpp");
	const std::string Body = ExtractSourceFunctionBody(BackgroundSource, "bool CBackground::RenderBackgroundTexture()");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(ComponentHeader.find("bool InterfacesInitialized() const { return m_pClient != nullptr; }"), std::string::npos);
	EXPECT_NE(Body.find("if(!InterfacesInitialized())"), std::string::npos);
	EXPECT_LT(Body.find("if(!InterfacesInitialized())"), Body.find("Graphics()->GetScreen("));
	EXPECT_NE(Body.find("return false;"), std::string::npos);
}

TEST(QmMonitoringHelpers, MenuBackgroundRenderSkipsBeforeInterfacesAreReady)
{
	const std::string MenuBackgroundSource = ReadRepoFile("src/game/client/components/menu_background.cpp");
	const std::string Body = ExtractSourceFunctionBody(MenuBackgroundSource, "bool CMenuBackground::Render()");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("if(!InterfacesInitialized())"), std::string::npos);
	EXPECT_LT(Body.find("if(!InterfacesInitialized())"), Body.find("RenderBackgroundTexture()"));
	EXPECT_LT(Body.find("if(!InterfacesInitialized())"), Body.find("Client()->RenderFrameTime()"));
	EXPECT_NE(Body.find("return false;"), std::string::npos);
}

TEST(QmMonitoringHelpers, ConsoleQueuedResultCopyPreservesExternalArguments)
{
	const std::string ConsoleSource = ReadRepoFile("src/engine/shared/console.cpp");
	const std::string Body = ExtractSourceFunctionBody(ConsoleSource, "CConsole::CResult::CResult(const CResult &Other)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(ConsoleSource.find("const char *CConsole::CResult::CopyArgumentPointer"), std::string::npos);
	EXPECT_NE(Body.find("CopyArgumentPointer(Other.m_pArgsStart, Other)"), std::string::npos);
	EXPECT_NE(Body.find("CopyArgumentPointer(Other.m_pCommand, Other)"), std::string::npos);
	EXPECT_NE(Body.find("CopyArgumentPointer(Other.m_apArgs[i], Other)"), std::string::npos);
	EXPECT_EQ(Body.find("m_aStringStorage + (Other.m_apArgs[i] - Other.m_aStringStorage)"), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsLoadingPrewarmApiIsPublic)
{
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const size_t StartLoading = Header.find("void StartLoading(int Total);");
	const size_t PrewarmSettingsPages = Header.find("void PrewarmSettingsPages();");
	const size_t IsInit = Header.find("bool IsInit() const");
	ASSERT_NE(StartLoading, std::string::npos);
	ASSERT_NE(PrewarmSettingsPages, std::string::npos);
	ASSERT_NE(IsInit, std::string::npos);

	EXPECT_GT(PrewarmSettingsPages, StartLoading);
	EXPECT_LT(PrewarmSettingsPages, IsInit);
}

TEST(QmMonitoringHelpers, TextRuntimeTelemetryReportsGlyphContainerAndParagraphCosts)
{
	const std::string Text = ReadRepoFile("src/engine/client/text.cpp");
	const std::string Stats = ReadRepoFile("qmclient_scripts/perf/lib/stats.ts");

	EXPECT_NE(Text.find("glyph_rasterize_ms"), std::string::npos);
	EXPECT_NE(Text.find("text_container_create_ms"), std::string::npos);
	EXPECT_NE(Text.find("text_container_upload_ms"), std::string::npos);
	EXPECT_NE(Stats.find("glyphRasterizeMs"), std::string::npos);
	EXPECT_NE(Stats.find("paragraphLayoutMs"), std::string::npos);
	EXPECT_NE(Stats.find("paragraphBudgetBlocked"), std::string::npos);
	EXPECT_NE(Stats.find("paragraphCacheHit"), std::string::npos);
	EXPECT_NE(Stats.find("paragraphCacheMiss"), std::string::npos);
}

TEST(QmMonitoringHelpers, TextRuntimeCountersDoNotAccumulateWhilePerfDisabled)
{
	const std::string Text = ReadRepoFile("src/engine/client/text.cpp");
	const std::string Body = ExtractSourceFunctionBody(Text, "void FlushQmTextRuntimeBudgetLog() override");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Text.find("void ResetQmTextRuntimeBudgetCounters(bool ConsumeGlyphStats)"), std::string::npos);
	EXPECT_NE(Body.find("m_pGlyphMap->ConsumeQmPerfGlyphStats"), std::string::npos);
	EXPECT_NE(Body.find("if(!QmPerfEnabled()"), std::string::npos);
	EXPECT_LT(Body.find("m_pGlyphMap->ConsumeQmPerfGlyphStats"), Body.find("if(!QmPerfEnabled()"));
	EXPECT_GT(Body.find("ResetQmTextRuntimeBudgetCounters(false);", Body.find("if(!QmPerfEnabled()")), Body.find("if(!QmPerfEnabled()"));
}

TEST(QmMonitoringHelpers, TextRuntimeSnapshotUpdatesBeforePerfLoggingGate)
{
	const std::string Text = ReadRepoFile("src/engine/client/text.cpp");
	const std::string Body = ExtractSourceFunctionBody(Text, "void FlushQmTextRuntimeBudgetLog() override");
	ASSERT_FALSE(Body.empty());

	// The adaptive settings scheduler consumes this snapshot during normal
	// gameplay, so it must not depend on the perf log/debug switch being on.
	EXPECT_NE(Body.find("UpdateQmTextRuntimeBudgetSnapshot(GlyphNew, GlyphUploads, GlyphRasterizeMs, GlyphUploadMs);"), std::string::npos);
	EXPECT_NE(Body.find("if(!QmPerfEnabled()"), std::string::npos);
	EXPECT_LT(Body.find("UpdateQmTextRuntimeBudgetSnapshot(GlyphNew, GlyphUploads, GlyphRasterizeMs, GlyphUploadMs);"), Body.find("if(!QmPerfEnabled()"));
}

TEST(QmMonitoringHelpers, TextRuntimeTelemetrySkipsLowCostSingleContainerNoise)
{
	const std::string Text = ReadRepoFile("src/engine/client/text.cpp");
	const std::string Body = ExtractSourceFunctionBody(Text, "void FlushQmTextRuntimeBudgetLog() override");
	ASSERT_FALSE(Body.empty());

	// A long perf run can create millions of cheap one-off text containers.
	// Keep high-cost/glyph/upload rows for attribution, but do not log every
	// sub-threshold single-container create as its own perf/text line.
	EXPECT_NE(Text.find("bool ShouldLogTextRuntimeBudget("), std::string::npos);
	EXPECT_NE(Text.find("TextContainerWork >= 8"), std::string::npos);
	EXPECT_NE(Body.find("if(!ShouldLogTextRuntimeBudget("), std::string::npos);
	EXPECT_NE(Body.find("ResetQmTextRuntimeBudgetCounters(false);"), std::string::npos);
}

TEST(QmMonitoringHelpers, TextRuntimeTelemetryFlushesOncePerUiFrame)
{
	const std::string Header = ReadRepoFile("src/engine/textrender.h");
	const std::string Text = ReadRepoFile("src/engine/client/text.cpp");
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string CreateBody = ExtractSourceFunctionBody(Text, "bool CreateTextContainer(STextContainerIndex &TextContainerIndex, CTextCursor *pCursor, const char *pText, int Length = -1) override");
	const std::string UploadBody = ExtractSourceFunctionBody(Text, "void UploadTextContainer(STextContainerIndex TextContainerIndex) override");
	const std::string OnRenderBody = ExtractSourceFunctionBody(Menus, "void CMenus::OnRender()");
	ASSERT_FALSE(CreateBody.empty());
	ASSERT_FALSE(UploadBody.empty());
	ASSERT_FALSE(OnRenderBody.empty());

	// Regression guard for 2GB perf logs: text render hot paths may only
	// accumulate counters. The UI frame owns the single aggregated flush.
	EXPECT_NE(Header.find("FlushQmTextRuntimeBudgetLog"), std::string::npos);
	EXPECT_EQ(CreateBody.find("LogQmTextRuntimeBudget("), std::string::npos);
	EXPECT_EQ(UploadBody.find("LogQmTextRuntimeBudget("), std::string::npos);
	EXPECT_NE(OnRenderBody.find("TextRender()->FlushQmTextRuntimeBudgetLog()"), std::string::npos);
}

TEST(QmMonitoringHelpers, TextRenderExposesRuntimeBudgetSnapshotForScheduler)
{
	const std::string Header = ReadRepoFile("src/engine/textrender.h");
	const std::string Text = ReadRepoFile("src/engine/client/text.cpp");

	EXPECT_NE(Header.find("struct SQmTextRuntimeBudgetSnapshot"), std::string::npos);
	EXPECT_NE(Header.find("virtual SQmTextRuntimeBudgetSnapshot QmTextRuntimeBudgetSnapshot() const"), std::string::npos);
	EXPECT_NE(Text.find("SQmTextRuntimeBudgetSnapshot m_QmLastTextRuntimeBudgetSnapshot"), std::string::npos);
	EXPECT_NE(Text.find("m_QmLastTextRuntimeBudgetSnapshot.m_TextContainerCreateMs"), std::string::npos);
}

TEST(QmMonitoringHelpers, HudSettingsTextHydratesUnderBudget)
{
	const std::string Settings = ReadRepoFile("src/game/client/components/menus_settings.cpp");
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string HudBranch = ExtractSourceBlock(Settings, "if(s_CurTab == APPEARANCE_TAB_HUD)", "else if(s_CurTab == APPEARANCE_TAB_CHAT)");
	ASSERT_FALSE(HudBranch.empty());

	EXPECT_NE(Settings.find("APPEARANCE_TAB_HUD"), std::string::npos);
	EXPECT_NE(Settings.find("DoSettingsScrollbarOption(SETTINGS_APPEARANCE, APPEARANCE_TAB_HUD"), std::string::npos);
	EXPECT_NE(HudBranch.find("DoSettingsButton_CheckBoxAutoVMarginAndSet(SETTINGS_APPEARANCE, APPEARANCE_TAB_HUD"), std::string::npos);
	EXPECT_EQ(HudBranch.find("DoButton_CheckBoxAutoVMarginAndSet("), std::string::npos);
	EXPECT_NE(HudBranch.find("DoSettingsLabelStreamed"), std::string::npos);
	EXPECT_EQ(HudBranch.find("Ui()->DoLabel_AutoLineSize"), std::string::npos);
	EXPECT_NE(Menus.find("m_pSettingsTextPrebuildBudget"), std::string::npos);
	EXPECT_NE(Menus.find("BeginSettingsUiFrameScheduler("), std::string::npos);
	EXPECT_NE(Menus.find("DrainMenuTextContainerBuildRequests()"), std::string::npos);
	EXPECT_NE(Menus.find("AdaptiveBudget.m_TextPrebuildTokens"), std::string::npos);
}

TEST(QmMonitoringHelpers, AppearanceHudChatNamePlateCheckboxesUseBudgetedTextPipeline)
{
	const std::string Settings = ReadRepoFile("src/game/client/components/menus_settings.cpp");
	const std::string HudBranch = ExtractSourceBlock(Settings, "if(s_CurTab == APPEARANCE_TAB_HUD)", "else if(s_CurTab == APPEARANCE_TAB_CHAT)");
	const std::string ChatBranch = ExtractSourceBlock(Settings, "else if(s_CurTab == APPEARANCE_TAB_CHAT)", "else if(s_CurTab == APPEARANCE_TAB_NAME_PLATE)");
	const std::string NamePlateBranch = ExtractSourceBlock(Settings, "else if(s_CurTab == APPEARANCE_TAB_NAME_PLATE)", "else if(s_CurTab == APPEARANCE_TAB_HOOK_COLLISION)");
	const std::string HookCollisionBranch = ExtractSourceBlock(Settings, "else if(s_CurTab == APPEARANCE_TAB_HOOK_COLLISION)", "else if(s_CurTab == APPEARANCE_TAB_INFO_MESSAGES)");
	const std::string InfoMessagesBranch = ExtractSourceBlock(Settings, "else if(s_CurTab == APPEARANCE_TAB_INFO_MESSAGES)", "else if(s_CurTab == APPEARANCE_TAB_LASER)");
	ASSERT_FALSE(HudBranch.empty());
	ASSERT_FALSE(ChatBranch.empty());
	ASSERT_FALSE(NamePlateBranch.empty());
	ASSERT_FALSE(HookCollisionBranch.empty());
	ASSERT_FALSE(InfoMessagesBranch.empty());

	// These visible Appearance subtabs contain many fixed checkbox labels.
	// They must use the settings text cache/drain wrappers so first entry does
	// not synchronously create text containers for every visible checkbox.
	EXPECT_NE(HudBranch.find("DoSettingsButton_CheckBoxAutoVMarginAndSet(SETTINGS_APPEARANCE, APPEARANCE_TAB_HUD"), std::string::npos);
	EXPECT_NE(ChatBranch.find("DoSettingsButton_CheckBoxAutoVMarginAndSet(SETTINGS_APPEARANCE, APPEARANCE_TAB_CHAT"), std::string::npos);
	EXPECT_NE(NamePlateBranch.find("DoSettingsButton_CheckBoxAutoVMarginAndSet(SETTINGS_APPEARANCE, APPEARANCE_TAB_NAME_PLATE"), std::string::npos);
	EXPECT_EQ(HudBranch.find("DoButton_CheckBoxAutoVMarginAndSet("), std::string::npos);
	EXPECT_EQ(ChatBranch.find("DoButton_CheckBoxAutoVMarginAndSet("), std::string::npos);
	EXPECT_EQ(NamePlateBranch.find("DoButton_CheckBoxAutoVMarginAndSet("), std::string::npos);
	EXPECT_EQ(ChatBranch.find("DoButton_CheckBox("), std::string::npos);
	EXPECT_EQ(NamePlateBranch.find("DoButton_CheckBox("), std::string::npos);
	EXPECT_EQ(NamePlateBranch.find("DoButton_Menu(&s_NameplateResetLayoutButton"), std::string::npos);
	EXPECT_NE(HookCollisionBranch.find("DoSettingsButton_CheckBox(SETTINGS_APPEARANCE, APPEARANCE_TAB_HOOK_COLLISION"), std::string::npos);
	EXPECT_NE(InfoMessagesBranch.find("DoSettingsButton_CheckBox(SETTINGS_APPEARANCE, APPEARANCE_TAB_INFO_MESSAGES"), std::string::npos);
	EXPECT_EQ(HookCollisionBranch.find("DoButton_CheckBox("), std::string::npos);
	EXPECT_EQ(InfoMessagesBranch.find("DoButton_CheckBox("), std::string::npos);
}

TEST(QmMonitoringHelpers, AppearanceSettingsHeadingsUseBudgetedTextPipeline)
{
	const std::string Settings = ReadRepoFile("src/game/client/components/menus_settings.cpp");
	const std::string Body = ExtractSourceFunctionBody(Settings, "void CMenus::RenderSettingsAppearance(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	// Appearance subtab headings are visible settings chrome. They must go
	// through the settings text cache/drain path instead of direct UI labels,
	// otherwise first-entry HUD/Appearance tabs can create containers in render.
	EXPECT_NE(Body.find("auto DoAppearanceHeading"), std::string::npos);
	EXPECT_NE(Body.find("SettingsTextElement(SETTINGS_APPEARANCE, s_CurTab"), std::string::npos);
	EXPECT_NE(Body.find("DoSettingsLabelStreamed(HeadingElement"), std::string::npos);
	EXPECT_EQ(Body.find("Ui()->DoLabel_AutoLineSize"), std::string::npos);
}

TEST(QmMonitoringHelpers, ControlsSettingsChromeUsesBudgetedTextPipeline)
{
	const std::string Controls = ReadRepoFile("src/game/client/components/menus_settings_controls.cpp");

	// The Controls settings page has a dense first frame: block headings,
	// option labels, controller labels and bind labels all sit above the fold.
	// They must use the shared settings text cache/drain path so first entry
	// cannot synchronously create a large batch of text containers in render.
	EXPECT_NE(Controls.find("DoSettingsControlsMenuLabel("), std::string::npos);
	EXPECT_NE(Controls.find("DoSettingsControlsLabel("), std::string::npos);
	EXPECT_NE(Controls.find("DoSettingsControlsCheckBox("), std::string::npos);
	EXPECT_NE(Controls.find("DoSettingsControlsScrollbarOption("), std::string::npos);
	EXPECT_NE(Controls.find("DoSettingsButton_Menu(CMenus::SETTINGS_CONTROLS"), std::string::npos);
	EXPECT_EQ(Controls.find("Ui()->DoLabel("), std::string::npos);
	EXPECT_EQ(Controls.find("Ui()->DoLabel_AutoLineSize"), std::string::npos);
}

TEST(QmMonitoringHelpers, TeeSkinFilterCheckboxesUseBudgetedTextPipeline)
{
	const std::string Settings = ReadRepoFile("src/game/client/components/menus_settings.cpp");
	const std::string TeeBody = ExtractSourceFunctionBody(Settings, "void CMenus::RenderSettingsTee(CUIRect MainView)");
	ASSERT_FALSE(TeeBody.empty());

	// The Tee page is list-heavy, but the fixed skin filter checkboxes are
	// still settings chrome. Keep them on the shared settings text cache/drain
	// path instead of direct checkbox labels.
	EXPECT_NE(TeeBody.find("DoSettingsButton_CheckBox(SETTINGS_TEE"), std::string::npos);
	EXPECT_EQ(TeeBody.find("DoButton_CheckBox(&g_Config.m_ClDownloadSkins"), std::string::npos);
	EXPECT_EQ(TeeBody.find("DoButton_CheckBox(&g_Config.m_ClDownloadCommunitySkins"), std::string::npos);
	EXPECT_EQ(TeeBody.find("DoButton_CheckBox(&g_Config.m_ClVanillaSkinsOnly"), std::string::npos);
	EXPECT_EQ(TeeBody.find("DoButton_CheckBox(&g_Config.m_ClFatSkins"), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsRadioMenusUseBudgetedTextPipeline)
{
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Source = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Controls = ReadRepoFile("src/game/client/components/menus_settings_controls.cpp");
	const std::string Settings = ReadRepoFile("src/game/client/components/menus_settings.cpp");
	const std::string TClient = ReadRepoFile("src/game/client/components/tclient/menus_tclient.cpp");
	const std::string QmClient = ReadRepoFile("src/game/client/components/qmclient/menus_qmclient.cpp");

	// Settings radio rows are fixed UI chrome: one label plus a small set of
	// fixed buttons. They should use the same text cache/drain helpers as
	// checkboxes and scrollbars instead of the generic direct-label helper.
	EXPECT_NE(Header.find("DoSettingsLine_RadioMenu("), std::string::npos);
	EXPECT_NE(Source.find("bool CMenus::DoSettingsLine_RadioMenu("), std::string::npos);
	EXPECT_NE(Source.find("DoSettingsLabel(Page, Tab, pLabelTextId"), std::string::npos);
	EXPECT_NE(Source.find("DoSettingsButton_Menu(Page, Tab, Subtab"), std::string::npos);
	EXPECT_EQ(Controls.find("DoLine_RadioMenu("), std::string::npos);
	EXPECT_EQ(Settings.find("DoLine_RadioMenu("), std::string::npos);
	EXPECT_EQ(TClient.find("DoLine_RadioMenu("), std::string::npos);
	EXPECT_EQ(QmClient.find("DoLine_RadioMenu("), std::string::npos);
}

TEST(QmMonitoringHelpers, TClientVisibleCheckboxesUseBudgetedTextPipeline)
{
	const std::string TClient = ReadRepoFile("src/game/client/components/tclient/menus_tclient.cpp");
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::vector<std::string> vFunctionBodies = {
		ExtractSourceFunctionBody(TClient, "float CMenus::LayoutTClientAutoReplyCacheSection(CUIRect &CurrentColumn, bool Render)"),
		ExtractSourceFunctionBody(TClient, "float CMenus::RenderTClientAutoReplyInteractiveLayer(CUIRect &CurrentColumn)"),
		ExtractSourceFunctionBody(TClient, "float CMenus::LayoutTClientPetCacheSection(CUIRect &CurrentColumn, bool Render)"),
		ExtractSourceFunctionBody(TClient, "float CMenus::RenderTClientPetInteractiveLayer(CUIRect &CurrentColumn)"),
		ExtractSourceFunctionBody(TClient, "float CMenus::LayoutTClientHudCacheSection(CUIRect &CurrentColumn, bool Render)"),
		ExtractSourceFunctionBody(TClient, "float CMenus::RenderTClientHudInteractiveLayer(CUIRect &CurrentColumn)")};
	const std::vector<std::string> vRenderBlocks = {
		ExtractSourceBlock(TClient, "auto LayoutVisualNameplateSection", "auto LayoutVisualEffectsSection"),
		ExtractSourceBlock(TClient, "auto LayoutVisualEffectsSection", "auto LayoutInputSection"),
		ExtractSourceBlock(TClient, "auto LayoutInputSection", "auto LayoutAntiLatencyToolsSection"),
		ExtractSourceBlock(TClient, "auto LayoutAntiLatencyToolsSection", "auto LayoutAntiPingSmoothingSection"),
		ExtractSourceBlock(TClient, "auto LayoutAntiPingSmoothingSection", "auto LayoutAutoExecuteSection"),
		ExtractSourceBlock(TClient, "auto LayoutPetSection", "auto MeasurePetSection"),
		ExtractSourceBlock(TClient, "auto RenderPetInteractiveSection", "auto LayoutAutoReplySection"),
		ExtractSourceBlock(TClient, "auto LayoutAutoReplySection", "auto MeasureAutoReplySection"),
		ExtractSourceBlock(TClient, "auto RenderAutoReplyInteractiveSection", "auto LayoutPlayerIndicatorSection"),
		ExtractSourceBlock(TClient, "auto LayoutPlayerIndicatorSection", "// ---- CSectionLoader")};

	EXPECT_NE(Header.find("DoTClientSettingsButton_CheckBoxAutoVMarginAndSet("), std::string::npos);
	EXPECT_NE(Header.find("DoTClientSettingsButton_CheckBox("), std::string::npos);
	EXPECT_NE(Header.find("DoTClientSettingsButton_Menu("), std::string::npos);
	EXPECT_NE(TClient.find("int CMenus::DoTClientSettingsButton_CheckBoxAutoVMarginAndSet("), std::string::npos);
	EXPECT_NE(TClient.find("int CMenus::DoTClientSettingsButton_CheckBox("), std::string::npos);
	EXPECT_NE(TClient.find("int CMenus::DoTClientSettingsButton_Menu("), std::string::npos);
	EXPECT_NE(TClient.find("return DoSettingsButton_CheckBoxAutoVMarginAndSet(SETTINGS_TCLIENT, m_TClientSettingsTab"), std::string::npos);
	EXPECT_NE(TClient.find("return DoSettingsButton_CheckBox(SETTINGS_TCLIENT, m_TClientSettingsTab"), std::string::npos);
	EXPECT_NE(TClient.find("return DoSettingsButton_Menu(SETTINGS_TCLIENT, m_TClientSettingsTab"), std::string::npos);
	EXPECT_EQ(TClient.find("DoButton_CheckBoxAutoVMarginAndSet("), std::string::npos);
	EXPECT_EQ(TClient.find("DoButton_CheckBox(&g_Config"), std::string::npos);
	EXPECT_EQ(TClient.find("DoButton_CheckBox(&m_Dummy"), std::string::npos);
	EXPECT_EQ(TClient.find("DoButton_CheckBox(&s_CustomColorId"), std::string::npos);
	EXPECT_EQ(TClient.find("DoButton_CheckBox(&s_TcUiTag"), std::string::npos);
	EXPECT_EQ(TClient.find("DoButton_Menu(&s_LoadButton, Localize(\"Load\")"), std::string::npos);
	EXPECT_EQ(TClient.find("DoButton_Menu(&s_SaveButton, Localize(\"Save\")"), std::string::npos);
	EXPECT_EQ(TClient.find("DoButton_Menu(&s_DeleteButton, Localize(\"Delete\")"), std::string::npos);
	EXPECT_EQ(TClient.find("DoButton_Menu(&s_OverrideButton, Localize(\"Override\")"), std::string::npos);
	EXPECT_EQ(TClient.find("DoButton_Menu(&s_ProfilesFile, Localize(\"Profiles file\")"), std::string::npos);
	EXPECT_EQ(TClient.find("DoButton_Menu(&s_ApplyBtn, Localize(\"Apply Changes\")"), std::string::npos);
	EXPECT_EQ(TClient.find("DoButton_Menu(&s_ClearBtn, Localize(\"Clear Changes\")"), std::string::npos);
	EXPECT_EQ(TClient.find("DoButton_Menu(&ResetBtn, Localize(\"Reset\")"), std::string::npos);
	for(const auto &Body : vFunctionBodies)
	{
		ASSERT_FALSE(Body.empty());
		EXPECT_NE(Body.find("DoTClientSettingsButton_CheckBoxAutoVMarginAndSet("), std::string::npos);
		EXPECT_EQ(Body.find("DoButton_CheckBoxAutoVMarginAndSet("), std::string::npos);
	}
	for(const auto &Block : vRenderBlocks)
	{
		ASSERT_FALSE(Block.empty());
		EXPECT_NE(Block.find("DoTClientSettingsButton_CheckBoxAutoVMarginAndSet("), std::string::npos);
		EXPECT_EQ(Block.find("DoButton_CheckBoxAutoVMarginAndSet("), std::string::npos);
	}
}

TEST(QmMonitoringHelpers, AssetsEntityBgPreviewUsesArtifactJob)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Preview = ReadRepoFile("src/game/client/components/qmclient/settings_resource_preview.h");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Source.find("#include <game/client/components/qmclient/settings_resource_preview.h>"), std::string::npos);
	EXPECT_NE(Preview.find("CSettingsResourcePreviewJob"), std::string::npos);
	EXPECT_NE(Source.find("SResourcePreviewKey"), std::string::npos);
	EXPECT_NE(Source.find("CSettingsResourcePreviewCache"), std::string::npos);
	EXPECT_NE(Source.find("CSettingsResourcePreviewScheduler"), std::string::npos);
	EXPECT_NE(Body.find("SettingsResourcePreviewDrawResult"), std::string::npos);
	EXPECT_NE(Body.find("ESettingsResourcePreviewDrawResult::PLACEHOLDER"), std::string::npos);
	EXPECT_NE(Body.find("preview_jobs_started=%d preview_jobs_done=%d preview_uploads=%d preview_admissions=%d preview_artifact_ms=%.3f metadata_hydrate_ms=%.3f metadata_hydrated=%d placeholder_count=%d ready_texture_count=%d visible_ready_ratio=%.3f"), std::string::npos);
	EXPECT_NE(Body.find("CanStartPreviewJob(ESettingsResourcePreviewPriority::VISIBLE"), std::string::npos);
	EXPECT_NE(Source.find("CSettingsResourcePreviewUploadScheduler gs_SettingsAssetsResourcePreviewUploadScheduler"), std::string::npos);
	EXPECT_NE(Source.find("ProcessAssetsResourcePreviewJobs("), std::string::npos);
	EXPECT_NE(Source.find("StartAssetsEntityBgPreviewArtifactJob("), std::string::npos);
	EXPECT_NE(Source.find("m_vEntityBgPreviewJobs"), std::string::npos);
	EXPECT_NE(Body.find("ProcessAssetsResourcePreviewJobs(Graphics(), ResourcePreviewTelemetry"), std::string::npos);
	EXPECT_NE(Body.find("StartAssetsEntityBgPreviewArtifactJob(PreviewKey"), std::string::npos);
	EXPECT_EQ(Body.find("gs_SettingsAssetsResourcePreviewCache.MarkPreviewJobStarted(PreviewKey);"), std::string::npos);
	EXPECT_NE(Body.find("SettingsResourcePreviewDrawResult(ResourcePreviewState)"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsEntityBgPreviewArtifactJobDoesNotDependOnExistingPreviewImage)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string PreviewHeader = ReadRepoFile("src/game/client/components/qmclient/settings_resource_preview.h");
	const std::string PreviewSource = ReadRepoFile("src/game/client/components/qmclient/settings_resource_preview.cpp");
	const std::string StartBody = ExtractSourceFunctionBody(Source, "static bool StartAssetsEntityBgPreviewArtifactJob");
	ASSERT_FALSE(StartBody.empty());

	EXPECT_EQ(StartBody.find("pItem->m_PreviewImage.m_pData == nullptr"), std::string::npos);
	EXPECT_EQ(StartBody.find("m_PreviewImage.DeepCopy()"), std::string::npos);
	EXPECT_NE(StartBody.find("ResolveEntityBgPreviewArtifactSource("), std::string::npos);
	EXPECT_NE(StartBody.find("CSettingsResourcePreviewJob::FromPath("), std::string::npos);
	EXPECT_NE(PreviewHeader.find("static std::shared_ptr<CSettingsResourcePreviewJob> FromPath("), std::string::npos);
	EXPECT_NE(PreviewSource.find("LoadPng("), std::string::npos);
	EXPECT_NE(PreviewSource.find("BuildPreviewArtifactFromPath("), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsCardDrawLoopDoesNotRunHeavyPreview)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	const size_t RenderPreviewStart = Source.find("auto RenderAssetsCardPreview = ");
	ASSERT_NE(RenderPreviewStart, std::string::npos);
	const size_t RenderPreviewEnd = Source.find("auto RenderAssetsCardLoadingShells", RenderPreviewStart);
	ASSERT_NE(RenderPreviewEnd, std::string::npos);
	const std::string RenderPreviewBody = Source.substr(RenderPreviewStart, RenderPreviewEnd - RenderPreviewStart);

	EXPECT_NE(RenderPreviewBody.find("SettingsResourcePreviewDrawResult"), std::string::npos);
	EXPECT_NE(RenderPreviewBody.find("DrawResourcePreviewPlaceholder"), std::string::npos);
	EXPECT_EQ(RenderPreviewBody.find("Localize(\"Video Background\")"), std::string::npos);
	EXPECT_EQ(RenderPreviewBody.find("Localize(\"Map Preview\")"), std::string::npos);
	EXPECT_EQ(RenderPreviewBody.find("FONT_ICON_PLAY"), std::string::npos);
	EXPECT_EQ(RenderPreviewBody.find("m_DrawEntityBgVideoFallback"), std::string::npos);
	EXPECT_EQ(RenderPreviewBody.find("m_DrawEntityBgFallback"), std::string::npos);
	EXPECT_EQ(Body.find("PreviewState.m_DrawEntityBgVideoFallback ="), std::string::npos);
	EXPECT_EQ(Body.find("PreviewState.m_DrawEntityBgFallback ="), std::string::npos);
	const size_t MapFallback = Source.find("auto RenderEntityBgFallback = ");
	ASSERT_NE(MapFallback, std::string::npos);
	const size_t VideoFallback = Source.find("auto RenderEntityBgVideoFallback = ", MapFallback);
	ASSERT_NE(VideoFallback, std::string::npos);
	const size_t RenderLoop = Source.find("s_WorkshopAssetsListBox.SkipItems", VideoFallback);
	ASSERT_NE(RenderLoop, std::string::npos);
	const std::string FallbackBody = Source.substr(MapFallback, RenderLoop - MapFallback);
	EXPECT_NE(FallbackBody.find("DoMenuLabelStreamed(MENU_TEXT_SCOPE_SETTINGS"), std::string::npos);
	EXPECT_EQ(FallbackBody.find("Ui()->DoLabel(&LabelRect"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsCardDrawLoopDoesNotRunPreviewOrTextLayout)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("RequestAssetsCardMetadataHydration"), std::string::npos);
	EXPECT_NE(Body.find("RenderAssetsCardMetadataCached"), std::string::npos);
	EXPECT_NE(Body.find("SettingsResourcePreviewDrawResult(ResourcePreviewState)"), std::string::npos);
	EXPECT_EQ(Body.find("HydrateAssetsCardMetadataTimed("), std::string::npos);
	EXPECT_EQ(Body.find("RefreshAssetsCardMetadata("), std::string::npos);
	EXPECT_NE(Body.find("PreviewState.m_DrawEntityTileArtifact ="), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsCardMetadataRenderingUsesBudgetedMenuText)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const size_t MetadataStart = Source.find("auto RenderAssetsCardMetadata = ");
	ASSERT_NE(MetadataStart, std::string::npos);
	const size_t MetadataEnd = Source.find("auto RenderEntityBgFallback = ", MetadataStart);
	ASSERT_NE(MetadataEnd, std::string::npos);
	const std::string MetadataBody = Source.substr(MetadataStart, MetadataEnd - MetadataStart);

	EXPECT_NE(MetadataBody.find("DoMenuLabelStreamed(MENU_TEXT_SCOPE_SETTINGS"), std::string::npos);
	EXPECT_EQ(MetadataBody.find("Ui()->DoLabelStreamed("), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsLocalCardsUseSharedMetadataRenderer)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());
	const size_t LocalBranch = Body.find("if(!UsesCombinedAssetList(pCurrentCategory))");
	ASSERT_NE(LocalBranch, std::string::npos);
	const size_t LocalBranchEnd = Body.find("if(const SAssetResourceCategory *pCategory = AssetResourceCategoryByTab(s_CurCustomTab); UsesCombinedAssetList(pCategory)", LocalBranch);
	ASSERT_NE(LocalBranchEnd, std::string::npos);
	const std::string LocalBody = Body.substr(LocalBranch, LocalBranchEnd - LocalBranch);

	EXPECT_NE(LocalBody.find("RenderAssetsCardMetadataCached("), std::string::npos);
	EXPECT_NE(LocalBody.find("RequestAssetsCardMetadataHydration("), std::string::npos);
	EXPECT_EQ(LocalBody.find("Ui()->DoLabel(&TitleRect"), std::string::npos);
	EXPECT_EQ(LocalBody.find("Ui()->DoLabel(&HeaderLayout.m_AuthorRect"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsMetadataCacheMissUsesVisibleFallbackOutsideShellOnlyFrame)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	// The card shell can be frame-0 only, but the first readable non-shell frame
	// should still paint the current title/author/status instead of staying blank.
	EXPECT_NE(Body.find("RenderAssetsCardMetadataFallback(Ui(), Shell"), std::string::npos);
	EXPECT_NE(Body.find("else if(AssetsRenderCardMetadataFallback)"), std::string::npos);
	EXPECT_NE(Body.find("RenderAssetsCardMetadataCached(Shell, pMetadata, RenderAssetsCardMetadata);"), std::string::npos);
}

TEST(QmMonitoringHelpers, SkinsAndTeeDoNotExposePartialPreviewUploads)
{
	const std::string Skins = ReadRepoFile("src/game/client/components/skins.cpp");
	const std::string Settings = ReadRepoFile("src/game/client/components/menus_settings.cpp");
	const std::string DrainBody = ExtractSourceFunctionBody(Skins, "CSkins::ESkinProcessResult CSkins::DrainSettingsSkinPreviewUpload(CSkinContainer *pSkinContainer, CSkinLoadingStats &Stats,\n\tint &SkinsProcessedThisFrame, std::chrono::nanoseconds StartTime,\n\tstd::chrono::nanoseconds MaxTime)");
	ASSERT_FALSE(DrainBody.empty());

	EXPECT_NE(DrainBody.find("LoadSkinFinish(pSkinContainer"), std::string::npos);
	EXPECT_EQ(DrainBody.find("BeginSkinPreviewUpload(pSkinContainer"), std::string::npos);
	EXPECT_EQ(DrainBody.find("UploadNextSkinPreviewSprite(pSkinContainer, SkinPreviewUploadBudget)"), std::string::npos);
	EXPECT_EQ(DrainBody.find("FinishSkinPreviewUpload(pSkinContainer)"), std::string::npos);
	EXPECT_NE(DrainBody.find("SettingsResourcePreviewConsumeUploadBudget(SkinPreviewUploadBudget, SETTINGS_SKIN_SOURCE_TEXTURE_UPLOADS)"), std::string::npos);
	EXPECT_NE(Settings.find("tee_preview_pipeline"), std::string::npos);
	EXPECT_NE(Settings.find("BeginSettingsUiFrameScheduler(EFrameSchedulerConsumer::SettingsText, \"tee\""), std::string::npos);
}

TEST(QmMonitoringHelpers, SkinsTeeUploadBudgetRequeuesInsteadOfFailing)
{
	const std::string Skins = ReadRepoFile("src/game/client/components/skins.cpp");
	const std::string DrainBody = ExtractSourceFunctionBody(Skins, "CSkins::ESkinProcessResult CSkins::DrainSettingsSkinPreviewUpload(CSkinContainer *pSkinContainer, CSkinLoadingStats &Stats,\n\tint &SkinsProcessedThisFrame, std::chrono::nanoseconds StartTime,\n\tstd::chrono::nanoseconds MaxTime)");
	ASSERT_FALSE(DrainBody.empty());

	// When the shared upload budget is exhausted, Tee/skin preview completion must
	// stay queued. Marking it failed here caused default yellow tees/question marks
	// to leak into the list during fast scrolling.
	const size_t BudgetCheck = DrainBody.find("!SettingsResourcePreviewConsumeUploadBudget(SkinPreviewUploadBudget, SETTINGS_SKIN_SOURCE_TEXTURE_UPLOADS)");
	const size_t LoadFinish = DrainBody.find("LoadSkinFinish(pSkinContainer");
	ASSERT_NE(BudgetCheck, std::string::npos);
	ASSERT_NE(LoadFinish, std::string::npos);
	EXPECT_LT(BudgetCheck, LoadFinish);
	const std::string BudgetBlockedBody = DrainBody.substr(BudgetCheck, LoadFinish - BudgetCheck);
	EXPECT_NE(BudgetBlockedBody.find("return ESkinProcessResult::BREAK_GPU_LIMIT;"), std::string::npos);
	EXPECT_EQ(BudgetBlockedBody.find("SetState(CSkinContainer::EState::ERROR"), std::string::npos);
	EXPECT_EQ(BudgetBlockedBody.find("LoadSkinFinish("), std::string::npos);
}

TEST(QmMonitoringHelpers, SharedPreviewUploadSchedulerRejectsPartialCommit)
{
	const std::string Source = ReadRepoFile("src/game/client/components/qmclient/settings_resource_preview.cpp");
	const std::string Header = ReadRepoFile("src/game/client/components/qmclient/settings_resource_preview.h");
	const std::string DrainOneBody = ExtractSourceFunctionBody(Source, "bool CSettingsResourcePreviewUploadScheduler::DrainOne(SResourcePreviewUploadBudget &Budget, SResourcePreviewTelemetry &Telemetry, CSettingsResourcePreviewCache &Cache, IGraphics *pGraphics)");
	ASSERT_FALSE(DrainOneBody.empty());

	// Commit/finalize must be reserve -> valid texture -> commit. A failed or
	// budget-blocked upload must not consume budget or expose a half-ready texture.
	EXPECT_EQ(Header.find("m_pTargetTexture"), std::string::npos);
	const size_t ConsumePos = DrainOneBody.find("!SettingsResourcePreviewConsumeUploadBudget(Budget)");
	const size_t UploadPos = DrainOneBody.find("pGraphics->LoadTextureRawMove");
	const size_t ValidPos = DrainOneBody.find("if(Texture.IsValid())");
	const size_t CommitPos = DrainOneBody.find("SettingsResourcePreviewCommitUploadBudget(Budget)");
	const size_t FinalizeTruePos = DrainOneBody.find("Item.m_Finalize(true, Texture)");
	ASSERT_NE(ConsumePos, std::string::npos);
	ASSERT_NE(UploadPos, std::string::npos);
	ASSERT_NE(ValidPos, std::string::npos);
	ASSERT_NE(CommitPos, std::string::npos);
	ASSERT_NE(FinalizeTruePos, std::string::npos);
	EXPECT_LT(ConsumePos, UploadPos);
	EXPECT_LT(UploadPos, ValidPos);
	EXPECT_LT(ValidPos, CommitPos);
	EXPECT_LT(CommitPos, FinalizeTruePos);
	const std::string BudgetBlockedBody = DrainOneBody.substr(ConsumePos, UploadPos - ConsumePos);
	EXPECT_EQ(BudgetBlockedBody.find("Item.m_Finalize(false)"), std::string::npos);
	EXPECT_EQ(BudgetBlockedBody.find("Cache.MarkPreviewJobDone(Item.m_Key, false)"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsTabSwitchFirstFrameShellOnly)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Source.find("m_TabSwitchShellOnlyFrame"), std::string::npos);
	EXPECT_NE(Body.find("AssetsTabSwitchFirstFrame"), std::string::npos);
	EXPECT_NE(Body.find("CardHydrationScheduler.m_TabSwitchShellOnlyFrame"), std::string::npos);
	EXPECT_NE(Body.find("PreviewPipelineScheduler.BeginFrame("), std::string::npos);
	EXPECT_NE(Body.find("PreviewPipelineScheduler.SetShellOnlyFrame(AssetsShellOnlyFrame)"), std::string::npos);
	EXPECT_NE(Body.find("const int AssetsPreviewArtifactTokensThisFrame = AssetsContentWarmupBlocked ? 0 : AdaptivePreviewArtifactTokens;"), std::string::npos);
	EXPECT_NE(Body.find("tab_switch_shell_only=%d"), std::string::npos);
	EXPECT_EQ(Body.find("if(AssetsTabSwitchFirstFrame)\n\t\t\t\t\t\t++ResourcePreviewTelemetry.m_PlaceholderCount;"), std::string::npos);
	EXPECT_EQ(Body.find("StartWorkshopThumb(Asset, SettingsWorkshopThumbShouldStartHighPriority"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsTabSwitchUsesShellFirstFrame)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string BeginFrameBody = ExtractSourceFunctionBody(Source, "static SSettingsAssetsCardHydrationScheduler BeginAssetsCardHydrationFrame");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(BeginFrameBody.empty());
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Source.find("constexpr int AssetsTabSwitchCooldownFrames = 8;"), std::string::npos);
	EXPECT_NE(BeginFrameBody.find("Scheduler.m_TabSwitchShellOnlyFrame = AssetsTabSwitchFirstFrame;"), std::string::npos);
	EXPECT_NE(BeginFrameBody.find("if(AssetsTabSwitchFirstFrame)"), std::string::npos);
	EXPECT_NE(BeginFrameBody.find("Scheduler.m_MetadataBudget = maximum(1, minimum(VisibleCardCount, MetadataLayoutTokens));"), std::string::npos);
	EXPECT_NE(BeginFrameBody.find("Scheduler.m_PreviewBudget = 0;"), std::string::npos);
	EXPECT_NE(BeginFrameBody.find("return Scheduler;"), std::string::npos);
	EXPECT_NE(BeginFrameBody.find("Scheduler.m_PreviewBudget = AssetsTabSwitchCooldownActive ? 0"), std::string::npos);
	EXPECT_NE(Body.find("LogAssetsFramePerfStage(\"assets_tab_switch_shell_first\""), std::string::npos);
	EXPECT_NE(Body.find("operation=settings_assets_tab_switch"), std::string::npos);
	EXPECT_NE(Body.find("tab_switch_shell_only=1"), std::string::npos);
	EXPECT_NE(Body.find("CardHydrationScheduler.m_TabSwitchShellOnlyFrame ? 1 : 0"), std::string::npos);
}

TEST(QmMonitoringHelpers, SkinsUseSharedPreviewUploadBudget)
{
	const std::string Skins = ReadRepoFile("src/game/client/components/skins.cpp");
	const std::string Menus = ReadRepoFile("src/game/client/components/menus_settings.cpp");
	const std::string Header = ReadRepoFile("src/game/client/components/qmclient/settings_resource_preview.h");

	EXPECT_NE(Header.find("CSettingsResourcePreviewScheduler"), std::string::npos);
	EXPECT_NE(Header.find("CSettingsResourcePreviewUploadScheduler"), std::string::npos);
	EXPECT_NE(Skins.find("#include <game/client/components/qmclient/settings_resource_preview.h>"), std::string::npos);
	EXPECT_NE(Skins.find("SResourcePreviewUploadBudget SkinPreviewUploadBudget"), std::string::npos);
	const std::string DrainBody = ExtractSourceFunctionBody(Skins, "CSkins::ESkinProcessResult CSkins::DrainSettingsSkinPreviewUpload(CSkinContainer *pSkinContainer, CSkinLoadingStats &Stats,\n\tint &SkinsProcessedThisFrame, std::chrono::nanoseconds StartTime,\n\tstd::chrono::nanoseconds MaxTime)");
	ASSERT_FALSE(DrainBody.empty());
	EXPECT_NE(DrainBody.find("SkinPreviewUploadBudget.m_MaxUploads = GameClient()->GpuUploadLimiter()->RemainingUploads();"), std::string::npos);
	EXPECT_EQ(DrainBody.find("UploadNextSkinPreviewSprite(pSkinContainer, SkinPreviewUploadBudget)"), std::string::npos);
	EXPECT_NE(DrainBody.find("SettingsResourcePreviewCommitUploadBudget(SkinPreviewUploadBudget, SETTINGS_SKIN_SOURCE_TEXTURE_UPLOADS)"), std::string::npos);
	EXPECT_NE(Skins.find("preview_uploads"), std::string::npos);
	EXPECT_NE(DrainBody.find("SettingsResourcePreviewConsumeUploadBudget(SkinPreviewUploadBudget, SETTINGS_SKIN_SOURCE_TEXTURE_UPLOADS)"), std::string::npos);
	EXPECT_NE(DrainBody.find("LoadSkinFinish(pSkinContainer"), std::string::npos);
	EXPECT_NE(Menus.find("SResourcePreviewTelemetry TeePreviewTelemetry"), std::string::npos);
	EXPECT_NE(Menus.find("SettingsResourcePreviewDrawResult(TeeResourcePreviewState)"), std::string::npos);
	EXPECT_NE(Menus.find("tee_preview_admissions=%d tee_ready_textures=%d tee_placeholders=%d"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsMetadataHydrationIsBudgetedAndTimed)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Source.find("HydrateAssetsCardMetadataTimed("), std::string::npos);
	EXPECT_NE(Source.find("ResourcePreviewTelemetry.m_MetadataHydrateMs += HydrateTimer.ElapsedMs();"), std::string::npos);
	EXPECT_NE(Source.find("static int DrainAssetsCardMetadataHydrationRequests"), std::string::npos);
	EXPECT_NE(Body.find("const int AssetsInitialMetadataLayoutTokens = maximum(1, minimum(AdaptiveBudget.m_VisibleTokens, 4));"), std::string::npos);
	EXPECT_NE(Body.find("const int AssetsMetadataLayoutTokensThisFrame = AssetsShellOnlyFrame ? AssetsInitialMetadataLayoutTokens"), std::string::npos);
	EXPECT_NE(Body.find("DrainAssetsCardMetadataHydrationRequests(AssetsMetadataLayoutTokensThisFrame"), std::string::npos);
	EXPECT_NE(Body.find("RequestAssetsCardMetadataHydration("), std::string::npos);
	EXPECT_EQ(Body.find("HydrateAssetsCardMetadataTimed("), std::string::npos);
	EXPECT_EQ(Body.find("HydrateAssetsCardMetadata(\n"), std::string::npos);
}

TEST(QmMonitoringHelpers, TextGlyphContainerTelemetryExists)
{
	const std::string Text = ReadRepoFile("src/engine/client/text.cpp");
	const std::string Stats = ReadRepoFile("qmclient_scripts/perf/lib/stats.ts");
	const std::string Report = ReadRepoFile("qmclient_scripts/perf/lib/report.ts");
	const std::string Tests = ReadRepoFile("qmclient_scripts/perf/test.ts");

	EXPECT_NE(Text.find("perf/text"), std::string::npos);
	EXPECT_NE(Text.find("event=text_runtime_budget"), std::string::npos);
	EXPECT_NE(Text.find("glyph_new=%d glyph_uploads=%d glyph_rasterize_ms=%.3f glyph_upload_ms=%.3f text_container_new=%d text_container_uploads=%d text_container_create_ms=%.3f text_container_upload_ms=%.3f"), std::string::npos);
	EXPECT_NE(Text.find("m_QmPerfGlyphNew"), std::string::npos);
	EXPECT_NE(Text.find("m_QmPerfTextContainerUploads"), std::string::npos);
	EXPECT_NE(Stats.find("export interface TextRuntimeBudgetSummary"), std::string::npos);
	EXPECT_NE(Stats.find("textRuntimeBudgetSummary(entries"), std::string::npos);
	EXPECT_NE(Report.find("Text Pipeline"), std::string::npos);
	EXPECT_NE(Tests.find("testTextRuntimeBudgetSummary"), std::string::npos);
}

TEST(QmMonitoringHelpers, PerfAnalyzerReportsOnePctLowAndPreviewBudget)
{
	const std::string Stats = ReadRepoFile("qmclient_scripts/perf/lib/stats.ts");
	const std::string Report = ReadRepoFile("qmclient_scripts/perf/lib/report.ts");
	const std::string Quality = ReadRepoFile("qmclient_scripts/perf/lib/quality.ts");
	const std::string Tests = ReadRepoFile("qmclient_scripts/perf/test.ts");

	EXPECT_NE(Stats.find("export interface PreviewBudgetSummary"), std::string::npos);
	EXPECT_NE(Stats.find("previewBudgetSummary(entries"), std::string::npos);
	EXPECT_NE(Stats.find("preview_jobs_started"), std::string::npos);
	EXPECT_NE(Stats.find("preview_jobs_done"), std::string::npos);
	EXPECT_NE(Stats.find("preview_uploads"), std::string::npos);
	EXPECT_NE(Stats.find("preview_admissions"), std::string::npos);
	EXPECT_NE(Stats.find("visible_ready_ratio"), std::string::npos);
	EXPECT_NE(Stats.find("fpsOnePctLowAvailable"), std::string::npos);
	EXPECT_NE(Stats.find("coldTabSwitchFpsSummaries"), std::string::npos);
	EXPECT_NE(Stats.find("warmTabSwitchFpsSummaries"), std::string::npos);
	EXPECT_NE(Report.find("Cold/Warm Tab Switch"), std::string::npos);
	EXPECT_NE(Report.find("Preview Budget"), std::string::npos);
	EXPECT_NE(Report.find("1% Low Target"), std::string::npos);
	EXPECT_NE(Quality.find("previewBudget"), std::string::npos);
	EXPECT_NE(Tests.find("testPreviewBudgetSummaryAndColdWarmTabSwitches"), std::string::npos);
}

TEST(QmMonitoringHelpers, PerfAnalyzerCorrelatesOnePctLowWithTextAndResourceBudgets)
{
	const std::string Stats = ReadRepoFile("qmclient_scripts/perf/lib/stats.ts");
	const std::string Report = ReadRepoFile("qmclient_scripts/perf/lib/report.ts");
	const std::string Quality = ReadRepoFile("qmclient_scripts/perf/lib/quality.ts");
	const std::string Tests = ReadRepoFile("qmclient_scripts/perf/test.ts");

	EXPECT_NE(Stats.find("export interface BudgetCorrelationWindow"), std::string::npos);
	EXPECT_NE(Stats.find("export function budgetCorrelationSummary(entries: PerfEntry[]): BudgetCorrelationSummary"), std::string::npos);
	EXPECT_NE(Stats.find("dominantAttribution"), std::string::npos);
	EXPECT_NE(Stats.find("culpritRank"), std::string::npos);
	EXPECT_NE(Quality.find("budgetCorrelation: budgetCorrelationSummary(entries)"), std::string::npos);
	EXPECT_NE(Report.find("Budget Attribution by Window"), std::string::npos);
	EXPECT_NE(Report.find("Top Culprit"), std::string::npos);
	EXPECT_NE(Report.find("Text Pipeline"), std::string::npos);
	EXPECT_NE(Report.find("Preview Budget"), std::string::npos);
	EXPECT_NE(Report.find("UI Frame Scheduler"), std::string::npos);
	EXPECT_NE(Report.find("1% Low Target"), std::string::npos);
	EXPECT_NE(Tests.find("testUnifiedFrameSchedulerAndTextPipelineBudgetSummary"), std::string::npos);
	EXPECT_NE(Tests.find("testPerfAnalyzerCorrelatesOnePctLowWithTextAndResourceBudgets"), std::string::npos);
	EXPECT_NE(Tests.find("testBudgetCorrelationSummaryByFpsWindow"), std::string::npos);
	EXPECT_NE(Tests.find("testBudgetCorrelationRanksCulprits"), std::string::npos);
}

TEST(QmMonitoringHelpers, PerfReportDefaultsToStatisticalSections)
{
	const std::string Report = ReadRepoFile("qmclient_scripts/perf/lib/report.ts");
	const std::string Tests = ReadRepoFile("qmclient_scripts/perf/test.ts");

	// The perf report should default to statistical summaries and sampled charts
	// instead of dumping the full raw log stream.
	EXPECT_NE(Report.find("Assets Draw Distribution"), std::string::npos);
	EXPECT_NE(Report.find("Text Pipeline"), std::string::npos);
	EXPECT_NE(Report.find("Budget Attribution by Window"), std::string::npos);
	EXPECT_NE(Report.find("statisticalSummary("), std::string::npos);
	EXPECT_NE(Report.find("sampleArrayEvenly("), std::string::npos);
	EXPECT_NE(Tests.find("testReportUsesStatisticalBudgetReportInsteadOfRawBudgetDump"), std::string::npos);
	EXPECT_NE(Tests.find("testReportSamplesLargeEmbeddedChartData"), std::string::npos);
	EXPECT_EQ(Report.find("raw event stream"), std::string::npos);
}

TEST(QmMonitoringHelpers, LowFpsWindowRequiresRealSampledAndTopCulprit)
{
	const std::string Report = ReadRepoFile("qmclient_scripts/perf/lib/report.ts");
	const std::string Quality = ReadRepoFile("qmclient_scripts/perf/lib/quality.ts");
	const std::string Tests = ReadRepoFile("qmclient_scripts/perf/test.ts");

	// A 240Hz gate cannot be satisfied with P99-derived placeholders or an
	// unattributed low-fps window.
	EXPECT_NE(Report.find("Top Culprit"), std::string::npos);
	EXPECT_NE(Report.find("unattributed_spike"), std::string::npos);
	EXPECT_NE(Quality.find("fps_1pct_low_missing_real_sampled"), std::string::npos);
	EXPECT_NE(Quality.find("unattributed_spike"), std::string::npos);
	EXPECT_NE(Quality.find("quality.failed ? 'FAIL'"), std::string::npos);
	EXPECT_NE(Tests.find("testPerfAnalyzerFailsUnattributedLowFpsWindow"), std::string::npos);
	EXPECT_NE(Tests.find("testMissingOnePctLowIsMarkedP99DerivedAndNotTargetPass"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsCardShellUsesCompactCurrentLabelBadges)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const size_t LayoutStart = Source.find("auto LayoutAssetsCardShell = ");
	ASSERT_NE(LayoutStart, std::string::npos);
	const size_t LayoutEnd = Source.find("auto ComputeAssetPreviewContentSize = ", LayoutStart);
	ASSERT_NE(LayoutEnd, std::string::npos);
	const std::string LayoutBody = Source.substr(LayoutStart, LayoutEnd - LayoutStart);

	// Small resource cards should size the right-side chip from the current
	// label with tight padding instead of reserving the old wide English badge
	// widths. The left title/id lane keeps the remaining width.
	EXPECT_NE(Source.find("AssetsCardStatusTagHorizontalPadding"), std::string::npos);
	EXPECT_NE(Source.find("AssetsCardStatusTagMaxWidth"), std::string::npos);
	EXPECT_NE(Source.find("constexpr float AssetCardHeaderMargin = 3.0f;"), std::string::npos);
	EXPECT_NE(Source.find("constexpr float AssetCardHeaderControlMargin = 1.0f;"), std::string::npos);
	EXPECT_EQ(Source.find("auto LayoutAssetCardHeader = "), std::string::npos);
	EXPECT_EQ(Source.find("const float AssetsCardDownloadedTagWidth = TextRender()->TextWidth"), std::string::npos);
	EXPECT_EQ(Source.find("const float AssetsCardLocalOnlyBadgeWidth = TextRender()->TextWidth"), std::string::npos);
	EXPECT_NE(LayoutBody.find("auto ComputeBadgeWidth = [&](const char *pLabel)"), std::string::npos);
	EXPECT_NE(LayoutBody.find("TextRender()->TextWidth(AssetsCardStatusTagFontSize, pLabel"), std::string::npos);
	EXPECT_NE(LayoutBody.find("const float TitleMinWidth = 12.0f;"), std::string::npos);
	EXPECT_NE(LayoutBody.find("ReserveTrailingRect(TitleRect, ComputeBadgeWidth(pStatusLabel)"), std::string::npos);
	EXPECT_NE(LayoutBody.find("ReserveTrailingRect(TitleRect, ComputeBadgeWidth(Localize(\"Local-only\"))"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsCardMetadataFallbackUsesStableTextGeometry)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string FallbackBody = ExtractSourceFunctionBody(Source, "static void RenderAssetsCardMetadataFallback");
	const std::string MetadataBody = ExtractSourceFunctionBody(Source, "auto RenderAssetsCardMetadata");
	ASSERT_FALSE(FallbackBody.empty());
	ASSERT_FALSE(MetadataBody.empty());

	// Cache-miss frames must look like cache-hit frames. Larger fallback text
	// caused title/status overlap and visible size changes while metadata hydrated.
	EXPECT_NE(FallbackBody.find("AssetsCardTitleFontSize"), std::string::npos);
	EXPECT_NE(FallbackBody.find("AssetsCardAuthorFontSize"), std::string::npos);
	EXPECT_NE(MetadataBody.find("AssetsCardTitleFontSize"), std::string::npos);
	EXPECT_NE(MetadataBody.find("AssetsCardAuthorFontSize"), std::string::npos);
	EXPECT_NE(FallbackBody.find("SLabelProperties TitleProps"), std::string::npos);
	EXPECT_NE(FallbackBody.find("TitleProps.m_EllipsisAtEnd = true;"), std::string::npos);
	EXPECT_NE(FallbackBody.find("AuthorProps.m_EllipsisAtEnd = true;"), std::string::npos);
	EXPECT_EQ(FallbackBody.find("Shell.m_ActionButtonRect : Shell.m_TitleRect"), std::string::npos);
	EXPECT_NE(FallbackBody.find("Shell.m_StatusTagRect"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsCardMetadataFallbackRendersStatusTags)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string FallbackBody = ExtractSourceFunctionBody(Source, "static void RenderAssetsCardMetadataFallback");
	const std::string MetadataBody = ExtractSourceFunctionBody(Source, "auto RenderAssetsCardMetadata");
	ASSERT_FALSE(FallbackBody.empty());
	ASSERT_FALSE(MetadataBody.empty());

	// Status tags are part of the fixed card shell. A cache miss must not hide
	// Local/Network/Downloaded badges while metadata hydration catches up.
	EXPECT_NE(FallbackBody.find("Shell.m_HasStatusTag"), std::string::npos);
	EXPECT_NE(FallbackBody.find("Shell.m_StatusTagRect"), std::string::npos);
	EXPECT_NE(FallbackBody.find("pStatusLabel"), std::string::npos);
	EXPECT_NE(FallbackBody.find("pUi->DoLabel(&StatusRect"), std::string::npos);
	EXPECT_NE(MetadataBody.find("Metadata.m_StatusLabel.c_str()"), std::string::npos);
	EXPECT_EQ(FallbackBody.find("(void)pStatusLabel"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsScrollRendersCachedMetadata)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	// Scroll pressure blocks preview/upload work, not visible metadata drawing.
	// A ready cache entry is cheap O(visible) drawing and must stay visible.
	EXPECT_NE(Body.find("const bool AssetsRenderCardMetadataFallback = !AssetsShellOnlyFrame;"), std::string::npos);
	EXPECT_NE(Body.find("if(pMetadata != nullptr)\n\t\t\t\tRenderAssetsCardMetadataCached"), std::string::npos);
	EXPECT_NE(Body.find("if(pMetadata != nullptr)\n\t\t\t\t\t\tRenderAssetsCardMetadataCached"), std::string::npos);
	EXPECT_EQ(Body.find("if(pMetadata != nullptr && !AssetsContentWarmupBlocked)"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsScrollRendersCachedMetadataWithoutStreamedContainerWait)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	// Holding the scrollbar disables text-container creation. Cached metadata
	// must still be readable by drawing the cached strings through fixed-geometry
	// fallback instead of waiting for DoMenuLabelStreamed containers to hydrate.
	EXPECT_NE(Body.find("pMetadata != nullptr && AssetsContentWarmupBlocked"), std::string::npos);
	EXPECT_NE(Body.find("pMetadata->m_Title.c_str()"), std::string::npos);
	EXPECT_NE(Body.find("pMetadata->m_Author.c_str()"), std::string::npos);
	EXPECT_NE(Body.find("pMetadata->m_StatusLabel.c_str()"), std::string::npos);
	EXPECT_LT(Body.find("pMetadata != nullptr && AssetsContentWarmupBlocked"), Body.find("RenderAssetsCardMetadataCached(Shell, pMetadata, RenderAssetsCardMetadata);"));
}

TEST(QmMonitoringHelpers, AssetsScrollMissStillShowsImmediateMetadataFallback)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	// Fast scrollbar drags block hydration work, but visible card titles must
	// remain readable even when the metadata cache missed for a newly exposed
	// range. Missing metadata may use immediate fixed-geometry fallback; it must
	// not be hidden behind AssetsRenderCardMetadataFallback.
	EXPECT_NE(Body.find("const bool AssetsRenderCardMetadataFallback = !AssetsShellOnlyFrame;"), std::string::npos);
	EXPECT_EQ(Body.find("const bool AssetsRenderCardMetadataFallback = !AssetsContentWarmupBlocked;"), std::string::npos);
	EXPECT_NE(Body.find("else if(AssetsRenderCardMetadataFallback)"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsFirstVisibleFrameHasMetadataWarmupBudget)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string BeginFrameBody = ExtractSourceFunctionBody(Source, "static SSettingsAssetsCardHydrationScheduler BeginAssetsCardHydrationFrame");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(BeginFrameBody.empty());
	ASSERT_FALSE(Body.empty());

	// First visible frame should have ready metadata for the first screen, not a
	// long shell-only phase. Preview/upload can stay blocked; metadata gets a
	// small first-frame budget.
	EXPECT_NE(BeginFrameBody.find("if(AssetsTabSwitchFirstFrame)"), std::string::npos);
	EXPECT_NE(BeginFrameBody.find("Scheduler.m_MetadataBudget = maximum(1, minimum(VisibleCardCount, MetadataLayoutTokens));"), std::string::npos);
	EXPECT_NE(Body.find("const int AssetsMetadataLayoutTokensThisFrame = AssetsShellOnlyFrame ? AssetsInitialMetadataLayoutTokens"), std::string::npos);
	EXPECT_NE(Body.find("DrainAssetsCardMetadataHydrationRequests(AssetsMetadataLayoutTokensThisFrame"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsTabSwitchResetsListScrollToTop)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("s_AssetsResetListScrollOnTabSwitch = true;"), std::string::npos);
	EXPECT_NE(Body.find("if(s_AssetsResetListScrollOnTabSwitch)\n\t\t{\n\t\t\ts_ListBox.ResetScroll();"), std::string::npos);
	EXPECT_NE(Body.find("if(s_AssetsResetListScrollOnTabSwitch)\n\t\t\t{\n\t\t\t\ts_WorkshopAssetsListBox.ResetScroll();"), std::string::npos);
	EXPECT_LT(Body.find("s_AssetsResetListScrollOnTabSwitch = true;"), Body.find("s_WorkshopAssetsListBox.DoStart("));
}

TEST(QmMonitoringHelpers, AssetsStatusLabelsDistinguishLocalNetworkWorkshopDownloaded)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Source.find("auto ResolveLocalAssetStatusLabel = "), std::string::npos);
	EXPECT_NE(Source.find("pWorkshopAsset != nullptr ? Localize(\"Downloaded\") : Localize(\"Local\")"), std::string::npos);
	EXPECT_NE(Body.find("ResolveLocalAssetStatusLabel(pLocalItem, ShowLocalOnlyBadge)"), std::string::npos);
	EXPECT_NE(Body.find("ResolveLocalAssetStatusLabel(pItem, ShowLocalOnlyBadge)"), std::string::npos);
	EXPECT_NE(Body.find("Localize(Asset.m_Installed ? \"Downloaded\" : \"Network\")"), std::string::npos);
	EXPECT_EQ(Body.find("Asset.m_Installed ? \"Downloaded\" : \"Not downloaded\""), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsStatusTagKeepsMinimumReadableWidth)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const size_t LayoutStart = Source.find("auto LayoutAssetsCardShell = ");
	ASSERT_NE(LayoutStart, std::string::npos);
	const size_t LayoutEnd = Source.find("auto ComputeAssetPreviewContentSize = ", LayoutStart);
	ASSERT_NE(LayoutEnd, std::string::npos);
	const std::string LayoutBody = Source.substr(LayoutStart, LayoutEnd - LayoutStart);

	// Narrow cards must not squeeze the status tag into the delete icon area.
	// If the tag cannot keep a readable minimum width, it must not reserve a
	// header slot.
	EXPECT_NE(LayoutBody.find("AssetsCardStatusTagMinWidth"), std::string::npos);
	EXPECT_NE(LayoutBody.find("if(MaxWidth < DesiredMinWidth)"), std::string::npos);
	EXPECT_NE(LayoutBody.find("DesiredMinWidth"), std::string::npos);
	EXPECT_NE(LayoutBody.find("ReserveTrailingRect(TitleRect, ComputeBadgeWidth(pStatusLabel), AssetsCardStatusTagMinWidth"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsStatusTagColorsDistinguishReadyFromNetwork)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const size_t RenderStart = Source.find("auto RenderAssetStatusTag = ");
	ASSERT_NE(RenderStart, std::string::npos);
	const size_t RenderEnd = Source.find("auto ComputePreviewDrawRect = ", RenderStart);
	ASSERT_NE(RenderEnd, std::string::npos);
	const std::string RenderBody = Source.substr(RenderStart, RenderEnd - RenderStart);

	// Ready-to-use local/downloaded assets stay green. Network-only assets use
	// the neutral label color so the availability state remains visible.
	EXPECT_NE(RenderBody.find("AssetsCardStatusReadyColor"), std::string::npos);
	EXPECT_NE(RenderBody.find("AssetsCardStatusNetworkColor"), std::string::npos);
	EXPECT_NE(RenderBody.find("Positive ? AssetsCardStatusReadyColor : AssetsCardStatusNetworkColor"), std::string::npos);
	EXPECT_NE(Source.find("RenderAssetsCardMetadataFallback(Ui(), Shell, pMetadata->m_Title.c_str(), pMetadata->m_Author.c_str(), pMetadata->m_StatusLabel.c_str(), pMetadata->m_Installed || pMetadata->m_LocalOnly"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsStatusTagsUseTightHorizontalPadding)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string FallbackBody = ExtractSourceFunctionBody(Source, "static void RenderAssetsCardMetadataFallback");
	const size_t RenderStart = Source.find("auto RenderAssetStatusTag = ");
	ASSERT_FALSE(FallbackBody.empty());
	ASSERT_NE(RenderStart, std::string::npos);
	const size_t RenderEnd = Source.find("auto ComputePreviewDrawRect = ", RenderStart);
	ASSERT_NE(RenderEnd, std::string::npos);
	const std::string RenderBody = Source.substr(RenderStart, RenderEnd - RenderStart);

	EXPECT_NE(Source.find("AssetsCardStatusTagHorizontalPadding"), std::string::npos);
	EXPECT_NE(FallbackBody.find("StatusLabelProps.m_MaxWidth = static_cast<int>(StatusRect.w - AssetsCardStatusTagHorizontalPadding * 2.0f);"), std::string::npos);
	EXPECT_NE(RenderBody.find("StatusLabelProps.m_MaxWidth = static_cast<int>(StatusRect.w - AssetsCardStatusTagHorizontalPadding * 2.0f);"), std::string::npos);
	EXPECT_EQ(FallbackBody.find("StatusRect.w - 2.0f"), std::string::npos);
	EXPECT_EQ(RenderBody.find("StatusRect.w - 2.0f"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsEntityBgFallbackTextIsCenteredTodo)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const size_t FallbackStart = Source.find("auto RenderEntityBgFallback = ");
	ASSERT_NE(FallbackStart, std::string::npos);
	const size_t FallbackEnd = Source.find("auto RenderEntityBgVideoFallback = ", FallbackStart);
	ASSERT_NE(FallbackEnd, std::string::npos);
	const std::string FallbackBody = Source.substr(FallbackStart, FallbackEnd - FallbackStart);

	EXPECT_NE(FallbackBody.find("Localize(\"Map Preview TODO\")"), std::string::npos);
	EXPECT_NE(FallbackBody.find("TEXTALIGN_MC"), std::string::npos);
	EXPECT_NE(FallbackBody.find("LabelRect = FallbackRect;"), std::string::npos);
	EXPECT_EQ(FallbackBody.find("Localize(\"Map Preview\")"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsPreviewFinalizeBudgetsAreNotHardCappedToOnePerFrame)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");

	// Ready previews/thumbs should appear as soon as each one finishes; the
	// pipeline must not serialize visible finalization to 1 item per frame.
	// Raising the steady-state budget is fine, but scroll-active frames still
	// keep a hard visible-stage cap through SettingsResourceFrameStageBudget(..., 0).
	EXPECT_NE(Source.find("constexpr int MaxPreviewDecodeFinalizesPerFrame = 16;"), std::string::npos);
	EXPECT_NE(Source.find("constexpr int MaxWorkshopThumbDecodeFinalizesPerFrame = 16;"), std::string::npos);
	EXPECT_EQ(Source.find("constexpr int MaxPreviewDecodeFinalizesPerFrame = 1;"), std::string::npos);
	EXPECT_EQ(Source.find("constexpr int MaxWorkshopThumbDecodeFinalizesPerFrame = 1;"), std::string::npos);
	EXPECT_NE(Source.find("SettingsResourceFrameStageBudget(FinalizeFrameContext, FinalizePriority, MaxPreviewDecodeFinalizesPerFrame, 0)"), std::string::npos);
	EXPECT_NE(Source.find("SettingsResourceFrameStageBudget(FinalizeFrameContext, FinalizePriority, MaxWorkshopThumbDecodeFinalizesThisFrame, 0)"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsCardMetadataDoesNotHydrateWithoutBudget)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Source.find("FindAssetsCardMetadata("), std::string::npos);
	EXPECT_NE(Source.find("HydrateAssetsCardMetadata("), std::string::npos);
	EXPECT_EQ(Source.find("RefreshAssetsCardMetadata("), std::string::npos);
	EXPECT_EQ(Body.find("if(pMetadata == nullptr && CardHydrationScheduler.CanHydrateMetadata(CombinedVisible))"), std::string::npos);
	EXPECT_NE(Body.find("RequestAssetsCardMetadataHydration("), std::string::npos);
	EXPECT_EQ(Body.find("if(pMetadata == nullptr && RenderMetadata)"), std::string::npos);
	EXPECT_EQ(Body.find("if(pMetadata != nullptr && RenderMetadata)"), std::string::npos);
	EXPECT_EQ(Body.find("GetOrHydrateAssetsCardMetadata("), std::string::npos);
	EXPECT_EQ(Body.find("Ui()->DoLabel(&ErrorRect, Localize(\"Download failed\"), 9.0f, TEXTALIGN_MC);"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsCardInitialEntryUsesStableShellGeometry)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Source.find("AssetsCardShellShowsAuthorRow("), std::string::npos);
	EXPECT_NE(Source.find("AssetsCardListAreaWithStableScrollbar("), std::string::npos);
	EXPECT_NE(Source.find("RenderAssetsCardLoadingShells("), std::string::npos);
	EXPECT_NE(Body.find("const bool ShowAuthorRow = AssetsCardShellShowsAuthorRow(s_CurCustomTab, true"), std::string::npos);
	EXPECT_NE(Body.find("const bool ShowAuthorRow = AssetsCardShellShowsAuthorRow(s_CurCustomTab, false"), std::string::npos);
	EXPECT_NE(Body.find("AssetsCardListAreaWithStableScrollbar(WorkshopListArea, s_WorkshopAssetsListBox.ScrollbarWidthMax(), s_WorkshopAssetsListBox.ScrollbarMargin())"), std::string::npos);
	EXPECT_NE(Body.find("s_WorkshopAssetsListBox.DoStart(WorkshopRowHeight, CombinedCount, Columns, 1, OldCombinedSelected, &WorkshopListArea, false, IGraphics::CORNER_ALL, true);"), std::string::npos);
	EXPECT_NE(Body.find("RenderAssetsCardLoadingShells("), std::string::npos);
	EXPECT_NE(Body.find("const bool AssetsInitialEntryLoading = m_aAssetLoadStates[s_CurCustomTab] == ASSET_LOAD_STATE_LOADING || WorkshopState.m_pListTask != nullptr;"), std::string::npos);
	EXPECT_NE(Body.find("stage=assets_card_geometry"), std::string::npos);
	EXPECT_NE(Body.find("geometry_changed=%d"), std::string::npos);
	EXPECT_LT(Body.find("if(AssetsInitialEntryLoading)"), Body.find("Ui()->DoLabel(&WorkshopListArea, Localize(\"No assets\"), 12.0f, TEXTALIGN_MC);"));
	EXPECT_EQ(Source.find("ShouldShowAssetCardAuthorRow(!Asset.m_Author.empty(), false)"), std::string::npos);
	EXPECT_EQ(Source.find("ShowAuthorRow = !Asset.m_Author.empty()"), std::string::npos);
}

TEST(QmMonitoringHelpers, DemoBrowserUsesAdaptiveMetadataBudget)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_demo.cpp");
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderDemoBrowserList(CUIRect ListView, bool &WasListboxItemActivated)");
	ASSERT_FALSE(Body.empty());

	EXPECT_EQ(Header.find("SSettingsAdaptiveBudgetState m_DemoBrowserAdaptiveBudgetState"), std::string::npos);
	EXPECT_NE(Body.find("BeginSettingsUiFrameScheduler(EFrameSchedulerConsumer::DemoBrowser, \"demo_browser\""), std::string::npos);
	EXPECT_NE(Body.find("AdaptiveBudget.m_DemoMetadataTokens"), std::string::npos);
	EXPECT_NE(ReadRepoFile("src/game/client/components/menus.cpp").find("event=settings_adaptive_budget"), std::string::npos);
	EXPECT_EQ(Body.find("AdvanceDemoBrowserMetadata(g_Config.m_BrDemoFetchInfo && !BrowsingScreenshots ? 2 : 0, g_Config.m_BrDemoSort == SORT_DATE ? 4 : 0"), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsTextPrebuildUsesAdaptiveBudget)
{
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string PrebuildBody = ExtractSourceFunctionBody(Menus, "int CMenus::PrebuildSettingsTextPoolForLoading(int Budget, const char *pOperationOverride)");
	const std::string EscBody = ExtractSourceFunctionBody(Menus, "void CMenus::PrebuildIngameEscTextPoolBeforeOpen(int Budget)");
	ASSERT_FALSE(PrebuildBody.empty());
	ASSERT_FALSE(EscBody.empty());

	EXPECT_EQ(Header.find("SSettingsAdaptiveBudgetState m_SettingsTextAdaptiveBudgetState"), std::string::npos);
	EXPECT_NE(PrebuildBody.find("BeginSettingsUiFrameScheduler(EFrameSchedulerConsumer::SettingsText, \"stable_text_prebuild\""), std::string::npos);
	EXPECT_NE(PrebuildBody.find("AdaptiveBudget.m_TextPrebuildTokens"), std::string::npos);
	EXPECT_NE(Menus.find("event=settings_adaptive_budget"), std::string::npos);
	EXPECT_NE(EscBody.find("BeginSettingsUiFrameScheduler(EFrameSchedulerConsumer::SettingsText, \"stable_text_ingame_esc\""), std::string::npos);
	EXPECT_EQ(EscBody.find("PrebuildSettingsMenuTextPool(minimum(Budget, 4), \"target_settings\", \"ingame_esc_open\");"), std::string::npos);
}

TEST(QmMonitoringHelpers, ReportStableTextSamplesDoNotWrapKeyVertically)
{
	const std::string Report = ReadRepoFile("qmclient_scripts/perf/lib/report.ts");
	const std::string Tests = ReadRepoFile("qmclient_scripts/perf/test.ts");

	EXPECT_NE(Report.find("sample-key-cell"), std::string::npos);
	EXPECT_NE(Report.find("white-space:nowrap"), std::string::npos);
	EXPECT_NE(Report.find("table-layout:fixed"), std::string::npos);
	EXPECT_NE(Report.find("truncateMiddle(sampleField(sample, 'key')"), std::string::npos);
	EXPECT_NE(Tests.find("must-not-wrap-vertically"), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsStableTextTargetAcceptanceRequiresFullCoverage)
{
	const std::string Stats = ReadRepoFile("qmclient_scripts/perf/lib/stats.ts");
	const std::string Report = ReadRepoFile("qmclient_scripts/perf/lib/report.ts");
	const std::string Quality = ReadRepoFile("qmclient_scripts/perf/lib/quality.ts");

	EXPECT_NE(Stats.find("acceptanceBlocked: missCount > 0 || staleCount > 0 || prebuildRemainingBeforeTarget > 0 || !planCollectionAvailable || !planCollectionComplete || !utilizationAvailable || !planCoverageAvailable || unplannedVisibleCount > 0 || keyMismatchCount > 0 || textNew > 0"), std::string::npos);
	EXPECT_NE(Stats.find("unplanned visible stable text candidates"), std::string::npos);
	EXPECT_NE(Stats.find("stable text key mismatches"), std::string::npos);
	EXPECT_NE(Stats.find("settings_text_plan_collection"), std::string::npos);
	EXPECT_NE(Stats.find("planCollectionRemainingBeforeTarget"), std::string::npos);
	EXPECT_NE(Stats.find("!planCollectionAvailable || !planCollectionComplete"), std::string::npos);
	EXPECT_NE(Report.find("collection remaining=0 只表示计划收集完成"), std::string::npos);
	EXPECT_NE(Report.find("Plan Collection"), std::string::npos);
	EXPECT_NE(Report.find("Container Remaining"), std::string::npos);
	EXPECT_NE(Report.find("Visible Coverage"), std::string::npos);
	EXPECT_NE(Report.find("Assets Visible-First Admission"), std::string::npos);
	EXPECT_NE(Stats.find("assetsVisibleReadySummary"), std::string::npos);
	EXPECT_NE(Stats.find("thumbStartsDuringDraw > 0"), std::string::npos);
	EXPECT_NE(Stats.find("geometryStable === false"), std::string::npos);
	EXPECT_NE(Report.find("Visible Ready"), std::string::npos);
	EXPECT_NE(Quality.find("stable text coverage blocked settings acceptance"), std::string::npos);
	EXPECT_NE(Quality.find("assets visible-ready preflight missing"), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsTextMissLogsAreSampledPerFrameBucket)
{
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Body = ExtractSourceFunctionBody(Menus, "void LogSettingsTextPoolCoverageGap(IClient *pClient, const char *pEvent, CMenus::EMenuTextScope Scope, const char *pScopeName, int Page, int Tab, int Subtab, const char *pKey, const char *pReason, const char *pPlanStatus, const char *pOperation, uint64_t Frame)");
	ASSERT_FALSE(Body.empty());

	// This guard keeps target coverage useful without returning to multi-GB
	// logs when thousands of visible labels share the same key-mismatch cause.
	EXPECT_NE(Body.find("MaxGapSamplesPerFrameBucket"), std::string::npos);
	EXPECT_NE(Body.find("s_SamplesThisBucket"), std::string::npos);
	EXPECT_NE(Body.find("log_sample_limit"), std::string::npos);
}

TEST(QmMonitoringHelpers, DefaultGateRunsFullAutomatedTests)
{
	const std::string Gate = ReadRepoFile("qmclient_scripts/gate/check_gate.py");
	const std::string Verification = ReadRepoFile("docs/ai-workflow/verification.md");
	const std::string ScriptsOverview = ReadRepoFile("qmclient_scripts/scripts_overview.md");
	ASSERT_FALSE(Gate.empty());

	const size_t DefaultMode = Gate.find("\"default\": {");
	ASSERT_NE(DefaultMode, std::string::npos);
	const size_t FullMode = Gate.find("\"full\": {", DefaultMode);
	ASSERT_NE(FullMode, std::string::npos);
	const std::string DefaultSpec = Gate.substr(DefaultMode, FullMode - DefaultMode);

	// The default gate is the normal pre-submit gate, so it must run the full
	// automated test set. Full mode is reserved for extra heavyweight/noisy
	// checks, not for merely getting Rust tests.
	EXPECT_NE(DefaultSpec.find("\"tests\": {\"cxx\": True, \"rust\": True, \"all\": False}"), std::string::npos);
	EXPECT_EQ(DefaultSpec.find("\"strict_build\""), std::string::npos);
	EXPECT_EQ(DefaultSpec.find("\"dilate\""), std::string::npos);
	EXPECT_NE(DefaultSpec.find("C++ 全量测试和 Rust 全量测试"), std::string::npos);
	EXPECT_NE(Gate.substr(FullMode).find("\"strict_build\""), std::string::npos);
	EXPECT_NE(Gate.substr(FullMode).find("\"dilate\""), std::string::npos);
	EXPECT_NE(Verification.find("C++ 全量测试和 Rust 全量测试"), std::string::npos);
	EXPECT_NE(Verification.find("严格构建与静态分析只属于 full gate"), std::string::npos);
	EXPECT_NE(ScriptsOverview.find("C++ 全量测试和 Rust 全量测试"), std::string::npos);
	EXPECT_NE(ScriptsOverview.find("严格构建与静态分析只属于 full gate"), std::string::npos);
	EXPECT_NE(ScriptsOverview.find("不作为“全量测试”的默认入口"), std::string::npos);
}

TEST(QmMonitoringHelpers, NightlyWorkflowPublishesPdbFreePrerelease)
{
	const std::string Nightly = ReadRepoFile(".github/workflows/nightly.yml");
	ASSERT_FALSE(Nightly.empty());

	EXPECT_NE(Nightly.find("workflow_dispatch:"), std::string::npos);
	EXPECT_NE(Nightly.find("schedule:"), std::string::npos);
	EXPECT_NE(Nightly.find("Check Nightly Changes"), std::string::npos);
	EXPECT_NE(Nightly.find("git ls-remote --tags origin refs/tags/nightly"), std::string::npos);
	EXPECT_NE(Nightly.find("should_build=false"), std::string::npos);
	EXPECT_NE(Nightly.find("github.event_name"), std::string::npos);
	EXPECT_NE(Nightly.find("needs.changes.outputs.should_build == 'true'"), std::string::npos);
	EXPECT_NE(Nightly.find("cmake --build build --target package_default --parallel"), std::string::npos);
	EXPECT_NE(Nightly.find("QmClient-windows.zip"), std::string::npos);
	EXPECT_NE(Nightly.find("QmClient-windows.7z"), std::string::npos);
	EXPECT_NE(Nightly.find("QmClient-ubuntu.tar.xz"), std::string::npos);
	EXPECT_NE(Nightly.find("QmClient-macOS.dmg"), std::string::npos);
	EXPECT_NE(Nightly.find("PDB files must not be published"), std::string::npos);
	EXPECT_NE(Nightly.find("\\.pdb$"), std::string::npos);
	EXPECT_NE(Nightly.find("gh release delete nightly --cleanup-tag --yes || true"), std::string::npos);
	EXPECT_NE(Nightly.find("gh release create nightly"), std::string::npos);
	EXPECT_NE(Nightly.find("--prerelease"), std::string::npos);
	EXPECT_NE(Nightly.find("contents: write"), std::string::npos);
	EXPECT_EQ(Nightly.find("startsWith(github.ref, 'refs/tags/')"), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsScrollRegionHelperExists)
{
	const std::string Header = ReadRepoFile("src/game/client/components/menus.h");
	const std::string Menus = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string ScrollRegion = ReadRepoFile("src/game/client/ui_scrollregion.cpp");

	EXPECT_NE(Header.find("struct SSettingsScrollRegionFrame"), std::string::npos);
	EXPECT_NE(Header.find("BeginSettingsScrollRegion(CScrollRegion &ScrollRegion"), std::string::npos);
	EXPECT_NE(Header.find("FinishSettingsScrollRegion(CScrollRegion &ScrollRegion"), std::string::npos);
	EXPECT_NE(Header.find("const CUIRect *pEndRect = nullptr"), std::string::npos);
	EXPECT_NE(Menus.find("CMenus::SSettingsScrollRegionFrame CMenus::BeginSettingsScrollRegion(CScrollRegion &ScrollRegion"), std::string::npos);
	EXPECT_NE(Menus.find("void CMenus::FinishSettingsScrollRegion(CScrollRegion &ScrollRegion"), std::string::npos);
	EXPECT_NE(Menus.find("if(pEndRect != nullptr)\n\t\tScrollRegion.AddRect(*pEndRect);"), std::string::npos);
	EXPECT_NE(Menus.find("Frame.m_FinalOffsetY = ScrollRegion.ScrollbarShown() ? ScrollRegion.ContentScrollOffsetY() : 0.0f;"), std::string::npos);
	EXPECT_NE(Menus.find("m_SettingsScrollActive = m_SettingsScrollActive ||"), std::string::npos);
	EXPECT_NE(Menus.find("m_SettingsRuntimeMetadata.m_LastScrollY = Frame.m_FinalOffsetY;"), std::string::npos);
	EXPECT_NE(ScrollRegion.find("m_ContentScrollOff.y = -m_ScrollY;"), std::string::npos);
	const std::string ScrollRegionEnd = ExtractSourceFunctionBody(ScrollRegion, "void CScrollRegion::End()");
	ASSERT_FALSE(ScrollRegionEnd.empty());
	const size_t NoOverflowPos = ScrollRegionEnd.find("if(m_ContentH <= m_ClipRect.h)");
	const size_t ScrollWheelPos = ScrollRegionEnd.find("// scroll wheel");
	ASSERT_NE(NoOverflowPos, std::string::npos);
	ASSERT_NE(ScrollWheelPos, std::string::npos);
	const std::string NoOverflowBranch = ScrollRegionEnd.substr(NoOverflowPos, ScrollWheelPos - NoOverflowPos);
	EXPECT_EQ(NoOverflowBranch.find("m_ScrollY = 0.0f;"), std::string::npos);
	EXPECT_EQ(NoOverflowBranch.find("m_AnimTargetScrollY = 0.0f;"), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsScrollRegionPagesUseUnifiedHelper)
{
	const std::string TClient = ReadRepoFile("src/game/client/components/tclient/menus_tclient.cpp");
	const std::string QmClient = ReadRepoFile("src/game/client/components/qmclient/menus_qmclient.cpp");
	const std::string Controls = ReadRepoFile("src/game/client/components/menus_settings_controls.cpp");
	const std::string Settings = ReadRepoFile("src/game/client/components/menus_settings.cpp");

	EXPECT_NE(TClient.find("BeginSettingsScrollRegion(s_ScrollRegion, &MainView, ScrollParams"), std::string::npos);
	EXPECT_NE(TClient.find("FinishSettingsScrollRegion(s_ScrollRegion, ScrollFrame, &ScrollRegion, SETTINGS_TCLIENT"), std::string::npos);
	EXPECT_NE(TClient.find("BeginSettingsScrollRegion(s_ScrollRegion, &ListArea, ScrollParams"), std::string::npos);
	EXPECT_NE(TClient.find("FinishSettingsScrollRegion(s_ScrollRegion, ScrollFrame, &EndPad"), std::string::npos);
	EXPECT_NE(QmClient.find("BeginSettingsScrollRegion(s_ScrollRegion, &MainView, ScrollParams"), std::string::npos);
	EXPECT_NE(QmClient.find("FinishSettingsScrollRegion(s_ScrollRegion, ScrollFrame, &EndPad"), std::string::npos);
	EXPECT_NE(QmClient.find("FinishSettingsScrollRegion(s_ScrollRegion, ScrollFrame, &ScrollRegion"), std::string::npos);
	EXPECT_NE(Controls.find("BeginSettingsScrollRegion(m_SettingsScrollRegion, &MainView, ScrollParams"), std::string::npos);
	EXPECT_NE(Controls.find("FinishSettingsScrollRegion(m_SettingsScrollRegion, ScrollFrame);"), std::string::npos);
	EXPECT_EQ(Controls.find("FinishSettingsScrollRegion(m_SettingsScrollRegion, ScrollFrame, &"), std::string::npos);
	EXPECT_NE(Settings.find("BeginSettingsScrollRegion(gs_LanguageScrollRegion, &MainView, ScrollParams"), std::string::npos);
	EXPECT_NE(Settings.find("FinishSettingsScrollRegion(gs_LanguageScrollRegion, ScrollFrame, &ScrollRegion, SETTINGS_LANGUAGE"), std::string::npos);
}

TEST(QmMonitoringHelpers, QmClientFunctionHotspotModulesHaveFirstFrameStages)
{
	const std::string Source = ReadRepoFile("src/game/client/components/qmclient/menus_qmclient.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsQmClient(CUIRect MainView, bool ContributorsPage, bool PrewarmOnly)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Source.find("function_first_frame_light_path"), std::string::npos);
	EXPECT_NE(Body.find("const bool FunctionFirstFrameLightPath"), std::string::npos);
	EXPECT_NE(Body.find("QmFunctionModuleUsesLightFirstFramePath("), std::string::npos);
	EXPECT_NE(Body.find("LogQmPerfStage(Client(), \"pie_menu_layout\""), std::string::npos);
	EXPECT_NE(Body.find("LogQmPerfStage(Client(), \"pie_menu_controls\""), std::string::npos);
	EXPECT_NE(Body.find("LogQmPerfStage(Client(), \"block_words_layout\""), std::string::npos);
	EXPECT_NE(Body.find("LogQmPerfStage(Client(), \"block_words_controls\""), std::string::npos);
	EXPECT_NE(Body.find("LogQmPerfStage(Client(), \"gores_layout\""), std::string::npos);
	EXPECT_NE(Body.find("LogQmPerfStage(Client(), \"gores_controls\""), std::string::npos);
	EXPECT_NE(Body.find("LogQmPerfStage(Client(), \"gores_bind_lookup\""), std::string::npos);
}

TEST(QmMonitoringHelpers, SettingsPagesExposeSectionLevelPerfStages)
{
	const std::string Settings = ReadRepoFile("src/game/client/components/menus_settings.cpp");
	const std::string Controls = ReadRepoFile("src/game/client/components/menus_settings_controls.cpp");
	const std::string TClient = ReadRepoFile("src/game/client/components/tclient/menus_tclient.cpp");

	EXPECT_NE(Settings.find("ddnet_tab_shell"), std::string::npos);
	EXPECT_NE(Settings.find("ddnet_demo_section"), std::string::npos);
	EXPECT_NE(Settings.find("ddnet_gameplay_section"), std::string::npos);
	EXPECT_NE(Settings.find("ddnet_controls_section"), std::string::npos);
	EXPECT_NE(Controls.find("controls_tab_shell"), std::string::npos);
	EXPECT_NE(Controls.find("controls_bind_list"), std::string::npos);
	EXPECT_NE(Controls.find("controls_interactive_layer"), std::string::npos);
	EXPECT_NE(Controls.find("controls_text_cache"), std::string::npos);
	EXPECT_NE(TClient.find("tclient_tab_3_shell"), std::string::npos);
	EXPECT_NE(TClient.find("tclient_tab_4_shell"), std::string::npos);
	EXPECT_NE(TClient.find("tclient_tab_5_shell"), std::string::npos);
}

TEST(QmMonitoringHelpers, HudAppearanceTabExposesSectionLevelPerfStages)
{
	const std::string Settings = ReadRepoFile("src/game/client/components/menus_settings.cpp");
	const std::string Body = ExtractSourceFunctionBody(Settings, "void CMenus::RenderSettingsAppearance(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("appearance_hud_tab_shell"), std::string::npos);
	EXPECT_NE(Body.find("appearance_hud_core_section"), std::string::npos);
	EXPECT_NE(Body.find("appearance_hud_ddrace_section"), std::string::npos);
	EXPECT_NE(Body.find("appearance_hud_freeze_bars_section"), std::string::npos);
	EXPECT_NE(Body.find("appearance_hud_text_cache"), std::string::npos);
}

TEST(QmMonitoringHelpers, TClientSettingsDoesNotWriteScrollMetadataBeforeFinish)
{
	const std::string Source = ReadRepoFile("src/game/client/components/tclient/menus_tclient.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsTClientSettings(CUIRect MainView, bool PrewarmOnly)");
	ASSERT_FALSE(Body.empty());

	const size_t BeginPos = Body.find("BeginSettingsScrollRegion(s_ScrollRegion, &MainView, ScrollParams");
	const size_t FinishPos = Body.find("FinishSettingsScrollRegion(s_ScrollRegion, ScrollFrame, &ScrollRegion, SETTINGS_TCLIENT");
	ASSERT_NE(BeginPos, std::string::npos);
	ASSERT_NE(FinishPos, std::string::npos);
	EXPECT_LT(BeginPos, FinishPos);

	const std::string BeforeFinish = Body.substr(0, FinishPos);
	EXPECT_EQ(BeforeFinish.find("m_SettingsRuntimeMetadata.m_LastScrollY ="), std::string::npos);
	EXPECT_EQ(BeforeFinish.find("m_SettingsRuntimeMetadata.m_LastScrollPage ="), std::string::npos);
	EXPECT_EQ(BeforeFinish.find("m_SettingsRuntimeMetadata.m_Valid = true;"), std::string::npos);
}

TEST(QmMonitoringHelpers, TClientTabNamesInitializeBeforeLanguageChange)
{
	const std::string Source = ReadRepoFile("src/game/client/components/tclient/menus_tclient.cpp");
	const size_t TabNamesPos = Source.find("static const char *s_apTClientTabNames[NUMBER_OF_TCLIENT_TABS] = {};");
	ASSERT_NE(TabNamesPos, std::string::npos);
	const size_t FirstTabDrawPos = Source.find("DoButton_MenuTab(&s_aPageTabs[Tab], s_apTClientTabNames[Tab]", TabNamesPos);
	ASSERT_NE(FirstTabDrawPos, std::string::npos);
	const std::string TabNamesBody = Source.substr(TabNamesPos, FirstTabDrawPos - TabNamesPos);

	EXPECT_NE(TabNamesBody.find("s_TClientTabNamesInitialized"), std::string::npos);
	EXPECT_NE(TabNamesBody.find("!s_TClientTabNamesInitialized || str_comp"), std::string::npos);
}

TEST(QmMonitoringHelpers, MenuTextStyleKeyRejectsNonFiniteMaxWidth)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "CMenus::SMenuTextStyleKey CMenus::BuildMenuTextStyleKey");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("std::isfinite(MaxWidth)"), std::string::npos);
	EXPECT_NE(Body.find("MaxWidth >= 0.0f && std::isfinite(MaxWidth)"), std::string::npos);
}

TEST(QmMonitoringHelpers, MenuTextStyleKeyRejectsNonFiniteHiDpiScale)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "CMenus::SMenuTextStyleKey CMenus::BuildMenuTextStyleKey");
	ASSERT_FALSE(Body.empty());

	EXPECT_NE(Body.find("const float HiDpiScale ="), std::string::npos);
	EXPECT_NE(Body.find("std::isfinite(HiDpiScale)"), std::string::npos);
	EXPECT_NE(Body.find("HiDpiScale >= 0.0f && std::isfinite(HiDpiScale)"), std::string::npos);
}

TEST(QmMonitoringHelpers, AppearanceTabNamesInitializeBeforeLanguageChange)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsAppearance(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	const size_t TabNamesPos = Body.find("static const char *s_apAppearanceTabNames[NUMBER_OF_APPEARANCE_TABS] = {};");
	ASSERT_NE(TabNamesPos, std::string::npos);
	const size_t FirstTabDrawPos = Body.find("DoButton_MenuTab(&s_aPageTabs[Tab], s_apAppearanceTabNames[Tab]", TabNamesPos);
	ASSERT_NE(FirstTabDrawPos, std::string::npos);
	const std::string TabNamesBody = Body.substr(TabNamesPos, FirstTabDrawPos - TabNamesPos);

	EXPECT_NE(TabNamesBody.find("s_AppearanceTabNamesInitialized"), std::string::npos);
	EXPECT_NE(TabNamesBody.find("!s_AppearanceTabNamesInitialized || str_comp"), std::string::npos);
}

TEST(QmMonitoringHelpers, AssetsTabNamesInitializeBeforeLanguageChange)
{
	const std::string Source = ReadRepoFile("src/game/client/components/menus_settings_assets.cpp");
	const std::string Body = ExtractSourceFunctionBody(Source, "void CMenus::RenderSettingsCustom(CUIRect MainView)");
	ASSERT_FALSE(Body.empty());

	const size_t TabNamesPos = Body.find("static const char *s_apAssetsTabNames[NUMBER_OF_ASSETS_TABS] = {};");
	ASSERT_NE(TabNamesPos, std::string::npos);
	const size_t FirstTabDrawPos = Body.find("DoButton_MenuTab(&s_aPageTabs[Tab], s_apAssetsTabNames[Tab]", TabNamesPos);
	ASSERT_NE(FirstTabDrawPos, std::string::npos);
	const std::string TabNamesBody = Body.substr(TabNamesPos, FirstTabDrawPos - TabNamesPos);

	EXPECT_NE(TabNamesBody.find("s_AssetsTabNamesInitialized"), std::string::npos);
	EXPECT_NE(TabNamesBody.find("!s_AssetsTabNamesInitialized || str_comp"), std::string::npos);
}

TEST(QmMonitoringHelpers, MenuPerfEventsExposePageAttributionFields)
{
	{
		std::ifstream File(TestSourcePath("src/game/client/components/menus_settings.cpp"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();

		EXPECT_NE(Source.find("event=page_switch from=%s to=%s dur_ms=%.3f"), std::string::npos);
		EXPECT_NE(Source.find("event=section page=%s section=%s dur_ms=%.3f visible=%d dirty=%s text_new=%d text_reused=%d"), std::string::npos);
		EXPECT_NE(Source.find("LogSettingsSectionPerf(IClient *pClient, int Page, int Tab, const char *pSectionId, double DurationMs, const char *pDirtyReason, int TextNew, int TextReused)"), std::string::npos);
		EXPECT_NE(Source.find("pDirtyReason != nullptr ? pDirtyReason : \"unknown\""), std::string::npos);
		EXPECT_NE(Source.find("TextNew, TextReused"), std::string::npos);
		EXPECT_NE(Source.find("CScopedSettingsTextPerfStats TextStats(this);"), std::string::npos);
		EXPECT_NE(Source.find("TextStats.Stats().m_New"), std::string::npos);
		EXPECT_NE(Source.find("TextStats.Stats().m_Reused"), std::string::npos);
		EXPECT_EQ(Source.find("SectionCacheHit ? \"clean\" : \"cache_miss\", 0, 0"), std::string::npos);
		EXPECT_EQ(Source.find("\"cache_miss\""), std::string::npos);
		EXPECT_NE(Source.find("page=%s transition=%d sections=%d sections_visible=%d tab=%s"), std::string::npos);
	}
	{
		std::ifstream File(TestSourcePath("src/game/client/components/menus.h"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();

		EXPECT_NE(Source.find("class CScopedSettingsTextPerfStats"), std::string::npos);
		EXPECT_NE(Source.find("m_pActiveSettingsTextPerfStats = &m_Stats;"), std::string::npos);
		EXPECT_NE(Source.find("m_pActiveSettingsTextPerfStats = m_pPrevious;"), std::string::npos);
	}
	{
		std::ifstream File(TestSourcePath("src/game/client/components/menus.cpp"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();

		EXPECT_NE(Source.find("event=page_switch from=%s to=%s dur_ms=%.3f source=menu_page_switch"), std::string::npos);
		EXPECT_NE(Source.find("event=page_switch from=%s to=%s dur_ms=%.3f source=game_page_switch"), std::string::npos);
		EXPECT_NE(Source.find("DoSettingsLabelStreamed"), std::string::npos);
	}
	{
		std::ifstream File(TestSourcePath("src/game/client/ui.cpp"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();

		EXPECT_NE(Source.find("pTextContainerRecreated"), std::string::npos);
	}
	{
		std::ifstream File(TestSourcePath("src/game/client/components/section_loader.cpp"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();

		EXPECT_NE(Source.find("void CSectionLoader::InvalidateCache(ESettingsCacheDirtyReason Reason)"), std::string::npos);
		EXPECT_EQ(Source.find("(void)Reason;"), std::string::npos);
		EXPECT_NE(Source.find("m_LastDirtyReason = Reason;"), std::string::npos);
		EXPECT_NE(Source.find("Section.m_Dirty = true;"), std::string::npos);
		EXPECT_NE(Source.find("event=section_loader sections_total=%d sections_visible=%d sections_skipped=%d layout_dirty=%d dirty_reason=%s"), std::string::npos);
		EXPECT_NE(Source.find("SettingsCacheDirtyReasonName(m_LastFrameStats.m_DirtyReason)"), std::string::npos);
		EXPECT_EQ(Source.find("*pDirtyReason = Section.m_DirtyReason"), std::string::npos);
	}
	{
		std::ifstream File(TestSourcePath("src/game/client/components/menus_demo.cpp"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();

		EXPECT_NE(Source.find("event=list_frame page=demo_browser items_total=%d rows_visible=%d rows_processed=%d rows_skipped=%d dur_ms=%.3f"), std::string::npos);
		EXPECT_NE(Source.find("const double ListFrameDurationMs = ListFrameTimer.ElapsedMs();"), std::string::npos);
		EXPECT_NE(Source.find("ListFrameDurationMs >= QmPerfThresholdMs()"), std::string::npos);
		EXPECT_NE(Source.find("ListFrameDurationMs);"), std::string::npos);
	}
	{
		std::ifstream File(TestSourcePath("src/game/client/components/menus_browser.cpp"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();

		EXPECT_NE(Source.find("event=list_frame page=server_browser items_total=%d rows_visible=%d rows_rendered=%d rows_iterated=%d rows_skipped=%d dur_ms=%.3f source=server_browser"), std::string::npos);
		EXPECT_NE(Source.find("QmPerfShouldLogDuration(ListFrameDurationMs, false)"), std::string::npos);
		EXPECT_NE(Source.find("const bool PerfListFrameEnabled = QmPerfEnabled();"), std::string::npos);
		EXPECT_NE(Source.find("RowsIterated += PerfListFrameEnabled ? 1 : 0;"), std::string::npos);
	}
	{
		std::ifstream File(TestSourcePath("src/game/client/components/menus_settings.cpp"));
		ASSERT_TRUE(File.good());
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		const std::string Source = Buffer.str();

		EXPECT_NE(Source.find("event=work_drain page=settings:tee kind=merge count=%llu bytes=%d dur_ms=%.3f stop=%s source=list_drain_summary scope=session"), std::string::npos);
	}
}

TEST(QmMonitoringHelpers, VulkanStandardLinePipelineCreatesTexturedVariant)
{
	std::ifstream File(TestSourcePath("src/engine/client/backend/vulkan/backend_vulkan.cpp"));
	ASSERT_TRUE(File.good());
	std::stringstream Buffer;
	Buffer << File.rdbuf();
	const std::string Source = Buffer.str();

	EXPECT_NE(Source.find("SPipelineContainer m_StandardLinePipeline;"), std::string::npos);
	EXPECT_NE(Source.find("GetStandardPipe(IsLineGeometry, IsTextured, BlendModeIndex, DynamicIndex)"), std::string::npos);
	EXPECT_NE(Source.find("GetStandardPipeLayout(IsLineGeometry, IsTextured, BlendModeIndex, DynamicIndex)"), std::string::npos);
	EXPECT_NE(Source.find("if(!CreateStandardGraphicsPipeline(\"shader/vulkan/prim.vert.spv\", \"shader/vulkan/prim.frag.spv\", false, true))"), std::string::npos);
	EXPECT_NE(Source.find("if(!CreateStandardGraphicsPipeline(\"shader/vulkan/prim_textured.vert.spv\", \"shader/vulkan/prim_textured.frag.spv\", true, true))"), std::string::npos);
}

TEST(QmMonitoringHelpers, WindowsReleaseBuildProducesPdbSymbols)
{
	const std::string Source = ReadRepoFile("CMakeLists.txt");

	EXPECT_NE(Source.find("QM_MSVC_USE_EMBEDDED_DEBUG_INFO"), std::string::npos);
	EXPECT_NE(Source.find("$<$<CONFIG:Debug,Release,RelWithDebInfo>:Embedded>"), std::string::npos);
	EXPECT_NE(Source.find("$<$<CONFIG:Release,RelWithDebInfo>:ProgramDatabase>"), std::string::npos);
	EXPECT_NE(Source.find("$<$<CONFIG:Release,RelWithDebInfo>:/DEBUG>"), std::string::npos);
	EXPECT_NE(Source.find("$<$<CONFIG:Release,RelWithDebInfo>:/OPT:REF>"), std::string::npos);
	EXPECT_NE(Source.find("$<$<CONFIG:Release,RelWithDebInfo>:/OPT:ICF>"), std::string::npos);
}

// Phase A 阶段 1: IFrameScheduler service 抽出。独立 service 持有 per-consumer state，
// 让 scheduler 从 menus-private 升级为全局 service。本测试锁定 service 接口契约。
TEST(QmMonitoringHelpers, FrameSchedulerServiceExposesConsumerScopedInterface)
{
	const std::string Header = ReadRepoFile("src/game/client/frame_scheduler.h");
	const std::string Source = ReadRepoFile("src/game/client/frame_scheduler.cpp");
	const std::string Client = ReadRepoFile("src/engine/client/client.cpp");
	const std::string Cmake = ReadRepoFile("CMakeLists.txt");

	ASSERT_FALSE(Header.empty());
	ASSERT_FALSE(Source.empty());

	EXPECT_NE(Header.find("class IFrameScheduler : public IInterface"), std::string::npos);
	EXPECT_NE(Header.find("MACRO_INTERFACE(\"frame_scheduler\")"), std::string::npos);
	EXPECT_NE(Header.find("enum class EFrameSchedulerConsumer"), std::string::npos);
	EXPECT_NE(Header.find("SettingsText"), std::string::npos);
	EXPECT_NE(Header.find("IngameText"), std::string::npos);
	EXPECT_NE(Header.find("Assets"), std::string::npos);
	EXPECT_NE(Header.find("DemoBrowser"), std::string::npos);
	EXPECT_NE(Header.find("IngameServerInfo"), std::string::npos);
	EXPECT_NE(Header.find("Count"), std::string::npos);
	EXPECT_NE(Header.find("ComputeBudget"), std::string::npos);
	EXPECT_NE(Header.find("Reset()"), std::string::npos);
	EXPECT_NE(Header.find("BeginFrame"), std::string::npos);
	EXPECT_NE(Header.find("EndFrame"), std::string::npos);
	EXPECT_NE(Header.find("CreateFrameScheduler"), std::string::npos);

	EXPECT_NE(Source.find("SettingsAdaptiveBudgetStep(Input, m_aState"), std::string::npos);
	EXPECT_NE(Source.find("IFrameScheduler *CreateFrameScheduler()"), std::string::npos);

	EXPECT_NE(Client.find("#include <game/client/frame_scheduler.h>"), std::string::npos);
	EXPECT_NE(Client.find("IFrameScheduler *pFrameScheduler = CreateFrameScheduler();"), std::string::npos);
	EXPECT_NE(Client.find("pKernel->RegisterInterface(pFrameScheduler)"), std::string::npos);

	// CMakeLists.txt 必须显式列出 frame_scheduler.cpp 才会被纳入 GAME_CLIENT 目标；
	// set_src(GAME_CLIENT GLOB_RECURSE ...) 仅用 GLOB 校验与磁盘文件对齐，
	// 真正参与编译的是 ${ARGN} 显式列表（见 CMakeLists.txt set_glob 函数）。
	EXPECT_NE(Cmake.find("frame_scheduler.cpp"), std::string::npos);

	EXPECT_NE(Header.find("#include <game/client/components/settings_resource_jobs.h>"), std::string::npos);
}

// Phase A 阶段 2: CGameClient::OnRender 在帧入口/出口调用 IFrameScheduler 的 BeginFrame/EndFrame。
// 这是阶段 3 同步渲染路径消费 token 的前置：所有同步 UI 都在 frame scope 内执行。
TEST(QmMonitoringHelpers, OnRenderHooksFrameSchedulerBeginAndEndFrame)
{
	const std::string GameClientHeader = ReadRepoFile("src/game/client/gameclient.h");
	const std::string GameClientSource = ReadRepoFile("src/game/client/gameclient.cpp");

	ASSERT_FALSE(GameClientHeader.empty());
	ASSERT_FALSE(GameClientSource.empty());

	EXPECT_NE(GameClientHeader.find("class IFrameScheduler *m_pFrameScheduler;"), std::string::npos);

	EXPECT_NE(GameClientSource.find("#include <game/client/frame_scheduler.h>"), std::string::npos);
	EXPECT_NE(GameClientSource.find("m_pFrameScheduler = Kernel()->RequestInterface<IFrameScheduler>();"), std::string::npos);
	EXPECT_NE(GameClientSource.find("m_pFrameScheduler->BeginFrame(Client()->PerfFrame());"), std::string::npos);
	EXPECT_NE(GameClientSource.find("m_pFrameScheduler->EndFrame();"), std::string::npos);
}

// Phase A 阶段 3 前置：把文档测试改成运行时行为测试。
// service 真正被调用并产出非平凡 token；per-consumer state 互相独立；
// LastOutput 反映最近一次 ComputeBudget 的结果。
TEST(QmMonitoringHelpers, FrameSchedulerServiceProducesRealTokensAndIsolatesConsumers)
{
	std::unique_ptr<IFrameScheduler, void (*)(IFrameScheduler *)> Scheduler(CreateFrameScheduler(), [](IFrameScheduler *p) { delete p; });
	ASSERT_NE(Scheduler, nullptr);

	SSettingsAdaptiveBudgetInput Input;
	Input.m_WindowActive = true;
	Input.m_TargetFrameMs = 8.333f;
	Input.m_FrameMsAverage = 5.0f;
	Input.m_FrameMsP95 = 6.0f;

	const SSettingsAdaptiveBudgetOutput IngameOutput = Scheduler->ComputeBudget(EFrameSchedulerConsumer::IngameServerInfo, Input);
	EXPECT_GE(IngameOutput.m_VisibleTokens, 1);
	EXPECT_GE(IngameOutput.m_TextContainerTokens, 1);
	EXPECT_EQ(IngameOutput.m_Mode, ESettingsAdaptiveBudgetMode::IDLE);

	const SSettingsAdaptiveBudgetOutput &IngameLast = Scheduler->LastOutput(EFrameSchedulerConsumer::IngameServerInfo);
	EXPECT_EQ(IngameLast.m_VisibleTokens, IngameOutput.m_VisibleTokens);
	EXPECT_EQ(IngameLast.m_TextContainerTokens, IngameOutput.m_TextContainerTokens);

	const SSettingsAdaptiveBudgetOutput SettingsLast = Scheduler->LastOutput(EFrameSchedulerConsumer::SettingsText);
	EXPECT_EQ(SettingsLast.m_VisibleTokens, 0);
	EXPECT_EQ(SettingsLast.m_TextContainerTokens, 0);

	Input.m_FrameMsAverage = 30.0f;
	Input.m_FrameMsP95 = 40.0f;
	const SSettingsAdaptiveBudgetOutput PressuredOutput = Scheduler->ComputeBudget(EFrameSchedulerConsumer::IngameServerInfo, Input);
	EXPECT_EQ(PressuredOutput.m_Mode, ESettingsAdaptiveBudgetMode::FRAME_PRESSURE);
}

TEST(QmMonitoringHelpers, FrameSchedulerResetClearsConsumerStateAndFrameScope)
{
	std::unique_ptr<IFrameScheduler, void (*)(IFrameScheduler *)> Scheduler(CreateFrameScheduler(), [](IFrameScheduler *p) { delete p; });
	ASSERT_NE(Scheduler, nullptr);

	Scheduler->BeginFrame(42);

	SSettingsAdaptiveBudgetInput Input;
	Input.m_WindowActive = true;
	Input.m_TargetFrameMs = 8.333f;
	Input.m_FrameMsAverage = 5.0f;
	Input.m_FrameMsP95 = 6.0f;

	const SSettingsAdaptiveBudgetOutput Output = Scheduler->ComputeBudget(EFrameSchedulerConsumer::IngameServerInfo, Input);
	ASSERT_GT(Output.m_TextContainerTokens, 0);
	ASSERT_GT(Scheduler->LastOutput(EFrameSchedulerConsumer::IngameServerInfo).m_TextContainerTokens, 0);
	ASSERT_EQ(Scheduler->CurrentFrameId(), 42);

	Scheduler->Reset();

	EXPECT_EQ(Scheduler->CurrentFrameId(), 0);
	for(size_t i = 0; i < FRAME_SCHEDULER_CONSUMER_COUNT; ++i)
	{
		const EFrameSchedulerConsumer Consumer = static_cast<EFrameSchedulerConsumer>(i);
		EXPECT_FALSE(Scheduler->State(Consumer).m_Initialized);
		EXPECT_EQ(Scheduler->State(Consumer).m_HealthyFrames, 0);
		EXPECT_EQ(Scheduler->State(Consumer).m_BackgroundWindow, 1);
		EXPECT_EQ(Scheduler->LastOutput(Consumer).m_VisibleTokens, 0);
		EXPECT_EQ(Scheduler->LastOutput(Consumer).m_TextContainerTokens, 0);
	}
}
