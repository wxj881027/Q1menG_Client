#include <gtest/gtest.h>
#include <test/test.h>

#include <algorithm>
#include <string>

namespace
{

	std::string ReadTextFile(const char *pPath)
	{
		std::string Content = ReadTestSourceFile(pPath);
		// menus_settings.cpp ships with CRLF line terminators; normalize so the
		// multi-line BlockBodyAfter anchors below match regardless of source EOL.
		Content.erase(std::remove(Content.begin(), Content.end(), '\r'), Content.end());
		return Content;
	}

	std::string FunctionBody(const std::string &Source, const std::string &Signature)
	{
		const size_t FunctionStart = Source.find(Signature);
		EXPECT_NE(FunctionStart, std::string::npos) << Signature;
		const size_t BodyStart = Source.find("{", FunctionStart);
		EXPECT_NE(BodyStart, std::string::npos) << Signature;
		int Depth = 0;
		for(size_t Index = BodyStart; Index < Source.size(); ++Index)
		{
			if(Source[Index] == '{')
				++Depth;
			else if(Source[Index] == '}')
			{
				--Depth;
				if(Depth == 0)
					return Source.substr(BodyStart, Index - BodyStart);
			}
		}
		ADD_FAILURE() << Signature;
		return {};
	}

	std::string BlockBodyAfter(const std::string &Source, const std::string &Anchor)
	{
		const size_t AnchorPos = Source.find(Anchor);
		EXPECT_NE(AnchorPos, std::string::npos) << Anchor;
		const size_t BodyStart = Source.find("{", AnchorPos);
		EXPECT_NE(BodyStart, std::string::npos) << Anchor;
		int Depth = 0;
		for(size_t Index = BodyStart; Index < Source.size(); ++Index)
		{
			if(Source[Index] == '{')
				++Depth;
			else if(Source[Index] == '}')
			{
				--Depth;
				if(Depth == 0)
					return Source.substr(BodyStart, Index - BodyStart);
			}
		}
		ADD_FAILURE() << Anchor;
		return {};
	}

	size_t MatchingBrace(const std::string &Source, size_t BodyStart)
	{
		int Depth = 0;
		for(size_t Index = BodyStart; Index < Source.size(); ++Index)
		{
			if(Source[Index] == '{')
				++Depth;
			else if(Source[Index] == '}')
			{
				--Depth;
				if(Depth == 0)
					return Index;
			}
		}
		return std::string::npos;
	}

} // namespace

TEST(QmNewUiMenuBranches, MenubarUsesExplicitQmNewUiColorBranch)
{
	const std::string Source = ReadTextFile("src/game/client/components/menus.cpp");
	const std::string DoMenuTabV2 = FunctionBody(Source, "int CMenus::DoMenuTabV2(");
	const std::string RenderMenubar = FunctionBody(Source, "void CMenus::RenderMenubar(");
	const size_t UseNewUiIfPos = RenderMenubar.find("if(UseNewUi)");
	ASSERT_NE(UseNewUiIfPos, std::string::npos);
	const size_t UseNewUiBodyStart = RenderMenubar.find("{", UseNewUiIfPos);
	ASSERT_NE(UseNewUiBodyStart, std::string::npos);
	const size_t UseNewUiBodyEnd = MatchingBrace(RenderMenubar, UseNewUiBodyStart);
	ASSERT_NE(UseNewUiBodyEnd, std::string::npos);
	const std::string UseNewUiBlock = RenderMenubar.substr(UseNewUiBodyStart, UseNewUiBodyEnd - UseNewUiBodyStart);
	const size_t OldUiElsePos = RenderMenubar.find("else", UseNewUiBodyEnd);
	ASSERT_NE(OldUiElsePos, std::string::npos);
	const size_t OldUiBodyStart = RenderMenubar.find("{", OldUiElsePos);
	ASSERT_NE(OldUiBodyStart, std::string::npos);
	const size_t OldUiBodyEnd = MatchingBrace(RenderMenubar, OldUiBodyStart);
	ASSERT_NE(OldUiBodyEnd, std::string::npos);
	const std::string OldUiBlock = RenderMenubar.substr(OldUiBodyStart, OldUiBodyEnd - OldUiBodyStart);
	const size_t HoverBranch = DoMenuTabV2.find("if(Hover)");
	const size_t ActiveBranch = DoMenuTabV2.find("else if(Active)");

	EXPECT_NE(Source.find("const bool UseNewUi = g_Config.m_QmNewUi != 0;"), std::string::npos);
	EXPECT_NE(Source.find("MenuTabDefaultColor("), std::string::npos);
	EXPECT_NE(Source.find("MenuTabActiveColor("), std::string::npos);
	EXPECT_NE(Source.find("MenuTabHoverColor("), std::string::npos);
	EXPECT_NE(Source.find("MenuIconButtonDefaultColor("), std::string::npos);
	ASSERT_NE(HoverBranch, std::string::npos);
	ASSERT_NE(ActiveBranch, std::string::npos);
	EXPECT_LT(HoverBranch, ActiveBranch);
	EXPECT_NE(DoMenuTabV2.find("Target = pCustomHover != nullptr ? *pCustomHover : HoverColor;"), std::string::npos);
	EXPECT_NE(DoMenuTabV2.find("Target = pCustomActive != nullptr ? *pCustomActive : ActiveColor;"), std::string::npos);
	EXPECT_NE(Source.find("return UseNewUi ? MenuTabDefaultColor() : ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f);"), std::string::npos);
	EXPECT_NE(Source.find("const ColorRGBA DefaultColor = UseNewUi ? MenuTabDefaultColor() : ms_ColorTabbarInactive;"), std::string::npos);
	EXPECT_NE(Source.find("const ColorRGBA ActiveColor = UseNewUi ? MenuTabActiveColor() : ms_ColorTabbarActive;"), std::string::npos);
	EXPECT_NE(Source.find("const ColorRGBA HoverColor = UseNewUi ? MenuTabHoverColor() : ms_ColorTabbarHover;"), std::string::npos);
	EXPECT_NE(DoMenuTabV2.find("pRect->Draw(Resolved, Corners, UseNewUi ? 7.0f : 10.0f);"), std::string::npos);
	EXPECT_NE(DoMenuTabV2.find("const float LabelFontSize = UseNewUi ? minimum(Label.h * CUi::ms_FontmodHeight, 13.0f) : Label.h * CUi::ms_FontmodHeight;"), std::string::npos);
	EXPECT_NE(DoMenuTabV2.find("Ui()->DoLabel(&Label, pText, LabelFontSize, TEXTALIGN_MC);"), std::string::npos);
	EXPECT_NE(Source.find("const bool UseNewUi = g_Config.m_QmNewUi != 0;"), std::string::npos);
	EXPECT_NE(Source.find("ColorRGBA InactiveColor = MenuTabDefaultColor();"), std::string::npos);
	EXPECT_NE(Source.find("ColorRGBA ActiveColor = MenuTabActiveColor();"), std::string::npos);
	EXPECT_NE(Source.find("ColorRGBA HoverColor = MenuMenubarHoverColor();"), std::string::npos);
	EXPECT_NE(Source.find("ColorRGBA InactiveColor = ms_ColorTabbarInactive;"), std::string::npos);
	EXPECT_NE(Source.find("ColorRGBA ActiveColor = ms_ColorTabbarActive;"), std::string::npos);
	EXPECT_NE(Source.find("ColorRGBA HoverColor = ms_ColorTabbarHover;"), std::string::npos);
	EXPECT_NE(Source.find("const ColorRGBA IndicatorColor = g_Config.m_QmNewUi != 0 ? MenuUiColorAccent(1.0f) : ui_token::color::ACCENT_PRIMARY;"), std::string::npos);
	EXPECT_NE(RenderMenubar.find("if(!UseNewUi && MenubarHaveActive)"), std::string::npos);
	EXPECT_NE(RenderMenubar.find("if(UseNewUi)"), std::string::npos);
	EXPECT_NE(UseNewUiBlock.find("Box.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.12f)"), std::string::npos);
	EXPECT_NE(UseNewUiBlock.find("Box.VMargin(MenubarOuterInsetX, &Box);"), std::string::npos);
	EXPECT_NE(UseNewUiBlock.find("Box.HMargin(MenubarOuterInsetY, &Box);"), std::string::npos);
	EXPECT_NE(UseNewUiBlock.find("const float BrowserButtonWidth = 58.0f;"), std::string::npos);
	EXPECT_NE(UseNewUiBlock.find("const float GameButtonWidth = CompactOnlineMenuTabs ? 56.0f : 64.0f;"), std::string::npos);
	EXPECT_NE(UseNewUiBlock.find("const float ServerInfoButtonWidth = CompactOnlineMenuTabs ? 94.0f : 104.0f;"), std::string::npos);
	EXPECT_NE(UseNewUiBlock.find("const float OnlineTabGap = 4.0f;"), std::string::npos);
	EXPECT_NE(UseNewUiBlock.find("if(DoMenuTabV2(&s_SettingsButton"), std::string::npos);
	EXPECT_NE(UseNewUiBlock.find("if(DoMenuTabV2(&s_InternetButton"), std::string::npos);
	EXPECT_EQ(UseNewUiBlock.find("DoButton_MenuTab(&s_SettingsButton"), std::string::npos);
	EXPECT_EQ(UseNewUiBlock.find("DoButton_MenuTab(&s_InternetButton"), std::string::npos);
	EXPECT_EQ(OldUiBlock.find("Box.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.12f)"), std::string::npos);
	EXPECT_EQ(OldUiBlock.find("Box.VMargin(MenubarOuterInsetX, &Box);"), std::string::npos);
	EXPECT_EQ(OldUiBlock.find("Box.HMargin(MenubarOuterInsetY, &Box);"), std::string::npos);
	EXPECT_NE(OldUiBlock.find("if(DoButton_MenuTab(&s_SettingsButton"), std::string::npos);
	EXPECT_NE(OldUiBlock.find("if(DoButton_MenuTab(&s_InternetButton"), std::string::npos);
	EXPECT_EQ(OldUiBlock.find("DoMenuTabV2(&s_SettingsButton"), std::string::npos);
	EXPECT_EQ(OldUiBlock.find("DoMenuTabV2(&s_InternetButton"), std::string::npos);
}

TEST(QmNewUiMenuBranches, BrowserUsesExplicitQmNewUiShellBranch)
{
	const std::string Source = ReadTextFile("src/game/client/components/menus_browser.cpp");
	const std::string RenderServerbrowser = FunctionBody(Source, "void CMenus::RenderServerbrowser(");
	const size_t TopUseNewUiIfPos = RenderServerbrowser.find("if(UseNewUi)\n\t\tView.Margin(6.0f, &View);");
	ASSERT_NE(TopUseNewUiIfPos, std::string::npos);
	const size_t TopOldUiElsePos = RenderServerbrowser.find("else\n\t{", TopUseNewUiIfPos);
	ASSERT_NE(TopOldUiElsePos, std::string::npos);
	const size_t TopOldUiBodyStart = RenderServerbrowser.find("{", TopOldUiElsePos);
	ASSERT_NE(TopOldUiBodyStart, std::string::npos);
	const size_t TopOldUiBodyEnd = MatchingBrace(RenderServerbrowser, TopOldUiBodyStart);
	ASSERT_NE(TopOldUiBodyEnd, std::string::npos);
	const std::string TopOldUiBlock = RenderServerbrowser.substr(TopOldUiBodyStart, TopOldUiBodyEnd - TopOldUiBodyStart);

	const size_t UseNewUiIfPos = RenderServerbrowser.find("if(UseNewUi)", TopOldUiBodyEnd);
	ASSERT_NE(UseNewUiIfPos, std::string::npos);
	const size_t UseNewUiBodyStart = RenderServerbrowser.find("{", UseNewUiIfPos);
	ASSERT_NE(UseNewUiBodyStart, std::string::npos);
	const size_t UseNewUiBodyEnd = MatchingBrace(RenderServerbrowser, UseNewUiBodyStart);
	ASSERT_NE(UseNewUiBodyEnd, std::string::npos);
	const size_t OldUiElsePos = RenderServerbrowser.find("else", UseNewUiBodyEnd);
	ASSERT_NE(OldUiElsePos, std::string::npos);
	const size_t OldUiBodyStart = RenderServerbrowser.find("{", OldUiElsePos);
	ASSERT_NE(OldUiBodyStart, std::string::npos);
	const size_t OldUiBodyEnd = MatchingBrace(RenderServerbrowser, OldUiBodyStart);
	ASSERT_NE(OldUiBodyEnd, std::string::npos);
	const std::string OldUiBlock = RenderServerbrowser.substr(OldUiBodyStart, OldUiBodyEnd - OldUiBodyStart);

	EXPECT_NE(Source.find("const bool UseNewUi = g_Config.m_QmNewUi != 0;"), std::string::npos);
	EXPECT_NE(Source.find("if(UseNewUi)"), std::string::npos);
	EXPECT_NE(Source.find("ServerListBase.Draw(BrowserPanelColor()"), std::string::npos);
	EXPECT_NE(Source.find("(void)DrawBackground;"), std::string::npos);
	EXPECT_NE(Source.find("const float ToolBoxWidth = UseNewUi ? 205.0f : 188.0f;"), std::string::npos);
	EXPECT_NE(Source.find("const float ColumnGap = UseNewUi ? 10.0f : 6.0f;"), std::string::npos);
	EXPECT_NE(Source.find("const float StatusHeight = UseNewUi ? 84.0f : 76.0f;"), std::string::npos);
	EXPECT_NE(Source.find("CUIRect ServerListStackBase = ServerListBase;"), std::string::npos);
	EXPECT_NE(Source.find("ServerListStackBase.HSplitBottom(StatusHeight, &ServerListBase, &StatusBox);"), std::string::npos);
	EXPECT_NE(Source.find("StatusBox.y = ServerListStackBase.y + ServerListStackBase.h - StatusHeight;"), std::string::npos);
	EXPECT_NE(Source.find("ServerListBase.h = maximum(StatusBox.y - ColumnGap - ServerListBase.y, 0.0f);"), std::string::npos);
	EXPECT_EQ(Source.find("ServerListBase.HSplitBottom(ColumnGap, &ServerListBase, nullptr);"), std::string::npos);
	EXPECT_NE(Source.find("ServerListBase.Margin(std::clamp(ServerListBase.w * 0.006f, 1.0f, 4.0f), &ServerListBase);"), std::string::npos);
	EXPECT_NE(TopOldUiBlock.find("View.Draw(ms_ColorTabbarActive, IGraphics::CORNER_B, 10.0f);"), std::string::npos);
	EXPECT_NE(TopOldUiBlock.find("View.Margin(10.0f, &View);"), std::string::npos);
	EXPECT_EQ(TopOldUiBlock.find("View.Margin(std::clamp(View.w * 0.008f, 4.0f, 8.0f), &View);"), std::string::npos);
}

TEST(QmNewUiMenuBranches, BrowserInteriorBackgroundsUseMapBrowserOpacity)
{
	const std::string Source = ReadTextFile("src/game/client/components/menus_browser.cpp");

	EXPECT_NE(Source.find("g_Config.m_QmMapBrowserOpacity / 100.0f"), std::string::npos);
	EXPECT_NE(Source.find("Headers.Draw(BrowserOpacityColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f))"), std::string::npos);
	EXPECT_NE(Source.find("View.Draw(BrowserOpacityColor(ColorRGBA(0.0f, 0.0f, 0.0f, 0.15f))"), std::string::npos);
	EXPECT_NE(Source.find("Panel.Draw(BrowserOpacityColor(ColorRGBA(0.0f, 0.0f, 0.0f, 0.22f))"), std::string::npos);
	EXPECT_NE(Source.find("Tab.Draw(BrowserOpacityColor(ColorRGBA(0.0f, 0.0f, 0.0f, 0.3f))"), std::string::npos);
	EXPECT_EQ(Source.find("BrowserOpacityColor(ColorRGBA(0.0f, 0.0f, 0.3f))"), std::string::npos);
}

TEST(QmNewUiMenuBranches, DemoBrowserUsesExplicitLegacyShellBranches)
{
	const std::string Source = ReadTextFile("src/game/client/components/menus_demo.cpp");
	const std::string RenderDemoBrowser = FunctionBody(Source, "void CMenus::RenderDemoBrowser(CUIRect MainView)");
	const std::string RenderDemoBrowserList = FunctionBody(Source, "void CMenus::RenderDemoBrowserList(CUIRect ListView, bool &WasListboxItemActivated)");
	const std::string RenderDemoBrowserDetails = FunctionBody(Source, "void CMenus::RenderDemoBrowserDetails(CUIRect DetailsView)");
	const std::string RenderDemoBrowserButtons = FunctionBody(Source, "void CMenus::RenderDemoBrowserButtons(CUIRect ButtonsView, bool WasListboxItemActivated)");
	const size_t UseNewUiButtonsPos = RenderDemoBrowserButtons.find("if(UseNewUi)");
	ASSERT_NE(UseNewUiButtonsPos, std::string::npos);
	const size_t UseNewUiButtonsBodyStart = RenderDemoBrowserButtons.find("{", UseNewUiButtonsPos);
	ASSERT_NE(UseNewUiButtonsBodyStart, std::string::npos);
	const size_t UseNewUiButtonsBodyEnd = MatchingBrace(RenderDemoBrowserButtons, UseNewUiButtonsBodyStart);
	ASSERT_NE(UseNewUiButtonsBodyEnd, std::string::npos);
	const std::string UseNewUiButtonsBranch = RenderDemoBrowserButtons.substr(UseNewUiButtonsBodyStart, UseNewUiButtonsBodyEnd - UseNewUiButtonsBodyStart);
	const size_t LegacyButtonsElsePos = RenderDemoBrowserButtons.find("CUIRect ButtonBarTop, ButtonBarBottom;", UseNewUiButtonsBodyEnd);
	ASSERT_NE(LegacyButtonsElsePos, std::string::npos);
	const std::string LegacyButtonsBranch = RenderDemoBrowserButtons.substr(LegacyButtonsElsePos);

	EXPECT_NE(Source.find("const bool UseNewUi = g_Config.m_QmNewUi != 0;"), std::string::npos);
	EXPECT_NE(RenderDemoBrowser.find("if(UseNewUi)"), std::string::npos);
	EXPECT_NE(RenderDemoBrowser.find("MainView.Draw(ms_ColorTabbarActive, IGraphics::CORNER_B, 10.0f);"), std::string::npos);
	EXPECT_NE(RenderDemoBrowser.find("MainView.Margin(10.0f, &MainView);"), std::string::npos);
	EXPECT_NE(RenderDemoBrowser.find("MainView.HSplitBottom(44.0f, &ListView, &ButtonsView);"), std::string::npos);
	EXPECT_NE(RenderDemoBrowser.find("MainView.HSplitBottom(22.0f * 2.0f + 5.0f, &ListView, &ButtonsView);"), std::string::npos);
	EXPECT_EQ(RenderDemoBrowser.find("MainView.HSplitBottom(22.0f * 2.0f + 10.0f, &ListView, &ButtonsView);"), std::string::npos);
	EXPECT_NE(RenderDemoBrowserList.find("Headers.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f), IGraphics::CORNER_T, 5.0f);"), std::string::npos);
	EXPECT_NE(RenderDemoBrowserList.find("ListBox.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.15f), IGraphics::CORNER_B, 5.0f);"), std::string::npos);
	EXPECT_NE(RenderDemoBrowserList.find("const float HeaderGap = UseNewUi ? 4.0f : 2.0f;"), std::string::npos);
	EXPECT_NE(RenderDemoBrowserList.find("const float RowHeight = UseNewUi ? ms_ListheaderHeight + 1.0f : ms_ListheaderHeight;"), std::string::npos);
	EXPECT_NE(RenderDemoBrowserList.find("CColumn aCols[] = {"), std::string::npos);
	EXPECT_EQ(RenderDemoBrowserList.find("static CColumn s_aCols[] = {"), std::string::npos);
	EXPECT_NE(RenderDemoBrowserList.find("{COL_MARKERS, SORT_MARKERS, FONT_ICON_BOOKMARK, 1, true, UseNewUi ? 34.0f : 30.0f, {0}, Localizable(\"Markers\")}"), std::string::npos);
	EXPECT_NE(RenderDemoBrowserList.find("{COL_LENGTH, SORT_LENGTH, Localizable(\"Length\"), 1, false, UseNewUi ? 84.0f : 75.0f, {0}, nullptr}"), std::string::npos);
	EXPECT_NE(RenderDemoBrowserList.find("{COL_DATE, SORT_DATE, Localizable(\"Date\"), 1, false, UseNewUi ? 156.0f : 150.0f, {0}, nullptr}"), std::string::npos);
	EXPECT_NE(RenderDemoBrowserList.find("aCols[9].m_Width = BrowsingScreenshots ? (UseNewUi ? 176.0f : 170.0f) : (UseNewUi ? 156.0f : 150.0f);"), std::string::npos);
	EXPECT_NE(RenderDemoBrowserList.find("s_ListBox.DoStart(UseNewUi ? RowHeight : ms_ListheaderHeight, m_vpFilteredDemos.size(), 1, 3, m_DemolistSelectedIndex, &ListBox, false, IGraphics::CORNER_ALL, true);"), std::string::npos);
	EXPECT_NE(RenderDemoBrowserDetails.find("Header.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f), IGraphics::CORNER_T, 5.0f);"), std::string::npos);
	EXPECT_NE(RenderDemoBrowserDetails.find("Contents.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.15f), IGraphics::CORNER_B, 5.0f);"), std::string::npos);
	EXPECT_NE(RenderDemoBrowserDetails.find("Contents.Margin(5.0f, &Contents);"), std::string::npos);
	EXPECT_NE(RenderDemoBrowserButtons.find("if(UseNewUi)"), std::string::npos);
	EXPECT_NE(UseNewUiButtonsBranch.find("CUIRect MainRow = ButtonsView;"), std::string::npos);
	EXPECT_NE(UseNewUiButtonsBranch.find("const float ButtonWidth = MainRow.h * 1.55f;"), std::string::npos);
	EXPECT_NE(UseNewUiButtonsBranch.find("const float RowHeight = minimum(22.0f, ButtonsView.h);"), std::string::npos);
	EXPECT_NE(UseNewUiButtonsBranch.find("ButtonsView.HSplitTop(3.0f, nullptr, &ButtonsView);"), std::string::npos);
	EXPECT_NE(UseNewUiButtonsBranch.find("ButtonsView.HSplitBottom(3.0f, &ButtonsView, nullptr);"), std::string::npos);
	EXPECT_NE(LegacyButtonsBranch.find("ButtonsView.HSplitMid(&ButtonBarTop, &ButtonBarBottom, 5.0f);"), std::string::npos);
	EXPECT_EQ(RenderDemoBrowser.find("MainView.Draw(MenuPanelColor()"), std::string::npos);
}

TEST(QmNewUiMenuBranches, BrowserFavoriteMapsEarlyReturnAvoidsLegacyDoubleInset)
{
	const std::string Source = ReadTextFile("src/game/client/components/menus_browser.cpp");
	const std::string RenderServerbrowser = FunctionBody(Source, "void CMenus::RenderServerbrowser(");
	const size_t FavoriteMapsPos = RenderServerbrowser.find("if(g_Config.m_UiPage == PAGE_FAVORITE_MAPS)");
	ASSERT_NE(FavoriteMapsPos, std::string::npos);
	const size_t DrawPos = RenderServerbrowser.find("View.Draw(ms_ColorTabbarActive, IGraphics::CORNER_B, 10.0f);");
	ASSERT_NE(DrawPos, std::string::npos);
	EXPECT_LT(FavoriteMapsPos, DrawPos);
	EXPECT_NE(RenderServerbrowser.find("RenderServerbrowserFavoriteMaps(MainView);"), std::string::npos);
	EXPECT_NE(RenderServerbrowser.find("View.Margin(6.0f, &View);\n\t\t\tRenderServerbrowserFavoriteMaps(View);"), std::string::npos);
}

TEST(QmNewUiMenuBranches, QmLocalizationEnglishOverlayUsesExplicitEnglishFile)
{
	const std::string Source = ReadTextFile("src/game/client/gameclient.cpp");

	EXPECT_EQ(Source.find("str_format(aBuf, sizeof(aBuf), \"qmclient/%s\", g_Config.m_ClLanguagefile);"), std::string::npos);
	EXPECT_EQ(Source.find("static void LoadQmClientLanguageOverlay("), std::string::npos);
	EXPECT_EQ(Source.find("const char *pQmLanguageFile = g_Config.m_ClLanguagefile[0] != '\\0' ? g_Config.m_ClLanguagefile : \"english.txt\";"), std::string::npos);
	EXPECT_EQ(Source.find("const char *pQmLanguageFile = pLanguageFile[0] != '\\0' ? pLanguageFile : \"english.txt\";"), std::string::npos);
	EXPECT_EQ(Source.find("if(str_comp(pLanguageFile, \"languages/simplified_chinese.txt\") == 0)"), std::string::npos);
	EXPECT_EQ(Source.find("const char *pQmLanguageFile = pLanguageFile[0] != '\\0' ? pLanguageFile : \"languages/english.txt\";"), std::string::npos);
	EXPECT_EQ(Source.find("str_format(aBuf, sizeof(aBuf), \"qmclient/%s\", pQmLanguageFile);"), std::string::npos);
	EXPECT_EQ(Source.find("LoadQmClientLanguageOverlay(g_Localization, g_Config.m_ClLanguagefile, Storage(), Console());"), std::string::npos);
	EXPECT_NE(Source.find("g_Localization.Load(g_Config.m_ClLanguagefile, Storage(), Console());"), std::string::npos);
}

TEST(QmNewUiMenuBranches, QmClientUpdateFlowUsesQmClientNamingAndComparisonHelper)
{
	const std::string TClientSource = ReadTextFile("src/game/client/components/tclient/tclient.cpp");
	const std::string TClientHeader = ReadTextFile("src/game/client/components/tclient/tclient.h");
	const std::string MenusStartSource = ReadTextFile("src/game/client/components/menus_start.cpp");

	EXPECT_NE(TClientSource.find("#include <game/client/components/qmclient/update_version.h>"), std::string::npos);
	EXPECT_NE(TClientSource.find("static constexpr const char *QMCLIENT_INFO_URL"), std::string::npos);
	EXPECT_NE(TClientSource.find("static constexpr const char *QMCLIENT_UPDATE_EXE_URL"), std::string::npos);
	EXPECT_NE(TClientSource.find("FetchQmClientUpdateInfo();"), std::string::npos);
	EXPECT_NE(TClientSource.find("FinishQmClientUpdateInfo();"), std::string::npos);
	EXPECT_NE(TClientSource.find("ResetQmClientUpdateInfoTask();"), std::string::npos);
	EXPECT_NE(TClientSource.find("NeedQmClientUpdate()"), std::string::npos);
	EXPECT_NE(TClientSource.find("RequestQmClientUpdateCheckAndUpdate()"), std::string::npos);
	EXPECT_NE(TClientSource.find("IsQmClientRemoteVersionNewer(pLatestVersion, QMCLIENT_VERSION)"), std::string::npos);
	EXPECT_EQ(TClientSource.find("NeedUpdate()"), std::string::npos);
	EXPECT_EQ(TClientSource.find("FetchTClientInfo()"), std::string::npos);
	EXPECT_EQ(TClientSource.find("FinishTClientInfo()"), std::string::npos);
	EXPECT_EQ(TClientSource.find("ResetTClientInfoTask()"), std::string::npos);
	EXPECT_EQ(TClientSource.find("TCLIENT_INFO_URL"), std::string::npos);
	EXPECT_EQ(TClientSource.find("TCLIENT_UPDATE_EXE_URL"), std::string::npos);

	EXPECT_NE(TClientHeader.find("m_pQmClientUpdateInfoTask"), std::string::npos);
	EXPECT_NE(TClientHeader.find("m_FetchedQmClientUpdateInfo"), std::string::npos);
	EXPECT_NE(TClientHeader.find("m_QmClientAutoUpdateAfterCheck"), std::string::npos);
	EXPECT_NE(TClientHeader.find("m_aQmClientLatestVersionStr"), std::string::npos);
	EXPECT_EQ(TClientHeader.find("m_pTClientInfoTask"), std::string::npos);
	EXPECT_EQ(TClientHeader.find("m_FetchedTClientInfo"), std::string::npos);
	EXPECT_EQ(TClientHeader.find("m_AutoUpdateAfterCheck"), std::string::npos);
	EXPECT_EQ(TClientHeader.find("m_aVersionStr"), std::string::npos);

	EXPECT_NE(MenusStartSource.find("m_FetchedQmClientUpdateInfo"), std::string::npos);
	EXPECT_NE(MenusStartSource.find("NeedQmClientUpdate()"), std::string::npos);
	EXPECT_EQ(MenusStartSource.find("m_FetchedTClientInfo"), std::string::npos);
	EXPECT_EQ(MenusStartSource.find("NeedUpdate()"), std::string::npos);
}

TEST(QmNewUiMenuBranches, StartMenuKeepsExplicitUseV2AndLegacyButtonPaths)
{
	const std::string Source = ReadTextFile("src/game/client/components/menus_start.cpp");
	const std::string RenderStartMenuImpl = FunctionBody(Source, "void CMenusStart::RenderStartMenuImpl(");
	const std::string UseV2Block = BlockBodyAfter(RenderStartMenuImpl, "if(UseV2Layout)");

	EXPECT_NE(Source.find("void CMenusStart::RenderStartMenu(CUIRect MainView)"), std::string::npos);
	EXPECT_NE(Source.find("RenderStartMenuImpl(MainView, false);"), std::string::npos);
	EXPECT_NE(Source.find("void CMenusStart::RenderStartMenuV2(CUIRect MainView)"), std::string::npos);
	EXPECT_NE(Source.find("RenderStartMenuImpl(MainView, true);"), std::string::npos);
	EXPECT_NE(RenderStartMenuImpl.find("if(UseV2Layout)"), std::string::npos);
	EXPECT_NE(UseV2Block.find("ui_widget::PrimaryButton"), std::string::npos);
	EXPECT_NE(UseV2Block.find("ui_widget::SecondaryButton"), std::string::npos);
	EXPECT_EQ(UseV2Block.find("DoButton_Menu("), std::string::npos);
	EXPECT_NE(RenderStartMenuImpl.find("static float s_aMenuButtonScale[MenuButtonCount] = {};"), std::string::npos);
	EXPECT_NE(RenderStartMenuImpl.find("const auto ScaleButtonRect = [](const CUIRect &Base, float Scale) {"), std::string::npos);
	EXPECT_NE(RenderStartMenuImpl.find("GameClient()->m_Menus.DoButton_Menu(&s_QuitButton"), std::string::npos);
	EXPECT_NE(RenderStartMenuImpl.find("GameClient()->m_Menus.DoButton_Menu(&s_PlayButton"), std::string::npos);
}

TEST(QmNewUiMenuBranches, StartMenuEntryKeepsLegacyStartPageWithQmNewUi)
{
	const std::string Source = ReadTextFile("src/game/client/components/menus.cpp");
	EXPECT_NE(Source.find("else if(m_ShowStart)"), std::string::npos);
	const std::string Render = FunctionBody(Source, "void CMenus::Render()");
	const size_t StartMenuPos = Render.find("else if(m_ShowStart)");
	ASSERT_NE(StartMenuPos, std::string::npos);
	const size_t StartMenuBodyStart = Render.find("{", StartMenuPos);
	ASSERT_NE(StartMenuBodyStart, std::string::npos);
	const size_t StartMenuBodyEnd = MatchingBrace(Render, StartMenuBodyStart);
	ASSERT_NE(StartMenuBodyEnd, std::string::npos);
	const std::string StartMenuBlock = Render.substr(StartMenuBodyStart, StartMenuBodyEnd - StartMenuBodyStart);
	EXPECT_NE(StartMenuBlock.find("m_MenusStart.RenderStartMenu(Screen);"), std::string::npos);
	EXPECT_EQ(StartMenuBlock.find("m_MenusStart.RenderStartMenuV2(Screen);"), std::string::npos);
	EXPECT_EQ(StartMenuBlock.find("g_Config.m_QmNewUi"), std::string::npos);
}

TEST(QmNewUiMenuBranches, SettingsShellKeepsExplicitQmNewUiContainerBranch)
{
	const std::string Source = ReadTextFile("src/game/client/components/menus_settings.cpp");
	const std::string RenderSettings = FunctionBody(Source, "void CMenus::RenderSettings(CUIRect MainView)");
	const size_t UseNewSettingsUiIfPos = RenderSettings.find("if(UseNewSettingsUi)");
	ASSERT_NE(UseNewSettingsUiIfPos, std::string::npos);
	const size_t UseNewSettingsUiBodyStart = RenderSettings.find("{", UseNewSettingsUiIfPos);
	ASSERT_NE(UseNewSettingsUiBodyStart, std::string::npos);
	const size_t UseNewSettingsUiBodyEnd = MatchingBrace(RenderSettings, UseNewSettingsUiBodyStart);
	ASSERT_NE(UseNewSettingsUiBodyEnd, std::string::npos);
	const std::string UseNewSettingsUiBlock = RenderSettings.substr(UseNewSettingsUiBodyStart, UseNewSettingsUiBodyEnd - UseNewSettingsUiBodyStart);
	const size_t OldSettingsUiElsePos = RenderSettings.find("else", UseNewSettingsUiBodyEnd);
	ASSERT_NE(OldSettingsUiElsePos, std::string::npos);
	const size_t OldSettingsUiBodyStart = RenderSettings.find("{", OldSettingsUiElsePos);
	ASSERT_NE(OldSettingsUiBodyStart, std::string::npos);
	const size_t OldSettingsUiBodyEnd = MatchingBrace(RenderSettings, OldSettingsUiBodyStart);
	ASSERT_NE(OldSettingsUiBodyEnd, std::string::npos);
	const std::string OldSettingsUiBlock = RenderSettings.substr(OldSettingsUiBodyStart, OldSettingsUiBodyEnd - OldSettingsUiBodyStart);
	const std::string SettingsHeaderBranch = BlockBodyAfter(RenderSettings, "if(UseNewSettingsUi)\n\t{\n\t\tTabBar.Margin(10.0f, &TabBar);");
	const std::string SettingsHeaderLegacyBranch = BlockBodyAfter(RenderSettings, "else\n\t{\n\t\tTabBar.HSplitTop(50.0f, &Button, &TabBar);");

	EXPECT_NE(Source.find("const bool UseNewSettingsUi = g_Config.m_QmNewUi != 0;"), std::string::npos);
	EXPECT_NE(UseNewSettingsUiBlock.find("TabBar.Draw(SettingsTabbarColor()"), std::string::npos);
	EXPECT_NE(UseNewSettingsUiBlock.find("MainView.Draw(MenuPanelColor()"), std::string::npos);
	EXPECT_EQ(UseNewSettingsUiBlock.find("MainView.Draw(ms_ColorTabbarActive"), std::string::npos);
	EXPECT_NE(OldSettingsUiBlock.find("MainView.Draw(ms_ColorTabbarActive"), std::string::npos);
	EXPECT_EQ(OldSettingsUiBlock.find("SettingsTabbarColor()"), std::string::npos);
	EXPECT_EQ(OldSettingsUiBlock.find("MenuPanelColor()"), std::string::npos);
	EXPECT_EQ(SettingsHeaderBranch.find("Button.Draw(ms_ColorTabbarActive"), std::string::npos);
	EXPECT_NE(SettingsHeaderLegacyBranch.find("Button.Draw(ms_ColorTabbarActive"), std::string::npos);
}

TEST(QmNewUiMenuBranches, LegacyMenusKeepTabAndPanelShellConnected)
{
	const std::string MenusSource = ReadTextFile("src/game/client/components/menus.cpp");
	EXPECT_NE(MenusSource.find("const bool UseNewUi = g_Config.m_QmNewUi != 0;\n\t\t\tconst float MenubarHeight = UseNewUi ? 24.0f : 34.0f;\n\t\t\tScreen.HSplitTop(MenubarHeight, &TabBar, &MainView);\n\t\t\tif(UseNewUi)\n\t\t\t\tMainView.HSplitTop(6.0f, nullptr, &MainView);"), std::string::npos);
	EXPECT_NE(MenusSource.find("case IClient::STATE_ONLINE:"), std::string::npos);
	EXPECT_NE(MenusSource.find("const bool UseNewUi = g_Config.m_QmNewUi != 0;\n\t\t\tconst float MenubarHeight = UseNewUi ? 24.0f : 34.0f;\n\t\t\tScreen.HSplitTop(MenubarHeight, &TabBar, &MainView);\n\t\t\tif(UseNewUi)\n\t\t\t\tMainView.HSplitTop(6.0f, nullptr, &MainView);"), std::string::npos);

	const std::string QmClientSource = ReadTextFile("src/game/client/components/qmclient/menus_qmclient.cpp");
	EXPECT_NE(QmClientSource.find("const bool UseNewUi = g_Config.m_QmNewUi != 0;"), std::string::npos);
	EXPECT_NE(QmClientSource.find("if(UseNewUi)\n\t\t\tMainView.HSplitTop(Margin, nullptr, &MainView);"), std::string::npos);
}

TEST(QmNewUiMenuBranches, AssetsPreviewUsesInnerFrameRectForPreviewImage)
{
	const std::string Source = ReadTextFile("src/game/client/components/menus_settings_assets.cpp");

	EXPECT_NE(Source.find("auto DrawPreviewFrame = [&](const CUIRect &TextureRect) -> CUIRect {"), std::string::npos);
	EXPECT_NE(Source.find("PreviewFrame.Margin(3.0f, &PreviewFrame);"), std::string::npos);
	EXPECT_NE(Source.find("return PreviewFrame;"), std::string::npos);
	EXPECT_NE(Source.find("auto ComputeAssetPreviewContentSize = [&](bool WorkshopCard)"), std::string::npos);
	EXPECT_NE(Source.find("CUIRect PreviewFrameRect = DrawPreviewFrame(Shell.m_TextureRect);"), std::string::npos);
	EXPECT_NE(Source.find("const auto [PreviewContentWidth, PreviewContentHeight] = ComputeAssetPreviewContentSize(WorkshopCard);"), std::string::npos);
	EXPECT_NE(Source.find("const auto [PreviewContentWidth, PreviewContentHeight] = ComputeAssetPreviewContentSize(true);"), std::string::npos);
	EXPECT_NE(Source.find("const CUIRect PreviewRect = ComputePreviewDrawRect(PreviewFrameRect, PreviewContentWidth, PreviewContentHeight);"), std::string::npos);
	EXPECT_EQ(Source.find("const CUIRect PreviewRect = ComputePreviewDrawRect(HeaderLayout.m_TextureRect, TextureWidth, TextureHeight);"), std::string::npos);
	EXPECT_EQ(Source.find("const CUIRect PreviewRect = ComputePreviewDrawRect(HeaderLayout.m_TextureRect, TextureWidth, TextureWidth);"), std::string::npos);
}

TEST(QmNewUiMenuBranches, SettingsColorLabelsUseQmLocalizedKeys)
{
	const std::string Source = ReadTextFile("src/game/client/components/menus_settings.cpp");
	const std::string MenusToml = ReadTextFile("qmclient_scripts/languages_qmclient/translations/i18n/menus.toml");

	EXPECT_EQ(Source.find("Localize(\"UI Color\")"), std::string::npos);
	EXPECT_EQ(Source.find("Localize(\"Menu panel color\")"), std::string::npos);
	EXPECT_EQ(Source.find("Localize(\"Menu panel opacity\")"), std::string::npos);
	EXPECT_EQ(Source.find("Localize(\"Menu panel elevated opacity\")"), std::string::npos);
	EXPECT_EQ(Source.find("s_MenuPanelColorResetId"), std::string::npos);
	EXPECT_EQ(Source.find("g_Config.m_ClMenuPanelColor"), std::string::npos);
	EXPECT_EQ(Source.find("g_Config.m_UiColor"), std::string::npos);
	EXPECT_NE(Source.find("DoLine_ColorPicker(&s_UiColorResetId"), std::string::npos);
	EXPECT_NE(Source.find("DoLine_ColorPicker(&s_MapBrowserColorResetId"), std::string::npos);
	EXPECT_NE(Source.find("DoLine_ColorPicker(&s_ScoreboardColorResetId"), std::string::npos);
	EXPECT_NE(Source.find("g_Config.m_QmUiColor"), std::string::npos);
	EXPECT_NE(Source.find("g_Config.m_QmMapBrowserColor"), std::string::npos);
	EXPECT_NE(Source.find("g_Config.m_QmScoreboardColor"), std::string::npos);
	EXPECT_NE(Source.find("g_Config.m_QmUiOpacity"), std::string::npos);
	EXPECT_NE(Source.find("g_Config.m_QmMapBrowserOpacity"), std::string::npos);
	EXPECT_NE(Source.find("g_Config.m_QmScoreboardOpacity"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"UI color\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Map browser color\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Scoreboard color\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"UI opacity\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Map browser opacity\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Scoreboard opacity\")"), std::string::npos);
	EXPECT_NE(MenusToml.find("key = \"UI opacity\""), std::string::npos);
	EXPECT_NE(MenusToml.find("simplified_chinese = \"界面不透明度\""), std::string::npos);
	EXPECT_NE(MenusToml.find("key = \"Map browser opacity\""), std::string::npos);
	EXPECT_NE(MenusToml.find("simplified_chinese = \"地图浏览器不透明度\""), std::string::npos);
	EXPECT_NE(MenusToml.find("key = \"Scoreboard opacity\""), std::string::npos);
	EXPECT_NE(MenusToml.find("simplified_chinese = \"计分板不透明度\""), std::string::npos);
}

TEST(QmNewUiMenuBranches, SettingsGraphicsOpacitySlidersExposeIndependentUiDomains)
{
	const std::string Source = ReadTextFile("src/game/client/components/menus_settings.cpp");

	EXPECT_NE(Source.find("DoSliderWithValueInput(&g_Config.m_QmUiOpacity, &g_Config.m_QmUiOpacity, Button, Localize(\"UI opacity\")"), std::string::npos);
	EXPECT_NE(Source.find("DoSliderWithValueInput(&g_Config.m_QmMapBrowserOpacity, &g_Config.m_QmMapBrowserOpacity, Button, Localize(\"Map browser opacity\")"), std::string::npos);
	EXPECT_NE(Source.find("DoSliderWithValueInput(&g_Config.m_QmScoreboardOpacity, &g_Config.m_QmScoreboardOpacity, Button, Localize(\"Scoreboard opacity\")"), std::string::npos);
}

TEST(QmNewUiMenuBranches, DefaultUiSurfacesUseBlackThirtyPercent)
{
	const std::string QmConfigSource = ReadTextFile("src/engine/shared/config_variables_qmclient.h");
	const std::string ConfigSource = ReadTextFile("src/engine/shared/config_variables.h");

	EXPECT_NE(QmConfigSource.find("MACRO_CONFIG_COL(QmUiColor, qm_ui_color, 0x000000"), std::string::npos);
	EXPECT_NE(QmConfigSource.find("MACRO_CONFIG_COL(QmMapBrowserColor, qm_map_browser_color, 0x000000"), std::string::npos);
	EXPECT_NE(QmConfigSource.find("MACRO_CONFIG_COL(QmScoreboardColor, qm_scoreboard_color, 0x000000"), std::string::npos);
	EXPECT_NE(QmConfigSource.find("MACRO_CONFIG_INT(QmUiOpacity, qm_ui_opacity, 30"), std::string::npos);
	EXPECT_NE(QmConfigSource.find("MACRO_CONFIG_INT(QmMapBrowserOpacity, qm_map_browser_opacity, 30"), std::string::npos);
	EXPECT_NE(QmConfigSource.find("MACRO_CONFIG_INT(QmScoreboardOpacity, qm_scoreboard_opacity, 30"), std::string::npos);

	EXPECT_NE(ConfigSource.find("MACRO_CONFIG_COL(UiColor, ui_color, 0x4D000000"), std::string::npos);
	EXPECT_NE(ConfigSource.find("MACRO_CONFIG_COL(ClMenuPanelColor, cl_menu_panel_color, 0x000000"), std::string::npos);
	EXPECT_NE(ConfigSource.find("MACRO_CONFIG_INT(ClMenuPanelOpacity, cl_menu_panel_opacity, 30"), std::string::npos);
	EXPECT_NE(ConfigSource.find("MACRO_CONFIG_INT(ClMenuPanelElevatedOpacity, cl_menu_panel_elevated_opacity, 30"), std::string::npos);
	EXPECT_NE(ConfigSource.find("MACRO_CONFIG_INT(ClSettingsTabbarOpacity, cl_settings_tabbar_opacity, 30"), std::string::npos);
}

TEST(QmNewUiMenuBranches, SkinTransitionAnimationToggleOwnsAdvancedControls)
{
	const std::string ConfigSource = ReadTextFile("src/engine/shared/config_variables_qmclient.h");
	const std::string MenusSource = ReadTextFile("src/game/client/components/qmclient/menus_qmclient.cpp");
	const std::string GameClientSource = ReadTextFile("src/game/client/gameclient.cpp");
	const std::string SettingsSource = ReadTextFile("src/game/client/components/menus_settings.cpp");
	const std::string LanguageSource = ReadTextFile("data/languages/simplified_chinese.txt");

	EXPECT_NE(ConfigSource.find("MACRO_CONFIG_INT(QmSkinChangeTransition, qm_skin_change_transition, 1, 0, 1"), std::string::npos);
	EXPECT_NE(MenusSource.find("pSkinTransitionAnimationFeatureId = \"qm_2_72_0_skin_transition_animation_toggle\""), std::string::npos);
	EXPECT_NE(MenusSource.find("pSkinTransitionAnimationFeatureId,\n\t\t\t\t\t\t\"qm_2_62_8_weapon_animation\""), std::string::npos);
	EXPECT_NE(MenusSource.find("return \"皮肤切换 pifu qiehuan skin transition 换皮 huanpi 动画 donghua 开关 kaiguan 类型 leixing 时长 shichang 锤中偷皮 chuizhong toupi\";"), std::string::npos);

	const size_t Toggle = MenusSource.find("DoQmSettingsCheckboxAuto(&g_Config.m_QmSkinChangeTransition");
	ASSERT_NE(Toggle, std::string::npos);
	const size_t AdvancedIf = MenusSource.find("if(g_Config.m_QmSkinChangeTransition)", Toggle);
	const size_t TypeLabel = MenusSource.find("Localize(\"Skin transition type\")", Toggle);
	const size_t DurationLabel = MenusSource.find("Localize(\"Skin transition duration\")", Toggle);
	ASSERT_NE(AdvancedIf, std::string::npos);
	ASSERT_NE(TypeLabel, std::string::npos);
	ASSERT_NE(DurationLabel, std::string::npos);
	EXPECT_LT(Toggle, AdvancedIf);
	EXPECT_LT(AdvancedIf, TypeLabel);
	EXPECT_LT(TypeLabel, DurationLabel);

	EXPECT_NE(GameClientSource.find("if(!g_Config.m_QmSkinChangeTransition || g_Config.m_QmSkinChangeTransitionMs <= 0)"), std::string::npos);
	EXPECT_NE(GameClientSource.find("if(!g_Config.m_QmSkinChangeTransition || g_Config.m_QmSkinChangeTransitionMs <= 0 || !m_SkinTransitionStart.has_value()"), std::string::npos);
	EXPECT_NE(SettingsSource.find("if(!g_Config.m_QmSkinChangeTransition || g_Config.m_QmSkinChangeTransitionMs <= 0)"), std::string::npos);
	EXPECT_NE(SettingsSource.find("if(!g_Config.m_QmSkinChangeTransition || g_Config.m_QmSkinChangeTransitionMs <= 0 || !m_StartTime.has_value()"), std::string::npos);
	EXPECT_NE(LanguageSource.find("Skin transition animation\n== 皮肤切换动画"), std::string::npos);
}

TEST(QmNewUiMenuBranches, NameplateOthersModeSuppressesLocalIdentityRows)
{
	const std::string Source = ReadTextFile("src/game/client/components/nameplates.cpp");
	const std::string UpdateCoordXAlignFrameState = FunctionBody(Source, "void CNamePlates::UpdateCoordXAlignFrameState");
	const std::string RenderNamePlateGame = FunctionBody(Source, "void CNamePlates::RenderNamePlateGame");

	EXPECT_EQ(UpdateCoordXAlignFrameState.find("FrameState.m_LocalRoundedX = RoundCoordToCentitiles(GameClient()->m_LocalCharacterPos.x / 32.0f);\n\tFrameState.m_LocalAligned = true;"), std::string::npos);
	EXPECT_NE(UpdateCoordXAlignFrameState.find("if(CoordXAlignState.m_Aligned)\n\t\t\tFrameState.m_LocalAligned = true;"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("const bool IsAnyLocalClient = GameClient()->IsLocalClientId(ClientId);"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("ClientId == m_pData->m_CoordXAlignFrame.m_LocalClientId &&"), std::string::npos);
	EXPECT_EQ(RenderNamePlateGame.find("IsLocalClient &&\n\t\tm_pData->m_CoordXAlignFrame.m_LocalAligned"), std::string::npos);
	EXPECT_EQ(RenderNamePlateGame.find("const bool OwnNameplateScopeVisible"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("Data.m_ShowName = pPlayerInfo->m_Local ? g_Config.m_ClNamePlatesOwn : g_Config.m_ClNamePlates;"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("Data.m_ShowClientId = Data.m_ShowName && (g_Config.m_Debug || g_Config.m_ClNamePlatesIds) && !HideIdentity;"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("Data.m_ShowClan = Data.m_ShowName && g_Config.m_ClNamePlatesClan && !HideIdentity;"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("const bool NameplateScopeAllowsCoords = pPlayerInfo->m_Local ? g_Config.m_ClNamePlatesOwn : g_Config.m_ClNamePlates;"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("const bool CoordModuleAllowsCoords = pPlayerInfo->m_Local ? g_Config.m_QmNameplateCoordsOwn : g_Config.m_QmNameplateCoords;"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("const bool ShowLocalAlignedCoordX = NameplateScopeAllowsCoords && CoordXAlignHintEnabled && LocalCoordXAligned;"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("Data.m_ShowCoordX = (NameplateScopeAllowsCoords && CoordModuleAllowsCoords && g_Config.m_QmNameplateCoordX != 0) || ShowLocalAlignedCoordX;"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("Data.m_ShowCoordY = NameplateScopeAllowsCoords && CoordModuleAllowsCoords && g_Config.m_QmNameplateCoordY != 0;"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("Data.m_ShowCoords = (NameplateScopeAllowsCoords && CoordModuleAllowsCoords) || ShowLocalAlignedCoordX;"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("!IsAnyLocalClient &&\n\t\tGameClient()->m_Snap.m_LocalClientId >= 0"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("if(Data.m_ShowName && !HideIdentity && g_Config.m_TcWarList && g_Config.m_TcWarListShowClan"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("Data.m_Local = pPlayerInfo->m_Local;"), std::string::npos);
}

TEST(QmNewUiMenuBranches, NameplatePreviewShowsPlayerStrongHookMarker)
{
	const std::string Source = ReadTextFile("src/game/client/components/nameplates.cpp");
	const std::string RenderNamePlatePreview = FunctionBody(Source, "void CNamePlates::RenderNamePlatePreview");

	EXPECT_NE(RenderNamePlatePreview.find("const bool IsPlayerPreview = IsOwnPreview;"), std::string::npos);
	EXPECT_NE(RenderNamePlatePreview.find("Data.m_HookStrongWeakState = IsPlayerPreview ? EHookStrongWeakState::STRONG : EHookStrongWeakState::WEAK;"), std::string::npos);
	EXPECT_NE(RenderNamePlatePreview.find("Data.m_ShowHookStrongWeak = NameplateScopeAllowsPreview && g_Config.m_ClNamePlatesStrong > 0;"), std::string::npos);
	EXPECT_EQ(RenderNamePlatePreview.find("if(DummyIdx == g_Config.m_ClDummy)"), std::string::npos);
}

TEST(QmNewUiMenuBranches, NameplatePreviewNameScopeGatesPlateExceptDirectionKeys)
{
	const std::string Source = ReadTextFile("src/game/client/components/nameplates.cpp");
	const std::string RenderNamePlatePreview = FunctionBody(Source, "void CNamePlates::RenderNamePlatePreview");

	EXPECT_NE(RenderNamePlatePreview.find("const bool IsOwnPreview = DummyIdx == 0;"), std::string::npos);
	EXPECT_NE(RenderNamePlatePreview.find("const bool NameplateScopeAllowsPreview = ForceNameplateScopeAll || (IsOwnPreview ? g_Config.m_ClNamePlatesOwn : g_Config.m_ClNamePlates);"), std::string::npos);
	EXPECT_NE(RenderNamePlatePreview.find("const bool CoordModuleAllowsPreview = IsOwnPreview ? g_Config.m_QmNameplateCoordsOwn : g_Config.m_QmNameplateCoords;"), std::string::npos);
	EXPECT_NE(RenderNamePlatePreview.find("Data.m_ShowName = NameplateScopeAllowsPreview;"), std::string::npos);
	EXPECT_NE(RenderNamePlatePreview.find("Data.m_ShowClientId = Data.m_ShowName && (g_Config.m_Debug || g_Config.m_ClNamePlatesIds);"), std::string::npos);
	EXPECT_NE(RenderNamePlatePreview.find("Data.m_ShowClan = Data.m_ShowName && g_Config.m_ClNamePlatesClan;"), std::string::npos);
	EXPECT_NE(RenderNamePlatePreview.find("Data.m_ShowCoords = NameplateScopeAllowsPreview && CoordModuleAllowsPreview;"), std::string::npos);
	EXPECT_NE(RenderNamePlatePreview.find("Data.m_ShowCoordX = Data.m_ShowCoords && g_Config.m_QmNameplateCoordX != 0;"), std::string::npos);
	EXPECT_NE(RenderNamePlatePreview.find("Data.m_ShowCoordY = Data.m_ShowCoords && g_Config.m_QmNameplateCoordY != 0;"), std::string::npos);
	EXPECT_NE(RenderNamePlatePreview.find("case 1: // Others\n\t\t\tData.m_ShowDirection = !IsOwnPreview;"), std::string::npos);
	EXPECT_NE(RenderNamePlatePreview.find("case 2: // Everyone\n\t\t\tData.m_ShowDirection = true;"), std::string::npos);
	EXPECT_NE(RenderNamePlatePreview.find("case 3: // Only self\n\t\t\tData.m_ShowDirection = IsOwnPreview;"), std::string::npos);
	EXPECT_NE(RenderNamePlatePreview.find("Data.m_ShowHookStrongWeakId = NameplateScopeAllowsPreview && g_Config.m_ClNamePlatesStrong == 2;"), std::string::npos);
	EXPECT_EQ(RenderNamePlatePreview.find("Data.m_ShowName = g_Config.m_ClNamePlates || g_Config.m_ClNamePlatesOwn;"), std::string::npos);
	EXPECT_EQ(RenderNamePlatePreview.find("Data.m_ShowDirection = NameplateScopeAllowsPreview && !IsOwnPreview;"), std::string::npos);
	EXPECT_EQ(RenderNamePlatePreview.find("Data.m_ShowDirection = NameplateScopeAllowsPreview;"), std::string::npos);
	EXPECT_EQ(RenderNamePlatePreview.find("Data.m_ShowDirection = NameplateScopeAllowsPreview && IsOwnPreview;"), std::string::npos);
	EXPECT_EQ(RenderNamePlatePreview.find("Data.m_ShowDirection = g_Config.m_ClShowDirection != 0 ? true : false;"), std::string::npos);
}

TEST(QmNewUiMenuBranches, NameplatePreviewUsesFullScopeReferenceFrame)
{
	const std::string Source = ReadTextFile("src/game/client/components/nameplates.cpp");
	const std::string RenderNamePlatePreview = FunctionBody(Source, "void CNamePlates::RenderNamePlatePreview");

	EXPECT_NE(RenderNamePlatePreview.find("auto BuildPreviewData = [&](int DummyIdx, CNamePlateData &Data, bool ForceNameplateScopeAll = false)"), std::string::npos);
	EXPECT_NE(RenderNamePlatePreview.find("BuildPreviewData(Dummy, Data);"), std::string::npos);
	EXPECT_NE(RenderNamePlatePreview.find("BuildPreviewData(Dummy, FrameData, true);"), std::string::npos);
	EXPECT_NE(RenderNamePlatePreview.find("BuildPreviewData(Dummy == 0 ? 1 : 0, OtherFrameData, true);"), std::string::npos);
	EXPECT_NE(RenderNamePlatePreview.find("pFrameNamePlate->ComputeBaselineFrame(NameplateBottomMiddle"), std::string::npos);
	EXPECT_NE(RenderNamePlatePreview.find("NamePlate.CollectCoreRowRects(Position, aEditorRects, pFrameNamePlate);"), std::string::npos);
	EXPECT_NE(RenderNamePlatePreview.find("DragHasRow, DragRowCenter, DragRowSize, pFrameNamePlate);"), std::string::npos);
	EXPECT_NE(RenderNamePlatePreview.find("NamePlate.Render(*GameClient(), Position, pFrameNamePlate);"), std::string::npos);
	EXPECT_EQ(RenderNamePlatePreview.find("NamePlate.ComputeBaselineFrame(NameplateBottomMiddle"), std::string::npos);
	EXPECT_NE(Source.find("LayoutCoreRowSize(const SCoreRowParts &CoreRow, const CNamePlate *pLayoutReference) const"), std::string::npos);
	EXPECT_NE(Source.find("Position.y -= LayoutSize.y;"), std::string::npos);
}

TEST(QmNewUiMenuBranches, NameplateGameUsesFullScopeReferenceFrame)
{
	const std::string Source = ReadTextFile("src/game/client/components/nameplates.cpp");
	const std::string RenderNamePlateGame = FunctionBody(Source, "void CNamePlates::RenderNamePlateGame");
	const std::string ResetNamePlates = FunctionBody(Source, "void CNamePlates::ResetNamePlates");

	EXPECT_NE(Source.find("CNamePlate m_aNamePlateFrameReferences[MAX_CLIENTS];"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("CNamePlate *pLayoutReference = nullptr;"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("if(Alpha > 0.0f && NameplateFreeMoveEnabled() && (!g_Config.m_ClNamePlates || !g_Config.m_ClNamePlatesOwn))"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("CNamePlateData FrameData = Data;"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("FrameData.m_ShowName = true;"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("FrameData.m_ShowCoords = CoordModuleAllowsCoords || FrameShowLocalAlignedCoordX;"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("CNamePlate &FrameNamePlate = m_pData->m_aNamePlateFrameReferences[ClientId];"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("FrameNamePlate.Update(*GameClient(), FrameData);"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("pLayoutReference = &FrameNamePlate;"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("NamePlate.Render(*GameClient(), Position - vec2(0.0f, (float)g_Config.m_ClNamePlatesOffset), pLayoutReference);"), std::string::npos);
	EXPECT_NE(ResetNamePlates.find("for(CNamePlate &NamePlate : m_pData->m_aNamePlateFrameReferences)\n\t\tNamePlate.Reset(*GameClient());"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("Data.m_ShowDirection = !pPlayerInfo->m_Local;"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("Data.m_ShowDirection = true;"), std::string::npos);
	EXPECT_NE(RenderNamePlateGame.find("Data.m_ShowDirection = pPlayerInfo->m_Local;"), std::string::npos);
}

TEST(QmNewUiMenuBranches, NewOpacityControlsDoNotChainLegacyPanelOpacity)
{
	const std::string MenusSource = ReadTextFile("src/game/client/components/menus.cpp");

	EXPECT_EQ(MenusSource.find("m_ClMenuPanelOpacity / 100.0f) * (g_Config.m_QmUiOpacity"), std::string::npos);
	EXPECT_EQ(MenusSource.find("m_ClMenuPanelElevatedOpacity / 100.0f) * (g_Config.m_QmUiOpacity"), std::string::npos);
	EXPECT_EQ(MenusSource.find("m_ClSettingsTabbarOpacity / 100.0f) * (g_Config.m_QmUiOpacity"), std::string::npos);
	EXPECT_EQ(MenusSource.find("m_ClMenuPanelOpacity / 100.0f) * (g_Config.m_QmMapBrowserOpacity"), std::string::npos);
	EXPECT_EQ(MenusSource.find("m_ClMenuPanelElevatedOpacity / 100.0f) * (g_Config.m_QmMapBrowserOpacity"), std::string::npos);
}

TEST(QmNewUiMenuBranches, NewColorControlsUseIndependentUiDomains)
{
	const std::string ConfigSource = ReadTextFile("src/engine/shared/config_variables_qmclient.h");
	const std::string MenusSource = ReadTextFile("src/game/client/components/menus.cpp");
	const std::string BrowserSource = ReadTextFile("src/game/client/components/menus_browser.cpp");
	const std::string ScoreboardSource = ReadTextFile("src/game/client/components/scoreboard.cpp");

	EXPECT_NE(ConfigSource.find("MACRO_CONFIG_COL(QmUiColor, qm_ui_color"), std::string::npos);
	EXPECT_NE(ConfigSource.find("MACRO_CONFIG_COL(QmMapBrowserColor, qm_map_browser_color"), std::string::npos);
	EXPECT_NE(ConfigSource.find("MACRO_CONFIG_COL(QmScoreboardColor, qm_scoreboard_color"), std::string::npos);
	EXPECT_NE(MenusSource.find("ColorHSLA(g_Config.m_QmUiColor)"), std::string::npos);
	EXPECT_NE(MenusSource.find("ColorHSLA(g_Config.m_QmMapBrowserColor)"), std::string::npos);
	EXPECT_EQ(MenusSource.find("ColorHSLA(g_Config.m_ClMenuPanelColor)"), std::string::npos);
	EXPECT_EQ(MenusSource.find("ColorHSLA(g_Config.m_UiColor"), std::string::npos);
	EXPECT_NE(BrowserSource.find("ColorHSLA(g_Config.m_QmMapBrowserColor)"), std::string::npos);
	EXPECT_NE(ScoreboardSource.find("ColorHSLA(g_Config.m_QmScoreboardColor)"), std::string::npos);
}

TEST(QmNewUiMenuBranches, ScoreboardBackgroundsUseScoreboardOpacity)
{
	const std::string Source = ReadTextFile("src/game/client/components/scoreboard.cpp");

	EXPECT_NE(Source.find("Color.a = ScoreboardUiAlpha(AlphaScale);"), std::string::npos);
	EXPECT_NE(Source.find("g_Config.m_QmScoreboardOpacity / 100.0f"), std::string::npos);
	EXPECT_NE(Source.find("ScoreboardDecorationColor(GameClient()->GetDDTeamColor(DDTeam).WithAlpha(0.5f * ItemAlpha))"), std::string::npos);
	EXPECT_NE(Source.find("Row.Draw(ScoreboardDecorationColor(ui_token::color::ACCENT_PRIMARY_DIM.WithMultipliedAlpha(ItemAlpha * 1.45f))"), std::string::npos);
	EXPECT_NE(Source.find("Row.Draw(ScoreboardDecorationColor(ColorRGBA(0.7f, 0.7f, 0.7f, 0.7f * ItemAlpha))"), std::string::npos);
}

TEST(QmNewUiMenuBranches, IngameMenuPrimaryActionLabelsUseEnglishKeys)
{
	const std::string Source = ReadTextFile("src/game/client/components/menus_ingame.cpp");

	EXPECT_NE(Source.find("pDisconnectButtonLabel = Localize(\"Disconnect\")"), std::string::npos);
	EXPECT_NE(Source.find("pDummyButtonLabel = Localize(\"Connect dummy\")"), std::string::npos);
	EXPECT_NE(Source.find("pDummyButtonLabel = Localize(\"Connecting dummy\")"), std::string::npos);
	EXPECT_NE(Source.find("pDummyButtonLabel = Localize(\"Disconnect dummy\")"), std::string::npos);
	EXPECT_NE(Source.find("pEditHudButtonLabel = Localize(\"Edit HUD\")"), std::string::npos);
	EXPECT_NE(Source.find("pDemoButtonLabel = Recording ? Localize(\"Stop record\") : Localize(\"Record demo\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Save last %d min\")"), std::string::npos);
	EXPECT_NE(Source.find("pDemoMarkerButtonLabel = Localize(\"Mark demo\")"), std::string::npos);
	EXPECT_NE(Source.find("pJoinRedButtonLabel = Localize(\"Join red\")"), std::string::npos);
	EXPECT_NE(Source.find("pJoinBlueButtonLabel = Localize(\"Join blue\")"), std::string::npos);
	EXPECT_NE(Source.find("pJoinGameButtonLabel = Localize(\"Join game\")"), std::string::npos);
	EXPECT_NE(Source.find("pKillButtonLabel = Localize(\"Kill\")"), std::string::npos);
	EXPECT_NE(Source.find("pPauseButtonLabel = (!Paused && !Spec) ? Localize(\"Pause\") : Localize(\"Join game\")"), std::string::npos);
	EXPECT_NE(Source.find("pFastPracticeLabel = FastPracticeEnabled ? Localize(\"Stop practice\") : Localize(\"Fast practice\")"), std::string::npos);
	EXPECT_NE(Source.find("DoToolTip(&s_DummyButton, &Button, Localize(\"Please wait…\"))"), std::string::npos);
}

TEST(QmNewUiMenuBranches, DummyAndSpectateBindLabelsUseEnglishKeys)
{
	const std::string ControlsSource = ReadTextFile("src/game/client/components/menus_settings_controls.cpp");
	const std::string TouchSource = ReadTextFile("src/game/client/components/touch_controls.cpp");

	EXPECT_NE(ControlsSource.find("Localizable(\"Toggle dummy\")"), std::string::npos);
	EXPECT_NE(ControlsSource.find("Localizable(\"Dummy jump\")"), std::string::npos);
	EXPECT_NE(ControlsSource.find("Localizable(\"Dummy fire\")"), std::string::npos);
	EXPECT_NE(ControlsSource.find("Localizable(\"Dummy hook\")"), std::string::npos);
	EXPECT_NE(ControlsSource.find("Localizable(\"Dummy copy\")"), std::string::npos);
	EXPECT_NE(ControlsSource.find("Localizable(\"Dummy hammer fly\")"), std::string::npos);
	EXPECT_NE(ControlsSource.find("Localizable(\"Control dummy\")"), std::string::npos);
	EXPECT_NE(ControlsSource.find("Localizable(\"Spectate mode\")"), std::string::npos);
	EXPECT_NE(ControlsSource.find("Localizable(\"Spectate teleport\")"), std::string::npos);
	EXPECT_NE(ControlsSource.find("Localizable(\"Spectate next\")"), std::string::npos);
	EXPECT_NE(ControlsSource.find("Localizable(\"Spectate previous\")"), std::string::npos);
	EXPECT_NE(TouchSource.find("Localizable(\"Toggle dummy\")"), std::string::npos);
	EXPECT_NE(TouchSource.find("Localizable(\"Spectate mode\")"), std::string::npos);
}

TEST(QmNewUiMenuBranches, ConsoleChatExportLabelsUseEnglishKeys)
{
	const std::string Source = ReadTextFile("src/game/client/components/console.cpp");

	EXPECT_NE(Source.find("Localize(\"QmClient chat log\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Total\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Messages\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"No chat log selected\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Chat export failed\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Exported %d chat messages\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Selected %d\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Cancel\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Export selected\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Clear\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Select all chat\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Select export\")"), std::string::npos);
}

TEST(QmNewUiMenuBranches, HudDummyStatusLabelsUseEnglishKeys)
{
	const std::string Source = ReadTextFile("src/game/client/components/hud.cpp");

	EXPECT_NE(Source.find("Localize(\"Dummy mini view\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Connect dummy to enable\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Key Sticking: ?\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Key Sticking: On\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Key Sticking: Off\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Key Sticking: Reset Self\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Hammer: %s\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Dummy Control: %s\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Dummy sync: %s\")"), std::string::npos);
}

TEST(QmNewUiMenuBranches, TranslationAndDemoUiLabelsUseEnglishKeys)
{
	const std::string ChatSource = ReadTextFile("src/game/client/components/chat.cpp");
	const std::string DemoSource = ReadTextFile("src/game/client/components/menus_demo.cpp");
	const std::string BrowserSource = ReadTextFile("src/game/client/components/menus_browser.cpp");

	EXPECT_NE(ChatSource.find("Localize(\"Translation Settings\")"), std::string::npos);
	EXPECT_NE(ChatSource.find("Localize(\"Auto-translate incoming messages\")"), std::string::npos);
	EXPECT_NE(ChatSource.find("Localize(\"Auto-translate outgoing messages\")"), std::string::npos);
	EXPECT_NE(ChatSource.find("Localize(\"Incoming language\")"), std::string::npos);
	EXPECT_NE(ChatSource.find("Localize(\"Outgoing language\")"), std::string::npos);
	EXPECT_NE(ChatSource.find("Localize(\"Translation service\")"), std::string::npos);
	EXPECT_NE(DemoSource.find("Localize(\"Could not preview this image\")"), std::string::npos);
	EXPECT_NE(DemoSource.find("BrowsingScreenshots ? Localize(\"Open the folder containing screenshots\") : Localize(\"Open the folder containing demo files\")"), std::string::npos);
	EXPECT_NE(BrowserSource.find("Localize(\"Map\")"), std::string::npos);
	EXPECT_NE(BrowserSource.find("Localize(\"Category\")"), std::string::npos);
	EXPECT_NE(BrowserSource.find("Localize(\"Difficulty stars\")"), std::string::npos);
	EXPECT_NE(BrowserSource.find("Localize(\"Note\")"), std::string::npos);
	EXPECT_NE(BrowserSource.find("Localize(\"Has save\")"), std::string::npos);
	EXPECT_NE(BrowserSource.find("Localize(\"None\")"), std::string::npos);
}

TEST(QmNewUiMenuBranches, ClientSourceDoesNotUseChineseLocalizeKeys)
{
	const std::string HudEditorSource = ReadTextFile("src/game/client/components/hud_editor.cpp");
	const std::string MenusSource = ReadTextFile("src/game/client/components/menus.cpp");
	const std::string BrowserSource = ReadTextFile("src/game/client/components/menus_browser.cpp");
	const std::string DemoSource = ReadTextFile("src/game/client/components/menus_demo.cpp");
	const std::string IngameTouchSource = ReadTextFile("src/game/client/components/menus_ingame_touch_controls.cpp");
	const std::string IngameSource = ReadTextFile("src/game/client/components/menus_ingame.cpp");
	const std::string SettingsSource = ReadTextFile("src/game/client/components/menus_settings.cpp");
	const std::string SettingsControlsSource = ReadTextFile("src/game/client/components/menus_settings_controls.cpp");
	const std::string Settings7Source = ReadTextFile("src/game/client/components/menus_settings7.cpp");
	const std::string StartSource = ReadTextFile("src/game/client/components/menus_start.cpp");
	const std::string PieMenuSource = ReadTextFile("src/game/client/components/pie_menu.cpp");
	const std::string ScoreboardSource = ReadTextFile("src/game/client/components/scoreboard.cpp");

	EXPECT_NE(HudEditorSource.find("Localize(\"Position jump tip\")"), std::string::npos);
	EXPECT_NE(MenusSource.find("m_apSettingsTabs[SETTINGS_SOUND] = Localize(\"Sound\")"), std::string::npos);
	EXPECT_NE(BrowserSource.find("Localize(\"DDmaX Easy\")"), std::string::npos);
	EXPECT_NE(BrowserSource.find("Localize(\"Favorite map\")"), std::string::npos);
	EXPECT_NE(DemoSource.find("Localize(\"Screenshots directory\")"), std::string::npos);
	EXPECT_NE(IngameTouchSource.find("Localize(\"Allow dummy\", \"Touch button visibilities\")"), std::string::npos);
	EXPECT_NE(IngameTouchSource.find("Localize(\"Dummy connected\", \"Touch button visibilities\")"), std::string::npos);
	EXPECT_NE(IngameTouchSource.find("Localize(\"Spectate\", \"Predefined touch button behaviors\")"), std::string::npos);
	EXPECT_NE(IngameSource.find("Localize(\"Spectate\")"), std::string::npos);
	EXPECT_NE(IngameSource.find("Localize(\"Dummies are not allowed on this server\")"), std::string::npos);
	EXPECT_NE(SettingsSource.find("Localize(\"Show spectator cursor\")"), std::string::npos);
	EXPECT_NE(SettingsSource.find("Localize(\"Auto save chat log\")"), std::string::npos);
	EXPECT_NE(SettingsControlsSource.find("Localize(\"Dummy\")"), std::string::npos);
	EXPECT_NE(Settings7Source.find("Localize(\"Dummy\")"), std::string::npos);
	EXPECT_NE(StartSource.find("Localize(\"(Update required)\")"), std::string::npos);
	EXPECT_NE(PieMenuSource.find("Localize(\"Spectate\")"), std::string::npos);
	EXPECT_NE(ScoreboardSource.find("Localize(\"Spectators\")"), std::string::npos);
}

TEST(QmNewUiMenuBranches, QmClientAxiomAutoLoginLivesInQmClientComponent)
{
	const std::string Source = ReadTextFile("src/game/client/components/qmclient/axiom_auto_login.cpp");
	const std::string Header = ReadTextFile("src/game/client/components/qmclient/axiom_auto_login.h");
	const std::string TClientHeader = ReadTextFile("src/game/client/components/tclient/tclient.h");
	const std::string TClientSource = ReadTextFile("src/game/client/components/tclient/tclient.cpp");
	const std::string GameClientHeader = ReadTextFile("src/game/client/gameclient.h");

	EXPECT_NE(Header.find("class CQmAxiomAutoLogin : public CComponent"), std::string::npos);
	EXPECT_NE(GameClientHeader.find("CQmAxiomAutoLogin m_QmAxiomAutoLogin;"), std::string::npos);
	EXPECT_NE(Source.find("void CQmAxiomAutoLogin::TrySendLogin()"), std::string::npos);
	EXPECT_NE(Source.find("void CQmAxiomAutoLogin::TrySendDummyLogin()"), std::string::npos);
	EXPECT_NE(Source.find("bool CQmAxiomAutoLogin::IsAxiomCommunity() const"), std::string::npos);
	EXPECT_NE(Source.find("QMCLIENT_AXIOM_AUTO_LOGIN_SLOW_RETRY_SECONDS"), std::string::npos);
	EXPECT_NE(Source.find("m_AutoLoginSlowRetryMode"), std::string::npos);
	EXPECT_NE(Source.find("m_AutoLoginHardFailed"), std::string::npos);
	EXPECT_NE(Source.find("ScheduleSlowRetry"), std::string::npos);
	EXPECT_NE(Source.find("IsHardLoginFailure"), std::string::npos);
	EXPECT_NE(Source.find("IsLoginContextMessage"), std::string::npos);
	EXPECT_NE(Source.find("return IsLoginContextMessage(pText) &&"), std::string::npos);
	const size_t HardFailureCheck = Source.find("IsHardLoginFailure(pText)");
	const size_t LoginMessageFilter = Source.find("const bool IsLoginMessage");
	EXPECT_NE(HardFailureCheck, std::string::npos);
	EXPECT_NE(LoginMessageFilter, std::string::npos);
	EXPECT_LT(HardFailureCheck, LoginMessageFilter);
	EXPECT_NE(Source.find("Localize(\"Trying Axiom auto login\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Axiom auto login succeeded\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Axiom auto login failed, retrying\")"), std::string::npos);
	EXPECT_NE(Source.find("Localize(\"Axiom auto login failed\")"), std::string::npos);

	EXPECT_EQ(TClientHeader.find("IsAxiomCommunity() const"), std::string::npos);
	EXPECT_EQ(TClientHeader.find("ResetAxiomAutoLoginState"), std::string::npos);
	EXPECT_EQ(TClientHeader.find("UpdateAxiomAutoLogin"), std::string::npos);
	EXPECT_EQ(TClientHeader.find("HandleAxiomAutoLoginMessage"), std::string::npos);
	EXPECT_EQ(TClientSource.find("TrySendAxiomLogin"), std::string::npos);
	EXPECT_EQ(TClientSource.find("HandleAxiomAutoLoginMessage"), std::string::npos);
}

TEST(QmNewUiMenuBranches, TClientSettingsTabsPreserveHiddenStateAndVisibleCorners)
{
	const std::string Source = ReadTextFile("src/game/client/components/tclient/menus_tclient.cpp");
	const std::string RenderSettingsTClient = FunctionBody(Source, "void CMenus::RenderSettingsTClient(CUIRect MainView, bool PrewarmOnly)");
	const std::string RenderSettingsTClientInfo = FunctionBody(Source, "void CMenus::RenderSettingsTClientInfo(CUIRect MainView)");

	EXPECT_NE(RenderSettingsTClient.find("if(TabCount <= 0)"), std::string::npos);
	EXPECT_NE(RenderSettingsTClient.find("FirstVisibleTab"), std::string::npos);
	EXPECT_NE(RenderSettingsTClient.find("VisibleTabIndex"), std::string::npos);
	EXPECT_NE(RenderSettingsTClient.find("VisibleTabIndex == 0"), std::string::npos);
	EXPECT_NE(RenderSettingsTClient.find("VisibleTabIndex == TabCount - 1"), std::string::npos);
	EXPECT_NE(RenderSettingsTClientInfo.find("s_aShowTabs[i] = IsFlagSet(g_Config.m_TcTClientSettingsTabs, i);"), std::string::npos);
}

TEST(QmNewUiMenuBranches, TClientWarListDefersDeletesAndValidatesSelections)
{
	const std::string Source = ReadTextFile("src/game/client/components/tclient/menus_tclient.cpp");
	const std::string RenderSettingsTClientWarList = FunctionBody(Source, "void CMenus::RenderSettingsTClientWarList(CUIRect MainView)");

	EXPECT_NE(RenderSettingsTClientWarList.find("static CWarType *s_pSelectedType = nullptr;"), std::string::npos);
	EXPECT_NE(RenderSettingsTClientWarList.find("WarTypeExists"), std::string::npos);
	EXPECT_NE(RenderSettingsTClientWarList.find("WarEntryExists"), std::string::npos);
	EXPECT_NE(RenderSettingsTClientWarList.find("CWarEntry *pEntryToRemove = nullptr;"), std::string::npos);
	EXPECT_NE(RenderSettingsTClientWarList.find("RemoveWarEntry(pEntryToRemove);"), std::string::npos);
	EXPECT_NE(RenderSettingsTClientWarList.find("s_pSelectedEntry = nullptr;"), std::string::npos);
	EXPECT_NE(RenderSettingsTClientWarList.find("NewSelectedEntry < (int)s_vFilteredEntries.size()"), std::string::npos);
	EXPECT_NE(RenderSettingsTClientWarList.find("NewSelectedType < (int)GameClient()->m_WarList.m_WarTypes.size()"), std::string::npos);
}

TEST(QmNewUiMenuBranches, TClientProfilesAndStatusBarClampUiIndices)
{
	const std::string Source = ReadTextFile("src/game/client/components/tclient/menus_tclient.cpp");
	const std::string RenderSettingsTClientProfiles = FunctionBody(Source, "void CMenus::RenderSettingsTClientProfiles(CUIRect MainView)");
	const std::string RenderSettingsTClientStatusBar = FunctionBody(Source, "void CMenus::RenderSettingsTClientStatusBar(CUIRect MainView)");

	EXPECT_NE(RenderSettingsTClientProfiles.find("Profile.m_FeetColor >= 0"), std::string::npos);
	EXPECT_NE(RenderSettingsTClientProfiles.find("ProfilesPerRow = maximum(1"), std::string::npos);
	EXPECT_NE(RenderSettingsTClientStatusBar.find("StatusItemTypeCount"), std::string::npos);
	EXPECT_NE(RenderSettingsTClientStatusBar.find("s_TypeSelectedOld < StatusItemTypeCount"), std::string::npos);
	EXPECT_NE(RenderSettingsTClientStatusBar.find("s_SelectedItem < (int)GameClient()->m_StatusBar.m_StatusBarItems.size()"), std::string::npos);
}

TEST(QmNewUiMenuBranches, SettingsListSelectionsClampBeforeIndexing)
{
	const std::string Source = ReadTextFile("src/game/client/components/menus_settings.cpp");
	const std::string RenderSettingsPlayer = FunctionBody(Source, "void CMenus::RenderSettingsPlayer(CUIRect MainView)");
	const std::string RenderSettingsGraphics = FunctionBody(Source, "void CMenus::RenderSettingsGraphics(CUIRect MainView)");
	const std::string PopupMapPicker = FunctionBody(Source, "CUi::EPopupMenuFunctionResult CMenus::PopupMapPicker(void *pContext, CUIRect View, bool Active)");

	EXPECT_NE(RenderSettingsPlayer.find("NewSelected >= 0 && NewSelected < (int)vpFilteredFlags.size()"), std::string::npos);
	EXPECT_NE(RenderSettingsGraphics.find("NewSelected >= 0 && NewSelected < s_NumNodes"), std::string::npos);
	EXPECT_NE(PopupMapPicker.find("const int ItemIndex = MapIndex++;"), std::string::npos);
	EXPECT_NE(PopupMapPicker.find("ItemIndex == pPopupContext->m_Selection"), std::string::npos);
	EXPECT_NE(PopupMapPicker.find("NewSelected >= 0 && NewSelected < (int)pPopupContext->m_vMaps.size()"), std::string::npos);
	EXPECT_NE(PopupMapPicker.find("pPopupContext->m_Selection >= 0"), std::string::npos);
}

TEST(QmNewUiMenuBranches, QmClientLanguageReadmeDescribesChineseSourceKeys)
{
	EXPECT_FALSE(fs_is_dir(TestSourcePath("data/qmclient/languages").c_str()));
}

TEST(QmNewUiMenuBranches, KcpLogUsesBoundedFormatting)
{
	const std::string Source = ReadTextFile("src/engine/external/kcp/ikcp.c");

	EXPECT_EQ(Source.find("vsprintf(buffer, fmt, argptr);"), std::string::npos);
	EXPECT_NE(Source.find("vsnprintf(buffer, sizeof(buffer), fmt, argptr);"), std::string::npos);
}

TEST(QmNewUiMenuBranches, DisplayChangedDoesNotUseDisplayUnionData)
{
	const std::string Source = ReadTextFile("src/engine/client/input.cpp");
	const size_t CaseStart = Source.find("case SDL_WINDOWEVENT_DISPLAY_CHANGED:");
	ASSERT_NE(CaseStart, std::string::npos);
	const size_t Break = Source.find("break;", CaseStart);
	ASSERT_NE(Break, std::string::npos);
	const std::string Body = Source.substr(CaseStart, Break - CaseStart);

	EXPECT_EQ(Body.find("Event.display.data1"), std::string::npos);
	EXPECT_NE(Body.find("Event.window.data1"), std::string::npos);
	EXPECT_NE(Body.find("Graphics()->SwitchWindowScreen(DisplayIndex, false);"), std::string::npos);
}

TEST(QmNewUiMenuBranches, GraphicsDriverCrashRecoveryUsesSafeStartupFallback)
{
	const std::string Source = ReadTextFile("src/engine/client/client.cpp");
	const std::string Detector = FunctionBody(Source, "static bool QmCrashTextHasGraphicsDriverFault");
	const std::string Recovery = FunctionBody(Source, "static bool ApplyQmSafeGraphicsRecovery");
	const std::string StartupHook = FunctionBody(Source, "static void RecoverQmGraphicsSettingsAfterDriverCrash");

	EXPECT_NE(Detector.find("Exception module: nvoglv64.dll"), std::string::npos);
	EXPECT_NE(Detector.find(" in module nvoglv64.dll"), std::string::npos);
	EXPECT_NE(Detector.find("Exception module: vulkan-1.dll"), std::string::npos);
	EXPECT_NE(Detector.find("Exception module: D3D12Core.dll"), std::string::npos);
	EXPECT_NE(Detector.find(" in module D3D12Core.dll"), std::string::npos);
	EXPECT_NE(Detector.find("Exception module: d3d12.dll"), std::string::npos);
	EXPECT_NE(Detector.find("Exception module: dxgi.dll"), std::string::npos);
	EXPECT_NE(Detector.find(" in module opengl32.dll"), std::string::npos);

	EXPECT_NE(StartupHook.find("gs_pQmLifecycleMarkerFile"), std::string::npos);
	EXPECT_NE(StartupHook.find("ListDirectoryInfo"), std::string::npos);
	EXPECT_NE(StartupHook.find("ReadFileStr"), std::string::npos);

	EXPECT_NE(Recovery.find("str_copy(g_Config.m_GfxBackend, \"OpenGL\");"), std::string::npos);
	EXPECT_NE(Recovery.find("g_Config.m_GfxGLMajor = 3;"), std::string::npos);
	EXPECT_NE(Recovery.find("g_Config.m_GfxGLMinor = 0;"), std::string::npos);
	EXPECT_NE(Recovery.find("g_Config.m_GfxFsaaSamples = 0;"), std::string::npos);
	EXPECT_NE(Recovery.find("g_Config.m_GfxFullscreen = 0;"), std::string::npos);

	const size_t HookCall = Source.find("RecoverQmGraphicsSettingsAfterDriverCrash(pStorage);");
	const size_t CommandLineParse = Source.find("pConsole->ParseArguments(argc - 1, &argv[1]);");
	ASSERT_NE(HookCall, std::string::npos);
	ASSERT_NE(CommandLineParse, std::string::npos);
	EXPECT_LT(HookCall, CommandLineParse);
}

TEST(QmNewUiMenuBranches, LiveDirectorChatToggleIsHandledByOverlayInput)
{
	const std::string Source = ReadTextFile("src/game/client/gameclient.cpp");
	const std::string Contains = FunctionBody(Source, "bool CGameClient::LiveObserverOverlayContains");
	const std::string Input = FunctionBody(Source, "bool CGameClient::HandleLiveObserverInput");
	const std::string Render = FunctionBody(Source, "void CGameClient::RenderLiveObserverOverlay");

	EXPECT_NE(Source.find("constexpr float LIVE_OBSERVER_CHAT_TOGGLE_W"), std::string::npos);
	EXPECT_NE(Source.find("constexpr float LIVE_OBSERVER_CHAT_TOGGLE_H"), std::string::npos);
	EXPECT_NE(Source.find("CUIRect LiveObserverChatToggleRect(float Height)"), std::string::npos);
	EXPECT_NE(Contains.find("LiveObserverChatToggleRect(LIVE_OBSERVER_UI_HEIGHT).Inside(MousePos)"), std::string::npos);
	EXPECT_NE(Input.find("g_Config.m_ClShowChat = g_Config.m_ClShowChat == 0 ? 1 : 0;"), std::string::npos);
	EXPECT_NE(Input.find("Input()->MouseModeAbsolute();"), std::string::npos);
	EXPECT_LT(Input.find("LiveObserverChatToggleRect(LIVE_OBSERVER_UI_HEIGHT).Inside(MousePos)"), Input.find("if(Panel.Inside(MousePos))"));
	EXPECT_NE(Render.find("const CUIRect ChatToggle = LiveObserverChatToggleRect(Height);"), std::string::npos);
	EXPECT_NE(Render.find("ChatVisible ? Localize(\"Hide Chat\") : Localize(\"Show chat\")"), std::string::npos);
}

TEST(QmNewUiMenuBranches, ImplausibleRefreshRatesAreNotPersisted)
{
	const std::string Backend = ReadTextFile("src/engine/client/backend_sdl.cpp");
	const std::string Graphics = ReadTextFile("src/engine/client/graphics_threaded.cpp");

	EXPECT_NE(Backend.find("static bool IsPlausibleRefreshRate(int RefreshRate)"), std::string::npos);
	EXPECT_NE(Backend.find("static bool IsPlausibleWindowSize(int Width, int Height)"), std::string::npos);
	EXPECT_NE(Backend.find("Ignoring implausible configured window size"), std::string::npos);
	EXPECT_NE(Backend.find("*pWidth = DisplayMode.w;"), std::string::npos);
	EXPECT_NE(Backend.find("*pHeight = DisplayMode.h;"), std::string::npos);
	EXPECT_NE(Backend.find("Ignoring implausible configured refresh rate"), std::string::npos);
	EXPECT_NE(Backend.find("*pRefreshRate = 0;"), std::string::npos);
	EXPECT_NE(Graphics.find("static bool IsPlausibleWindowRefreshRate(int RefreshRate)"), std::string::npos);
	EXPECT_NE(Graphics.find("Ignoring implausible refresh rate during resize"), std::string::npos);
	EXPECT_NE(Graphics.find("RefreshRate = m_ScreenRefreshRate;"), std::string::npos);
	EXPECT_NE(Graphics.find("static bool IsPlausibleWindowSize(int Width, int Height)"), std::string::npos);
	EXPECT_NE(Graphics.find("static int LogicalWindowSizeFromViewport(int ViewportSize, float HiDPIScale)"), std::string::npos);
	EXPECT_NE(Graphics.find("Ignoring implausible resize dimensions"), std::string::npos);
	EXPECT_NE(Graphics.find("if(IsPlausibleWindowSize(g_Config.m_GfxScreenWidth, g_Config.m_GfxScreenHeight))"), std::string::npos);
	EXPECT_EQ(Graphics.find("w = g_Config.m_GfxScreenWidth > 0 ? g_Config.m_GfxScreenWidth : m_ScreenWidth;"), std::string::npos);
	EXPECT_EQ(Graphics.find("h = g_Config.m_GfxScreenHeight > 0 ? g_Config.m_GfxScreenHeight : m_ScreenHeight;"), std::string::npos);
	EXPECT_NE(Graphics.find("w = LogicalWindowSizeFromViewport(m_ScreenWidth, m_ScreenHiDPIScale);"), std::string::npos);
	EXPECT_NE(Graphics.find("h = LogicalWindowSizeFromViewport(m_ScreenHeight, m_ScreenHiDPIScale);"), std::string::npos);
}

TEST(QmNewUiMenuBranches, GraphicsCurrentModeLabelSanitizesScaleAndAspectRatio)
{
	const std::string Source = ReadTextFile("src/game/client/components/menus_settings.cpp");

	EXPECT_NE(Source.find("const float HiDPIScale = std::isfinite(RawHiDPIScale) && RawHiDPIScale > 0.0f ? RawHiDPIScale : 1.0f;"), std::string::npos);
	EXPECT_NE(Source.find("const int AspectGcd = G > 0 ? G : 1;"), std::string::npos);
	EXPECT_NE(Source.find("g_Config.m_GfxScreenWidth / AspectGcd"), std::string::npos);
	EXPECT_NE(Source.find("g_Config.m_GfxScreenHeight / AspectGcd"), std::string::npos);
}
