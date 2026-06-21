#include "test.h"

#include <gtest/gtest.h>

#include <string>

TEST(SteamPresence, UsesClientNameForVisibleStatus)
{
	const std::string Source = ReadTestSourceFile("src/engine/client/steam.cpp");

	EXPECT_NE(Source.find("#include <game/version.h>"), std::string::npos);
	EXPECT_NE(Source.find("SteamAPI_ISteamFriends_SetRichPresence(m_pSteamFriends, \"status\", CLIENT_NAME);"), std::string::npos);
	EXPECT_NE(Source.find("SteamAPI_ISteamFriends_SetRichPresence(m_pSteamFriends, \"map\", pMapName);"), std::string::npos);
	EXPECT_EQ(Source.find("SteamAPI_ISteamFriends_SetRichPresence(m_pSteamFriends, \"status\", pMapName);"), std::string::npos);
}

TEST(SteamPresence, KeepsBrandPresenceWhenGameInfoClears)
{
	const std::string Source = ReadTestSourceFile("src/engine/client/steam.cpp");

	const size_t ResetClientPresencePos = Source.find("void ResetClientPresence()");
	ASSERT_NE(ResetClientPresencePos, std::string::npos);
	const size_t PublicSectionPos = Source.find("public:", ResetClientPresencePos);
	ASSERT_NE(PublicSectionPos, std::string::npos);
	const std::string ResetClientPresenceBody = Source.substr(ResetClientPresencePos, PublicSectionPos - ResetClientPresencePos);
	EXPECT_NE(ResetClientPresenceBody.find("SteamAPI_ISteamFriends_ClearRichPresence(m_pSteamFriends);"), std::string::npos);
	EXPECT_NE(ResetClientPresenceBody.find("SetClientPresence();"), std::string::npos);

	const size_t ConstructorPos = Source.find("CSteam()");
	ASSERT_NE(ConstructorPos, std::string::npos);
	const size_t DestructorPos = Source.find("~CSteam()", ConstructorPos);
	ASSERT_NE(DestructorPos, std::string::npos);
	const std::string ConstructorBody = Source.substr(ConstructorPos, DestructorPos - ConstructorPos);
	EXPECT_NE(ConstructorBody.find("ResetClientPresence();"), std::string::npos);

	const size_t ClearGameInfoPos = Source.find("void ClearGameInfo() override");
	ASSERT_NE(ClearGameInfoPos, std::string::npos);
	const size_t SetGameInfoPos = Source.find("void SetGameInfo", ClearGameInfoPos);
	ASSERT_NE(SetGameInfoPos, std::string::npos);
	const std::string ClearGameInfoBody = Source.substr(ClearGameInfoPos, SetGameInfoPos - ClearGameInfoPos);
	EXPECT_NE(ClearGameInfoBody.find("ResetClientPresence();"), std::string::npos);
}
